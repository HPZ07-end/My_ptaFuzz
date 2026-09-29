import json
import shutil
import subprocess
from dataclasses import dataclass
from enum import Enum
from pathlib import Path

from scripts.pta_testing import FAILURE, SUCCESS, TIMEOUT, simulate_on_pta


class DcStage(str, Enum):
    NR = "nr"
    GMIN = "gmin"
    SOURCE_STEPPING = "source_stepping"
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

    def as_dict(self):
        payload = {
            "profile": self.profile,
            "code": self.code,
            "stage": self.stage.value,
            "command": [str(item) for item in self.artifacts.get("command", [])],
            "returncode": self.artifacts.get("returncode"),
            "stdout": str(self.artifacts.get("stdout", ""))[-4000:],
            "stderr": str(self.artifacts.get("stderr", ""))[-4000:],
        }
        if "attempted_stages" in self.artifacts:
            payload["attempted_stages"] = self.artifacts["attempted_stages"]
        if "failure_kind" in self.artifacts:
            payload["failure_kind"] = self.artifacts["failure_kind"]
        return payload


@dataclass
class DcEvaluation:
    outcomes: dict
    ngspice: SolverOutcome | None = None

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
            outcome.succeeded and outcome.stage == DcStage.PTA
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
        """Stock ngspice genuinely failed to converge and PTA recovered it."""
        return (
            self.ngspice is not None
            and self.baseline_failed
            and self.pta_succeeded
        )

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
            "internal_target": self.internal_target,
            "pta_succeeded": self.pta_succeeded,
            "full_pta_required": self.full_pta_required,
            "profiles": {
                name: outcome.as_dict()
                for name, outcome in self.outcomes.items()
            },
            "ngspice": self.ngspice.as_dict() if self.ngspice else None,
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

    # A fallback executable can return success before entering its nominal last
    # stage.  The independent prefix runs disambiguate the actual stopping point.
    if outcomes["nr_only"].succeeded:
        for profile in ("nr_source", "nr_pta", "full_fallback"):
            if outcomes[profile].succeeded:
                outcomes[profile].stage = DcStage.NR
    else:
        if outcomes["nr_source"].succeeded:
            outcomes["nr_source"].stage = DcStage.SOURCE_STEPPING
            if outcomes["full_fallback"].succeeded:
                outcomes["full_fallback"].stage = DcStage.SOURCE_STEPPING
        if outcomes["nr_pta"].succeeded:
            outcomes["nr_pta"].stage = DcStage.PTA
        if (
            not outcomes["nr_source"].succeeded
            and outcomes["full_fallback"].succeeded
        ):
            outcomes["full_fallback"].stage = DcStage.PTA
    return DcEvaluation(outcomes)


def _classify_ngspice_stage(text):
    lower = text.lower()
    source_position = max(
        lower.rfind("source stepping"),
        lower.rfind("source-step"),
    )
    gmin_position = max(
        lower.rfind("gmin stepping"),
        lower.rfind("gmin-step"),
    )
    if source_position >= 0 and source_position > gmin_position:
        return DcStage.SOURCE_STEPPING
    if gmin_position >= 0:
        return DcStage.GMIN
    return DcStage.NR


def _ngspice_attempted_stages(text):
    lower = text.lower()
    stages = [DcStage.NR.value]
    if "gmin stepping" in lower or "gmin-step" in lower:
        stages.append(DcStage.GMIN.value)
    if "source stepping" in lower or "source-step" in lower:
        stages.append(DcStage.SOURCE_STEPPING.value)
    return stages


def _ngspice_failure_kind(text):
    lower = text.lower()
    syntax_invalid_markers = (
        "error on line",
        "unknown parameter",
        "unknown device",
        "unknown subckt",
        "model definition not found",
        "could not find a model",
        "undefined parameter",
        "syntax error",
        "fatal error",
    )
    convergence_markers = (
        "failed to converge",
        "no convergence",
        "dc solution failed",
        "operating point failed",
        "timestep too small",
        "iteration limit reached",
    )
    structural_invalid_markers = (
        "singular matrix",
        "no dc path",
    )
    if any(marker in lower for marker in syntax_invalid_markers):
        return "invalid"
    if any(marker in lower for marker in convergence_markers):
        return "convergence"
    if any(marker in lower for marker in structural_invalid_markers):
        return "invalid"
    return "simulator_error"


def run_ngspice_baseline(filepath, filename, maxtime, simulator_path, output_root):
    """Run stock ngspice and infer the successful DC recovery stage from its log."""
    case_directory = Path(output_root).resolve() / Path(filename).stem
    case_directory.mkdir(parents=True, exist_ok=True)
    netlist_path = case_directory / Path(filename).name
    shutil.copy2(filepath, netlist_path)
    raw_path = case_directory / f"{Path(filename).stem}.raw"
    log_path = case_directory / f"{Path(filename).stem}.log"
    command = [
        str(Path(simulator_path).resolve()),
        "-b",
        "-r",
        str(raw_path),
        "-o",
        str(log_path),
        str(netlist_path),
    ]
    try:
        result = subprocess.run(
            command,
            cwd=case_directory,
            timeout=maxtime,
            capture_output=True,
            text=True,
            shell=False,
        )
        stdout = result.stdout or ""
        stderr = result.stderr or ""
        log_text = log_path.read_text(errors="replace") if log_path.is_file() else ""
        succeeded = result.returncode == 0 and raw_path.is_file() and raw_path.stat().st_size > 0
        code = SUCCESS if succeeded else FAILURE
        diagnostic_text = log_text + "\n" + stdout + "\n" + stderr
        stage = _classify_ngspice_stage(diagnostic_text)
        failure_kind = None if succeeded else _ngspice_failure_kind(diagnostic_text)
        return SolverOutcome("ngspice", code, stage, {
            "directory": case_directory,
            "raw": raw_path,
            "log": log_path,
            "command": command,
            "stdout": stdout,
            "stderr": stderr,
            "returncode": result.returncode,
            "attempted_stages": _ngspice_attempted_stages(diagnostic_text),
            "failure_kind": failure_kind,
        })
    except subprocess.TimeoutExpired as error:
        return SolverOutcome("ngspice", TIMEOUT, DcStage.TIMEOUT, {
            "directory": case_directory,
            "raw": raw_path,
            "log": log_path,
            "command": command,
            "stdout": error.stdout or "",
            "stderr": error.stderr or "",
            "returncode": None,
            "failure_kind": "timeout",
        })
    except OSError as error:
        return SolverOutcome("ngspice", FAILURE, DcStage.FAILED, {
            "directory": case_directory,
            "raw": raw_path,
            "log": log_path,
            "command": command,
            "stdout": "",
            "stderr": str(error),
            "returncode": None,
            "failure_kind": "simulator_error",
        })


def write_evaluation_manifest(evaluation, path, reward=None):
    payload = evaluation.as_dict()
    if reward is not None:
        payload["reward"] = reward
    Path(path).write_text(
        json.dumps(payload, indent=2, sort_keys=True),
        encoding="utf-8",
    )
