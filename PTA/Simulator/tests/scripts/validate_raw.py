#!/usr/bin/env python3
import argparse
import math
import sys
from pathlib import Path

from compare_spice import normalize_variable, parse_listing


def read_value(token, path):
    try:
        value = float(token)
    except ValueError as exc:
        raise ValueError(f"{path}: invalid raw value {token}") from exc
    if not math.isfinite(value):
        raise ValueError(f"{path}: non-finite raw value {token}")
    return value


def parse_rawfile(path):
    lines = [
        line.strip()
        for line in path.read_text(errors="replace").splitlines()
        if line.strip()
    ]
    position = 0
    plots = []

    while position < len(lines):
        if not lines[position].startswith("Title:"):
            raise ValueError(f"{path}: expected Title at line {position + 1}")
        position += 1

        headers = {}
        while position < len(lines) and lines[position] != "Variables:":
            line = lines[position]
            if ":" not in line:
                raise ValueError(f"{path}: malformed raw header: {line}")
            key, value = line.split(":", 1)
            headers[key.strip()] = value.strip()
            position += 1

        required = {"Date", "Plotname", "Flags", "No. Variables", "No. Points"}
        missing = required - headers.keys()
        if missing:
            raise ValueError(f"{path}: missing raw headers: {sorted(missing)}")
        if not headers["Date"]:
            raise ValueError(f"{path}: raw Date header is empty")
        if not headers["Plotname"]:
            raise ValueError(f"{path}: raw Plotname header is empty")
        if headers["Flags"].lower() != "real":
            raise ValueError(f"{path}: only real raw data is expected")

        variable_count = int(headers["No. Variables"])
        point_count = int(headers["No. Points"])
        if variable_count < 0 or point_count < 0:
            raise ValueError(f"{path}: raw counts must be non-negative")
        position += 1

        variables = []
        for expected_index in range(variable_count):
            if position >= len(lines):
                raise ValueError(f"{path}: truncated Variables section")
            parts = lines[position].split()
            if len(parts) != 3 or int(parts[0]) != expected_index:
                raise ValueError(f"{path}: malformed variable row: {lines[position]}")
            variables.append(
                (normalize_variable(parts[1]), parts[2].lower())
            )
            position += 1
        variable_names = [name for name, _ in variables]
        if len(set(variable_names)) != len(variable_names):
            raise ValueError(f"{path}: duplicate raw variable name")

        if position >= len(lines) or lines[position] != "Values:":
            raise ValueError(f"{path}: missing Values section")
        position += 1

        points = []
        for expected_point in range(point_count):
            if position >= len(lines):
                raise ValueError(f"{path}: truncated Values section")
            first = lines[position].split()
            position += 1
            if not first or int(first[0]) != expected_point:
                raise ValueError(f"{path}: malformed point row: {' '.join(first)}")

            values = []
            if variable_count:
                if len(first) != 2:
                    raise ValueError(f"{path}: point {expected_point} has no first value")
                values.append(read_value(first[1], path))
                for _ in range(variable_count - 1):
                    if position >= len(lines):
                        raise ValueError(f"{path}: truncated point {expected_point}")
                    parts = lines[position].split()
                    position += 1
                    if len(parts) != 1:
                        raise ValueError(
                            f"{path}: malformed continuation value: {' '.join(parts)}"
                        )
                    values.append(read_value(parts[0], path))
            elif len(first) != 1:
                raise ValueError(f"{path}: zero-variable point contains data")
            points.append(values)

        if headers["Plotname"] == "Transient Analysis":
            if not variables or variables[0] != ("time", "time"):
                raise ValueError(f"{path}: transient raw data must start with time")
            times = [point[0] for point in points]
            if any(current < previous for previous, current in zip(times, times[1:])):
                raise ValueError(f"{path}: transient time values are not monotonic")

        plots.append(
            {
                "plotname": headers["Plotname"],
                "variables": variables,
                "points": points,
            }
        )

    if not plots:
        raise ValueError(f"{path}: no raw plots found")
    return plots


