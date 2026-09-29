#!/usr/bin/env python3
import argparse
import math
import re
import sys
from pathlib import Path


NUMBER_RE = re.compile(
    r"^[+-]?(?:(?:\d+(?:\.\d*)?)|(?:\.\d+))(?:[eE][+-]?\d+)?$"
)
INDEX_RE = re.compile(r"^[+-]?\d+$")


def normalize_variable(token):
    token = token.strip().strip("\f").lower()
    if token == "time":
        return token
    if token.startswith("v(") and token.endswith(")"):
        body = ",".join(part.strip() for part in token[2:-1].split(","))
        return f"v({body})"
    if token.startswith("i(") and token.endswith(")"):
        return f"i({token[2:-1].strip()})"
    if token.endswith("#branch"):
        return f"i({token[:-7].strip()})"
    return token


def parse_number(token):
    token = token.strip()
    if not NUMBER_RE.match(token):
        raise ValueError(f"not a finite SPICE number: {token}")
    value = float(token)
    if not math.isfinite(value):
        raise ValueError(f"non-finite SPICE number: {token}")
    return value


def parse_listing(path, analysis):
    points = {}
    header = None
    saw_data = False
    active_analysis = None

    for raw_line in path.read_text(errors="replace").splitlines():
        line = raw_line.strip().strip("\f")
        if not line:
            continue

        lower_line = line.lower()
        if lower_line.startswith("operating point"):
            active_analysis = "op"
            header = None
            saw_data = False
            continue
        if lower_line.startswith("transient analysis"):
            active_analysis = "tran"
            header = None
            saw_data = False
            continue

        parts = line.split()
        if parts and parts[0].lower() == "index":
            if active_analysis != analysis:
                header = None
                saw_data = False
                continue
            header = [normalize_variable(part) for part in parts[1:]]
            saw_data = False
            continue

        if header is None:
            continue

        if set(line) == {"-"}:
            continue

        if parts and INDEX_RE.match(parts[0]):
            values = parts[1:]
            if len(values) < len(header):
                raise ValueError(
                    f"row {parts[0]} has {len(values)} values for "
                    f"{len(header)} columns"
                )

            point_index = int(parts[0])
            point = points.setdefault(point_index, {})
            for name, token in zip(header, values):
                value = parse_number(token)
                if name in point and point[name] != value:
                    raise ValueError(
                        f"point {point_index} repeats {name} with different values"
                    )
                point[name] = value
            saw_data = True
            continue

        if saw_data:
            header = None

    if not points:
        raise ValueError(f"no {analysis.upper()} SPICE Index table found")
    return points


def close_enough(actual, expected, atol, rtol):
    difference = abs(actual - expected)
    limit = atol + rtol * abs(expected)
    return difference <= limit, difference, limit


def compare_case(
    standard_path,
    actual_path,
    error_path,
    analysis,
    atol,
    rtol,
    time_atol,
):
    failures = []
    comparisons = []

    if not actual_path.exists():
        return [f"missing actual output: {actual_path}"], comparisons
    if error_path.exists() and error_path.stat().st_size:
        failures.append(f"stderr is not empty: {error_path}")

    try:
        expected_points = parse_listing(standard_path, analysis)
    except Exception as exc:
        return [f"cannot parse standard output: {exc}"], comparisons

    try:
        actual_points = parse_listing(actual_path, analysis)
    except Exception as exc:
        return [f"cannot parse actual output: {exc}"], comparisons

    expected_indices = sorted(expected_points)
    actual_indices = sorted(actual_points)
    if actual_indices != expected_indices:
        failures.append(
            f"point indices differ: expected {expected_indices}, actual {actual_indices}"
        )

    for point_index in expected_indices:
        if point_index not in actual_points:
            continue
        expected = expected_points[point_index]
        actual = actual_points[point_index]

        expected_variables = set(expected)
        actual_variables = set(actual)
        if actual_variables != expected_variables:
            missing = sorted(expected_variables - actual_variables)
            extra = sorted(actual_variables - expected_variables)
            if missing:
                failures.append(
                    f"point {point_index}: missing variables {missing}"
                )
            if extra:
                failures.append(
                    f"point {point_index}: unexpected variables {extra}"
                )

        for variable in sorted(expected):
            if variable not in actual:
                failures.append(f"point {point_index}: missing {variable}")
                continue

            variable_atol = time_atol if variable == "time" else atol
            variable_rtol = 0.0 if variable == "time" else rtol
            ok, difference, limit = close_enough(
                actual[variable],
                expected[variable],
                variable_atol,
                variable_rtol,
            )
            comparison = {
                "point": point_index,
                "variable": variable,
                "expected": expected[variable],
                "actual": actual[variable],
                "difference": difference,
                "limit": limit,
            }
            comparisons.append(comparison)
            if not ok:
                failures.append(
                    f"point {point_index} {variable}: expected "
                    f"{expected[variable]:.10e}, actual {actual[variable]:.10e}, "
                    f"diff {difference:.3e}, allowed +/-{limit:.3e}"
                )

    return failures, comparisons


