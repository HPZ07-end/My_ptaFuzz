import json
import subprocess
import tempfile
import unittest
import contextlib
import io
import os
from pathlib import Path
from unittest.mock import patch

import numpy as np
import torch

from main_pta import archive_evaluation, update_a2c
import main_pta
from scripts.dc_evaluation import DcEvaluation, DcStage, SolverOutcome, run_ngspice_ablation
from scripts.netlist_features import (FEATURE_NAMES, SOLVER_FEATURE_NAMES, build_a2c_state,
                                     circuit_novelty, extract_netlist_features, extract_solver_features)
from scripts.ngspice_testing import PROFILES, failure_kind, run_profile, successful_stage
from scripts.pta_pipeline import evaluate_candidate
from scripts.pta_reward import calculate_pta_reward
from scripts.pta_testing import initial_hint_signature, validate_pta_dc_netlist
from scripts.solver_validation import read_operating_point, validate_pta_artifacts
from scripts.reinforcement_learning import Net


DECK = "Test circuit\nV1 a 0 DC 1\nR1 a 0 1k\n.op\n.end\n"
RAW = "Title: test\nPlotname: Operating Point\nFlags: real\nNo. Variables: 2\nNo. Points: 1\nVariables:\n0 v(a) voltage\n1 i(v1) current\nValues:\n0 1.0\n-0.001\n"


class WorkspaceTest(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[1] / "test"
        root.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="feedback-test-", dir=root)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.deck = self.root / "case.cir"
        self.deck.write_text(DECK)

    def evidence(self, residual=0.1):
        paths = {"directory": self.root}
        for key, suffix in (("raw", ".raw"), ("report", ".solve.txt"), ("trace", ".pta.jsonl")):
            paths[key] = self.root / ("case" + suffix)
        paths["raw"].write_text(RAW)
        paths["report"].write_text("Status: succeeded\n  Method path: direct Newton failed -> adaptive PTA succeeded\n"
                                  "  Total iterations: 20\n    Accepted pseudo-time steps: 3\n    Rejected pseudo-time steps: 1\n"
                                  "  pta.derivative_tolerance: 1\n  pta.dc_residual_tolerance: 1\n")
        records = [{"record_type": "attempt", "accepted": True, "reached_steady_state": True,
                    "newton": {"converged": True}, "source_scale": 1.0,
                    "normalized_derivative": 0.1, "normalized_dc_residual": residual},
                   {"record_type": "final", "status": "succeeded", "converged": True,
                    "solution": {"node_voltages": [{"name": "a", "value": 1.0}],
                                 "branch_currents": [{"name": "v1", "value": -0.001}]}}]
        paths["trace"].write_text("\n".join(json.dumps(r) for r in records) + "\n")
        return paths

    def evaluation(self, stage=DcStage.PTA, confirmed=True):
        evidence = {"verified_pta": True, "op_valid": True, "actual_stage": "pta", "solution": {"v(a)": 1.0},
                    "accepted_steps": 3, "rejected_steps": 1, "normalized_dc_residual": 0.1, "normalized_derivative": 0.1}
        outcomes = {"nr_only": SolverOutcome("nr_only", 1, DcStage.NR, {}),
                    "nr_source": SolverOutcome("nr_source", 1, DcStage.SOURCE_STEPPING, {}),
                    "nr_pta": SolverOutcome("nr_pta", 0, DcStage.PTA, {"validation": evidence}),
                    "full_fallback": SolverOutcome("full_fallback", 0, DcStage.PTA, {"validation": evidence})}
        ng = SolverOutcome("ng_full", 1 if stage == DcStage.PTA else 0,
                           DcStage.UNKNOWN if stage == DcStage.PTA else stage,
                           {"failure_kind": "convergence" if stage == DcStage.PTA else None, "iterations": 120})
        evaluation = DcEvaluation(outcomes, ng, {"ng_full": ng}, {"confirmed": confirmed})
        evaluation.mutation["device_count"] = 2
        return evaluation


