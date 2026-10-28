import json
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import Optional

from scripts.pta_testing import FAILURE, SUCCESS, TIMEOUT, simulate_on_pta


class DcStage(str, Enum):
    NR = "nr"
    GMIN = "gmin"
    SOURCE_STEPPING = "source_stepping"
    OPTRAN = "optran"
    UNKNOWN = "unknown"
    PTA = "pta"
    FAILED = "failed"
    TIMEOUT = "timeout"
    INVALID = "invalid"


@dataclass
class SolverOutcome:
    profile: str
    code: int
    stage: DcStage
    artifacts: dict

    @property
    def succeeded(self):
        return self.code == SUCCESS

    @property
    def timed_out(self):
        return self.code == TIMEOUT

    @property
    def verified_pta(self):
        return self.succeeded and self.stage == DcStage.PTA and self.artifacts.get("validation", {}).get("verified_pta") is True

    def as_dict(self):
        payload = {
            "profile": self.profile,
            "code": self.code,
            "stage": self.stage.value,
            "command": [str(item) for item in self.artifacts.get("command", [])],
            "returncode": self.artifacts.get("returncode"),
            "stdout": str(self.artifacts.get("stdout", ""))[-4000:],
            "stderr": str(self.artifacts.get("stderr", ""))[-4000:],
            "artifacts": {key: str(value) for key, value in self.artifacts.items() if isinstance(value, Path)},
        }
        if "attempted_stages" in self.artifacts:
            payload["attempted_stages"] = self.artifacts["attempted_stages"]
        if "failure_kind" in self.artifacts:
            payload["failure_kind"] = self.artifacts["failure_kind"]
        for key in ("validation", "events", "iterations", "elapsed_seconds", "profile_settings", "version", "input_sha256"):
            if key in self.artifacts:
                payload[key] = self.artifacts[key]
        return payload


@dataclass
class DcEvaluation:
    outcomes: dict
    ngspice: Optional[SolverOutcome] = None
    ngspice_profiles: dict = field(default_factory=dict)
    confirmation: dict = field(default_factory=dict)
    mutation: dict = field(default_factory=dict)

    @property
    def nr_only(self):
        return self.outcomes["nr_only"]

    @property
    def nr_source(self):
        return self.outcomes["nr_source"]

    @property
    def nr_pta(self):
        return self.outcomes["nr_pta"]

    @property
    def full_fallback(self):
        return self.outcomes["full_fallback"]

    @property
    def pta_succeeded(self):
        return any(
            outcome.verified_pta
            for outcome in (self.nr_pta, self.full_fallback)
        )

    @property
    def baseline_failed(self):
        if self.ngspice is not None:
            return (
                not self.ngspice.succeeded
                and self.ngspice.artifacts.get("failure_kind") == "convergence"
            )
        return not self.nr_source.succeeded and not self.nr_source.timed_out

    @property
    def authoritative_target(self):
        """A reproducible cross-engine recovery, not proof of model equivalence."""
        return (
            self.ngspice is not None
            and self.baseline_failed
            and self.pta_succeeded
            and self.confirmation.get("confirmed") is True
        )

    @property
    def nr_differential_candidate(self):
        nr = self.ngspice_profiles.get("ng_nr")
        return bool(nr and not nr.succeeded and nr.artifacts.get("failure_kind") == "convergence" and self.pta_succeeded)

    @property
    def trainable(self):
        outcomes = list(self.outcomes.values()) + list(self.ngspice_profiles.values())
        outcomes += [value for value in self.confirmation.values() if isinstance(value, SolverOutcome)]
        if self.ngspice:
            outcomes.append(self.ngspice)
        return not any(outcome and outcome.artifacts.get("failure_kind") in {
            "infrastructure", "crash", "unsettled_optran"
        } for outcome in outcomes)

    @property
    def internal_target(self):
        """PTA's internal non-PTA prefixes failed and PTA recovered them."""
        return (
            not self.nr_source.succeeded
            and not self.nr_source.timed_out
            and self.pta_succeeded
        )

    @property
    def full_pta_required(self):
        return (
            self.full_fallback.succeeded
            and self.full_fallback.stage == DcStage.PTA
        )

    @property
    def stage(self):
        # A configured ngspice run is the authoritative NR/gmin/source baseline.
        if self.ngspice is not None and self.ngspice.succeeded:
            return self.ngspice.stage
        if self.ngspice is not None:
            failure_kind = self.ngspice.artifacts.get("failure_kind")
            if failure_kind == "invalid":
                return DcStage.INVALID
            if failure_kind == "timeout":
                return DcStage.TIMEOUT
            if failure_kind != "convergence":
                return DcStage.FAILED
        if self.ngspice is None:
            if any(outcome.stage == DcStage.INVALID for outcome in self.outcomes.values()):
                return DcStage.INVALID
            if self.nr_only.succeeded:
                return DcStage.NR
            if self.nr_source.succeeded:
                return DcStage.SOURCE_STEPPING
        if self.pta_succeeded:
            return DcStage.PTA
        all_outcomes = list(self.outcomes.values())
        if self.ngspice is not None:
            all_outcomes.append(self.ngspice)
        if any(outcome.timed_out for outcome in all_outcomes):
            return DcStage.TIMEOUT
        return DcStage.FAILED

    def as_dict(self):
        return {
            "stage": self.stage.value,
            "baseline": "ngspice" if self.ngspice is not None else "pta_ordinary",
            "baseline_failed": self.baseline_failed,
            "authoritative_target": self.authoritative_target,
            "nr_differential_candidate": self.nr_differential_candidate,
            "trainable": self.trainable,
            "model_equivalence": "not_established",
            "confirmation": {key: value.as_dict() if isinstance(value, SolverOutcome) else value for key, value in self.confirmation.items()},
            "mutation": self.mutation,
            "internal_target": self.internal_target,
            "pta_succeeded": self.pta_succeeded,
            "full_pta_required": self.full_pta_required,
            "profiles": {
                name: outcome.as_dict()
                for name, outcome in self.outcomes.items()
            },
            "ngspice": self.ngspice.as_dict() if self.ngspice else None,
            "ngspice_profiles": {key: value.as_dict() for key, value in self.ngspice_profiles.items()},
        }