def worst_comparison(comparisons):
    if not comparisons:
        return None
    return max(
        comparisons,
        key=lambda item: (
            item["difference"] / item["limit"]
            if item["limit"] > 0.0
            else item["difference"]
        ),
    )


def main():
    parser = argparse.ArgumentParser(
        description="Compare SPICE-style listing tables with numeric tolerances."
    )
    parser.add_argument("--analysis", choices=("op", "tran"), required=True)
    parser.add_argument("--standard", required=True)
    parser.add_argument("--actual", required=True)
    parser.add_argument("--atol", type=float, required=True)
    parser.add_argument("--rtol", type=float, required=True)
    parser.add_argument("--time-atol", type=float, default=1.0e-15)
    parser.add_argument(
        "--include",
        action="append",
        default=[],
        metavar="STEM",
        help="compare only this case stem (repeatable)",
    )
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    standard_dir = Path(args.standard)
    actual_dir = Path(args.actual)
    if not standard_dir.is_dir():
        print(f"error: standard directory not found: {standard_dir}", file=sys.stderr)
        return 2
    if not actual_dir.is_dir():
        print(f"error: actual directory not found: {actual_dir}", file=sys.stderr)
        return 2

    standards = sorted(standard_dir.glob("*.out"))
    if args.include:
        included = set(args.include)
        standards = [path for path in standards if path.stem in included]
    if not standards:
        print(f"error: no .out standards in {standard_dir}", file=sys.stderr)
        return 2

    failed_cases = 0
    total_values = 0
    print(f"Comparing {args.analysis.upper()} SPICE listings")
    print(f"Tolerance: atol={args.atol:g}, rtol={args.rtol:g}")

    for standard_path in standards:
        case_dir = actual_dir / standard_path.stem
        if case_dir.is_dir():
            actual_path = case_dir / standard_path.name
            error_path = case_dir / f"{standard_path.stem}.err"
        else:
            # Keep flat hand-built fixtures usable; simulator regression
            # output itself always takes the nested branch above.
            actual_path = actual_dir / standard_path.name
            error_path = actual_dir / f"{standard_path.stem}.err"
        failures, comparisons = compare_case(
            standard_path,
            actual_path,
            error_path,
            args.analysis,
            args.atol,
            args.rtol,
            args.time_atol,
        )
        total_values += len(comparisons)

        if failures:
            failed_cases += 1
            print(f"FAIL {standard_path.stem}")
            for failure in failures:
                print(f"  {failure}")
        else:
            worst = worst_comparison(comparisons)
            detail = ""
            if worst is not None and worst["limit"] > 0.0:
                ratio = worst["difference"] / worst["limit"]
                detail = (
                    f", worst point {worst['point']} {worst['variable']} "
                    f"uses {ratio:.1%} of tolerance"
                )
            print(f"PASS {standard_path.stem} ({len(comparisons)} values{detail})")

        if args.verbose:
            for item in comparisons:
                print(
                    f"  point {item['point']} {item['variable']}: "
                    f"expected {item['expected']:.10e}, "
                    f"actual {item['actual']:.10e}, "
                    f"diff {item['difference']:.3e}, "
                    f"allowed +/-{item['limit']:.3e}"
                )

    passed_cases = len(standards) - failed_cases
    print(
        f"Summary: {passed_cases}/{len(standards)} cases passed, "
        f"{total_values} values checked"
    )
    return 1 if failed_cases else 0


if __name__ == "__main__":
    sys.exit(main())
