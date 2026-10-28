import json
import unittest

import httpx
import numpy as np
import torch

from scripts.dc_evaluation import (
    DcEvaluation,
    DcStage,
    SolverOutcome,
    _classify_ngspice_stage,
    _ngspice_failure_kind,
)
from scripts.deepseek_client import DeepSeekClient, validate_deepseek_api_key
from scripts.deepseek_netlist import extract_deepseek_netlist
from scripts.netlist_features import (
    FEATURE_NAMES,
    SOLVER_FEATURE_NAMES,
    build_a2c_state,
    extract_netlist_features,
)
from scripts.pta_reward import calculate_pta_reward
from scripts.pta_testing import FAILURE, SUCCESS
from scripts.reinforcement_learning import Net, v_wrap


def outcome(profile, code, stage, failure_kind=None):
    artifacts = {"validation": {"verified_pta": stage == DcStage.PTA and code == SUCCESS}}
    if failure_kind is not None:
        artifacts["failure_kind"] = failure_kind
    return SolverOutcome(profile, code, stage, artifacts)


def evaluation_for(stage):
    nr_success = stage == DcStage.NR
    source_success = stage in {DcStage.NR, DcStage.SOURCE_STEPPING}
    profiles = {
        "nr_only": outcome(
            "nr_only",
            SUCCESS if nr_success else FAILURE,
            DcStage.NR,
        ),
        "nr_source": outcome(
            "nr_source",
            SUCCESS if source_success else FAILURE,
            DcStage.NR if nr_success else DcStage.SOURCE_STEPPING,
        ),
        "nr_pta": outcome(
            "nr_pta",
            SUCCESS if stage in {DcStage.NR, DcStage.PTA} else FAILURE,
            stage if stage in {DcStage.NR, DcStage.PTA} else DcStage.PTA,
        ),
        "full_fallback": outcome(
            "full_fallback",
            SUCCESS if stage in {DcStage.NR, DcStage.SOURCE_STEPPING, DcStage.PTA} else FAILURE,
            stage,
        ),
    }
    evaluation = DcEvaluation(profiles)
    evaluation.confirmation = {"confirmed": stage == DcStage.PTA}
    if stage in {DcStage.NR, DcStage.GMIN, DcStage.SOURCE_STEPPING}:
        evaluation.ngspice = outcome("ngspice", SUCCESS, stage)
    elif stage == DcStage.PTA:
        evaluation.ngspice = outcome(
            "ngspice",
            FAILURE,
            DcStage.SOURCE_STEPPING,
            failure_kind="convergence",
        )
    return evaluation


class DcEvaluationTests(unittest.TestCase):
    def test_reward_orders_recovery_stages(self):
        nr = calculate_pta_reward(evaluation_for(DcStage.NR)).total
        gmin = calculate_pta_reward(evaluation_for(DcStage.GMIN)).total
        source = calculate_pta_reward(
            evaluation_for(DcStage.SOURCE_STEPPING)
        ).total
        pta = calculate_pta_reward(evaluation_for(DcStage.PTA)).total
        self.assertLess(nr, gmin)
        self.assertLess(gmin, source)
        self.assertLess(source, pta)

    def test_invalid_ngspice_is_not_a_differential_success(self):
        evaluation = evaluation_for(DcStage.PTA)
        evaluation.ngspice = outcome(
            "ngspice",
            FAILURE,
            DcStage.NR,
            failure_kind="invalid",
        )
        reward = calculate_pta_reward(evaluation)
        self.assertEqual(evaluation.stage, DcStage.INVALID)
        self.assertFalse(evaluation.baseline_failed)
        self.assertEqual(reward.total, -6.0)

    def test_ngspice_success_blocks_internal_pta_bonus(self):
        evaluation = evaluation_for(DcStage.PTA)
        evaluation.ngspice = outcome("ngspice", SUCCESS, DcStage.NR)
        reward = calculate_pta_reward(evaluation, novelty=1.0)
        self.assertFalse(evaluation.authoritative_target)
        self.assertFalse(reward.target_hit)
        self.assertEqual(reward.differential_bonus, 0.0)
        self.assertEqual(reward.full_fallback_bonus, 0.0)
        self.assertEqual(reward.nr_pta_bonus, 0.0)
        self.assertLessEqual(reward.novelty_bonus, 0.25)
        self.assertLess(reward.total, 0.0)

    def test_internal_objective_is_explicit(self):
        evaluation = evaluation_for(DcStage.PTA)
        evaluation.ngspice = None
        with self.assertRaisesRegex(ValueError, "requires an ngspice outcome"):
            calculate_pta_reward(evaluation)
        reward = calculate_pta_reward(
            evaluation,
            objective="pta_internal",
        )
        self.assertTrue(reward.target_hit)
        self.assertGreater(reward.total, 0.0)

    def test_ngspice_stage_and_failure_parsers(self):
        self.assertEqual(
            _classify_ngspice_stage("Starting dynamic gmin stepping\ncompleted"),
            DcStage.GMIN,
        )
        self.assertEqual(
            _classify_ngspice_stage(
                "gmin stepping failed\nStarting source stepping\ncompleted"
            ),
            DcStage.SOURCE_STEPPING,
        )
        self.assertEqual(
            _ngspice_failure_kind("DC solution failed: no convergence"),
            "convergence",
        )
        self.assertEqual(
            _ngspice_failure_kind("Error on line 4: unknown parameter"),
            "invalid",
        )
        self.assertEqual(
            _ngspice_failure_kind(
                "singular matrix: check nodes q and qb\nDC solution failed"
            ),
            "convergence",
        )
        self.assertEqual(
            _ngspice_failure_kind("singular matrix: check nodes q and qb"),
            "invalid",
        )


