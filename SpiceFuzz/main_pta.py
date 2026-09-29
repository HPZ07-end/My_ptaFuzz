import argparse
import json
import random
import shutil
import time
from datetime import datetime
from pathlib import Path

import Levenshtein
import numpy as np
import torch

from scripts.dc_evaluation import (
    DcStage,
    run_ngspice_baseline,
    run_pta_ablation,
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
    build_a2c_state,
    extract_netlist_features,
)
from scripts.pta_prompts import (
    PTA_MUTATOR_SYSTEM_PROMPT,
    build_pta_error_feedback,
    build_pta_mutation_prompt,
)
from scripts.pta_reward import calculate_pta_reward
from scripts.pta_testing import SUCCESS, TIMEOUT, parse_with_pta, validate_pta_dc_netlist
from scripts.reinforcement_learning import Net, v_wrap


def write_clean_variant(path, content):
    content = content.encode("gbk", errors="ignore").decode(
        "gbk", errors="ignore"
    ).replace("µ", "u")
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
    variant = variant_path.read_text(errors="replace")
    distances = []
    for seed_name in names:
        seed = (workdir / seed_name).read_text(errors="replace")
        distances.append(
            Levenshtein.distance(variant, seed) / max(1, len(variant), len(seed))
        )
    return min(distances) if distances else 1.0


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
    for profile, outcome in evaluation.outcomes.items():
        source = Path(outcome.artifacts["directory"])
        if source.is_dir():
            destination = simulations / profile
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(source), str(destination))
    if evaluation.ngspice is not None:
        source = Path(evaluation.ngspice.artifacts["directory"])
        if source.is_dir():
            destination = simulations / "ngspice"
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(source), str(destination))
    shutil.copy2(seed_path, result_directory / f"seed_{seed_path.name}")
    shutil.copy2(variant_path, result_directory / variant_path.name)
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
):
    if not states:
        return None
    with torch.no_grad():
        next_value = 0.0 if terminal else float(
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
    )
    loss.backward()
    torch.nn.utils.clip_grad_norm_(net.parameters(), 5.0)
    optimizer.step()
    states.clear()
    actions.clear()
    rewards.clear()
    return float(loss.detach().item())


def transfer_lineage_reward(
    reward_ema,
    lineage,
    reward,
    decay,
    negative_decay=0.25,
    alpha=0.1,
):
    """Propagate signed credit only through the selected seed's ancestry."""
    if reward == 0:
        return
    inherited_decay = decay if reward > 0 else negative_decay
    for distance, action in enumerate(reversed(lineage), start=1):
        inherited = reward * (inherited_decay ** distance)
        reward_ema[action] = (
            (1.0 - alpha) * reward_ema[action] + alpha * inherited
        )


