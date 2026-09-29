#!/usr/bin/env python3
import subprocess
import sys
import tempfile
from pathlib import Path


def run(simulator, *arguments, cwd=None):
    return subprocess.run(
        [str(simulator), *map(str, arguments)],
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=10,
    )


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def write_netlist(path):
    path.write_text(
        "Configuration CLI test\n"
        "V1 in 0 1\n"
        "R1 in 0 1k\n"
        ".op\n"
        ".end\n"
    )


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <simulator>", file=sys.stderr)
        return 2

    simulator = Path(sys.argv[1]).resolve()
    failures = []

    with tempfile.TemporaryDirectory(prefix="simulator-config-cli-") as directory:
        root = Path(directory)
        deck = root / "input.cir"
        write_netlist(deck)

        try:
            explicit = root / "explicit.json"
            explicit.write_text('{"schema_version": 1}\n')
            result = run(
                simulator,
                "--config",
                explicit,
                "--print-config-path",
                "--parse-only",
                deck,
            )
            require(
                result.returncode == 0,
                f"explicit configuration failed: {result.stderr}",
            )
            require(
                f"Configuration file: <{explicit}>" in result.stderr,
                "explicit configuration path was not reported",
            )
            require(not result.stdout, "parse-only configuration check wrote stdout")
            print("PASS explicit configuration path")
        except Exception as error:
            failures.append(str(error))

        try:
            project = root / "project"
            nested = project / "nested"
            nested.mkdir(parents=True)
            discovered = project / "config.json"
            discovered.write_text('{"schema_version": 1}\n')
            result = run(
                simulator,
                "--print-config-path",
                "--parse-only",
                deck,
                cwd=nested,
            )
            require(
                result.returncode == 0,
                f"automatic configuration discovery failed: {result.stderr}",
            )
            require(
                f"Configuration file: <{discovered.resolve()}>" in result.stderr,
                "nearest parent configuration was not reported",
            )

            result = run(
                simulator,
                "--config-search-depth",
                "0",
                "--print-config-path",
                "--parse-only",
                deck,
                cwd=nested,
            )
            require(
                result.returncode == 0,
                f"limited configuration search failed: {result.stderr}",
            )
            require(
                "Configuration file: none (using built-in defaults)"
                in result.stderr,
                "zero search depth did not retain built-in defaults",
            )
            print("PASS automatic configuration discovery and search depth")
        except Exception as error:
            failures.append(str(error))

        try:
            missing = root / "missing.json"
            result = run(
                simulator,
                "--config",
                missing,
                "--parse-only",
                deck,
            )
            require(
                result.returncode == 2,
                "missing explicit configuration did not return a usage error",
            )
            require(
                "Configuration file not found" in result.stderr,
                "missing explicit configuration diagnostic is absent",
            )

            invalid = root / "invalid.json"
            invalid.write_text("{\n")
            result = run(
                simulator,
                "--config",
                invalid,
                "--parse-only",
                deck,
            )
            require(
                result.returncode == 2,
                "invalid json did not return a usage error",
            )
            require(
                "Invalid json in configuration file" in result.stderr,
                "invalid json diagnostic is absent",
            )

            unsupported = root / "unsupported.json"
            unsupported.write_text('{"schema_version": 1, "source": true}\n')
            result = run(
                simulator,
                "--config",
                unsupported,
                "--parse-only",
                deck,
            )
            require(
                result.returncode == 2,
                "unsupported configuration field did not return a usage error",
            )
            require(
                "$.source is not supported" in result.stderr,
                "unsupported configuration field diagnostic is absent",
            )

            analysis_config = root / "analysis.json"
            analysis_config.write_text(
                "{\n"
                '  "schema_version": 1,\n'
                '  "op": {"newton": {"maximum_iterations": 5}},\n'
                '  "tran": {"output_interval": "1n", "stop_time": "2n"}\n'
                "}\n"
            )
            result = run(
                simulator,
                "--config",
                analysis_config,
                "--parse-only",
                deck,
            )
            require(
                result.returncode == 0,
                f"valid analysis configuration failed: {result.stderr}",
            )

            invalid_range = root / "invalid-range.json"
            invalid_range.write_text(
                '{"schema_version": 1, '
                '"op": {"newton": {"tolerance": 0}}}\n'
            )
            result = run(
                simulator,
                "--config",
                invalid_range,
                "--parse-only",
                deck,
            )
            require(
                result.returncode == 2,
                "invalid analysis configuration range was accepted",
            )
            require(
                "Invalid analysis configuration" in result.stderr,
                "invalid analysis configuration diagnostic is absent",
            )

            priority_config = root / "priority.json"
            priority_config.write_text(
                "{\n"
                '  "schema_version": 1,\n'
                '  "op": {"source_stepping": {\n'
                '    "initial_step": 0.9,\n'
                '    "maximum_step": 0.8\n'
                "  }}\n"
                "}\n"
            )
            result = run(
                simulator,
                "--config",
                priority_config,
                "--parse-only",
                deck,
            )
            require(
                result.returncode == 2,
                "invalid source-stepping configuration was accepted",
            )
            result = run(
                simulator,
                "--config",
                priority_config,
                "--op-option",
                "source_stepping.maximum_step=1",
                deck,
            )
            require(
                result.returncode == 0,
                "command-line OP override did not take priority over config: "
                f"{result.stderr}",
            )

            result = run(
                simulator,
                "--op-option",
                "newton.unknown=1",
                deck,
            )
            require(
                result.returncode == 2,
                "unknown command-line OP field was accepted",
            )
            require(
                "unknown operating-point option" in result.stderr,
                "unknown command-line OP field diagnostic is absent",
            )

            locked_tran = root / "locked-tran.cir"
            locked_tran.write_text(
                "Netlist transient controls take priority\n"
                "V1 in 0 1\n"
                "R1 in out 1k\n"
                "C1 out 0 1n\n"
                ".tran 1n 4n 0 1n UIC\n"
                ".end\n"
            )
            locked_tran_config = root / "locked-tran.json"
            locked_tran_config.write_text(
                "{\n"
                '  "schema_version": 1,\n'
                '  "tran": {\n'
                '    "enabled": false,\n'
                '    "output_interval": -1,\n'
                '    "stop_time": -1,\n'
                '    "output_start_time": -1,\n'
                '    "maximum_step": -1,\n'
                '    "use_initial_conditions": false\n'
                "  }\n"
                "}\n"
            )
            result = run(
                simulator,
                "--config",
                locked_tran_config,
                locked_tran,
            )
            require(
                result.returncode == 0,
                "netlist .tran controls did not override config values: "
                f"{result.stderr}",
            )
            result = run(
                simulator,
                "--tran-option",
                "enabled=false",
                "--tran-option",
                "output-interval=-1",
                "--tran-option",
                "stop-time=-1",
                "--tran-option",
                "output-start-time=-1",
                "--tran-option",
                "maximum-step=-1",
                "--tran-option",
                "use-initial-conditions=false",
                locked_tran,
            )
            require(
                result.returncode == 0,
                "netlist .tran controls did not override CLI values: "
                f"{result.stderr}",
            )

            locked_pstran = root / "locked-pstran.cir"
            locked_pstran.write_text(
                "Netlist pseudo-transient controls take priority\n"
                "V1 in 0 1\n"
                "R1 in 0 1k\n"
                ".pstran convval=1 initstep=1n minstep=1p maxstep=1u "
                "tau=1n vbe0=0.7 tauramp=1n\n"
                ".op\n"
                ".end\n"
            )
            locked_pstran_config = root / "locked-pstran.json"
            locked_pstran_config.write_text(
                "{\n"
                '  "schema_version": 1,\n'
                '  "pta": {\n'
                '    "mode": "disabled",\n'
                '    "initial_step": 0,\n'
                '    "derivative_tolerance": 0,\n'
                '    "compound_time_constant": -1\n'
                "  }\n"
                "}\n"
            )
            result = run(
                simulator,
                "--config",
                locked_pstran_config,
                locked_pstran,
            )
            require(
                result.returncode == 0,
                "netlist .pstran controls did not override config values: "
                f"{result.stderr}",
            )
            result = run(
                simulator,
                "--pta",
                "disabled",
                "--pta-option",
                "initial-step=0",
                "--pta-option",
                "derivative-tolerance=0",
                "--pta-option",
                "compound-time-constant=-1",
                locked_pstran,
            )
            require(
                result.returncode == 0,
                "netlist .pstran controls did not override CLI values: "
                f"{result.stderr}",
            )

            result = run(
                simulator,
                "--config-search-depth",
                "-1",
                "--parse-only",
                deck,
            )
            require(
                result.returncode == 2,
                "negative configuration search depth was accepted",
            )
            print("PASS configuration diagnostics and argument validation")
        except Exception as error:
            failures.append(str(error))

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1

    print("Configuration CLI summary: 3/3 checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