class A2cFeatureTests(unittest.TestCase):
    def test_feature_state_and_exploratory_update(self):
        deck = """SpiceFuzz generated DC operating-point netlist
VDD vdd 0 5
R1 vdd q 10k
M1 q q 0 0 NMOD W=10u L=1u
.model NMOD NMOS LEVEL=1 VTO=0.7 KP=100u
.ic V(q)=0.1
.op
.end
"""
        features = extract_netlist_features(deck)
        self.assertEqual(len(features), len(FEATURE_NAMES))
        self.assertTrue(np.isfinite(features).all())
        state = build_a2c_state(np.zeros(27), np.zeros(27), features)
        self.assertEqual(len(state), 54 + len(FEATURE_NAMES) + len(SOLVER_FEATURE_NAMES))

        net = Net(len(state), 27)
        action = net.choose_action(
            v_wrap(state[None, :]),
            temperature=1.15,
            epsilon=0.2,
            exploration_bonus=np.full(27, 0.1),
        )
        loss = net.loss_func(
            v_wrap(state[None, :]),
            action,
            v_wrap(np.asarray([[3.0]], dtype=np.float32)),
            entropy_beta=0.01,
        )
        loss.backward()
        self.assertTrue(torch.isfinite(loss))


class DeepSeekConfigurationTests(unittest.TestCase):
    NETLIST_BODY = """V1 in 0 1
R1 in out 1k
D1 out 0 DMOD
.model DMOD D IS=1e-14
.op
.end"""

    def test_rejects_placeholder_and_non_ascii_keys(self):
        with self.assertRaisesRegex(ValueError, "placeholder"):
            validate_deepseek_api_key("你的API密钥")
        with self.assertRaisesRegex(ValueError, "ASCII"):
            validate_deepseek_api_key("sk-test-中文")

    def test_accepts_ascii_key(self):
        self.assertEqual(
            validate_deepseek_api_key("sk-test-key"),
            "sk-test-key",
        )

    def test_chat_completion_uses_deepseek_endpoint(self):
        def handler(request):
            self.assertEqual(
                str(request.url),
                "https://api.deepseek.com/chat/completions",
            )
            self.assertEqual(request.headers["authorization"], "Bearer sk-test-key")
            self.assertEqual(
                json.loads(request.content)["thinking"],
                {"type": "disabled"},
            )
            return httpx.Response(200, json={
                "choices": [{
                    "finish_reason": "stop",
                    "message": {"content": "```netlist```"},
                }],
            })

        client = DeepSeekClient(
            "sk-test-key",
            transport=httpx.MockTransport(handler),
        )
        reply = client.create_chat_completion(
            [{"role": "user", "content": "mutate"}],
        )
        client.close()
        self.assertEqual(reply, "```netlist```")
        self.assertEqual(client.last_finish_reason, "stop")

    def test_extracts_marker_wrapped_netlist(self):
        reply = f"BEGIN_NETLIST\n{self.NETLIST_BODY}\nEND_NETLIST"
        extracted = extract_deepseek_netlist(reply)
        self.assertTrue(extracted.startswith("SpiceFuzz generated"))
        self.assertTrue(extracted.endswith(".end\n"))

    def test_extracts_tagged_fence_and_plain_text(self):
        fenced = f"Here is the result:\n```spice\n{self.NETLIST_BODY}\n```"
        plain = f"I made the requested mutation.\n{self.NETLIST_BODY}\nDone."
        self.assertIsNotNone(extract_deepseek_netlist(fenced))
        self.assertIsNotNone(extract_deepseek_netlist(plain))

    def test_repairs_missing_op_or_end(self):
        missing_op = extract_deepseek_netlist("R1 1 0 1k\n.end")
        missing_end = extract_deepseek_netlist("R1 1 0 1k\n.op")
        self.assertTrue(missing_op.endswith("R1 1 0 1k\n.op\n.end\n"))
        self.assertTrue(missing_end.endswith("R1 1 0 1k\n.op\n.end\n"))

    def test_extracts_json_wrapped_netlist(self):
        reply = json.dumps({"netlist": self.NETLIST_BODY})
        self.assertIsNotNone(extract_deepseek_netlist(reply))

    def test_does_not_treat_explanatory_i_as_current_source(self):
        reply = f"I made the requested mutation.\n{self.NETLIST_BODY}\nDone."
        extracted = extract_deepseek_netlist(reply)
        self.assertNotIn("I made the requested mutation", extracted)

    def test_rejects_content_without_elements(self):
        self.assertIsNone(extract_deepseek_netlist(".op\n.end"))


if __name__ == "__main__":
    unittest.main()
