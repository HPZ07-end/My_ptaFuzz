"""Shared single-circuit evaluation for fuzzing and offline replay."""

from pathlib import Path
import hashlib
import re

from scripts.dc_evaluation import (
    DcEvaluation, DcStage, SolverOutcome, run_ngspice_ablation,
    run_ngspice_baseline, run_pta_ablation,
)
from scripts.pta_testing import SUCCESS, TIMEOUT, parse_with_pta, simulate_on_pta, validate_pta_dc_netlist
from scripts.solver_validation import solutions_agree


def verify_optran_stability(evaluation, path, timeout, ngspice_bin, root):
    """Check duration sensitivity; this is not a device-equation residual proof.

    Unsettled transient endpoints are ambiguous, never convergence-failure hits.
    Keep their raw evidence and exclude them from policy updates.
    """
    ng = evaluation.ngspice
    if not ng or not ng.succeeded or ng.stage != DcStage.OPTRAN:
        return
    from scripts.ngspice_testing import run_profile
    original = ng.artifacts.get("validation", {}).get("solution")
    checks = []
    for factor, stop in ((10, "100u"), (100, "1m")):
        name = "optran_" + str(factor) + "x"
        code, stage, artifacts = run_profile(path, Path(path).name, timeout, ngspice_bin,
                                              Path(root) / name, "ng_full", "1 1 1 10n " + stop + " 0")
        outcome = SolverOutcome(name, code, DcStage(stage), artifacts)
        evaluation.confirmation[name] = outcome
        checks.append(outcome.succeeded and solutions_agree(original, artifacts.get("validation", {}).get("solution")))
    stable = all(checks)
    evaluation.confirmation["optran_duration_stable"] = stable
    evaluation.confirmation["optran_validation_limit"] = "duration sensitivity only; DC residual and model equivalence not established"
    ng.artifacts.setdefault("validation", {})["duration_stable"] = stable
    if not stable:
        ng.code = 1
        ng.artifacts["failure_kind"] = "unsettled_optran"


def evaluate_candidate(path, timeout, pta_bin, output_root, ngspice_bin=None,
                       config_path=None, pta_options=None, confirm=True):
    path, root = Path(path), Path(output_root)
    valid, reason = validate_pta_dc_netlist(path)
    source_hash = hashlib.sha256(path.read_bytes()).hexdigest()
    if valid:
        # Output selection must not hide physical unknowns from validation.
        # Both engines get this same deck; only .print selection is removed.
        lines, skipping = [], False
        for index, line in enumerate(path.read_text(errors="replace").splitlines()):
            if index and re.match(r"(?i)^\s*\.print\b", line):
                skipping = True
            elif skipping and (line.lstrip().startswith(("+", "*")) or not line.strip()):
                continue
            else:
                skipping = False
                lines.append(line)
        evaluated = root / "inputs" / path.name
        evaluated.parent.mkdir(parents=True, exist_ok=True)
        evaluated.write_text("\n".join(lines) + "\n", encoding="utf-8")
        path = evaluated
    code, details = parse_with_pta(path, timeout, pta_bin) if valid else (1, {"stderr": reason})
    if code != SUCCESS:
        stage = DcStage.TIMEOUT if code == TIMEOUT else DcStage.INVALID
        # Never treat a missing executable as invalid circuit data.
        kind = "infrastructure" if details.get("returncode") is None and valid and code != TIMEOUT else stage.value
        outcome = SolverOutcome("parse", code, stage, {**details, "failure_kind": kind})
        evaluation = DcEvaluation({name: outcome for name in ("nr_only", "nr_source", "nr_pta", "full_fallback")})
        evaluation.mutation["source_sha256"] = source_hash
        if valid:
            evaluation.mutation.update({"evaluation_input": str(path.resolve()),
                                        "evaluation_sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
        if ngspice_bin:
            evaluation.ngspice = outcome
        return evaluation
    evaluation = run_pta_ablation(path, path.name, timeout, pta_bin, root, config_path, pta_options)
    evaluation.mutation.update({"source_sha256": source_hash,
                                "evaluation_input": str(path.resolve()),
                                "evaluation_sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    evaluation.mutation["device_count"] = sum(1 for line in path.read_text(errors="replace").splitlines()[1:]
                                              if line.strip() and line.strip()[0].lower() in "rclvidqmx")
    if ngspice_bin:
        evaluation.ngspice_profiles = run_ngspice_ablation(path, path.name, timeout, ngspice_bin, root)
        evaluation.ngspice = evaluation.ngspice_profiles["ng_full"]
        verify_optran_stability(evaluation, path, timeout, ngspice_bin, root)
    if confirm and evaluation.baseline_failed and evaluation.pta_succeeded and ngspice_bin:
        original = next(o for o in (evaluation.nr_pta, evaluation.full_fallback) if o.verified_pta)
        repeat_ng = run_ngspice_baseline(path, path.name, timeout, ngspice_bin, root / "confirm_ng", "ng_full")
        op_options = ["source-stepping.enabled=" + ("true" if original.profile == "full_fallback" else "false")]
        repeat_code, artifacts = simulate_on_pta(path, path.name, timeout, pta_bin, root / "confirm_pta",
                                               "fallback", config_path, pta_options, op_options)
        repeat_pta = SolverOutcome("confirm_pta", repeat_code,
                                  DcStage(artifacts.get("validation", {}).get("actual_stage", "unknown")), artifacts)
        agreement = solutions_agree(original.artifacts.get("validation", {}).get("solution"),
                                    artifacts.get("validation", {}).get("solution"))
        evaluation.confirmation = {"ngspice": repeat_ng, "pta": repeat_pta,
                                   "solution_agreement": agreement,
                                   "confirmed": bool(repeat_ng.artifacts.get("failure_kind") == "convergence"
                                                     and not repeat_ng.succeeded and repeat_pta.verified_pta and agreement)}
    return evaluation


def iter_outcomes(evaluation):
    """Unique physical runs, including all baseline profiles and confirmations."""
    seen = set()
    for name, outcome in list(evaluation.outcomes.items()) + list(evaluation.ngspice_profiles.items()) + [
        ("confirm_" + key, value) for key, value in evaluation.confirmation.items() if isinstance(value, SolverOutcome)
    ] + ([("ngspice", evaluation.ngspice)] if evaluation.ngspice else []):
        if id(outcome) not in seen:
            seen.add(id(outcome))
            yield name, outcome
