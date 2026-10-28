import argparse
import hashlib
import json
import random
import shutil
import time
from datetime import datetime
from pathlib import Path

import numpy as np
import torch

from scripts.dc_evaluation import (
    DcStage,
    write_evaluation_manifest,
)
from scripts.deepseek_client import (
    DEFAULT_DEEPSEEK_BASE_URL,
    DEFAULT_DEEPSEEK_MODEL,
    DeepSeekAPIError,
    DeepSeekClient,
    deepseek_settings_from_environment,
)
from scripts.deepseek_netlist import (
    build_deepseek_format_repair_prompt,
    extract_deepseek_netlist_with_diagnostic,
)
from scripts.mutations import initialize_pta_forms, initialize_pta_mutations
from scripts.netlist_features import (
    FEATURE_NAMES,
    SOLVER_FEATURE_NAMES,
    extract_solver_features,
    circuit_novelty,
    build_a2c_state,
    extract_netlist_features,
)
from scripts.pta_prompts import (
    PTA_MUTATOR_SYSTEM_PROMPT,
    build_pta_error_feedback,
    build_pta_mutation_prompt,
)
from scripts.pta_reward import calculate_pta_reward
from scripts.pta_pipeline import evaluate_candidate, iter_outcomes
from scripts.pta_testing import initial_hint_signature
from scripts.reinforcement_learning import Net, v_wrap
from scripts.training_diagnostics import append_jsonl, exploration_progress, structural_group, sha256_file


def write_clean_variant(path, content):
    content = content.encode("gbk", errors="ignore").decode(
        "gbk", errors="ignore"
    ).replace("碌", "u")
    new_lines = []
    for raw_line in content.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("*"):
            continue
        comment_index = line.find(";")
        if comment_index != -1:
            line = line[:comment_index].strip()
        if line:
            new_lines.append(line)
    if not new_lines:
        return False
    title_tokens = ("spice", "plaintext", "netlist")
    if not any(token in new_lines[0].lower() for token in title_tokens):
        new_lines.insert(0, "spice")
    path.write_text("\n".join(new_lines) + "\n")
    return True


def resolve_executable(value, label):
    candidate = Path(value)
    if candidate.is_file():
        return candidate.resolve()
    located = shutil.which(str(value))
    if located:
        return Path(located).resolve()
    raise FileNotFoundError(f"{label} simulator not found: {value}")


def calculate_novelty(workdir, seed_names, variant_path, sample_size=128):
    names = list(seed_names)
    if len(names) > sample_size:
        names = random.sample(names, sample_size)
    return circuit_novelty(variant_path, [workdir / name for name in names])


def read_outcome_error(outcome):
    message = ""
    for key in ("error", "log", "report"):
        artifact_path = outcome.artifacts.get(key)
        if artifact_path and Path(artifact_path).is_file():
            message = Path(artifact_path).read_text(errors="replace").strip()
            if message:
                break
    if not message:
        message = str(outcome.artifacts.get("stderr", "")).strip()
    return message[-2000:]


def archive_evaluation(evaluation, result_directory, seed_path, variant_path, reward):
    result_directory.mkdir(parents=True, exist_ok=True)
    simulations = result_directory / "simulations"
    for profile, outcome in iter_outcomes(evaluation):
        if "directory" not in outcome.artifacts:
            continue
        source = Path(outcome.artifacts["directory"])
        if source.is_dir():
            destination = simulations / profile
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(source), str(destination))
            for key, value in list(outcome.artifacts.items()):
                if isinstance(value, Path):
                    try:
                        outcome.artifacts[key] = destination / value.relative_to(source)
                    except ValueError:
                        pass
    shutil.copy2(seed_path, result_directory / f"seed_{seed_path.name}")
    shutil.copy2(variant_path, result_directory / variant_path.name)
    evaluated = evaluation.mutation.get("evaluation_input")
    if evaluated and Path(evaluated).is_file():
        archived_input = result_directory / ("evaluated_" + variant_path.name)
        shutil.copy2(evaluated, archived_input)
        evaluation.mutation["evaluation_input"] = str(archived_input)
    write_evaluation_manifest(
        evaluation,
        result_directory / "evaluation.json",
        reward.as_dict(),
    )


