#!/usr/bin/env python3
"""Execute one analysis suite and report wall-clock time for every netlist."""

import argparse
import subprocess
import sys
import time
from pathlib import Path


REPORT_HEADER = "SPICE Solver Report"


def validate_report(path, analysis, succeeded):
    if not path.is_file():
        return f"missing solver report: {path}"

    text = path.read_text(errors="replace")
    expected_status = "Status: succeeded" if succeeded else "Status: failed"
    if REPORT_HEADER not in text:
        return f"solver report has no {REPORT_HEADER!r} header: {path}"
    if expected_status not in text:
        return f"solver report has no {expected_status!r} status: {path}"

    analysis_label = "Transient analysis:" if analysis == "tran" else "Method path:"
    if analysis_label not in text:
        return f"solver report has no {analysis_label!r} section: {path}"
    return None


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Run SPICE regression cases with per-case timing."
    )
    parser.add_argument("--analysis", choices=("op", "tran", "private"), required=True)
    parser.add_argument("--simulator", type=Path, required=True)
    parser.add_argument("--case-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument(
        "--include",
        action="append",
        default=[],
        metavar="STEM",
        help="run only this case stem (repeatable)",
    )
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    simulator = arguments.simulator.resolve()
    if arguments.analysis == "private":
        cases = sorted(
            path
            for path in arguments.case_dir.iterdir()
            if path.is_file() and path.suffix.lower() in {".cir", ".sp"}
        )
    else:
        cases = sorted(arguments.case_dir.glob("*.cir"))
    if arguments.include:
        included = set(arguments.include)
        cases = [case for case in cases if case.stem in included]
    if not cases:
        print(f"No {arguments.analysis} cases found in {arguments.case_dir}", file=sys.stderr)
        return 2

    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    failures = 0
    suite_start = time.perf_counter()

    for case in cases:
        stem = case.stem
        case_output = arguments.output_dir / stem
        listing = case_output / f"{stem}.out"
        raw = case_output / f"{stem}.raw"
        error = case_output / f"{stem}.err"
        report = case_output / f"{stem}.solve.txt"

        start = time.perf_counter()
        result = subprocess.run(
            [
                simulator,
                "-b",
                "--output-root",
                arguments.output_dir,
                case,
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        elapsed_ms = (time.perf_counter() - start) * 1_000.0

        artifact_failures = []
        for artifact in (listing, raw, error):
            if not artifact.is_file():
                artifact_failures.append(f"missing output artifact: {artifact}")
        report_failure = validate_report(
            report,
            arguments.analysis,
            result.returncode == 0,
        )
        if report_failure:
            artifact_failures.append(report_failure)
        if result.stdout:
            artifact_failures.append("batch mode unexpectedly wrote to stdout")

        passed = result.returncode == 0 and not artifact_failures
        if passed:
            outcome = "PASS"
        elif result.returncode != 0:
            outcome = f"FAIL ({result.returncode})"
        else:
            outcome = "FAIL (artifacts)"
        print(
            f"TIME {arguments.analysis:<4} {stem:<48} "
            f"{elapsed_ms:10.3f} ms  {outcome}"
        )
        for failure in artifact_failures:
            print(f"  {failure}", file=sys.stderr)
        if result.stderr:
            print(result.stderr, file=sys.stderr, end="")
        failures += not passed

    suite_elapsed_ms = (time.perf_counter() - suite_start) * 1_000.0
    print(
        f"TIME {arguments.analysis:<4} suite ({len(cases)} cases) "
        f"{suite_elapsed_ms:.3f} ms"
    )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
