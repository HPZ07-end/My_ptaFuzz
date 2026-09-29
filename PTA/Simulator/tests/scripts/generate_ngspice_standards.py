#!/usr/bin/env python3
"""Generate independent OP/TRAN references with ngspice.

The project simulator is never used for reference values.  Transient ngspice
data is linearly resampled onto this project's fixed .tran output grid.  For
UIC decks, the explicit t=0 row follows this project's documented all-zero
initial-sample convention; every t>0 value comes from ngspice.
"""

import argparse
import bisect
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from compare_spice import normalize_variable  # noqa: E402
from validate_raw import parse_rawfile  # noqa: E402


NUMBER_RE = re.compile(
    r"^([+-]?(?:(?:[0-9]+(?:\.[0-9]*)?)|(?:\.[0-9]+))"
    r"(?:[eE][+-]?[0-9]+)?)([A-Za-z]*)$"
)
PRINT_RE = re.compile(r"(?i)([vi])\s*\(\s*([^)]*?)\s*\)")
EPSILON = sys.float_info.epsilon

# TRAN references must describe a numerically converged solution rather than
# the result of ngspice's default, output-interval-dependent stepping policy.
# Keep this configuration in the generator so the committed baselines are
# reproducible without modifying the source netlists.
TRAN_REFERENCE_MAX_STEP_DIVISOR = 2000
MOS3_TRAN_REFERENCE_MAX_STEP_DIVISOR = 20
TRAN_REFERENCE_OPTIONS = (
    ".options reltol=1e-8 vntol=1e-10 abstol=1e-12 trtol=1"
)


def spice_number(text):
    match = NUMBER_RE.fullmatch(text.strip())
    if not match:
        raise ValueError(f"invalid SPICE number: {text}")
    value = float(match.group(1))
    suffix = match.group(2).lower()
    if suffix.startswith("meg"):
        multiplier = 1.0e6
    elif suffix.startswith("mil"):
        multiplier = 25.4e-6
    else:
        multiplier = {
            "a": 1.0e-18,
            "f": 1.0e-15,
            "p": 1.0e-12,
            "n": 1.0e-9,
            "u": 1.0e-6,
            "m": 1.0e-3,
            "k": 1.0e3,
            "g": 1.0e9,
            "t": 1.0e12,
        }.get(suffix[:1], 1.0)
    result = value * multiplier
    if not math.isfinite(result):
        raise ValueError(f"non-finite SPICE number: {text}")
    return result


def logical_lines(path):
    physical = path.read_text(errors="strict").splitlines()
    if not physical:
        raise ValueError(f"empty netlist: {path}")
    result = []
    for raw in physical[1:]:
        line = raw.strip()
        if not line or line.startswith("*"):
            continue
        if line.startswith("+"):
            if not result:
                raise ValueError(f"orphan continuation in {path}")
            result[-1] += " " + line[1:].strip()
        else:
            result.append(line)
    return physical[0].strip(), result


def canonical_node(name):
    name = name.strip().lower()
    return "0" if name == "gnd" else name


def print_variables(lines, analysis):
    variables = []
    seen = set()
    prefix = f".print {analysis}"
    for line in lines:
        if not line.lower().startswith(prefix):
            continue
        for function, body in PRINT_RE.findall(line[len(prefix) :]):
            arguments = [part.strip() for part in body.split(",")]
            if function.lower() == "v":
                if len(arguments) not in (1, 2) or not arguments[0]:
                    raise ValueError(f"unsupported print expression: {function}({body})")
                positive = canonical_node(arguments[0])
                negative = canonical_node(arguments[1]) if len(arguments) == 2 else "0"
                raw_name = (
                    f"v({positive})"
                    if negative == "0"
                    else f"v({positive},{negative})"
                )
                label = raw_name
            else:
                if len(arguments) != 1 or not arguments[0]:
                    raise ValueError(f"unsupported print expression: {function}({body})")
                device = arguments[0].lower()
                raw_name = f"i({device})"
                label = f"{device}#branch"
            raw_name = normalize_variable(raw_name)
            if raw_name not in seen:
                seen.add(raw_name)
                variables.append((raw_name, label))
    if not variables:
        raise ValueError(f"missing .print {analysis}")
    return variables