def archive_rejection(result_directory, seed_path, variant_path, reason, stage, reward):
    result_directory.mkdir(parents=True, exist_ok=True)
    shutil.copy2(seed_path, result_directory / f"seed_{seed_path.name}")
    shutil.copy2(variant_path, result_directory / variant_path.name)
    (result_directory / "evaluation.json").write_text(
        json.dumps({
            "stage": stage.value,
            "reason": reason,
            "reward": {"total": reward},
        }, indent=2, sort_keys=True),
        encoding="utf-8",
    )


def archive_deepseek_format_failure(
    result_root,
    seed_name,
    mutation,
    attempts,
    diagnostic,
):
    timestamp = datetime.now().strftime("%Y%m%d%H%M%S%f")
    directory = result_root / "deepseek_format_rejections" / (
        f"{timestamp}_{Path(seed_name).stem}"
    )
    directory.mkdir(parents=True, exist_ok=True)
    for index, reply in enumerate(attempts, start=1):
        (directory / f"attempt_{index}.txt").write_text(
            reply,
            encoding="utf-8",
            errors="replace",
        )
    (directory / "diagnostic.json").write_text(
        json.dumps({
            "seed": seed_name,
            "mutation": mutation,
            "diagnostic": diagnostic,
            "attempt_count": len(attempts),
        }, indent=2, ensure_ascii=False),
        encoding="utf-8",
    )
    return directory


def update_a2c(
    net,
    optimizer,
    states,
    actions,
    rewards,
    next_state,
    discount,
    entropy_beta,
    terminal=False,
    behavior=None,
    auxiliary_targets=None,
    metrics_out=None,
):
    if not states:
        return None
    with torch.no_grad():
        next_value = 0.0 if terminal or discount == 0 else float(
            net(v_wrap(next_state[None, :]))[-1].squeeze().item()
        )
    returns = []
    running_return = next_value
    for reward in reversed(rewards):
        running_return = float(reward) + discount * running_return
        returns.append(running_return)
    returns.reverse()
    optimizer.zero_grad()
    loss = net.loss_func(
        v_wrap(np.vstack(states)),
        torch.as_tensor(actions, dtype=torch.long),
        v_wrap(np.asarray(returns, dtype=np.float32)[:, None]),
        entropy_beta=entropy_beta,
        behavior=behavior,
        auxiliary_targets=auxiliary_targets,
        return_metrics=metrics_out is not None,
    )
    if metrics_out is not None:
        loss, metrics = loss
        metrics_out.update(metrics)
        metrics_out["batch_size"] = len(states)
    if not torch.isfinite(loss):
        raise FloatingPointError("non-finite training loss; experiment stopped")
    loss.backward()
    norm = torch.nn.utils.clip_grad_norm_(net.parameters(), 5.0)
    if not torch.isfinite(norm):
        raise FloatingPointError("non-finite training gradient; experiment stopped")
    if metrics_out is not None:
        metrics_out["gradient_norm"] = float(norm)
    optimizer.step()
    states.clear()
    actions.clear()
    rewards.clear()
    if behavior is not None:
        behavior.clear()
    if auxiliary_targets is not None:
        auxiliary_targets.clear()
    return float(loss.detach().item())


