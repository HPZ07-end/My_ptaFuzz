import subprocess
from pathlib import Path


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
    """Require an OP-compatible deck and reject non-OP analysis cards."""
    forbidden = {".tran", ".pstran", ".dc", ".ac", ".pz", ".noise"}
    directives = set()
    for raw_line in Path(filepath).read_text(errors="replace").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("*"):
            continue
        for marker in (";", "//", "$"):
            index = line.find(marker)
            if index != -1:
                line = line[:index].strip()
        if line.startswith("."):
            directives.add(line.split()[0].lower())
    unsupported = sorted(directives & forbidden)
    if unsupported:
        return False, "PTA fuzzing requires DC operating point; found " + ", ".join(unsupported)
    return True, ""


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
        })
        return TIMEOUT, artifacts
    except OSError as error:
        artifacts.update({
            "command": command,
            "stdout": "",
            "stderr": str(error),
            "returncode": None,
        })
        return FAILURE, artifacts

    artifacts.update({
        "command": command,
        "stdout": result.stdout,
        "stderr": result.stderr,
        "returncode": result.returncode,
    })
    succeeded = result.returncode == 0 and artifacts["raw"].is_file()
    return (SUCCESS if succeeded else FAILURE), artifacts
