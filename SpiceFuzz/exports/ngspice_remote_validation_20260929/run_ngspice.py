#!/usr/bin/env python3
"""Run stock ngspice over the exported SpiceFuzz/PTA validation corpus."""

import argparse
import csv
import json
import subprocess
import time
from collections import Counter
from pathlib import Path


INVALID_MARKERS = (
    "error on line",
    "unknown parameter",
    "unknown device",
    "unknown subckt",
    "model definition not found",
    "could not find a model",
    "undefined parameter",
    "syntax error",
    "fatal error",
    "singular matrix",
    "no dc path",
)
CONVERGENCE_MARKERS = (
    "failed to converge",
    "no convergence",
    "dc solution failed",
    "operating point failed",
    "timestep too small",
    "iteration limit reached",
)


def text(value):
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode(errors="replace")
    return str(value)


def attempted_stages(diagnostic):
    lower = diagnostic.lower()
    stages = ["nr"]
    if "gmin stepping" in lower or "gmin-step" in lower:
        stages.append("gmin")
    if "source stepping" in lower or "source-step" in lower:
        stages.append("source_stepping")
    return stages


def final_stage(diagnostic):
    lower = diagnostic.lower()
    source_position = max(
        lower.rfind("source stepping"),
        lower.rfind("source-step"),
    )
    gmin_position = max(
        lower.rfind("gmin stepping"),
        lower.rfind("gmin-step"),
    )
    if source_position >= 0 and source_position > gmin_position:
        return "source_stepping"
    if gmin_position >= 0:
        return "gmin"
    return "nr"


def failure_status(diagnostic):
    lower = diagnostic.lower()
    if any(marker in lower for marker in INVALID_MARKERS):
        return "invalid"
    if any(marker in lower for marker in CONVERGENCE_MARKERS):
        return "convergence_failure"
    return "simulator_error"


def run_case(ngspice, netlist, netlist_root, output_root, timeout):
    relative = netlist.relative_to(netlist_root)
    case_directory = output_root / relative.with_suffix("")
    case_directory.mkdir(parents=True, exist_ok=True)
    raw_path = case_directory / (netlist.stem + ".raw")
    log_path = case_directory / (netlist.stem + ".log")
    stdout_path = case_directory / "stdout.txt"
    stderr_path = case_directory / "stderr.txt"
    for stale_path in (raw_path, log_path, stdout_path, stderr_path):
        if stale_path.exists():
            stale_path.unlink()

    command = [
        str(ngspice),
        "-b",
        "-r",
        str(raw_path.resolve()),
        "-o",
        str(log_path.resolve()),
        str(netlist.resolve()),
    ]
    started = time.monotonic()
    try:
        completed = subprocess.run(
            command,
            cwd=case_directory,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
        elapsed = time.monotonic() - started
        stdout = completed.stdout or ""
        stderr = completed.stderr or ""
        stdout_path.write_text(stdout, encoding="utf-8", errors="replace")
        stderr_path.write_text(stderr, encoding="utf-8", errors="replace")
        log = (
            log_path.read_text(encoding="utf-8", errors="replace")
            if log_path.is_file()
            else ""
        )
        diagnostic = "\n".join((log, stdout, stderr))
        succeeded = (
            completed.returncode == 0
            and raw_path.is_file()
            and raw_path.stat().st_size > 0
        )
        status = "converged" if succeeded else failure_status(diagnostic)
        returncode = completed.returncode
    except subprocess.TimeoutExpired as error:
        elapsed = time.monotonic() - started
        stdout = text(error.stdout)
        stderr = text(error.stderr)
        stdout_path.write_text(stdout, encoding="utf-8", errors="replace")
        stderr_path.write_text(stderr, encoding="utf-8", errors="replace")
        log = (
            log_path.read_text(encoding="utf-8", errors="replace")
            if log_path.is_file()
            else ""
        )
        diagnostic = "\n".join((log, stdout, stderr))
        status = "timeout"
        returncode = None

    return {
        "netlist": relative.as_posix(),
        "tier": relative.parts[0],
        "status": status,
        "stage": final_stage(diagnostic),
        "attempted_stages": attempted_stages(diagnostic),
        "returncode": returncode,
        "elapsed_seconds": round(elapsed, 6),
        "raw_exists": raw_path.is_file() and raw_path.stat().st_size > 0,
        "case_directory": str(case_directory.resolve()),
        "command": command,
    }


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Validate exported SpiceFuzz/PTA netlists with stock ngspice"
    )
    parser.add_argument("--ngspice", required=True, type=Path)
    parser.add_argument("--netlist-root", type=Path)
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--timeout", type=float, default=60.0)
    return parser.parse_args()


def main():
    args = parse_arguments()
    package_root = Path(__file__).resolve().parent
    netlist_root = (args.netlist_root or package_root / "netlists").resolve()
    output_root = (args.output_root or package_root / "ngspice_results").resolve()
    ngspice = args.ngspice.expanduser().resolve()
    if not ngspice.is_file():
        raise FileNotFoundError(f"ngspice executable not found: {ngspice}")
    if not netlist_root.is_dir():
        raise FileNotFoundError(f"netlist directory not found: {netlist_root}")
    if args.timeout <= 0:
        raise ValueError("timeout must be positive")

    netlists = sorted(netlist_root.rglob("*.cir"))
    if not netlists:
        raise RuntimeError(f"no .cir netlists found under {netlist_root}")
    output_root.mkdir(parents=True, exist_ok=True)

    results = []
    for index, netlist in enumerate(netlists, start=1):
        result = run_case(
            ngspice,
            netlist,
            netlist_root,
            output_root,
            args.timeout,
        )
        results.append(result)
        print(
            f"[{index:03d}/{len(netlists):03d}] "
            f"{result['status']:19s} {result['netlist']}"
        )

    summary_json = output_root / "summary.json"
    summary_json.write_text(
        json.dumps(results, indent=2, ensure_ascii=False),
        encoding="utf-8",
    )
    fields = (
        "netlist",
        "tier",
        "status",
        "stage",
        "attempted_stages",
        "returncode",
        "elapsed_seconds",
        "raw_exists",
        "case_directory",
    )
    with (output_root / "summary.tsv").open(
        "w",
        encoding="utf-8",
        newline="",
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t")
        writer.writeheader()
        for result in results:
            row = {field: result[field] for field in fields}
            row["attempted_stages"] = ",".join(row["attempted_stages"])
            writer.writerow(row)

    counts = Counter(result["status"] for result in results)
    print("\nSummary")
    for status, count in sorted(counts.items()):
        print(f"  {status}: {count}")
    print(f"  total: {len(results)}")
    print(f"Results: {summary_json}")


if __name__ == "__main__":
    main()
