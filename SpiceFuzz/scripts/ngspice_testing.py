"""Controlled ngspice OP profiles and conservative evidence classification.

Profile control is owned by the evaluator, never by the generated circuit.
See ngspice manual 11.3.5 and the maintainer's optran disable recipe:
https://sourceforge.net/p/ngspice/discussion/133842/thread/06b6463c9e/
"""

import hashlib
import re
import subprocess
import time
from pathlib import Path

from scripts.solver_validation import read_operating_point


PROFILES = {
    "ng_nr": "1 0 0 0 0 0",
    "ng_gmin": "1 1 0 0 0 0",
    "ng_source": "1 1 1 0 0 0",
    "ng_full": "1 1 1 10n 10u 0",
}
SETTINGS = {"itl1": 100, "itl2": 50, "itl4": 10, "reltol": 1e-3, "vntol": 1e-6,
            "abstol": 1e-12, "gmin": 1e-12, "temp": 27, "trtol": 7, "method": "trap"}
SYNTAX_ERRORS = ("error on line", "unknown parameter", "unknown device", "unknown subckt",
                 "model definition not found", "could not find a model", "undefined parameter",
                 "syntax error", "fatal error")


def failure_kind(text):
    lower = text.lower()
    if any(marker in lower for marker in (
        "no such command", "unknown command", "can't open", "cannot open", "permission denied",
    )):
        return "infrastructure"
    if any(marker in lower for marker in SYNTAX_ERRORS):
        return "invalid"
    if any(marker in lower for marker in (
        "failed to converge", "no convergence", "dc solution failed", "operating point failed",
        "timestep too small", "iteration limit reached",
    )):
        return "convergence"
    if "singular matrix" in lower or "no dc path" in lower:
        return "invalid"
    return "simulator_error"


def parse_stage_events(text):
    events, active = [], "nr"
    for line in text.lower().splitlines():
        # A source stage may itself try gmin. Only its outer completion wins.
        stage = None
        if re.search(r"transient\s+op|optran(?:\s+analysis)?\s+(?:start|fail|complet)", line):
            stage = "optran"
        elif re.search(r"source[ -]step", line):
            stage = "source_stepping"
        elif re.search(r"gmin[ -]step", line):
            stage = "gmin"
        if stage is not None:
            nested = active == "source_stepping" and stage == "gmin"
            if "start" in line:
                if active != "source_stepping" or stage != "gmin":
                    active = stage
                events.append({"stage": stage, "status": "entered", "nested": nested})
            if "fail" in line:
                events.append({"stage": stage, "status": "failed", "nested": nested})
            elif "complet" in line or re.search(r"finished successfully|\b(?:stepping|op|optran)\s+succeeded\b", line):
                events.append({"stage": stage, "status": "succeeded", "nested": nested})
        elif line.strip() == "completed":
            events.append({"stage": active, "status": "succeeded"})
    return events


def successful_stage(text, valid_op=False, profile=None):
    events = parse_stage_events(text)
    outer = [event for event in events if not event.get("nested")]
    successes = [event["stage"] for event in outer if event["status"] == "succeeded"]
    if successes:
        return successes[-1] if outer[-1]["status"] == "succeeded" else "unknown"
    if valid_op and not events:
        return "nr"
    return "unknown"