def raw_voltage_component(variable_indices, values, node):
    node = node.strip().lower()
    if node in ("0", "gnd"):
        return 0.0
    variable = f"v({node})"
    if variable not in variable_indices:
        raise ValueError(f"rawfile is missing {variable}")
    return values[variable_indices[variable]]


def raw_value(variable_indices, values, variable):
    if variable in variable_indices:
        return values[variable_indices[variable]]

    if variable.startswith("v(") and variable.endswith(")"):
        nodes = [part.strip() for part in variable[2:-1].split(",")]
        if len(nodes) == 1:
            return raw_voltage_component(variable_indices, values, nodes[0])
        if len(nodes) == 2:
            return (
                raw_voltage_component(variable_indices, values, nodes[0])
                - raw_voltage_component(variable_indices, values, nodes[1])
            )

    raise ValueError(f"rawfile cannot provide listing variable {variable}")


def validate_against_listing(raw_path, plots, listing_dir, analysis):
    # Unified simulator output keeps every artifact beside the matching rawfile
    # in <output-root>/<netlist-stem>/.  Retain the flat fallback so this
    # standalone validator can still check hand-built fixtures.
    listing_path = raw_path.with_suffix(".out")
    if not listing_path.is_file():
        nested_path = listing_dir / raw_path.stem / f"{raw_path.stem}.out"
        flat_path = listing_dir / f"{raw_path.stem}.out"
        listing_path = nested_path if nested_path.is_file() else flat_path
    if not listing_path.is_file():
        raise ValueError(f"missing matching listing: {listing_path}")

    plot_name = "Operating Point" if analysis == "op" else "Transient Analysis"
    matching_plots = [plot for plot in plots if plot["plotname"] == plot_name]
    if len(matching_plots) != 1:
        raise ValueError(
            f"{raw_path}: expected exactly one {plot_name} raw plot, "
            f"found {len(matching_plots)}"
        )

    plot = matching_plots[0]
    listing_points = parse_listing(listing_path, analysis)
    expected_indices = list(range(len(plot["points"])))
    if sorted(listing_points) != expected_indices:
        raise ValueError(
            f"{raw_path}: listing/raw point indices differ: "
            f"listing={sorted(listing_points)}, raw={expected_indices}"
        )

    variable_indices = {
        name: index for index, (name, _) in enumerate(plot["variables"])
    }
    for point_index, listing_values in listing_points.items():
        values = plot["points"][point_index]
        for variable, listing_value in listing_values.items():
            raw_result = raw_value(variable_indices, values, variable)
            difference = abs(raw_result - listing_value)
            limit = 5.0e-10 + 1.0e-9 * abs(listing_value)
            if difference > limit:
                raise ValueError(
                    f"{raw_path}: point {point_index} {variable} differs "
                    f"between listing ({listing_value:.15e}) and rawfile "
                    f"({raw_result:.15e})"
                )


def main():
    parser = argparse.ArgumentParser(
        description="Validate SPICE ASCII rawfiles and optionally cross-check listings."
    )
    parser.add_argument("--analysis", choices=("op", "tran"))
    parser.add_argument("--listing-dir", type=Path)
    parser.add_argument("rawfiles", nargs="+", type=Path)
    args = parser.parse_args()

    if (args.analysis is None) != (args.listing_dir is None):
        parser.error("--analysis and --listing-dir must be used together")

    failed = 0
    plot_count = 0
    for path in args.rawfiles:
        try:
            plots = parse_rawfile(path)
            if args.listing_dir is not None:
                validate_against_listing(
                    path,
                    plots,
                    args.listing_dir,
                    args.analysis,
                )
        except Exception as exc:
            failed += 1
            print(f"FAIL {path}: {exc}")
            continue
        plot_count += len(plots)
        print(f"PASS {path} ({len(plots)} plot{'s' if len(plots) != 1 else ''})")

    print(
        f"Rawfile summary: {len(args.rawfiles) - failed}/{len(args.rawfiles)} "
        f"files passed, {plot_count} plots checked"
    )
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