def auxiliary_labels(evaluation, stage):
    if evaluation is None:
        return {"valid": 0.0, "pta": -1.0, "target": -1.0, "stage": -1}
    ng = evaluation.ngspice
    stages = {DcStage.NR: 0, DcStage.GMIN: 1, DcStage.SOURCE_STEPPING: 2, DcStage.OPTRAN: 3}
    return {"valid": float(stage != DcStage.INVALID), "pta": float(evaluation.pta_succeeded),
            "target": float(evaluation.authoritative_target),
            "stage": stages.get(ng.stage, -1) if ng and ng.succeeded else -1}


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Run convergence-directed SpiceFuzz with PTA ablations"
    )
    parser.add_argument(
        "--seed_dir",
        type=Path,
        default=Path("./data/seed_netlist/pta"),
        help="Seed netlist directory",
    )
    parser.add_argument("--max_time", type=int, default=86400)
    parser.add_argument("--max_candidates", type=int, help="Optional evaluated-candidate budget")
    parser.add_argument("--max_attempts", type=int, help="Generation-attempt limit, including duplicate/format failures; default 4x candidate budget")
    parser.add_argument("--random_seed", type=int, default=0)
    parser.add_argument("--allow_initial_hint_mutations", action="store_true")
    parser.add_argument("--lr", type=float, default=0.001)
    parser.add_argument("--maxtime", type=int, default=60)
    parser.add_argument("--pta_bin", type=Path, required=True)
    parser.add_argument(
        "--objective",
        choices=("ngspice_differential", "pta_internal"),
        default="ngspice_differential",
        help=(
            "Optimization target. The default requires stock ngspice to fail "
            "by convergence while PTA succeeds; pta_internal is diagnostic only."
        ),
    )
    parser.add_argument(
        "--ngspice_bin",
        type=Path,
        help=(
            "Stock ngspice executable; required for the default "
            "ngspice_differential objective"
        ),
    )
    parser.add_argument(
        "--pta_output_root",
        type=Path,
        default=Path("./test/pta_ablation"),
    )
    parser.add_argument("--result_root", type=Path, default=Path("./result/pta"))
    parser.add_argument("--pta_config", type=Path)
    parser.add_argument(
        "--pta_option",
        action="append",
        default=[],
        help="PTA name=value override used by both PTA ablation profiles",
    )
    parser.add_argument("--workdir", type=Path, default=Path("./netlist_pta"))
    deepseek_defaults = deepseek_settings_from_environment()
    parser.add_argument(
        "--deepseek_model",
        default=deepseek_defaults["model"],
        help=f"DeepSeek model name (default: {DEFAULT_DEEPSEEK_MODEL})",
    )
    parser.add_argument(
        "--deepseek_base_url",
        default=deepseek_defaults["base_url"],
        help=f"DeepSeek API base URL (default: {DEFAULT_DEEPSEEK_BASE_URL})",
    )
    parser.add_argument(
        "--deepseek_api_key",
        default=deepseek_defaults["api_key"],
        help="DeepSeek API key; prefer the DEEPSEEK_API_KEY environment variable",
    )
    parser.add_argument("--temperature", type=float, default=0.8)
    parser.add_argument("--max_tokens", type=int, default=2500)
    parser.add_argument(
        "--deepseek_format_retries",
        type=int,
        default=2,
        help="Retries used only when DeepSeek returns an unparseable format",
    )
    parser.add_argument(
        "--discount",
        type=float,
        default=0.0,
        help="Must be zero: candidates are independent contextual-bandit decisions",
    )
    parser.add_argument("--rollout_size", type=int, default=16)
    parser.add_argument("--entropy_beta", type=float, default=0.01)
    parser.add_argument("--policy_temperature", type=float, default=1.15)
    parser.add_argument("--exploration_start", type=float, default=0.25)
    parser.add_argument("--exploration_end", type=float, default=0.05)
    parser.add_argument("--count_exploration", type=float, default=0.35)
    parser.add_argument("--lineage_decay", type=float, default=0.5)
    parser.add_argument("--lineage_negative_decay", type=float, default=0.25, help="Deprecated compatibility option; descendant rewards no longer train ancestor actions")
    parser.add_argument("--reward_ema_alpha", type=float, default=0.1)
    parser.add_argument("--lineage_depth", type=int, default=4)
    parser.add_argument(
        "--frontier_seed_probability",
        type=float,
        default=0.25,
        help=(
            "Chance of retaining a non-target candidate that stock ngspice "
            "needed gmin/source stepping to solve"
        ),
    )
    parser.add_argument("--target_seed_weight", type=float, default=4.0)
    parser.add_argument("--frontier_seed_weight", type=float, default=1.5)
    return parser.parse_args()