def tran_config(lines):
    directives = [line for line in lines if line.lower().startswith(".tran ")]
    if len(directives) != 1:
        raise ValueError("expected exactly one .tran directive")
    tokens = directives[0].split()
    if len(tokens) < 3:
        raise ValueError(".tran requires TSTEP and TSTOP")
    output_interval = spice_number(tokens[1])
    stop_time = spice_number(tokens[2])
    optional_times = []
    use_initial_conditions = False
    for token in tokens[3:]:
        if token.lower() == "uic":
            use_initial_conditions = True
        else:
            optional_times.append(spice_number(token))
    if len(optional_times) > 2:
        raise ValueError("too many .tran time arguments")
    output_start = optional_times[0] if optional_times else 0.0
    return output_interval, stop_time, output_start, use_initial_conditions


def high_accuracy_tran_netlist(netlist, work_root, max_step_divisor):
    """Return a temporary TRAN deck with the reference accuracy policy."""
    title, lines = logical_lines(netlist)
    interval, stop_time, output_start, use_initial_conditions = tran_config(lines)
    maximum_step = interval / max_step_divisor

    if not math.isfinite(maximum_step) or maximum_step <= 0.0:
        raise ValueError(f"invalid high-accuracy maximum step for {netlist}")

    rewritten = []
    found_tran = False
    found_end = False
    for line in lines:
        if line.lower().startswith(".tran "):
            if found_tran:
                raise ValueError(f"multiple .tran directives in {netlist}")
            found_tran = True
            directive = (
                f".tran {interval:.15e} "
                f"{stop_time:.15e} "
                f"{output_start:.15e} {maximum_step:.15e}"
            )
            if use_initial_conditions:
                directive += " UIC"
            rewritten.append(directive)
        elif line.lower() == ".end":
            if found_end:
                raise ValueError(f"multiple .end directives in {netlist}")
            found_end = True
            rewritten.extend((TRAN_REFERENCE_OPTIONS, line))
        else:
            rewritten.append(line)

    if not found_tran or not found_end:
        raise ValueError(f"missing .tran or .end in {netlist}")

    destination = work_root / f"reference-{netlist.name}"
    destination.write_text(title + "\n" + "\n".join(rewritten) + "\n")
    return destination


def same_time(left, right):
    scale = max(abs(left), abs(right))
    return abs(left - right) <= 64.0 * EPSILON * scale


def output_times(output_interval, stop_time, output_start):
    times = []
    current = output_start
    if current == 0.0:
        times.append(0.0)
        current += output_interval
    while current < stop_time and not same_time(current, stop_time):
        times.append(current)
        following = current + output_interval
        if following <= current:
            raise ValueError("transient output time stopped advancing")
        current = following
    if not times or not same_time(times[-1], stop_time):
        times.append(stop_time)
    return times


def voltage_component(variable_indices, values, node):
    node = canonical_node(node)
    if node == "0":
        return 0.0
    name = f"v({node})"
    if name not in variable_indices:
        raise ValueError(f"ngspice rawfile is missing {name}")
    return values[variable_indices[name]]


def expression_value(variable_indices, values, expression):
    if expression in variable_indices:
        return values[variable_indices[expression]]
    if expression.startswith("v(") and expression.endswith(")"):
        nodes = [part.strip() for part in expression[2:-1].split(",")]
        if len(nodes) == 1:
            return voltage_component(variable_indices, values, nodes[0])
        if len(nodes) == 2:
            return (
                voltage_component(variable_indices, values, nodes[0])
                - voltage_component(variable_indices, values, nodes[1])
            )
    raise ValueError(f"ngspice rawfile cannot provide {expression}")