def run_profile(filepath, filename, maxtime, simulator_path, output_root, profile="ng_full"):
    if profile not in PROFILES:
        raise ValueError(f"unknown ngspice profile: {profile}")
    source = Path(filepath).resolve()
    directory = Path(output_root).resolve() / Path(filename).stem
    directory.mkdir(parents=True, exist_ok=True)
    deck, raw, log = directory / Path(filename).name, directory / "op.raw", directory / "ngspice.log"
    for path in (raw, log):
        path.unlink(missing_ok=True)
    original = source.read_text(errors="replace")
    # The common-netlist validator rejects user-owned control/options cards.
    from scripts.pta_testing import validate_pta_dc_netlist
    valid, reason = validate_pta_dc_netlist(source)
    if not valid:
        return 1, "invalid", {"directory": directory, "failure_kind": "invalid", "stderr": reason}
    body = re.sub(r"(?im)^\s*\.end\s*$", "", original).rstrip()
    control = [".control", "set noaskquit", "set filetype=ascii", "version",
               "optran " + PROFILES[profile],
               "option " + " ".join(f"{key}={value}" for key, value in SETTINGS.items()),
               "echo SPICEFuzz_OP_BEGIN", "op", "rusage totiter",
               "write op.raw all", "echo SPICEFuzz_OP_END", "quit", ".endc", ".end"]
    deck.write_text(body + "\n" + "\n".join(control) + "\n", encoding="utf-8")
    command = [str(Path(simulator_path).resolve()), "-n", "-b", "-o", str(log), str(deck)]
    artifacts = {"directory": directory, "raw": raw, "log": log, "command": command,
                 "profile_settings": {"optran": PROFILES[profile], **SETTINGS},
                 "input_sha256": hashlib.sha256(source.read_bytes()).hexdigest()}
    started = time.monotonic()
    try:
        result = subprocess.run(command, cwd=directory, timeout=maxtime,
                                capture_output=True, text=True, shell=False)
        stdout, stderr = result.stdout or "", result.stderr or ""
        diagnostic = (log.read_text(errors="replace") if log.is_file() else "") + "\n" + stdout + "\n" + stderr
        op = read_operating_point(raw)
        kind = failure_kind(diagnostic)
        if result.returncode < 0 or result.returncode >= 2**31:
            kind = "crash"
        fatal_op = bool(re.search(r"(?i)doanalyses:.*(?:failed|too small)|dc solution failed|operating point failed", diagnostic))
        valid_op = (result.returncode == 0 and op["valid"] and not fatal_op
                    and not any(marker in diagnostic.lower() for marker in SYNTAX_ERRORS)
                    and kind not in {"infrastructure", "crash"})
        stage = successful_stage(diagnostic, valid_op=valid_op, profile=profile) if valid_op else "unknown"
        iterations = re.findall(r"(?i)(?:total\s+iterations|totiter)\s*[:=]\s*(\d+)", diagnostic)
        version = re.search(r"(?im)^.*\bngspice[- ]\d+[^\r\n]*", diagnostic)
        artifacts.update({"stdout": stdout, "stderr": stderr, "returncode": result.returncode,
                          "failure_kind": None if valid_op else ("invalid_output" if kind == "simulator_error" and result.returncode == 0 else kind),
                          "validation": {"op_valid": bool(valid_op), "reason": op["reason"], "solution": op["solution"]},
                          "events": parse_stage_events(diagnostic),
                          "attempted_stages": list(dict.fromkeys(["nr"] + [e["stage"] for e in parse_stage_events(diagnostic)])),
                          "iterations": int(iterations[-1]) if iterations else None,
                          "version": version.group(0).strip() if version else "unknown"})
        code = 0 if valid_op else 1
    except subprocess.TimeoutExpired as error:
        artifacts.update({"stdout": _text(error.stdout), "stderr": _text(error.stderr),
                          "returncode": None, "failure_kind": "timeout"})
        code, stage = 2, "timeout"
    except OSError as error:
        artifacts.update({"stdout": "", "stderr": str(error), "returncode": None,
                          "failure_kind": "infrastructure"})
        code, stage = 1, "failed"
    artifacts["elapsed_seconds"] = time.monotonic() - started
    (directory / "stdout.txt").write_text(artifacts["stdout"], encoding="utf-8")
    (directory / "stderr.txt").write_text(artifacts["stderr"], encoding="utf-8")
    return code, stage, artifacts


def _text(value):
    return value.decode(errors="replace") if isinstance(value, bytes) else value or ""