def main():
    args = parse_arguments()
    random.seed(args.random_seed)
    np.random.seed(args.random_seed)
    torch.manual_seed(args.random_seed)
    pta_bin = resolve_executable(args.pta_bin, "PTA")
    ngspice_bin = (
        resolve_executable(args.ngspice_bin, "ngspice")
        if args.ngspice_bin else None
    )
    if args.objective == "ngspice_differential" and ngspice_bin is None:
        raise ValueError(
            "--ngspice_bin is required for --objective ngspice_differential; "
            "use --objective pta_internal only for local diagnostic runs"
        )
    if args.pta_config and not args.pta_config.resolve().is_file():
        raise FileNotFoundError(f"PTA config not found: {args.pta_config}")
    if not args.seed_dir.is_dir():
        raise FileNotFoundError(f"Seed directory not found: {args.seed_dir}")
    if not 0.0 <= args.temperature <= 2.0:
        raise ValueError("temperature must be between 0 and 2")
    if args.max_tokens <= 0 or args.rollout_size <= 0:
        raise ValueError("max_tokens and rollout_size must be positive")
    if args.deepseek_format_retries < 0:
        raise ValueError("deepseek_format_retries must be non-negative")
    if args.discount != 0.0:
        raise ValueError("random-seed contextual-bandit training requires --discount 0")
    if args.max_time <= 0 or args.maxtime <= 0:
        raise ValueError("time budgets must be positive")
    if args.max_candidates is not None and args.max_candidates <= 0:
        raise ValueError("max_candidates must be positive")
    if args.max_attempts is None and args.max_candidates is not None:
        args.max_attempts = 4 * args.max_candidates
    if args.max_attempts is not None and args.max_attempts <= 0:
        raise ValueError("max_attempts must be positive")
    if (args.result_root / "run.json").exists():
        raise FileExistsError("result_root already contains a run; choose a fresh result_root")
    if not 0.0 <= args.lineage_decay <= 1.0 or args.lineage_depth <= 0:
        raise ValueError("lineage_decay must be in [0, 1] and lineage_depth positive")
    if not 0.0 <= args.lineage_negative_decay <= 1.0:
        raise ValueError("lineage_negative_decay must be in [0, 1]")
    if not 0.0 < args.reward_ema_alpha <= 1.0:
        raise ValueError("reward_ema_alpha must be in (0, 1]")
    if not 0.0 <= args.frontier_seed_probability <= 1.0:
        raise ValueError("frontier_seed_probability must be in [0, 1]")
    if args.target_seed_weight <= 0 or args.frontier_seed_weight <= 0:
        raise ValueError("seed weights must be positive")
    if args.policy_temperature <= 0:
        raise ValueError("policy_temperature must be positive")
    if not 0.0 <= args.exploration_end <= args.exploration_start <= 1.0:
        raise ValueError("require 0 <= exploration_end <= exploration_start <= 1")

    workdir = args.workdir
    workdir.mkdir(parents=True, exist_ok=True)
    seed_names = []
    seen_inputs, structure_counts, seed_hashes = {}, {}, {}
    for seed_path in sorted(args.seed_dir.iterdir()):
        if seed_path.is_file() and seed_path.suffix.lower() in {".cir", ".net", ".sp"}:
            digest = sha256_file(seed_path)
            seed_hashes[seed_path.name] = digest
            if digest in seen_inputs:
                continue
            seen_inputs[digest] = str(seed_path.resolve())
            seed_names.append(seed_path.name)
            if seed_path.resolve() != (workdir / seed_path.name).resolve():
                shutil.copy2(seed_path, workdir / seed_path.name)
    if not seed_names:
        raise RuntimeError(f"No SPICE seed netlists found in {args.seed_dir}")
    seed_weights = {seed_name: 1.0 for seed_name in seed_names}

    mutations = initialize_pta_mutations()
    forms = initialize_pta_forms()
    if not args.allow_initial_hint_mutations:
        pairs = [(mutation, form) for mutation, form in zip(mutations, forms)
                 if not mutation.startswith("perturbing DC initial guesses")]
        mutations, forms = map(list, zip(*pairs))
    action_count = len(mutations)
    state_dimension = 2 * action_count + len(FEATURE_NAMES) + len(SOLVER_FEATURE_NAMES)
    net = Net(state_dimension, action_count)
    optimizer = torch.optim.Adam(net.parameters(), lr=args.lr)
    selection_counts = np.zeros(action_count, dtype=np.float32)
    reward_ema = np.zeros(action_count, dtype=np.float32)
    buffer_s, buffer_a, buffer_r = [], [], []
    buffer_behavior, buffer_auxiliary = [], []
    parent_evaluations = {}
    seed_parents = {}
    args.result_root.mkdir(parents=True, exist_ok=True)
    metadata = {"schema_version": 3, "random_seed": args.random_seed, "objective": args.objective,
                "seed_sha256": seed_hashes, "exploration_schedule": "candidate_budget" if args.max_candidates else "wall_time",
                "candidate_budget_semantics": "unique_assessed_inputs_including_hint_rejections",
                "optran_validation": "duration_10x_100x_stability_not_a_DC_residual_proof",
                "mutations": mutations, "features": list(FEATURE_NAMES), "solver_features": list(SOLVER_FEATURE_NAMES),
                "arguments": {key: str(value) if isinstance(value, Path) else value
                              for key, value in vars(args).items() if key != "deepseek_api_key"},
                "executables": {"pta_sha256": hashlib.sha256(pta_bin.read_bytes()).hexdigest(),
                                "ngspice_sha256": hashlib.sha256(ngspice_bin.read_bytes()).hexdigest() if ngspice_bin else None}}
    (args.result_root / "run.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")

    deepseek_client = DeepSeekClient(
        api_key=args.deepseek_api_key,
        base_url=args.deepseek_base_url,
    )
    error_feedback = {}
    stage_counts = {stage.value: 0 for stage in DcStage}
    total_num = target_num = 0
    attempts = duplicate_num = format_failures = training_updates = 0
    api_requests = api_failures = consecutive_api_failures = 0
    assessed_hashes, nr_target_hashes = set(), set()
    deepseek_time = simulation_time = parent_simulation_time = 0.0
    unique_targets = set()
    start_time = time.time()

    while (time.time() - start_time < args.max_time
           and (args.max_candidates is None or total_num < args.max_candidates)
           and (args.max_attempts is None or attempts < args.max_attempts)):
        print("---------------------")
        seed_name = random.choices(
            seed_names,
            weights=[seed_weights.get(name, 1.0) for name in seed_names],
            k=1,
        )[0]
        seed_path = workdir / seed_name
        seed_features = extract_netlist_features(seed_path)
        if seed_name not in parent_evaluations:
            parent_started = time.time()
            parent = evaluate_candidate(seed_path, args.maxtime, pta_bin, args.pta_output_root / "parents",
                                        ngspice_bin, args.pta_config, args.pta_option, confirm=False)
            parent_simulation_time += time.time() - parent_started
            parent_evaluations[seed_name] = parent
            write_evaluation_manifest(parent, args.result_root / ("parent_" + seed_path.stem + ".json"))
            if not parent.trainable:
                raise RuntimeError("parent evaluation failed due to simulator environment; see parent manifest")
            if parent.stage == DcStage.INVALID:
                seed_names.remove(seed_name)
                if not seed_names:
                    raise RuntimeError("all seeds violate the common-netlist contract")
                continue
        parent = parent_evaluations[seed_name]
        if time.time() - start_time >= args.max_time:
            break
        state = build_a2c_state(selection_counts, reward_ema, seed_features, extract_solver_features(parent))
        progress = exploration_progress(total_num, args.max_candidates, time.time() - start_time, args.max_time)
        epsilon = args.exploration_start + progress * (
            args.exploration_end - args.exploration_start
        )
        count_bonus = args.count_exploration / np.sqrt(selection_counts + 1.0)
        sampled, behavior = net.choose_action(
            v_wrap(state[None, :]), temperature=args.policy_temperature,
            epsilon=epsilon, exploration_bonus=count_bonus, return_info=True)
        mutation_no = int(sampled.item())
        mutation = mutations[mutation_no]
        attempts += 1
        selection_counts[mutation_no] += 1.0
        decision = {"attempt": attempts, "evaluations_before": total_num, "parent": seed_name,
                    "action": mutation_no, "strategy": mutation, "behavior": behavior,
                    "state": state.tolist(), "exploration_progress": progress}
        feedback_key = (seed_name, mutation_no)
        print(f"Selected seed: {seed_name}")
        print(f"Seed weight: {seed_weights.get(seed_name, 1.0):.2f}")
        print(f"Selected mutation: {mutation}")
        print(f"Exploration epsilon: {epsilon:.4f}")

        seed_lines = seed_path.read_text(errors="replace").splitlines()
        seed = "\n".join(line.strip() for line in seed_lines[1:]) + "\n"
        messages = [
            {"role": "system", "content": PTA_MUTATOR_SYSTEM_PROMPT},
            {"role": "user", "content": build_pta_mutation_prompt(
                seed, mutation, forms[mutation_no], args.allow_initial_hint_mutations,
                solver_feedback=json.dumps({"ngspice_stage": parent.ngspice.stage.value if parent.ngspice else None,
                                            "ngspice_failure": parent.ngspice.artifacts.get("failure_kind") if parent.ngspice else None,
                                            "ngspice_iterations": parent.ngspice.artifacts.get("iterations") if parent.ngspice else None,
                                            "pta_verified": parent.pta_succeeded})
            )},
        ]
        if feedback_key in error_feedback:
            messages.append({
                "role": "user",
                "content": build_pta_error_feedback(error_feedback[feedback_key]),
            })

        deepseek_start = time.time()
        variant_content = None
        format_diagnostic = "no response"
        format_replies = []
        request_messages = messages
        for format_attempt in range(args.deepseek_format_retries + 1):
            try:
                api_requests += 1
                reply = deepseek_client.create_chat_completion(
                    messages=request_messages,
                    model=args.deepseek_model,
                    temperature=(args.temperature if format_attempt == 0 else 0.0),
                    max_tokens=min(
                        384000,
                        args.max_tokens * (format_attempt + 1),
                    ),
                    thinking=False,
                )
                consecutive_api_failures = 0
            except DeepSeekAPIError as error:
                api_failures += 1
                consecutive_api_failures += 1
                if consecutive_api_failures >= 3:
                    raise RuntimeError("three consecutive DeepSeek request failures; stopping instead of spending the remaining budget") from error
                print(f"DeepSeek request failed: {error}")
                format_diagnostic = str(error)
                break
            format_replies.append(reply)
            print(f"DeepSeek reply (format attempt {format_attempt + 1}):\n{reply}")
            if deepseek_client.last_finish_reason == "length":
                format_diagnostic = (
                    "DeepSeek stopped at max_tokens before completing the netlist"
                )
                variant_content = None
            else:
                variant_content, format_diagnostic = (
                    extract_deepseek_netlist_with_diagnostic(reply)
                )
            if variant_content is not None:
                break
            if format_attempt < args.deepseek_format_retries:
                request_messages = messages + [
                    {"role": "assistant", "content": reply},
                    {
                        "role": "user",
                        "content": build_deepseek_format_repair_prompt(),
                    },
                ]
        deepseek_time += time.time() - deepseek_start
        if variant_content is None:
            format_failures += 1
            failure_directory = archive_deepseek_format_failure(
                args.result_root,
                seed_name,
                mutation,
                format_replies,
                format_diagnostic,
            )
            print(
                "DeepSeek reply did not contain a complete parseable .op netlist "
                f"after format repair: {format_diagnostic}. Raw replies saved to "
                f"{failure_directory}"
            )
            append_jsonl(args.result_root / "decisions.jsonl", {**decision, "status": "generation_failed", "diagnostic": format_diagnostic})
            continue

        variant_name = datetime.now().strftime("%Y%m%d%H%M%S%f") + ".cir"
        variant_path = workdir / variant_name
        if not write_clean_variant(variant_path, variant_content):
            format_failures += 1
            append_jsonl(args.result_root / "decisions.jsonl", {**decision, "status": "empty_variant"})
            continue
        digest = sha256_file(variant_path)
        if digest in seen_inputs:
            duplicate_num += 1
            append_jsonl(args.result_root / "decisions.jsonl", {**decision, "status": "duplicate",
                         "source_sha256": digest, "first_seen": seen_inputs[digest], "trainable": False})
            error_feedback[feedback_key] = "The generated netlist duplicates an already seen input. Make a substantive circuit change."
            variant_path.unlink()
            continue
        total_num += 1
        assessed_hashes.add(digest)
        group = structural_group(variant_path)
        structure_counts[group] = structure_counts.get(group, 0) + 1

        evaluation = reward_breakdown = None
        target_hit = False
        simulation_start = time.time()
        if not args.allow_initial_hint_mutations and initial_hint_signature(variant_path) != initial_hint_signature(seed_path):
            stage, reward_value, diagnostic = DcStage.INVALID, -6.0, "mutation changed frozen initial hints"
        else:
            evaluation = evaluate_candidate(variant_path, args.maxtime, pta_bin, args.pta_output_root,
                                            ngspice_bin, args.pta_config, args.pta_option)
            evaluation.mutation.update({"parent": seed_name, "action": mutation_no, "strategy": mutation,
                                        "behavior": behavior, "structure_group": group})
            novelty = calculate_novelty(workdir, seed_names, variant_path)
            reward_breakdown = calculate_pta_reward(evaluation, novelty, args.objective, parent,
                                                   cost_budget=args.maxtime * 8)
            target_hit = reward_breakdown.target_hit
            stage, reward_value = evaluation.stage, reward_breakdown.total
            if evaluation.ngspice and stage in {DcStage.INVALID, DcStage.FAILED, DcStage.TIMEOUT}:
                diagnostic = read_outcome_error(evaluation.ngspice)
            else:
                diagnostic = "" if evaluation.pta_succeeded else read_outcome_error(evaluation.full_fallback)
        simulation_time += time.time() - simulation_start

        stage_counts[stage.value] += 1
        trainable = evaluation is None or evaluation.trainable
        if trainable:
            reward_ema[mutation_no] = ((1.0 - args.reward_ema_alpha) * reward_ema[mutation_no]
                                      + args.reward_ema_alpha * reward_value)
            buffer_s.append(state.copy())
            buffer_a.append(mutation_no)
            buffer_r.append(reward_value)
            buffer_behavior.append(behavior)
            buffer_auxiliary.append(auxiliary_labels(evaluation, stage))
        print(
            f"DC stage: {stage.value}; reward: {reward_value:.4f}; "
            f"target: {target_hit}"
        )

        result_directory = args.result_root / stage.value / variant_path.stem
        seen_inputs[digest] = str((result_directory / variant_path.name).resolve())
        append_jsonl(args.result_root / "decisions.jsonl", {**decision, "status": "assessed",
                     "source_sha256": digest, "structure_group": group, "stage": stage.value,
                     "reward": reward_value, "trainable": trainable,
                     "nr_differential": bool(evaluation and evaluation.nr_differential_candidate),
                     "confirmed_target": target_hit})
        if evaluation is not None:
            archive_evaluation(
                evaluation,
                result_directory,
                seed_path,
                variant_path,
                reward_breakdown,
            )
            if evaluation.nr_differential_candidate:
                nr_target_hashes.add(digest)
            if not evaluation.trainable:
                raise RuntimeError("non-trainable simulator evidence; stopped after archiving " + str(result_directory))
            if target_hit:
                target_num += 1
                unique_targets.add(evaluation.mutation["source_sha256"])
            if args.objective == "ngspice_differential":
                frontier = (
                    evaluation.ngspice is not None
                    and evaluation.ngspice.succeeded
                    and evaluation.ngspice.stage in {
                        DcStage.GMIN,
                        DcStage.SOURCE_STEPPING,
                        DcStage.OPTRAN,
                    }
                    and evaluation.pta_succeeded
                )
                retain_candidate = target_hit or (
                    frontier
                    and random.random() < args.frontier_seed_probability
                )
            else:
                retain_candidate = evaluation.pta_succeeded and stage in {
                    DcStage.GMIN,
                    DcStage.SOURCE_STEPPING,
                    DcStage.PTA,
                }
            if retain_candidate:
                parent_evaluations[variant_name] = evaluation
                seed_parents[variant_name] = seed_name
                # Lineage influences seed scheduling only, never actor labels.
                if target_hit:
                    ancestor = seed_name
                    for depth in range(1, args.lineage_depth + 1):
                        seed_weights[ancestor] = max(seed_weights.get(ancestor, 1.0),
                                                     args.target_seed_weight * args.lineage_decay ** depth)
                        ancestor = seed_parents.get(ancestor)
                        if ancestor is None:
                            break
                seed_weights[variant_name] = (
                    args.target_seed_weight
                    if target_hit
                    else args.frontier_seed_weight
                )
                seed_names.append(variant_name)
                error_feedback.pop(feedback_key, None)
            else:
                variant_path.unlink(missing_ok=True)
                if diagnostic:
                    error_feedback[feedback_key] = diagnostic
        else:
            archive_rejection(
                result_directory,
                seed_path,
                variant_path,
                diagnostic,
                stage,
                reward_value,
            )
            variant_path.unlink(missing_ok=True)
            error_feedback[feedback_key] = diagnostic

        next_path = variant_path if variant_path.is_file() else seed_path
        next_state = build_a2c_state(
            selection_counts,
            reward_ema,
            extract_netlist_features(next_path),
            extract_solver_features(evaluation if next_path == variant_path else parent),
        )
        elapsed = time.time() - start_time
        if buffer_s and (len(buffer_s) >= args.rollout_size or elapsed >= args.max_time):
            metrics = {}
            loss = update_a2c(
                net,
                optimizer,
                buffer_s,
                buffer_a,
                buffer_r,
                next_state,
                args.discount,
                args.entropy_beta,
                terminal=elapsed >= args.max_time,
                behavior=buffer_behavior, auxiliary_targets=buffer_auxiliary,
                metrics_out=metrics,
            )
            training_updates += 1
            print(f"Policy/critic loss: {loss:.6f}")
            append_jsonl(args.result_root / "training.jsonl", {"evaluations": total_num,
                         "attempts": attempts, "loss": loss, "targets": target_num, **metrics})

    if buffer_s:
        metrics = {}
        fallback_state = build_a2c_state(
            selection_counts,
            reward_ema,
            extract_netlist_features(workdir / random.choice(seed_names)),
        )
        loss = update_a2c(
            net,
            optimizer,
            buffer_s,
            buffer_a,
            buffer_r,
            fallback_state,
            args.discount,
            args.entropy_beta,
            terminal=True,
            behavior=buffer_behavior, auxiliary_targets=buffer_auxiliary,
            metrics_out=metrics,
        )
        training_updates += 1
        append_jsonl(args.result_root / "training.jsonl", {"evaluations": total_num,
                     "attempts": attempts, "loss": loss, "targets": target_num, **metrics})

    torch.save({"model": net.state_dict(), "optimizer": optimizer.state_dict(),
                "metadata": metadata, "selection_counts": selection_counts, "reward_ema": reward_ema},
               args.result_root / "policy.pt")
    summary = {"evaluations": total_num, "confirmed_targets": target_num, "stage_counts": stage_counts,
               "attempts": attempts, "duplicates_skipped": duplicate_num, "format_failures": format_failures,
               "api_requests": api_requests, "api_failures": api_failures,
               "training_updates": training_updates, "unique_assessed_inputs": len(assessed_hashes),
               "unique_nr_differential_inputs": len(nr_target_hashes), "structure_group_counts": structure_counts,
               "budget_complete": args.max_candidates is not None and total_num >= args.max_candidates,
               "stop_reason": "candidate_budget" if args.max_candidates is not None and total_num >= args.max_candidates else
                              "attempt_budget" if args.max_attempts is not None and attempts >= args.max_attempts else "time_budget",
               "unique_target_inputs": len(unique_targets),
               "elapsed_seconds": time.time() - start_time, "deepseek_seconds": deepseek_time,
               "simulation_seconds": simulation_time + parent_simulation_time,
               "parent_simulation_seconds": parent_simulation_time}
    (args.result_root / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print("total_num", total_num)
    print("target_num", target_num)
    print("stage_counts", stage_counts)
    print("elapsed_time", time.time() - start_time)
    deepseek_client.close()
    print("deepseek_time", deepseek_time)
    print("simulation_time", simulation_time)


if __name__ == "__main__":
    main()
