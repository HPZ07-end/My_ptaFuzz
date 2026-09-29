#!/usr/bin/env python3
"""Parse every curated netlist without requiring a DC solution."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("simulator", type=Path)
    parser.add_argument("netlist_directory", type=Path)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument(
        "--recursive",
        action="store_true",
        help="parse netlists in descendant directories as well",
    )
    parser.add_argument(
        "--exclude-dir",
        action="append",
        type=Path,
        default=[],
        help="exclude this directory and all its descendants (repeatable)",
    )
    return parser.parse_args()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    arguments = parse_arguments()
    simulator = arguments.simulator.resolve()
    netlist_directory = arguments.netlist_directory.resolve()

    require(simulator.is_file(), f"simulator not found: {simulator}")
    require(netlist_directory.is_dir(), f"netlist directory not found: {netlist_directory}")
    require(arguments.timeout > 0.0, "timeout must be positive")
    excluded_directories = {
        (netlist_directory / path).resolve()
        if not path.is_absolute()
        else path.resolve()
        for path in arguments.exclude_dir
    }

    candidates = (
        netlist_directory.rglob("*")
        if arguments.recursive
        else netlist_directory.iterdir()
    )
    netlists = sorted(
        path for path in candidates
        if path.is_file()
        and path.suffix.lower() in {".cir", ".sp"}
        and not any(
            path == excluded or excluded in path.parents
            for excluded in excluded_directories
        )
    )
    require(netlists, f"no netlists found in {netlist_directory}")

    for netlist in netlists:
        try:
            result = subprocess.run(
                [str(simulator), "--parse-only", str(netlist)],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
                timeout=arguments.timeout,
            )
        except subprocess.TimeoutExpired as error:
            raise RuntimeError(
                f"{netlist.name}: parser timed out after "
                f"{arguments.timeout:g}s ({error.cmd})"
            ) from error

        require(
            result.returncode == 0,
            f"{netlist.name}: parse failed ({result.returncode}): "
            f"{result.stderr}{result.stdout}",
        )
        print(f"PASS parsed netlist {netlist.relative_to(netlist_directory)}")

    print(f"Netlist parse summary: {len(netlists)}/{len(netlists)} passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"FAIL netlist parsing: {error}", file=sys.stderr)
        raise SystemExit(1)
