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

    def as_dict(self):
        return asdict(self)


INTERNAL_STAGE_REWARDS = {
    DcStage.NR: -1.0,
    DcStage.GMIN: 1.5,
    DcStage.SOURCE_STEPPING: 2.5,
    DcStage.PTA: 5.0,
    DcStage.FAILED: -2.0,
    DcStage.TIMEOUT: -2.5,
    DcStage.INVALID: -4.0,
}


AUTHORITATIVE_STAGE_REWARDS = {
    # Any stock-ngspice success is not the target. Later fallback success is a
    # useful frontier signal, but remains negative so it cannot dominate true
    # differential cases.
    DcStage.NR: -3.0,
    DcStage.GMIN: -1.5,
    DcStage.SOURCE_STEPPING: -0.5,
    DcStage.PTA: 4.0,
    DcStage.FAILED: -3.0,
    DcStage.TIMEOUT: -4.0,
    DcStage.INVALID: -6.0,
}


def calculate_pta_reward(
    evaluation,
    novelty=0.0,
    objective="ngspice_differential",
):
    """Reward only legitimate convergence recovery for the chosen baseline."""
    if objective not in {"ngspice_differential", "pta_internal"}:
        raise ValueError(f"unknown reward objective: {objective}")
    if objective == "ngspice_differential" and evaluation.ngspice is None:
        raise ValueError(
            "ngspice_differential reward requires an ngspice outcome"
        )

    authoritative = objective == "ngspice_differential"
    target_hit = (
        evaluation.authoritative_target
        if authoritative
        else evaluation.internal_target
    )
    reward_table = (
        AUTHORITATIVE_STAGE_REWARDS
        if authoritative
        else INTERNAL_STAGE_REWARDS
    )
    stage_reward = reward_table[evaluation.stage]
    if evaluation.stage in {DcStage.INVALID, DcStage.TIMEOUT}:
        return RewardBreakdown(
            objective,
            False,
            stage_reward,
            0.0,
            0.0,
            0.0,
            0.0,
            0.0,
            stage_reward,
        )
    differential_bonus = (8.0 if authoritative else 4.0) if target_hit else 0.0
    full_fallback_bonus = (
        1.5 if target_hit and evaluation.full_pta_required else 0.0
    )
    nr_pta_bonus = 1.0 if (
        target_hit
        and not evaluation.nr_only.succeeded
        and evaluation.nr_pta.succeeded
        and evaluation.nr_pta.stage == DcStage.PTA
    ) else 0.0
    novelty_weight = 1.5 if authoritative else 0.75
    novelty_bonus = (
        novelty_weight * min(1.0, max(0.0, float(novelty)))
        if target_hit
        else 0.0
    )

    all_outcomes = list(evaluation.outcomes.values())
    if evaluation.ngspice is not None:
        all_outcomes.append(evaluation.ngspice)
    timeout_count = sum(outcome.timed_out for outcome in all_outcomes)
    timeout_penalty = -0.25 * timeout_count

    total = (
        stage_reward
        + differential_bonus
        + full_fallback_bonus
        + nr_pta_bonus
        + novelty_bonus
        + timeout_penalty
    )
    total = min(18.0, max(-6.0, total))
    return RewardBreakdown(
        objective,
        target_hit,
        stage_reward,
        differential_bonus,
        full_fallback_bonus,
        nr_pta_bonus,
        novelty_bonus,
        timeout_penalty,
        total,
    )