class EvidenceTests(WorkspaceTest):
    def test_complete_ascii_op(self):
        path = self.root / "op.raw"
        path.write_text(RAW)
        self.assertTrue(read_operating_point(path)["valid"])

    def test_rejects_partial_nonfinite_nonop_and_duplicate_vectors(self):
        for content in (RAW.split("Values:")[0], RAW.replace("-0.001", "nan"),
                        RAW.replace("Operating Point", "Transient Analysis"),
                        RAW.replace("i(v1)", "v(a)"), RAW.replace("-0.001", "")):
            with self.subTest(content=content[-60:]):
                path = self.root / "op.raw"
                path.write_text(content)
                self.assertFalse(read_operating_point(path)["valid"])

    def test_pta_needs_both_residual_and_derivative(self):
        artifacts = self.evidence()
        self.assertTrue(validate_pta_artifacts(artifacts)[0])
        self.assertFalse(validate_pta_artifacts(self.evidence(residual=1.1))[0])
        artifacts = self.evidence()
        text = artifacts["trace"].read_text().replace('"normalized_derivative": 0.1', '"normalized_derivative": 1.1')
        artifacts["trace"].write_text(text)
        self.assertFalse(validate_pta_artifacts(artifacts)[0])

    def test_missing_final_trace_and_incomplete_source_ramp_are_rejected(self):
        artifacts = self.evidence()
        artifacts["trace"].write_text(artifacts["trace"].read_text().splitlines()[0] + "\n")
        self.assertFalse(validate_pta_artifacts(artifacts)[0])
        artifacts = self.evidence()
        artifacts["trace"].write_text(artifacts["trace"].read_text().replace('"source_scale": 1.0', '"source_scale": 0.5'))
        self.assertFalse(validate_pta_artifacts(artifacts)[0])

    def test_final_solution_must_match_raw(self):
        artifacts = self.evidence()
        artifacts["raw"].write_text(RAW.replace("0 1.0", "0 2.0"))
        self.assertFalse(validate_pta_artifacts(artifacts)[0])

    def test_solver_cards_and_missing_op_rejected(self):
        for card in (".options itl1=1", ".control", ".include other.cir", ".tran 1n 10n", ".print tran v(a)"):
            self.deck.write_text(DECK.replace(".op", card + "\n.op"))
            self.assertFalse(validate_pta_dc_netlist(self.deck)[0])
        self.deck.write_text(DECK.replace(".op\n", ""))
        self.assertFalse(validate_pta_dc_netlist(self.deck)[0])

    def test_initial_hints_are_separate_from_physical_mutations(self):
        self.deck.write_text(DECK.replace(".op", ".nodeset V(a)=0.2\n.op"))
        first = initial_hint_signature(self.deck)
        self.deck.write_text(self.deck.read_text().replace("1k", "2k"))
        self.assertEqual(first, initial_hint_signature(self.deck))
        self.deck.write_text(self.deck.read_text().replace("0.2", "0.3"))
        self.assertNotEqual(first, initial_hint_signature(self.deck))


