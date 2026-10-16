"""Bounded rewards: true recovery dominates every frontier bonus."""

import math
from dataclasses import asdict, dataclass

from scripts.dc_evaluation import DcStage


@dataclass
class RewardBreakdown:
    objective: str
    target_hit: bool
    stage_reward: float
    differential_bonus: float
    full_fallback_bonus: float
    nr_pta_bonus: float
    novelty_bonus: float
    timeout_penalty: float
    total: float
    difficulty_bonus: float = 0.0
    progress_bonus: float = 0.0
    cost_penalty: float = 0.0
    trainable: bool = True

    def as_dict(self):
        return asdict(self)


AUTHORITATIVE_STAGE_REWARDS = {
    DcStage.NR: -3.0, DcStage.GMIN: -2.0, DcStage.SOURCE_STEPPING: -1.0,
    DcStage.OPTRAN: -1.0, DcStage.UNKNOWN: -3.0, DcStage.PTA: 0.0,
    DcStage.FAILED: -3.0, DcStage.TIMEOUT: -4.0, DcStage.INVALID: -6.0,
}
INTERNAL_STAGE_REWARDS = {**AUTHORITATIVE_STAGE_REWARDS, DcStage.INVALID: -4.0}


def difficulty_score(evaluation):
    """Stage plus size-normalized work; absent iteration telemetry stays absent."""
    if evaluation is None or evaluation.ngspice is None:
        return None
    outcome = evaluation.ngspice
    if outcome.artifacts.get("failure_kind") in {"invalid", "infrastructure", "timeout", "invalid_output"}:
        return None
    ranks = {DcStage.NR: 0.0, DcStage.GMIN: 0.4, DcStage.SOURCE_STEPPING: 0.7,
             DcStage.OPTRAN: 0.85}
    base = ranks.get(outcome.stage)
    if not outcome.succeeded:
        base = 1.0 if outcome.artifacts.get("failure_kind") == "convergence" else None
    if base is None:
        return None
    iterations = outcome.artifacts.get("iterations")
    size = max(1, evaluation.mutation.get("device_count", 1))
    work = min(1.0, math.log1p(max(0, iterations) / size) / math.log1p(100)) if iterations is not None else 0.0
    return min(1.0, base + 0.15 * work)


def calculate_pta_reward(evaluation, novelty=0.0, objective="ngspice_differential",
                         parent_evaluation=None, cost_budget=60.0):
    if objective not in {"ngspice_differential", "pta_internal"}:
        raise ValueError(f"unknown reward objective: {objective}")
    authoritative = objective == "ngspice_differential"
    if authoritative and evaluation.ngspice is None:
        raise ValueError("ngspice_differential reward requires an ngspice outcome")
    target = evaluation.authoritative_target if authoritative else evaluation.internal_target
    stage = evaluation.stage
    table = AUTHORITATIVE_STAGE_REWARDS if authoritative else INTERNAL_STAGE_REWARDS
    base = table[stage]
    # A failed PTA verification never earns frontier shaping, even if ngspice
    # completed a late recovery stage. Repeated paths are diagnostics, not bonuses.
    gate = evaluation.pta_succeeded and stage not in {DcStage.INVALID, DcStage.TIMEOUT}
    difficulty = difficulty_score(evaluation)
    parent = difficulty_score(parent_evaluation)
    d_bonus = 0.25 * difficulty if gate and difficulty is not None else 0.0
    n_bonus = 0.25 * min(1.0, max(0.0, float(novelty))) if gate else 0.0
    progress = 0.25 * max(-1.0, min(1.0, difficulty - parent)) if gate and difficulty is not None and parent is not None else 0.0
    from scripts.pta_pipeline import iter_outcomes
    elapsed = sum(max(0.0, float(o.artifacts.get("elapsed_seconds", 0))) for _, o in iter_outcomes(evaluation))
    cost = -0.25 * min(1.0, elapsed / max(1e-6, cost_budget)) if gate else 0.0
    target_bonus = 12.0 if target else 0.0
    total = base + target_bonus + d_bonus + n_bonus + progress + cost
    if not target:
        total = min(-0.25, total)
    return RewardBreakdown(objective, target, base, target_bonus, 0.0, 0.0,
                           n_bonus, 0.0, max(-6.0, min(13.0, total)),
                           d_bonus, progress, cost, evaluation.trainable)