def resampled_value(raw_times, raw_values, target_time, value_at):
    position = bisect.bisect_left(raw_times, target_time)
    if position < len(raw_times) and same_time(raw_times[position], target_time):
        return value_at(raw_values[position])
    if position == 0:
        return value_at(raw_values[0])
    if position >= len(raw_times):
        if same_time(raw_times[-1], target_time):
            return value_at(raw_values[-1])
        raise ValueError(
            f"ngspice stopped at {raw_times[-1]:.15e} before {target_time:.15e}"
        )
    left_time = raw_times[position - 1]
    right_time = raw_times[position]
    fraction = (target_time - left_time) / (right_time - left_time)
    left = value_at(raw_values[position - 1])
    right = value_at(raw_values[position])
    return left + fraction * (right - left)


def write_listing(path, title, analysis_name, variables, rows, include_time):
    labels = [label for _, label in variables]
    header = ["Index"] + (["time"] if include_time else []) + labels
    width = max(80, 8 + 20 * (len(header) - 1))
    lines = [
        f"Circuit: {title}",
        "",
        analysis_name,
        f"No. of Data Rows : {len(rows)}",
        "",
        "-" * width,
        "   ".join(header),
        "-" * width,
    ]
    for index, (time, values) in enumerate(rows):
        fields = [f"{index:8d}"]
        if include_time:
            fields.append(f"{time:.15e}")
        fields.extend(f"{value:.15e}" for value in values)
        lines.append("   ".join(fields))
    path.write_text("\n".join(lines) + "\n")


def run_ngspice(ngspice, netlist, raw_path, log_path):
    environment = os.environ.copy()
    environment["SPICE_ASCIIRAWFILE"] = "1"
    result = subprocess.run(
        [str(ngspice), "-b", "-o", str(log_path), "-r", str(raw_path), str(netlist)],
        cwd=netlist.parent,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=180,
    )
    log = log_path.read_text(errors="replace") if log_path.exists() else ""
    if result.returncode != 0 or not raw_path.is_file():
        raise RuntimeError(
            f"ngspice failed for {netlist}\n{result.stdout}{result.stderr}{log}"
        )
    return parse_rawfile(raw_path)


def generate_case(
    ngspice,
    netlist,
    analysis,
    staging_root,
    work_root,
    tran_reference_max_step_divisor,
):
    title, lines = logical_lines(netlist)
    variables = print_variables(lines, analysis)
    raw_path = work_root / f"{analysis}-{netlist.stem}.raw"
    log_path = work_root / f"{analysis}-{netlist.stem}.log"
    simulation_netlist = (
        high_accuracy_tran_netlist(
            netlist,
            work_root,
            tran_reference_max_step_divisor,
        )
        if analysis == "tran"
        else netlist
    )
    plots = run_ngspice(ngspice, simulation_netlist, raw_path, log_path)
    plot_name = "Operating Point" if analysis == "op" else "Transient Analysis"
    candidates = [plot for plot in plots if plot["plotname"] == plot_name]
    if len(candidates) != 1:
        raise ValueError(
            f"{netlist}: expected one {plot_name} plot, found {len(candidates)}"
        )
    plot = candidates[0]
    variable_indices = {
        normalize_variable(name): index
        for index, (name, _) in enumerate(plot["variables"])
    }

    if analysis == "op":
        if len(plot["points"]) != 1:
            raise ValueError(f"{netlist}: OP rawfile has {len(plot['points'])} points")
        point = plot["points"][0]
        values = [
            expression_value(variable_indices, point, expression)
            for expression, _ in variables
        ]
        rows = [(None, values)]
        include_time = False
    else:
        interval, stop, start, uic = tran_config(lines)
        times = output_times(interval, stop, start)
        if not plot["variables"] or plot["variables"][0] != ("time", "time"):
            raise ValueError(f"{netlist}: transient rawfile does not start with time")
        raw_times = [point[0] for point in plot["points"]]
        raw_values = plot["points"]
        rows = []
        for target in times:
            if uic and target == 0.0:
                values = [0.0] * len(variables)
            else:
                values = []
                for expression, _ in variables:
                    values.append(
                        resampled_value(
                            raw_times,
                            raw_values,
                            target,
                            lambda point, expression=expression: expression_value(
                                variable_indices, point, expression
                            ),
                        )
                    )
            rows.append((target, values))
        include_time = True

    output = staging_root / analysis / f"{netlist.stem}.out"
    output.parent.mkdir(parents=True, exist_ok=True)
    write_listing(output, title, plot_name, variables, rows, include_time)