def _stage_for_profile(profile, code):
    if code == TIMEOUT:
        return DcStage.TIMEOUT
    if profile == "nr_only":
        return DcStage.NR
    if profile == "nr_source":
        return DcStage.SOURCE_STEPPING
    return DcStage.PTA


def run_pta_ablation(
    filepath,
    filename,
    maxtime,
    simulator_path,
    output_root,
    config_path=None,
    pta_options=None,
):
    """Run the four solver chains used by the convergence ablation."""
    profiles = {
        "nr_only": ("disabled", ["source-stepping.enabled=false"], []),
        "nr_source": ("disabled", ["source-stepping.enabled=true"], []),
        "nr_pta": ("fallback", ["source-stepping.enabled=false"], pta_options or []),
        "full_fallback": ("fallback", ["source-stepping.enabled=true"], pta_options or []),
    }
    outcomes = {}
    for profile, (mode, op_options, profile_pta_options) in profiles.items():
        code, artifacts = simulate_on_pta(
            filepath,
            filename,
            maxtime,
            simulator_path,
            Path(output_root) / profile,
            mode,
            config_path,
            profile_pta_options,
            op_options,
        )
        outcomes[profile] = SolverOutcome(
            profile,
            code,
            _stage_for_profile(profile, code),
            artifacts,
        )
        if code == SUCCESS:
            outcomes[profile].stage = DcStage(artifacts["validation"]["actual_stage"])

    # The actual successful method comes from this run's report/trace; a
    # separate prefix run is not evidence that a fallback took the same path.
    return DcEvaluation(outcomes)


def _classify_ngspice_stage(text):
    from scripts.ngspice_testing import successful_stage
    return DcStage(successful_stage(text))


def _ngspice_attempted_stages(text):
    from scripts.ngspice_testing import parse_stage_events
    return list(dict.fromkeys(["nr"] + [event["stage"] for event in parse_stage_events(text)]))


def _ngspice_failure_kind(text):
    from scripts.ngspice_testing import failure_kind
    return failure_kind(text)


def run_ngspice_baseline(filepath, filename, maxtime, simulator_path, output_root, profile="ng_full"):
    from scripts.ngspice_testing import run_profile
    code, stage, artifacts = run_profile(filepath, filename, maxtime, simulator_path, output_root, profile)
    return SolverOutcome(profile, code, DcStage(stage), artifacts)


def run_ngspice_ablation(filepath, filename, maxtime, simulator_path, output_root):
    from scripts.ngspice_testing import PROFILES
    return {profile: run_ngspice_baseline(filepath, filename, maxtime, simulator_path,
                                         Path(output_root) / profile, profile)
            for profile in PROFILES}


def write_evaluation_manifest(evaluation, path, reward=None):
    payload = evaluation.as_dict()
    if reward is not None:
        payload["reward"] = reward
    Path(path).write_text(
        json.dumps(payload, indent=2, sort_keys=True),
        encoding="utf-8",
    )