class NgspiceTests(WorkspaceTest):
    def fake_success(self, command, **kwargs):
        directory = Path(kwargs["cwd"])
        (directory / "op.raw").write_text(RAW)
        Path(command[command.index("-o") + 1]).write_text("ngspice-47\nTotal iterations = 120\n")
        return subprocess.CompletedProcess(command, 0, "", "")

    def test_profiles_have_independent_outputs_and_explicit_controls(self):
        with patch("scripts.ngspice_testing.subprocess.run", side_effect=self.fake_success):
            profiles = run_ngspice_ablation(self.deck, self.deck.name, 1, self.root / "ngspice", self.root / "runs")
        self.assertEqual(set(profiles), set(PROFILES))
        for key, outcome in profiles.items():
            self.assertTrue(outcome.succeeded)
            self.assertEqual(outcome.artifacts["iterations"], 120)
            self.assertIn("-n", outcome.artifacts["command"])
            deck = Path(outcome.artifacts["command"][-1]).read_text()
            self.assertIn("optran " + PROFILES[key], deck)
            self.assertIn("set filetype=ascii", deck)
            self.assertEqual(outcome.artifacts["profile_settings"]["itl1"], 100)

    def test_old_raw_cannot_turn_a_failed_run_into_success(self):
        case = self.root / "runs" / "case"
        case.mkdir(parents=True)
        (case / "op.raw").write_text(RAW)
        with patch("scripts.ngspice_testing.subprocess.run", return_value=subprocess.CompletedProcess([], 0, "", "")):
            code, _, artifacts = run_profile(self.deck, self.deck.name, 1, "missing.exe", case.parent)
        self.assertNotEqual(code, 0)
        self.assertEqual(artifacts["failure_kind"], "invalid_output")

    def test_timeout_and_crash_do_not_become_convergence_failures(self):
        with patch("scripts.ngspice_testing.subprocess.run", side_effect=subprocess.TimeoutExpired("ng", 1, output=b"partial")):
            code, _, artifacts = run_profile(self.deck, self.deck.name, 1, "ng", self.root / "timeout")
        self.assertEqual(code, 2)
        json.dumps(artifacts, default=str)
        with patch("scripts.ngspice_testing.subprocess.run", return_value=subprocess.CompletedProcess([], -11, "no convergence", "")):
            code, _, artifacts = run_profile(self.deck, self.deck.name, 1, "ng", self.root / "crash")
        self.assertEqual(artifacts["failure_kind"], "crash")

    def test_nested_gmin_and_incomplete_stage_are_not_misreported(self):
        self.assertEqual(successful_stage("Starting source stepping\nStarting gmin stepping\ngmin stepping failed\nSource stepping completed", True), "source_stepping")
        self.assertEqual(successful_stage("Starting source stepping\nsource stepping failed", True), "unknown")
        self.assertEqual(successful_stage("Starting transient op\nTransient op finished successfully", True), "optran")
        self.assertEqual(failure_kind("singular matrix\nDC solution failed"), "convergence")
        self.assertEqual(failure_kind("optran: no such command\nDC solution failed"), "infrastructure")
        self.assertEqual(successful_stage("Starting gmin stepping\ngmin stepping completed\nStarting source stepping", True), "unknown")
        self.assertEqual(successful_stage("Starting source stepping\nOne successful source step", True), "unknown")

    def test_startup_singular_warning_does_not_override_recovered_op(self):
        def recovered(command, **kwargs):
            result = self.fake_success(command, **kwargs)
            Path(command[command.index("-o") + 1]).write_text(
                "Warning: singular matrix\nStarting gmin stepping\nDynamic gmin stepping completed\n")
            return result
        with patch("scripts.ngspice_testing.subprocess.run", side_effect=recovered):
            code, stage, _ = run_profile(self.deck, self.deck.name, 1, "ng", self.root / "recovered")
        self.assertEqual(code, 0)
        self.assertEqual(stage, "gmin")

    def test_declared_syntax_error_blocks_even_fresh_output(self):
        def invalid(command, **kwargs):
            result = self.fake_success(command, **kwargs)
            Path(command[command.index("-o") + 1]).write_text("Error on line 4: unknown parameter\n")
            return result
        with patch("scripts.ngspice_testing.subprocess.run", side_effect=invalid):
            code, _, evidence = run_profile(self.deck, self.deck.name, 1, "ng", self.root / "invalid")
        self.assertEqual(code, 1)
        self.assertEqual(evidence["failure_kind"], "invalid")


class RewardAndTrainingTests(WorkspaceTest):
    def test_target_needs_replay_and_validated_pta(self):
        evaluation = self.evaluation(confirmed=False)
        self.assertFalse(calculate_pta_reward(evaluation).target_hit)
        evaluation.confirmation["confirmed"] = True
        self.assertGreater(calculate_pta_reward(evaluation).total, 11)
        evaluation.nr_pta.artifacts["validation"]["verified_pta"] = False
        self.assertFalse(calculate_pta_reward(evaluation).target_hit)

    def test_frontier_shaping_stays_negative_and_improvement_is_bounded(self):
        parent = self.evaluation(DcStage.NR)
        frontier = self.evaluation(DcStage.SOURCE_STEPPING)
        reward = calculate_pta_reward(frontier, novelty=100, parent_evaluation=parent)
        self.assertGreater(reward.progress_bonus, 0)
        self.assertLess(reward.total, 0)
        self.assertEqual(reward.full_fallback_bonus, 0)

    def test_environment_failure_is_not_training_data(self):
        evaluation = self.evaluation()
        evaluation.ngspice.artifacts["failure_kind"] = "infrastructure"
        self.assertFalse(calculate_pta_reward(evaluation).trainable)

    def test_missing_telemetry_has_masks_and_state_shape_is_explicit(self):
        unknown = extract_solver_features(None)
        self.assertEqual(float(unknown.sum()), 0)
        evaluation = self.evaluation()
        features = extract_solver_features(evaluation)
        self.assertEqual(features[SOLVER_FEATURE_NAMES.index("ng_iterations_available")], 1)
        state = build_a2c_state(np.zeros(3), np.zeros(3), extract_netlist_features(DECK), features)
        self.assertEqual(len(state), 6 + len(FEATURE_NAMES) + len(SOLVER_FEATURE_NAMES))

    def test_novelty_ignores_names_and_detects_parameter_change(self):
        other = self.root / "other.cir"
        other.write_text(DECK.replace("a", "renamed").replace("R1", "Rother").replace("V1", "Vother"))
        self.assertAlmostEqual(circuit_novelty(self.deck, [other]), 0)
        other.write_text(DECK.replace("1k", "1g"))
        self.assertGreater(circuit_novelty(self.deck, [other]), 0)

    def test_actual_mixed_policy_probability_and_finite_update(self):
        torch.manual_seed(7)
        net = Net(4, 3, hidden_dim=8)
        state = torch.zeros((1, 4))
        action, info = net.choose_action(state, 1.7, 0.4, np.asarray([0.1, 0.5, -0.2]), return_info=True)
        logits, _ = net(state)
        expected = 0.6 * torch.softmax((logits + torch.tensor([[0.1, 0.5, -0.2]])) / 1.7, dim=1) + 0.4 / 3
        self.assertAlmostEqual(info["log_prob"], float(expected[0, action.item()].log()), places=6)
        before = net.pi2.bias.detach().clone()
        states, actions, rewards, behavior, labels = [np.zeros(4)], [action.item()], [12], [info], [{"valid": 1, "pta": 1, "target": 1, "stage": 2}]
        loss = update_a2c(net, torch.optim.Adam(net.parameters(), lr=0.01), states, actions, rewards,
                          np.zeros(4), 0, 0.01, terminal=True, behavior=behavior, auxiliary_targets=labels)
        self.assertTrue(np.isfinite(loss))
        self.assertFalse(torch.equal(before, net.pi2.bias))
        self.assertEqual(behavior, [])

    def test_all_random_exploration_does_not_train_actor_preferences(self):
        net = Net(4, 3, hidden_dim=8)
        state = torch.zeros((1, 4))
        action, info = net.choose_action(state, epsilon=1, return_info=True)
        loss = net.loss_func(state, action, torch.tensor([[12.0]]), behavior=[info])
        loss.backward()
        self.assertEqual(float(net.pi2.weight.grad.abs().sum()), 0)