def locate_ngspice(argument):
    if argument:
        candidate = Path(argument).expanduser().resolve()
        if candidate.is_file():
            return candidate
        raise FileNotFoundError(f"ngspice not found: {candidate}")
    found = shutil.which("ngspice")
    if found:
        return Path(found).resolve()
    homebrew = Path("/opt/homebrew/bin/ngspice")
    if homebrew.is_file():
        return homebrew.resolve()
    raise FileNotFoundError("ngspice was not found in PATH or /opt/homebrew/bin")


def suite_configuration(root, suite):
    if suite == "core":
        return {
            "case_root": root / "tests" / "cases",
            "reference_root": root / "tests" / "references",
            "expected_counts": {"op": 18, "tran": 18},
            "tran_reference_max_step_divisor": TRAN_REFERENCE_MAX_STEP_DIVISOR,
        }
    if suite == "mos3":
        return {
            "case_root": root / "tests" / "cases" / "mos3",
            "reference_root": root / "tests" / "references" / "mos3",
            "expected_counts": {"op": 4, "tran": 1},
            "tran_reference_max_step_divisor": MOS3_TRAN_REFERENCE_MAX_STEP_DIVISOR,
        }
    raise ValueError(f"unknown reference suite: {suite}")


def main():
    parser = argparse.ArgumentParser(
        description="Generate independent compact reference listings with ngspice."
    )
    parser.add_argument("--root", type=Path, default=SCRIPT_DIR.parents[1])
    parser.add_argument("--ngspice")
    parser.add_argument(
        "--suite",
        choices=("core", "mos3", "all"),
        default="core",
        help="reference suite to generate (default: core)",
    )
    args = parser.parse_args()

    root = args.root.resolve()
    ngspice = locate_ngspice(args.ngspice)
    suites = ("core", "mos3") if args.suite == "all" else (args.suite,)
    configurations = {
        suite: suite_configuration(root, suite)
        for suite in suites
    }
    all_cases = {}
    for suite, configuration in configurations.items():
        cases = {
            analysis: sorted(
                (configuration["case_root"] / analysis).glob("*.cir")
            )
            for analysis in ("op", "tran")
        }
        for analysis, netlists in cases.items():
            expected = configuration["expected_counts"][analysis]
            if len(netlists) != expected:
                raise RuntimeError(
                    f"{suite}: expected {expected} {analysis.upper()} netlists, "
                    f"found {len(netlists)}"
                )
        all_cases[suite] = cases

    with tempfile.TemporaryDirectory(prefix="ngspice-reference-", dir=root) as temp:
        temporary = Path(temp)
        staging = temporary / "references"
        work = temporary / "work"
        work.mkdir()
        completed = 0
        for suite, configuration in configurations.items():
            suite_staging = staging / suite
            for analysis in ("op", "tran"):
                for netlist in all_cases[suite][analysis]:
                    generate_case(
                        ngspice,
                        netlist,
                        analysis,
                        suite_staging,
                        work,
                        configuration["tran_reference_max_step_divisor"],
                    )
                    completed += 1
                    print(f"REFERENCE {suite.upper()} {analysis.upper()} {netlist.stem}")

            for analysis in ("op", "tran"):
                destination = configuration["reference_root"] / analysis
                destination.mkdir(parents=True, exist_ok=True)
                for old in destination.glob("*.out"):
                    old.unlink()
                for generated in sorted((suite_staging / analysis).glob("*.out")):
                    shutil.copy2(generated, destination / generated.name)

    print(
        f"Generated {completed} independent ngspice reference listings "
        f"for {', '.join(suites)}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
