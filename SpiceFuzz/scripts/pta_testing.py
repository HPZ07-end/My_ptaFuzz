import re
import subprocess
import time
from pathlib import Path

from scripts.solver_validation import validate_pta_artifacts


SUCCESS = 0
FAILURE = 1
TIMEOUT = 2


def artifact_paths(output_root, filename):
    output_root = Path(output_root).resolve()
    stem = Path(filename).stem
    case_directory = output_root / stem
    return {
        "directory": case_directory,
        "listing": case_directory / f"{stem}.out",
        "raw": case_directory / f"{stem}.raw",
        "error": case_directory / f"{stem}.err",
        "report": case_directory / f"{stem}.solve.txt",
        "trace": case_directory / f"{stem}.pta.jsonl",
    }


def validate_pta_dc_netlist(filepath):
    """Enforce the common-deck contract independently of the LLM prompt."""
    allowed = {".model", ".subckt", ".ends", ".op", ".nodeset", ".ic", ".print", ".end"}
    lines = []
    for raw_line in Path(filepath).read_text(errors="replace").splitlines()[1:]:
        line = raw_line.strip()
        if not line or line.startswith("*"):
            continue
        for marker in (";", "//", "$"):
            index = line.find(marker)
            if index != -1:
                line = line[:index].strip()
        if not line:
            continue
        if line.startswith("+"):
            if not lines:
                return False, "orphan continuation line"
            lines[-1] += " " + line[1:]
        else:
            lines.append(line)
    cards = [line.split()[0].lower() for line in lines]
    if cards.count(".op") != 1 or cards.count(".end") != 1 or not cards or cards[-1] != ".end":
        return False, "require exactly one .op and final .end"
    depth = 0
    for line in lines:
        tokens = line.split()
        card = tokens[0].lower()
        if card == ".subckt":
            if depth:
                return False, "nested .subckt definitions are not permitted"
            depth += 1
        elif card == ".ends":
            if not depth:
                return False, "unmatched .ends"
            depth -= 1
        elif depth and card.startswith("."):
            return False, "control directives inside .subckt are not permitted"
        if card.startswith(".") and card not in allowed:
            return False, "unsupported or solver-specific directive: " + card
        if card == ".print" and (len(tokens) < 3 or tokens[1].lower() != "op"):
            return False, "only .print op is permitted"
        if not card.startswith(".") and card[0] not in "rclvidqmx":
            return False, "unsupported device: " + tokens[0]
        if re.search(r"(?i)(?<![\w.])[+-]?(?:nan|inf(?:inity)?)(?![\w.])", " ".join(tokens[1:])):
            return False, "non-finite numeric value"
    if depth:
        return False, "unclosed .subckt"
    return True, ""


def initial_hint_signature(filepath):
    """Hints are an explicit experiment axis, frozen in ordinary mutation runs."""
    hints = []
    lines = []
    for raw in Path(filepath).read_text(errors="replace").splitlines()[1:]:
        line = re.split(r";|\$|//", raw, maxsplit=1)[0].strip().lower()
        if line.startswith("+") and lines:
            lines[-1] += " " + line[1:]
        elif line and not line.startswith("*"):
            lines.append(line)
    for line in lines:
        if line.startswith((".nodeset", ".ic")):
            hints.append(re.sub(r"\s+", "", line))
        elif line and not line.startswith(("*", ".")):
            fields = re.findall(r"\bic\s*=\s*.*?(?=\s+\w+\s*=|\s+off\b|$)|\boff\b", line)
            if fields:
                hints.append(line.split()[0] + ":" + re.sub(r"\s+", "", ",".join(fields)))
    return tuple(sorted(hints))


def parse_with_pta(filepath, maxtime, simulator_path):
    """Parse a netlist with PTA without running an analysis."""
    command = [
        str(Path(simulator_path).resolve()),
        "--parse-only",
        str(Path(filepath).resolve()),
    ]
    try:
        result = subprocess.run(
            command,
            timeout=maxtime,
            capture_output=True,
            text=True,
            shell=False,
        )
    except subprocess.TimeoutExpired as error:
        return TIMEOUT, {
            "command": command,
            "stdout": error.stdout or "",
            "stderr": error.stderr or "",
            "returncode": None,
        }
    except OSError as error:
        return FAILURE, {
            "command": command,
            "stdout": "",
            "stderr": str(error),
            "returncode": None,
        }

    return (SUCCESS if result.returncode == 0 else FAILURE), {
        "command": command,
        "stdout": result.stdout,
        "stderr": result.stderr,
        "returncode": result.returncode,
    }


def simulate_on_pta(
    filepath,
    filename,
    maxtime,
    simulator_path,
    output_root,
    mode="force",
    config_path=None,
    pta_options=None,
    op_options=None,
):
    """Run one netlist with the PTA simulator.

    Return codes follow the existing SpiceFuzz convention:
    0 means success, 1 means simulator failure, and 2 means timeout.
    """
    if mode not in {"disabled", "force", "fallback"}:
        raise ValueError("PTA mode must be 'disabled', 'force', or 'fallback'")
    if mode == "disabled" and pta_options:
        raise ValueError("PTA options require force or fallback mode")

    filepath = Path(filepath).resolve()
    output_root = Path(output_root).resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    artifacts = artifact_paths(output_root, filename)
    artifacts["directory"].mkdir(parents=True, exist_ok=True)
    for key in ("listing", "raw", "error", "report", "trace"):
        artifacts[key].unlink(missing_ok=True)

    command = [
        str(Path(simulator_path).resolve()),
        "--pta",
        mode,
        "-b",
        "--output-root",
        str(output_root),
    ]
    if mode != "disabled":
        command.append("--pta-diagnostics")
    if config_path:
        command.extend(["--config", str(Path(config_path).resolve())])
    for assignment in op_options or []:
        command.extend(["--op-option", assignment])
    for assignment in pta_options or []:
        command.extend(["--pta-option", assignment])
    command.append(str(filepath))

    started = time.monotonic()
    try:
        result = subprocess.run(
            command,
            timeout=maxtime,
            capture_output=True,
            text=True,
            shell=False,
        )
    except subprocess.TimeoutExpired as error:
        artifacts.update({
            "command": command,
            "stdout": error.stdout or "",
            "stderr": error.stderr or "",
            "returncode": None,
            "failure_kind": "timeout",
            "elapsed_seconds": time.monotonic() - started,
        })
        return TIMEOUT, artifacts
    except OSError as error:
        artifacts.update({
            "command": command,
            "stdout": "",
            "stderr": str(error),
            "returncode": None,
            "failure_kind": "infrastructure",
            "elapsed_seconds": time.monotonic() - started,
        })
        return FAILURE, artifacts

    artifacts.update({
        "command": command,
        "stdout": result.stdout,
        "stderr": result.stderr,
        "returncode": result.returncode,
        "elapsed_seconds": time.monotonic() - started,
    })
    valid, evidence = validate_pta_artifacts(artifacts) if result.returncode == 0 else (False, {})
    artifacts["validation"] = evidence
    if result.returncode == 0 and not valid:
        artifacts["failure_kind"] = "invalid_output"
    elif result.returncode != 0:
        artifacts["failure_kind"] = "convergence" if "did not converge" in str(result.stderr).lower() or "analysis failed" in str(result.stderr).lower() else "simulator_error"
    succeeded = result.returncode == 0 and valid
    return (SUCCESS if succeeded else FAILURE), artifacts