class ConfirmationTests(WorkspaceTest):
    def test_independent_confirmation_and_nr_target_are_separate(self):
        original = self.evaluation(confirmed=False)
        original.ngspice_profiles["ng_nr"] = SolverOutcome("ng_nr", 1, DcStage.UNKNOWN, {"failure_kind": "convergence"})
        ng_profiles = original.ngspice_profiles.copy()
        repeat = SolverOutcome("ng_full", 1, DcStage.UNKNOWN, {"failure_kind": "convergence"})
        with patch("scripts.pta_pipeline.parse_with_pta", return_value=(0, {})), \
             patch("scripts.pta_pipeline.run_pta_ablation", return_value=original), \
             patch("scripts.pta_pipeline.run_ngspice_ablation", return_value=ng_profiles), \
             patch("scripts.pta_pipeline.run_ngspice_baseline", return_value=repeat), \
             patch("scripts.pta_pipeline.simulate_on_pta", return_value=(0, original.nr_pta.artifacts)):
            evaluation = evaluate_candidate(self.deck, 1, "pta", self.root / "output", "ng")
        self.assertTrue(evaluation.authoritative_target)
        self.assertTrue(evaluation.nr_differential_candidate)
        self.assertTrue(evaluation.baseline_failed)

    def test_unavailable_simulator_is_not_an_invalid_mutation(self):
        with patch("scripts.pta_pipeline.parse_with_pta", return_value=(1, {"returncode": None})):
            evaluation = evaluate_candidate(self.deck, 1, "pta", self.root / "output", "ng")
        self.assertFalse(evaluation.trainable)

    def test_successful_ngspice_replay_cancels_target_credit(self):
        original = self.evaluation(confirmed=False)
        repeated = SolverOutcome("ng_full", 0, DcStage.NR, {})
        with patch("scripts.pta_pipeline.parse_with_pta", return_value=(0, {})), \
             patch("scripts.pta_pipeline.run_pta_ablation", return_value=original), \
             patch("scripts.pta_pipeline.run_ngspice_ablation", return_value=original.ngspice_profiles.copy()), \
             patch("scripts.pta_pipeline.run_ngspice_baseline", return_value=repeated), \
             patch("scripts.pta_pipeline.simulate_on_pta", return_value=(0, original.nr_pta.artifacts)):
            evaluation = evaluate_candidate(self.deck, 1, "pta", self.root / "output", "ng")
        self.assertFalse(evaluation.authoritative_target)
        self.assertLess(calculate_pta_reward(evaluation).total, 0)