def build_lineage_training_batch(
    training_lineage,
    reward,
    decay,
    negative_decay=0.25,
):
    """Build independent A2C replay samples for actual ancestor decisions."""
    if reward == 0 or not training_lineage:
        return [], [], []
    inherited_decay = decay if reward > 0 else negative_decay
    states, actions, rewards = [], [], []
    for distance, (ancestor_state, action) in enumerate(
        reversed(training_lineage),
        start=1,
    ):
        states.append(ancestor_state.copy())
        actions.append(action)
        rewards.append(reward * (inherited_decay ** distance))
    return states, actions, rewards


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
        help="Temporal A2C discount; zero avoids credit across unrelated random seeds",
    )
    parser.add_argument("--rollout_size", type=int, default=16)
    parser.add_argument("--entropy_beta", type=float, default=0.01)
    parser.add_argument("--policy_temperature", type=float, default=1.15)
    parser.add_argument("--exploration_start", type=float, default=0.25)
    parser.add_argument("--exploration_end", type=float, default=0.05)
    parser.add_argument("--count_exploration", type=float, default=0.35)
    parser.add_argument("--lineage_decay", type=float, default=0.5)
    parser.add_argument("--lineage_negative_decay", type=float, default=0.25)
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
    if not 0.0 <= args.discount <= 1.0:
        raise ValueError("discount must be between 0 and 1")
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
    for seed_path in args.seed_dir.iterdir():
        if seed_path.is_file() and seed_path.suffix.lower() in {".cir", ".net", ".sp"}:
            seed_names.append(seed_path.name)
            shutil.copy2(seed_path, workdir / seed_path.name)
    if not seed_names:
        raise RuntimeError(f"No SPICE seed netlists found in {args.seed_dir}")
    seed_weights = {seed_name: 1.0 for seed_name in seed_names}

    mutations = initialize_pta_mutations()
    forms = initialize_pta_forms()
    action_count = len(mutations)
    state_dimension = 2 * action_count + len(FEATURE_NAMES)
    net = Net(state_dimension, action_count)
    optimizer = torch.optim.Adam(net.parameters(), lr=args.lr)
    selection_counts = np.zeros(action_count, dtype=np.float32)
    reward_ema = np.zeros(action_count, dtype=np.float32)
    buffer_s, buffer_a, buffer_r = [], [], []

    deepseek_client = DeepSeekClient(
        api_key=args.deepseek_api_key,
        base_url=args.deepseek_base_url,
    )
    error_feedback = {}
    seed_lineages = {seed_name: () for seed_name in seed_names}
    seed_training_lineages = {seed_name: () for seed_name in seed_names}
    stage_counts = {stage.value: 0 for stage in DcStage}
    total_num = target_num = 0
    deepseek_time = simulation_time = 0.0
    start_time = time.time()

    while time.time() - start_time < args.max_time:
        print("---------------------")
        seed_name = random.choices(
            seed_names,
            weights=[seed_weights.get(name, 1.0) for name in seed_names],
            k=1,
        )[0]
        seed_path = workdir / seed_name
        seed_features = extract_netlist_features(seed_path)
        state = build_a2c_state(selection_counts, reward_ema, seed_features)
        progress = min(1.0, (time.time() - start_time) / max(1, args.max_time))
        epsilon = args.exploration_start + progress * (
            args.exploration_end - args.exploration_start
        )
        count_bonus = args.count_exploration / np.sqrt(selection_counts + 1.0)
        mutation_no = int(net.choose_action(
            v_wrap(state[None, :]),
            temperature=args.policy_temperature,
            epsilon=epsilon,
            exploration_bonus=count_bonus,
        ).item())
        mutation = mutations[mutation_no]
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
                seed, mutation, forms[mutation_no]
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
            except DeepSeekAPIError as error:
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
            continue

        variant_name = datetime.now().strftime("%Y%m%d%H%M%S%f") + ".cir"
        variant_path = workdir / variant_name
        if not write_clean_variant(variant_path, variant_content):
            continue
        total_num += 1

        evaluation = reward_breakdown = None
        target_hit = False
        simulation_start = time.time()
        dc_valid, validation_error = validate_pta_dc_netlist(variant_path)
        if not dc_valid:
            reward_value = -6.0 if args.objective == "ngspice_differential" else -4.0
            stage, diagnostic = DcStage.INVALID, validation_error
        else:
            parse_code, parse_details = parse_with_pta(
                variant_path, args.maxtime, pta_bin
            )
            if parse_code != SUCCESS:
                stage = DcStage.TIMEOUT if parse_code == TIMEOUT else DcStage.INVALID
                if args.objective == "ngspice_differential":
                    reward_value = -4.0 if parse_code == TIMEOUT else -6.0
                else:
                    reward_value = -2.5 if parse_code == TIMEOUT else -4.0
                diagnostic = str(parse_details.get("stderr", ""))[-2000:]
            else:
                evaluation = run_pta_ablation(
                    variant_path,
                    variant_name,
                    args.maxtime,
                    pta_bin,
                    args.pta_output_root,
                    args.pta_config,
                    args.pta_option,
                )
                if ngspice_bin:
                    evaluation.ngspice = run_ngspice_baseline(
                        variant_path,
                        variant_name,
                        args.maxtime,
                        ngspice_bin,
                        args.pta_output_root / "ngspice",
                    )
                novelty = calculate_novelty(workdir, seed_names, variant_path)
                reward_breakdown = calculate_pta_reward(
                    evaluation,
                    novelty,
                    objective=args.objective,
                )
                target_hit = reward_breakdown.target_hit
                stage, reward_value, diagnostic = (
                    evaluation.stage,
                    reward_breakdown.total,
                    "",
                )
                if (
                    evaluation.ngspice is not None
                    and stage in {DcStage.INVALID, DcStage.FAILED, DcStage.TIMEOUT}
                ):
                    diagnostic = read_outcome_error(evaluation.ngspice)
                elif not evaluation.pta_succeeded:
                    diagnostic = read_outcome_error(evaluation.full_fallback)
        simulation_time += time.time() - simulation_start

        stage_counts[stage.value] += 1
        selection_counts[mutation_no] += 1.0
        reward_ema[mutation_no] = (
            (1.0 - args.reward_ema_alpha) * reward_ema[mutation_no]
            + args.reward_ema_alpha * reward_value
        )
        transfer_lineage_reward(
            reward_ema,
            seed_lineages.get(seed_name, ()),
            reward_value,
            args.lineage_decay,
            args.lineage_negative_decay,
            args.reward_ema_alpha,
        )
        replay_s, replay_a, replay_r = build_lineage_training_batch(
            seed_training_lineages.get(seed_name, ()),
            reward_value,
            args.lineage_decay,
            args.lineage_negative_decay,
        )
        if replay_s:
            replay_loss = update_a2c(
                net,
                optimizer,
                replay_s,
                replay_a,
                replay_r,
                state,
                discount=0.0,
                entropy_beta=args.entropy_beta,
                terminal=True,
            )
            print(f"Lineage replay loss: {replay_loss:.6f}")
        buffer_s.append(state.copy())
        buffer_a.append(mutation_no)
        buffer_r.append(reward_value)
        print(
            f"DC stage: {stage.value}; reward: {reward_value:.4f}; "
            f"target: {target_hit}"
        )

        result_directory = args.result_root / stage.value / variant_path.stem
        if evaluation is not None:
            archive_evaluation(
                evaluation,
                result_directory,
                seed_path,
                variant_path,
                reward_breakdown,
            )
            if target_hit:
                target_num += 1
            if args.objective == "ngspice_differential":
                frontier = (
                    evaluation.ngspice is not None
                    and evaluation.ngspice.succeeded
                    and evaluation.ngspice.stage in {
                        DcStage.GMIN,
                        DcStage.SOURCE_STEPPING,
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
                parent_lineage = seed_lineages.get(seed_name, ())
                seed_lineages[variant_name] = (
                    parent_lineage + (mutation_no,)
                )[-args.lineage_depth:]
                parent_training_lineage = seed_training_lineages.get(
                    seed_name,
                    (),
                )
                seed_training_lineages[variant_name] = (
                    parent_training_lineage + ((state.copy(), mutation_no),)
                )[-args.lineage_depth:]
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
        )
        elapsed = time.time() - start_time
        if len(buffer_s) >= args.rollout_size or elapsed >= args.max_time:
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
            )
            print(f"A2C loss: {loss:.6f}")

    if buffer_s:
        fallback_state = build_a2c_state(
            selection_counts,
            reward_ema,
            extract_netlist_features(workdir / random.choice(seed_names)),
        )
        update_a2c(
            net,
            optimizer,
            buffer_s,
            buffer_a,
            buffer_r,
            fallback_state,
            args.discount,
            args.entropy_beta,
            terminal=True,
        )

    print("total_num", total_num)
    print("target_num", target_num)
    print("stage_counts", stage_counts)
    print("elapsed_time", time.time() - start_time)
    deepseek_client.close()
    print("deepseek_time", deepseek_time)
    print("simulation_time", simulation_time)


if __name__ == "__main__":
    main()