class MainLoopTests(WorkspaceTest):
    def test_one_candidate_trains_and_archives_without_network(self):
        seeds = self.root / "seeds"
        seeds.mkdir()
        (seeds / "seed.cir").write_text(DECK)
        binary = self.root / "simulator.exe"
        binary.write_bytes(b"test executable identity")
        result, output, work = self.root / "results", self.root / "output", self.root / "work"
        calls = []

        def evaluate(path, timeout, pta_bin, output_root, *args, **kwargs):
            calls.append(Path(path).name)
            evaluation = self.evaluation(DcStage.SOURCE_STEPPING)
            for key, outcome in list(evaluation.outcomes.items()) + list(evaluation.ngspice_profiles.items()):
                directory = Path(output_root) / key / Path(path).stem
                directory.mkdir(parents=True, exist_ok=True)
                artifact = directory / "evidence.txt"
                artifact.write_text("isolated test evidence")
                outcome.artifacts.update({"directory": directory, "report": artifact})
            return evaluation

        class Client:
            last_finish_reason = "stop"
            def __init__(self, **kwargs):
                pass
            def create_chat_completion(self, **kwargs):
                return "BEGIN_NETLIST\n" + DECK.replace("1k", "2k") + "END_NETLIST"
            def close(self):
                pass

        argv = ["main_pta.py", "--seed_dir", str(seeds), "--pta_bin", str(binary),
                "--ngspice_bin", str(binary), "--result_root", str(result),
                "--pta_output_root", str(output), "--workdir", str(work),
                "--max_candidates", "1", "--max_time", "30", "--rollout_size", "1",
                "--frontier_seed_probability", "1", "--deepseek_api_key", "sk-private-test"]
        with patch("sys.argv", argv), patch("main_pta.DeepSeekClient", Client), \
             patch("main_pta.evaluate_candidate", side_effect=evaluate), contextlib.redirect_stdout(io.StringIO()):
            main_pta.main()
        self.assertEqual(len(calls), 2)  # parent first, then one child
        self.assertTrue((result / "policy.pt").is_file())
        summary = json.loads((result / "summary.json").read_text())
        self.assertEqual(summary["evaluations"], 1)
        self.assertEqual(summary["confirmed_targets"], 0)
        self.assertNotIn("sk-private-test", (result / "run.json").read_text())
        manifest = next((result / "source_stepping").rglob("evaluation.json"))
        data = json.loads(manifest.read_text())
        self.assertIn("behavior", data["mutation"])
        for profile in data["profiles"].values():
            self.assertTrue(Path(profile["artifacts"]["report"]).is_file())


class NativePtaTests(WorkspaceTest):
    def binary(self):
        name = "spice.exe" if os.name == "nt" else "spice"
        path = Path(__file__).resolve().parents[2] / "PTA" / "Simulator" / name
        if not path.is_file():
            self.skipTest("local Windows PTA executable unavailable")
        return path

    def test_real_ordinary_solution_is_not_misreported_as_pta(self):
        evaluation = evaluate_candidate(self.deck, 10, self.binary(), self.root / "native")
        self.assertEqual(evaluation.stage, DcStage.NR)
        self.assertFalse(evaluation.pta_succeeded)
        self.assertTrue(all(outcome.succeeded for outcome in evaluation.outcomes.values()))

    def test_real_pta_recovery_with_partial_print_selection(self):
        self.deck.write_text("""SpiceFuzz generated DC operating-point netlist
.model NMOD NMOS LEVEL=1 VTO=0.7 KP=200u LAMBDA=0.02
.model PMOD PMOS LEVEL=1 VTO=-0.7 KP=100u LAMBDA=0.02
.model QMOD NPN IS=1e-16 BF=200 BR=2 NF=1 NR=1 VA=80 IKF=1e-3
VDD vdd 0 DC 5
RBIAS vdd q 500k
MNQ q qb 0 0 NMOD W=10u L=1u
MPQ q qb vdd vdd PMOD W=20u L=1u
MNQB qb q 0 0 NMOD W=10u L=1u
MPQB qb q vdd vdd PMOD W=20u L=1u
MNLOAD qb vdd 0 0 NMOD W=10u L=1u
QBOOST qb q 0 QMOD AREA=1
RQ q 0 1g
RQB qb 0 1g
.nodeset V(q)=0.68 V(qb)=0.68
.op
.print op v(q)
* output-only continuation
+ v(qb)
.end
""")
        evaluation = evaluate_candidate(self.deck, 10, self.binary(), self.root / "native")
        self.assertEqual(evaluation.stage, DcStage.PTA)
        self.assertTrue(evaluation.pta_succeeded)
        self.assertFalse(evaluation.nr_only.succeeded)
        self.assertFalse(evaluation.nr_source.succeeded)
        self.assertTrue(evaluation.nr_pta.verified_pta)
        self.assertTrue(evaluation.full_fallback.verified_pta)
        self.assertFalse(evaluation.authoritative_target)


if __name__ == "__main__":
    unittest.main()
