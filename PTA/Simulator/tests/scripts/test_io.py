#!/usr/bin/env python3
import os
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def run(simulator, *arguments):
    return subprocess.run(
        [str(simulator), *map(str, arguments)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=10,
    )


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <simulator>", file=sys.stderr)
        return 2

    simulator = Path(sys.argv[1]).resolve()
    compare_script = Path(__file__).with_name("compare_spice.py").resolve()
    validate_raw_script = Path(__file__).with_name("validate_raw.py").resolve()
    failures = []

    with tempfile.TemporaryDirectory(prefix="simulator-io-") as directory:
        root = Path(directory)

        try:
            valid = root / "valid.cir"
            valid.write_text(
                "Original title\n"
                ".title Mixed-case # node and continuation\n"
                ".model UNUSED D IS =1e-14 N= 1\n"
                "V1 IN 0 DC 5 ; source comment\n"
                "R1 in OUT#sense 1k // slash comment\n"
                "C1 out#SENSE 0 1u $ dollar comment\n"
                ".op\n"
                ".print op V(in) V(in,gnd) V(gnd) I(V1)\n"
                ".tran 100u 200u UIC\n"
                ".print tran time V(in) V(out#sense,in)\n"
                "+ I(V1)\n"
                ".end\n"
            )
            listing = root / "valid.out"
            raw = root / "valid.raw"
            listing.write_text("old listing\n")
            raw.write_text("old rawfile\n")
            result = run(
                simulator,
                "-b",
                "-o",
                listing,
                "-r",
                raw,
                valid,
            )
            require(result.returncode == 0, f"valid netlist failed: {result.stderr}")
            require(not result.stderr, f"valid netlist wrote stderr: {result.stderr}")
            listing_text = listing.read_text()
            require("v(out#sense,in)" in listing_text, "differential voltage missing")
            require("v1#branch" in listing_text, "branch current column missing")
            headers = [
                line.split()
                for line in listing_text.splitlines()
                if line.strip().startswith("Index")
            ]
            require(len(headers) == 2, "expected OP and TRAN listing headers")
            require(
                headers[0] == ["Index", "v(in)", "v(0)", "v1#branch"],
                f"unexpected OP .print order or duplicate: {headers[0]}",
            )
            require(
                headers[1]
                == ["Index", "time", "v(in)", "v(out#sense,in)", "v1#branch"],
                f"unexpected TRAN .print order: {headers[1]}",
            )
            raw_text = raw.read_text()
            require("Date:" in raw_text, "raw date header missing")
            require("Plotname: Transient Analysis" in raw_text, "raw plot missing")
            require(raw_text.count("Plotname:") == 2, "raw multi-plot output missing")
            require("\t0\ttime\ttime" in raw_text, "raw time variable missing")
            require(
                "\tv(out#sense)\tvoltage" in raw_text,
                "raw output was incorrectly filtered by .print",
            )

            bundle = root / "valid"
            bundled_listing = bundle / "valid.out"
            bundled_raw = bundle / "valid.raw"
            bundled_error = bundle / "valid.err"
            bundled_report = bundle / "valid.solve.txt"
            require(bundle.is_dir(), "netlist-named output directory is missing")
            require(
                bundled_listing.read_text() == listing_text,
                "bundled listing differs from the legacy mirror",
            )
            require(
                bundled_raw.read_text() == raw_text,
                "bundled rawfile differs from the legacy mirror",
            )
            require(
                bundled_error.read_text() == "",
                "successful bundled error log is not empty",
            )
            report_text = bundled_report.read_text()
            require(
                not (bundle / "valid.pta.jsonl").exists(),
                "non-PTA run unexpectedly wrote a PTA trace",
            )
            for expected in (
                "SPICE Solver Report",
                "Status: succeeded",
                "Circuit characteristics:",
                "Method path:",
                "Transient analysis:",
                "Effective solver configuration:",
                "Convergence and tuning observations:",
            ):
                require(expected in report_text, f"solve report missing {expected}")
            require(
                "Final update infinity norm:" not in report_text
                and "Final normalized update:" not in report_text
                and "Final normalized residual:" not in report_text,
                "linear OP/TRAN report included inapplicable Newton metrics",
            )

            alternate_root = root / "alternate-results"
            result = run(
                simulator,
                "-b",
                "--output-root",
                alternate_root,
                valid,
            )
            require(
                result.returncode == 0 and not result.stdout,
                f"custom output root failed: {result.stderr}",
            )
            require(
                (alternate_root / "valid" / "valid.solve.txt").is_file(),
                "custom output root did not retain the netlist-named directory",
            )

            debug_disabled_root = root / "debug-disabled-results"
            result = run(
                simulator,
                "-b",
                "--output-root",
                debug_disabled_root,
                valid,
            )
            require(
                result.returncode == 0,
                f"initial debug-enabled run failed: {result.stderr}",
            )
            require(
                (debug_disabled_root / "valid" / "valid.solve.txt").is_file(),
                "initial debug-enabled run did not write a solve report",
            )
            result = run(
                simulator,
                "-b",
                "--output-root",
                debug_disabled_root,
                "--debug",
                "false",
                valid,
            )
            require(
                result.returncode == 0,
                f"CLI debug=false run failed: {result.stderr}",
            )
            debug_disabled_bundle = debug_disabled_root / "valid"
            for artifact in ("valid.out", "valid.raw", "valid.err"):
                require(
                    (debug_disabled_bundle / artifact).is_file(),
                    f"debug=false omitted required artifact {artifact}",
                )
            require(
                not (debug_disabled_bundle / "valid.solve.txt").exists(),
                "CLI debug=false did not remove the stale solve report",
            )

            debug_config = root / "debug-disabled.json"
            debug_config.write_text('{"schema_version": 1, "debug": false}\n')
            config_disabled_root = root / "config-debug-disabled-results"
            result = run(
                simulator,
                "-b",
                "--config",
                debug_config,
                "--output-root",
                config_disabled_root,
                valid,
            )
            require(
                result.returncode == 0,
                f"config debug=false run failed: {result.stderr}",
            )
            require(
                not (
                    config_disabled_root / "valid" / "valid.solve.txt"
                ).exists(),
                "configuration debug=false unexpectedly wrote a solve report",
            )

            debug_override_root = root / "debug-cli-override-results"
            result = run(
                simulator,
                "-b",
                "--config",
                debug_config,
                "--debug",
                "true",
                "--output-root",
                debug_override_root,
                valid,
            )
            require(
                result.returncode == 0,
                f"CLI debug override run failed: {result.stderr}",
            )
            require(
                (
                    debug_override_root / "valid" / "valid.solve.txt"
                ).is_file(),
                "CLI debug=true did not override configuration debug=false",
            )

            multi_suffix = root / "multi.stage.cir"
            multi_suffix.write_text(valid.read_text())
            result = run(simulator, "-b", multi_suffix)
            require(result.returncode == 0, f"multi-suffix netlist failed: {result.stderr}")
            require(
                (root / "multi.stage" / "multi.stage.solve.txt").is_file(),
                "output directory did not remove only the final netlist suffix",
            )

            parse_only = root / "parse-only.cir"
            parse_only.write_text(valid.read_text())
            result = run(simulator, "--parse-only", parse_only)
            require(result.returncode == 0, f"parse-only failed: {result.stderr}")
            require(
                not (root / "parse-only").exists(),
                "parse-only unexpectedly created an output bundle",
            )

            standard_dir = root / "mixed-standard"
            actual_dir = root / "mixed-actual"
            standard_dir.mkdir()
            actual_dir.mkdir()
            (standard_dir / "mixed.out").write_text(listing_text)
            (actual_dir / "mixed.out").write_text(listing_text)
            mixed_raw = actual_dir / "mixed.raw"
            mixed_raw.write_text(raw_text)
            for analysis in ("op", "tran"):
                comparison = subprocess.run(
                    [
                        sys.executable,
                        str(compare_script),
                        "--analysis",
                        analysis,
                        "--standard",
                        str(standard_dir),
                        "--actual",
                        str(actual_dir),
                        "--atol",
                        "0",
                        "--rtol",
                        "0",
                    ],
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                )
                require(
                    comparison.returncode == 0,
                    f"mixed-analysis {analysis} comparison failed: "
                    f"{comparison.stdout}{comparison.stderr}",
                )
                raw_validation = subprocess.run(
                    [
                        sys.executable,
                        str(validate_raw_script),
                        "--analysis",
                        analysis,
                        "--listing-dir",
                        str(actual_dir),
                        str(mixed_raw),
                    ],
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                )
                require(
                    raw_validation.returncode == 0,
                    f"mixed-analysis {analysis} raw/listing check failed: "
                    f"{raw_validation.stdout}{raw_validation.stderr}",
                )
            print("PASS valid SPICE syntax, comments, continuation, case folding")
        except Exception as exc:
            failures.append(str(exc))

        try:
            def transient_initial_values(output):
                lines = output.splitlines()
                for index, line in enumerate(lines):
                    header = line.split()
                    if not header or header[0] != "Index" or "time" not in header:
                        continue
                    for data_line in lines[index + 1 :]:
                        values = data_line.split()
                        if values and values[0] == "0":
                            return {
                                name: float(values[position])
                                for position, name in enumerate(header)
                            }
                raise RuntimeError("transient listing has no t=0 sample")

            nodeset = root / "nodeset-guess.cir"
            nodeset.write_text(
                "Node-set remains a Newton hint\n"
                "V1 out 0 1\n"
                "R1 out 0 1k\n"
                ".nodeset V(out)=5\n"
                ".op\n"
                ".print op v(out)\n"
                ".end\n"
            )
            result = run(simulator, nodeset)
            require(result.returncode == 0, f".nodeset netlist failed: {result.stderr}")
            require(
                "1.0000000000e+00" in result.stdout,
                ".nodeset was incorrectly treated as a permanent voltage constraint",
            )

            ic_guess = root / "ic-op-guess.cir"
            ic_guess.write_text(
                "IC remains an OP guess without UIC\n"
                "V1 out 0 1\n"
                "R1 out 0 1k\n"
                ".ic V(out)=5\n"
                ".op\n"
                ".print op v(out)\n"
                ".end\n"
            )
            result = run(simulator, ic_guess)
            require(result.returncode == 0, f".ic OP netlist failed: {result.stderr}")
            require(
                "1.0000000000e+00" in result.stdout,
                ".ic without UIC was incorrectly treated as an OP constraint",
            )

            reactive_uic = root / "reactive-uic-ic.cir"
            reactive_uic.write_text(
                "UIC .ic and reactive device IC\n"
                "V1 in 0 1\n"
                "R1 in out 1k\n"
                "C1 out 0 1u IC=2\n"
                "L1 sense 0 1m IC=3m\n"
                "R2 sense 0 1k\n"
                ".ic V(in)=1\n"
                ".tran 1u 2u UIC\n"
                ".print tran time v(in) v(out) i(l1)\n"
                ".end\n"
            )
            result = run(simulator, reactive_uic)
            require(
                result.returncode == 0,
                f"reactive UIC initial-condition netlist failed: {result.stderr}",
            )
            initial = transient_initial_values(result.stdout)
            require(
                abs(initial["v(in)"] - 1.0) < 1e-12 and
                abs(initial["v(out)"] - 2.0) < 1e-12 and
                abs(initial["l1#branch"] - 3e-3) < 1e-12,
                "UIC did not preserve .ic, capacitor IC, and inductor IC at t=0",
            )

            semiconductor_uic = root / "semiconductor-uic-ic.cir"
            semiconductor_uic.write_text(
                "BJT and MOS initial-condition cards\n"
                ".model QMOD NPN IS=1e-16 BF=100\n"
                ".model MMOD NMOS LEVEL=1 VTO=0.7 KP=100u\n"
                "Q1 qc qb qe QMOD IC=0.7,1\n"
                "RQC qc 0 10k\n"
                "RQB qb 0 10k\n"
                "RQE qe 0 10k\n"
                "M1 md mg ms mb MMOD W=1u L=1u IC=1,2,0\n"
                "RMD md 0 10k\n"
                "RMG mg 0 10k\n"
                "RMS ms 0 10k\n"
                "RMB mb 0 10k\n"
                ".tran 1n 1n UIC\n"
                ".print tran time v(qc) v(qb) v(qe) v(md) v(mg) v(ms) v(mb)\n"
                ".end\n"
            )
            result = run(simulator, semiconductor_uic)
            require(
                result.returncode == 0,
                f"semiconductor UIC IC netlist failed: {result.stderr}",
            )
            initial = transient_initial_values(result.stdout)
            require(
                abs(initial["v(qb)"] - initial["v(qe)"] - 0.7) < 1e-12 and
                abs(initial["v(qc)"] - initial["v(qe)"] - 1.0) < 1e-12 and
                abs(initial["v(md)"] - initial["v(ms)"] - 1.0) < 1e-12 and
                abs(initial["v(mg)"] - initial["v(ms)"] - 2.0) < 1e-12 and
                abs(initial["v(mb)"] - initial["v(ms)"]) < 1e-12,
                "UIC did not preserve BJT/MOS IC terminal voltages at t=0",
            )

            conflicting_ic = root / "conflicting-ic.cir"
            conflicting_ic.write_text(
                "Conflicting explicit ICs\n"
                "R1 out 0 1k\n"
                "C1 out 0 1u IC=1\n"
                ".ic V(out)=2\n"
                ".tran 1u 1u UIC\n"
                ".end\n"
            )
            result = run(simulator, conflicting_ic)
            require(result.returncode != 0, "conflicting initial conditions were accepted")
            require(
                "Initial-condition conflict" in result.stderr,
                "conflicting initial-condition diagnostic is missing",
            )
            print("PASS .nodeset, .ic, and device UIC initial conditions")
        except Exception as exc:
            failures.append(str(exc))

        try:
            source_stepping_case = (
                Path(__file__).resolve().parents[1]
                / "cases"
                / "op"
                / "level2_07_bjt_differential_pair_mesh.cir"
            )
            source_root = root / "source-stepping-results"
            result = run(
                simulator,
                "-b",
                "--output-root",
                source_root,
                "--op-option",
                "newton.maximum-iterations=2",
                source_stepping_case,
            )
            require(
                result.returncode == 0,
                f"source-stepping recovery failed: {result.stderr}",
            )
            source_report = (
                source_root
                / source_stepping_case.stem
                / f"{source_stepping_case.stem}.solve.txt"
            ).read_text()
            require(
                "Method path: direct Newton failed -> source stepping succeeded"
                in source_report,
                "source-stepping report omitted the failed-to-recovered method chain",
            )
            require(
                "Total iterations:" in source_report
                and "Final solver attempt iterations:" in source_report
                and "Source stepping:" in source_report,
                "source-stepping report omitted iteration or attempt details",
            )
            source_attempt_lines = [
                line
                for line in source_report.splitlines()
                if "accepted_scale=" in line and "target_scale=" in line
            ]
            require(source_attempt_lines, "source-stepping attempt trace is empty")
            for line in source_attempt_lines:
                fields = {}
                for token in line.split():
                    for name in ("accepted_scale", "step", "target_scale"):
                        prefix = f"{name}="
                        if token.startswith(prefix):
                            fields[name] = float(token[len(prefix):])
                require(
                    set(fields) == {"accepted_scale", "step", "target_scale"},
                    f"malformed source-stepping trace line: {line}",
                )
                actual_step = fields["target_scale"] - fields["accepted_scale"]
                require(
                    abs(fields["step"] - actual_step)
                    <= 1.0e-12 * max(1.0, abs(actual_step)),
                    f"reported source step is not the actual clamped increment: {line}",
                )
            print("PASS source-stepping recovery report")
        except Exception as exc:
            failures.append(str(exc))

        try:
            hierarchical = root / "hierarchical-level3.sp"
            hierarchical.write_text(
                "Nested subcircuit with a Level-1 MOS model card\n"
                "VDD vdd 0 5\n"
                "XTOP out vdd STAGE\n"
                "RLOAD out 0 1k\n"
                ".subckt STAGE out in\n"
                "XLEAF out in LEAF\n"
                ".ends STAGE\n"
                ".subckt LEAF out in\n"
                "M1 out in 0 0 NMOD W=1u L=1u\n"
                ".ends LEAF\n"
                ".model NMOD NMOS (LEVEL=1 VTO=0.7 KP=1m TOX=1n "
                "UO=550 CGSO=0 CGDO=0)\n"
                ".op\n"
                ".print op v(out)\n"
                ".end\n"
            )
            result = run(simulator, hierarchical)
            require(
                result.returncode == 0,
                f"nested subcircuit netlist failed: {result.stderr}",
            )
            require("v(out)" in result.stdout, "nested subcircuit output missing")
            print("PASS nested .subckt expansion and Level-1 MOS evaluation")
        except Exception as exc:
            failures.append(str(exc))

        try:
            mos3_instance = root / "mos3-instance-parameters.cir"
            mos3_instance.write_text(
                "MOS3 parser coverage\n"
                "VDD d 0 5\n"
                "VG g 0 0\n"
                "M1 d g 0 0 PMOD W=2u L=1u AD=2p AS=3p PD=4u PS=5u "
                "NRD=2 NRS=3 M=1.5 OFF IC=0,0,0 TEMP=27\n"
                ".model PMOD PMOS LEVEL=3 VTO=-0.8 K=20u GAMMA=0.6 PHI=0.7 "
                "RD=1 RS=2 RSH=10 CBD=1p CBS=2p IS=1e-14 JS=1e-8 PB=0.8 FC=0.5 "
                "CJ=2e-4 MJ=0.5 CJSW=1e-10 MJSW=0.3 CGSO=1p CGDO=2p CGBO=3p "
                "TOX=20n LD=0 XL=-0.1u XW=-0.2u WD=0 U0=250 NSUB=1e16 TPG=-1 "
                "NSS=0 VMAX=1e5 XJ=0.2u NFS=1e11 ETA=0 DELTA=0 THETA=0.1 "
                "KAPPA=0.3 TNOM=27 KF=0 AF=1\n"
                ".op\n"
                ".end\n"
            )
            result = run(simulator, "--parse-only", mos3_instance)
            require(
                result.returncode == 0,
                f"MOS3 model or instance parsing failed: {result.stderr}",
            )
            print("PASS MOS3 model and instance parameter parsing")
        except Exception as exc:
            failures.append(str(exc))

        try:
            parameterized = root / "parameterized-subcircuit.sp"
            parameterized.write_text(
                "Parameterized nested subcircuit\n"
                "V1 in 0 5\n"
                "XTOP out in STAGE PARAMS: scale=4\n"
                "RLOAD out 0 1k\n"
                ".subckt STAGE out in PARAMS: base=1k scale=1\n"
                "XLEAF out in LEAF resistance={ (base * scale) }\n"
                ".ends STAGE\n"
                ".subckt LEAF out in PARAMS: resistance=2k\n"
                "R1 out in {resistance}\n"
                ".ends LEAF\n"
                ".op\n"
                ".print op v(out)\n"
                ".end\n"
            )
            result = run(simulator, parameterized)
            require(
                result.returncode == 0,
                f"parameterized subcircuit failed: {result.stderr}",
            )
            require(
                "1.0000000000e+00" in result.stdout,
                "subcircuit parameter override was not materialized",
            )

            invalid_parameter = root / "unknown-subcircuit-parameter.sp"
            invalid_parameter.write_text(
                "Unknown subcircuit parameter\n"
                "V1 in 0 1\n"
                "X1 out in STAGE unknown=1\n"
                ".subckt STAGE out in PARAMS: resistance=1k\n"
                "R1 out in {resistance}\n"
                ".ends STAGE\n"
                ".op\n"
                ".end\n"
            )
            result = run(simulator, invalid_parameter)
            require(
                result.returncode != 0,
                "unknown subcircuit parameter was accepted",
            )
            require(
                "Unknown subcircuit parameter unknown" in result.stderr,
                "missing unknown subcircuit parameter diagnostic",
            )
            print("PASS parameterized nested .subckt expansion")
        except Exception as exc:
            failures.append(str(exc))

        try:
            pstran = root / "pstran.sp"
            pstran.write_text(
                "Pseudo-transient control card\n"
                ".model NMOD NMOS LEVEL=3 VTO=0.7 KP=100u\n"
                "V1 in 0 1\n"
                "R1 in 0 1k\n"
                "M1 in in 0 0 NMOD W=1u L=1u\n"
                ".pstran convval=1.0e-05 initstep=1.0e-05 minstep=1.0e-09 "
                "maxstep=1.0e+6 tau=1.0e-05 vbe0=0.0 kvgs0=1.2 tauramp=0.0\n"
                ".print op v(in)\n"
                ".end\n"
            )
            result = run(simulator, pstran)
            require(result.returncode == 0, f".pstran netlist failed: {result.stderr}")
            require("Operating Point" in result.stdout, ".pstran did not run OP output")
            pstran_report = (root / "pstran" / "pstran.solve.txt").read_text()
            require(
                "pta.initial_mos_vgs: 1.2" in pstran_report,
                ".pstran kvgs0 did not map to the MOS limiter seed",
            )
            pstran_trace = root / "pstran" / "pstran.pta.jsonl"
            require(pstran_trace.is_file(), ".pstran JSONL trace is missing")
            trace_records = [
                json.loads(line)
                for line in pstran_trace.read_text().splitlines()
                if line.strip()
            ]
            require(
                trace_records[0]["record_type"] == "metadata" and
                trace_records[0]["schema_version"] == 1 and
                trace_records[0]["configuration_hash"].startswith("fnv1a64:") and
                trace_records[0]["configuration"]["newton"]["maximum_backtracks"] >= 0,
                "PTA trace metadata lacks its replayable configuration snapshot",
            )
            attempts = [
                record for record in trace_records
                if record["record_type"] == "attempt"
            ]
            require(attempts, "PTA JSONL trace contains no attempt records")
            require(
                all("newton" in attempt and "time_step" in attempt
                    for attempt in attempts),
                "PTA JSONL attempt record omits solver state",
            )
            final_trace = trace_records[-1]
            require(
                final_trace["record_type"] == "final" and
                final_trace["status"] == "succeeded" and
                final_trace["solution"]["node_voltages"],
                "PTA JSONL trace lacks the successful final solution",
            )

            duplicate = root / "duplicate-pstran.sp"
            duplicate.write_text(
                "Duplicate pseudo-transient parameter\nR1 in 0 1k\n"
                ".pstran convval=1 convval=2\n.end\n"
            )
            result = run(simulator, duplicate)
            require(result.returncode != 0, "duplicate .pstran parameter was accepted")
            require("Repeated .pstran parameter" in result.stderr, "missing .pstran duplicate diagnostic")
            print("PASS .pstran control-card parsing")
        except Exception as exc:
            failures.append(str(exc))

        try:
            bjt_model = root / "bjt-rb-va.cir"
            bjt_model.write_text(
                "BJT RB and VA model parameters\n"
                ".model QMOD NPN IS=1e-15 BF=100 RB=100 VA=50\n"
                "VCC c 0 5\n"
                "VBB b 0 0.8\n"
                "R1 c out 1k\n"
                "Q1 out b 0 QMOD\n"
                ".op\n"
                ".print op v(out)\n"
                ".end\n"
            )
            result = run(simulator, bjt_model)
            require(result.returncode == 0, f"BJT RB/VA netlist failed: {result.stderr}")
            require("v(out)" in result.stdout, "BJT RB/VA output missing")
            print("PASS BJT RB and VA model parameters")
        except Exception as exc:
            failures.append(str(exc))

        try:
            def operating_point_value(netlist, column):
                listing = netlist.with_suffix(".out")
                result = run(simulator, "-b", "-o", listing, netlist)
                require(
                    result.returncode == 0,
                    f"{netlist.name} failed: {result.stderr}",
                )

                lines = listing.read_text().splitlines()
                for index, line in enumerate(lines):
                    header = line.split()
                    if not header or header[0] != "Index" or column not in header:
                        continue
                    for data_line in lines[index + 1 :]:
                        values = data_line.split()
                        if values and values[0].isdigit():
                            return float(values[header.index(column)])
                raise RuntimeError(
                    f"{netlist.name} listing has no OP value for {column}"
                )

            diode_without_rs = root / "diode-without-rs.cir"
            diode_without_rs.write_text(
                "Diode model without series resistance\n"
                ".model DMOD D IS=1e-12 N=1\n"
                "V1 in 0 1\n"
                "D1 in out DMOD\n"
                "R1 out 0 1k\n"
                ".op\n"
                ".print op i(v1)\n"
                ".end\n"
            )
            diode_with_rs = root / "diode-with-rs.cir"
            diode_with_rs.write_text(
                "Diode model with series resistance\n"
                ".model DMOD D IS=1e-12 N=1 RS=1k\n"
                "V1 in 0 1\n"
                "D1 in out DMOD\n"
                "R1 out 0 1k\n"
                ".op\n"
                ".print op i(v1)\n"
                ".end\n"
            )
            diode_current_without_rs = abs(
                operating_point_value(diode_without_rs, "v1#branch")
            )
            diode_current_with_rs = abs(
                operating_point_value(diode_with_rs, "v1#branch")
            )
            require(
                diode_current_with_rs < 0.9 * diode_current_without_rs,
                "diode RS did not reduce the DC branch current",
            )

            bjt_without_shunts = root / "bjt-without-shunts.cir"
            bjt_without_shunts.write_text(
                "BJT model without leakage shunts\n"
                ".model QMOD NPN IS=1e-20 BF=100\n"
                "V1 b 0 1\n"
                "V2 c 0 1\n"
                "Q1 c b 0 QMOD\n"
                ".op\n"
                ".print op i(v1) i(v2)\n"
                ".end\n"
            )
            bjt_with_shunts = root / "bjt-with-shunts.cir"
            bjt_with_shunts.write_text(
                "BJT model with base-emitter and collector-emitter shunts\n"
                ".model QMOD NPN IS=1e-20 BF=100 RBE=1k RCE=2k\n"
                "V1 b 0 1\n"
                "V2 c 0 1\n"
                "Q1 c b 0 QMOD\n"
                ".op\n"
                ".print op i(v1) i(v2)\n"
                ".end\n"
            )
            bjt_base_without = abs(
                operating_point_value(bjt_without_shunts, "v1#branch")
            )
            bjt_base_with = abs(
                operating_point_value(bjt_with_shunts, "v1#branch")
            )
            bjt_collector_without = abs(
                operating_point_value(bjt_without_shunts, "v2#branch")
            )
            bjt_collector_with = abs(
                operating_point_value(bjt_with_shunts, "v2#branch")
            )
            require(
                bjt_base_with > bjt_base_without + 5e-4,
                "BJT RBE did not add a base-emitter shunt current",
            )
            require(
                bjt_collector_with > bjt_collector_without + 2e-4,
                "BJT RCE did not add a collector-emitter shunt current",
            )

            mos_without_rds = root / "mos-without-rds.cir"
            mos_without_rds.write_text(
                "MOS model without drain-source shunt\n"
                ".model MMOD NMOS VTO=5 KP=1m\n"
                "V1 d 0 1\n"
                "M1 d 0 0 0 MMOD W=1u L=1u\n"
                ".op\n"
                ".print op i(v1)\n"
                ".end\n"
            )
            mos_with_rds = root / "mos-with-rds.cir"
            mos_with_rds.write_text(
                "MOS model with drain-source shunt\n"
                ".model MMOD NMOS VTO=5 KP=1m RDS=1k\n"
                "V1 d 0 1\n"
                "M1 d 0 0 0 MMOD W=1u L=1u\n"
                ".op\n"
                ".print op i(v1)\n"
                ".end\n"
            )
            mos_current_without = abs(
                operating_point_value(mos_without_rds, "v1#branch")
            )
            mos_current_with = abs(
                operating_point_value(mos_with_rds, "v1#branch")
            )
            require(
                mos_current_with > mos_current_without + 5e-4,
                "MOS RDS did not add a drain-source shunt current",
            )
            print("PASS diode RS, BJT RBE/RCE, and MOS RDS model stamps")
        except Exception as exc:
            failures.append(str(exc))

        try:
            gummel_poon = root / "bjt-gummel-poon-dc.cir"
            gummel_poon.write_text(
                "BJT DC Gummel-Poon subset\n"
                ".model QMOD NPN IS=1e-15 BF=120 BR=2 RB=80 RC=25 RE=5 "
                "VAF=60 VAR=40 IKF=2m IKR=1m ISE=1e-16 ISC=2e-16 NE=1.4 NC=1.8\n"
                "VCC c 0 5\n"
                "VBB b 0 0.78\n"
                "R1 c out 1k\n"
                "Q1 out b e QMOD\n"
                "REXT e 0 20\n"
                ".op\n"
                ".print op v(out) v(e)\n"
                ".end\n"
            )
            result = run(simulator, gummel_poon)
            require(
                result.returncode == 0,
                f"BJT Gummel-Poon subset netlist failed: {result.stderr}",
            )
            require(
                "v(out)" in result.stdout and "v(e)" in result.stdout,
                "BJT Gummel-Poon subset output missing",
            )
            print("PASS BJT DC Gummel-Poon subset parameters")
        except Exception as exc:
            failures.append(str(exc))

        try:
            delmax = root / "delmax.cir"
            delmax.write_text(
                "DELMAX hard integration cap\n"
                "V1 in 0 1\n"
                "R1 in out 1k\n"
                "C1 out 0 1n\n"
                ".options DELMAX=100n\n"
                ".tran 1u 2u 0 500n UIC\n"
                ".print tran time v(out)\n"
                ".end\n"
            )
            result = run(simulator, delmax)
            require(result.returncode == 0, f"DELMAX netlist failed: {result.stderr}")
            require("Transient Analysis" in result.stdout, "DELMAX transient output missing")

            tmax = root / "tmax-equivalent.cir"
            tmax.write_text(
                "DELMAX hard integration cap\n"
                "V1 in 0 1\n"
                "R1 in out 1k\n"
                "C1 out 0 1n\n"
                ".tran 1u 2u 0 100n UIC\n"
                ".print tran time v(out)\n"
                ".end\n"
            )
            tmax_result = run(simulator, tmax)
            require(tmax_result.returncode == 0, f"TMAX netlist failed: {tmax_result.stderr}")
            require(
                result.stdout == tmax_result.stdout,
                "DELMAX did not impose the same hard cap as .tran TMAX",
            )

            invalid_delmax = root / "invalid-delmax.cir"
            invalid_delmax.write_text(
                "Invalid DELMAX\nR1 in 0 1k\n.options delmax=0\n.op\n.end\n"
            )
            result = run(simulator, invalid_delmax)
            require(result.returncode != 0, "zero DELMAX was accepted")
            require("DELMAX must be positive" in result.stderr, "missing DELMAX diagnostic")

            pstran_delmax = root / "pstran-delmax.cir"
            pstran_delmax.write_text(
                "DELMAX pseudo-transient cap\n"
                "V1 in 0 1\n"
                "R1 in 0 1k\n"
                ".options delmax=10n\n"
                ".pstran convval=1 initstep=1n minstep=1p maxstep=1u\n"
                ".op\n"
                ".end\n"
            )
            pstran_result = run(simulator, pstran_delmax)
            require(
                pstran_result.returncode == 0,
                f"DELMAX .pstran netlist failed: {pstran_result.stderr}",
            )

            pstran_delmax.write_text(
                "DELMAX clamps pseudo-transient initial step\n"
                "V1 in 0 1\n"
                "R1 in 0 1k\n"
                ".options delmax=10n\n"
                ".pstran convval=1 initstep=1u minstep=1p maxstep=10u\n"
                ".op\n"
                ".end\n"
            )
            pstran_result = run(simulator, pstran_delmax)
            require(
                pstran_result.returncode == 0,
                "DELMAX did not clamp .pstran initstep to the hard cap: "
                f"{pstran_result.stderr}",
            )
            print("PASS .options DELMAX parsing and transient cap")
        except Exception as exc:
            failures.append(str(exc))

        try:
            result = run(
                simulator,
                "--pta",
                "fallback",
                "--pta-diagnostics",
                "--pta-option",
                "initial-step=2n",
                "--pta-option",
                "maximum-steps=20000",
                "--pta-option",
                "derivative-tolerance=0.5",
                "--pta-option",
                "derivative-relative-tolerance=1e-4",
                "--pta-option",
                "derivative-voltage-absolute-tolerance=1u",
                "--pta-option",
                "derivative-current-absolute-tolerance=1n",
                "--pta-option",
                "dc-residual-tolerance=0.5",
                "--pta-option",
                "dc-residual-relative-tolerance=1e-4",
                "--pta-option",
                "dc-voltage-absolute-tolerance=1u",
                "--pta-option",
                "dc-current-absolute-tolerance=1n",
                "--pta-option",
                "successful-step-scale=1.5",
                "--pta-option",
                "include-diodes=true",
                "--pta-option",
                "medium-oscillation-ratio=0.4",
                "--pta-option",
                "heavy-oscillation-ratio=1.2",
                valid,
            )
            require(
                result.returncode == 0,
                f"valid PTA options failed: {result.stderr}",
            )
            require(
                "PTA diagnostics:" in result.stderr and
                "status: PTA was not invoked" in result.stderr,
                "PTA diagnostics did not report the fallback solver path",
            )

            compound_fallback = root / "compound-fallback.cir"
            compound_fallback.write_text(
                "Compound PTA fallback must preserve ordinary OP\n"
                "I1 out 0 1m\n"
                "R1 out 0 1k\n"
                ".op\n"
                ".end\n"
            )
            result = run(
                simulator,
                "--pta",
                "fallback",
                "--pta-diagnostics",
                "--pta-option",
                "compound-time-constant=5n",
                compound_fallback,
            )
            require(
                result.returncode == 0,
                f"compound fallback ordinary OP failed: {result.stderr}",
            )
            require(
                "status: PTA was not invoked" in result.stderr,
                "compound fallback activated PTA before ordinary OP completed",
            )
            compound_report = (
                root
                / "compound-fallback"
                / "compound-fallback.solve.txt"
            ).read_text()
            require(
                "MNA unknowns: 1" in compound_report and
                "PTA pseudo-devices: 0" in compound_report,
                "compound fallback changed the successful ordinary MNA",
            )

            result = run(
                simulator,
                "--pta",
                "force",
                "--pta-option",
                "initial-step=0",
                valid,
            )
            require(result.returncode == 2, "invalid PTA range was accepted")
            require(
                "Invalid PTA configuration" in result.stderr,
                "invalid PTA range did not report configuration validation",
            )

            result = run(
                simulator,
                "--pta",
                "force",
                "--pta-option",
                "derivative-relative-tolerance=-1",
                valid,
            )
            require(
                result.returncode == 2,
                "negative PTA derivative relative tolerance was accepted",
            )
            require(
                "PTA derivative convergence tolerances" in result.stderr,
                "invalid derivative tolerance did not report validation",
            )

            result = run(
                simulator,
                "--pta",
                "force",
                "--pta-option",
                "dc-residual-relative-tolerance=-1",
                valid,
            )
            require(
                result.returncode == 2,
                "negative PTA DC residual relative tolerance was accepted",
            )
            require(
                "PTA DC residual tolerances" in result.stderr,
                "invalid DC residual tolerance did not report validation",
            )

            result = run(
                simulator,
                "--pta",
                "force",
                "--pta-option",
                "successful-step-scale=1",
                valid,
            )
            require(
                result.returncode == 2,
                "invalid PTA successful-step scale was accepted",
            )
            require(
                "PTA successful-step scale" in result.stderr,
                "invalid successful-step scale did not report validation",
            )

            result = run(
                simulator,
                "--pta",
                "force",
                "--pta-option",
                "medium-oscillation-ratio=1",
                "--pta-option",
                "heavy-oscillation-ratio=1",
                valid,
            )
            require(
                result.returncode == 2,
                "invalid PTA oscillation-ratio ordering was accepted",
            )
            require(
                "PTA oscillation ratios" in result.stderr,
                "invalid oscillation-ratio ordering did not report validation",
            )

            result = run(
                simulator,
                "--pta",
                "fallback",
                "--pta-option",
                "unknown-option=1",
                valid,
            )
            require(result.returncode == 2, "unknown PTA option was accepted")
            require(
                "unknown PTA option" in result.stderr,
                "unknown PTA option did not report a diagnostic",
            )

            result = run(
                simulator,
                "--pta",
                "fallback",
                "--pta-option",
                "maximum-steps=100",
                "--pta-option",
                "maximum-steps=200",
                valid,
            )
            require(result.returncode == 2, "repeated PTA option was accepted")
            require(
                "Repeated PTA option" in result.stderr,
                "repeated PTA option did not report a diagnostic",
            )

            result = run(
                simulator,
                "--pta-option",
                "maximum-steps=100",
                valid,
            )
            require(
                result.returncode == 2,
                "PTA option without PTA mode was accepted",
            )
            require(
                "PTA options and diagnostics require" in result.stderr,
                "missing PTA mode did not report a diagnostic",
            )
            print("PASS PTA command-line configuration and validation")
        except Exception as exc:
            failures.append(str(exc))

        try:
            same_path = root / "same-path.cir"
            original = "Same path protection\nR1 1 0 1k\n.op\n.end\n"
            same_path.write_text(original)
            result = run(simulator, same_path, same_path)
            require(result.returncode != 0, "same input/output path was accepted")
            require(same_path.read_text() == original, "input file was truncated")

            hard_link = root / "same-path-hard-link.out"
            os.link(same_path, hard_link)
            result = run(simulator, same_path, hard_link)
            require(result.returncode != 0, "input hard-link alias was accepted")
            require(same_path.read_text() == original, "hard-link input was truncated")

            canonical_raw = root / "valid" / "valid.raw"
            canonical_raw_text = canonical_raw.read_text()
            result = run(simulator, "-o", canonical_raw, valid)
            require(
                result.returncode != 0,
                "legacy listing mirror was allowed to replace canonical raw output",
            )
            require(
                canonical_raw.read_text() == canonical_raw_text,
                "canonical raw output changed after mirror-path rejection",
            )

            canonical_report = root / "valid" / "valid.solve.txt"
            report_alias = root / "canonical-report-hard-link.out"
            os.link(canonical_report, report_alias)
            canonical_report_text = canonical_report.read_text()
            result = run(simulator, "-o", report_alias, valid)
            require(
                result.returncode != 0,
                "hard-link mirror alias of the canonical report was accepted",
            )
            require(
                canonical_report.read_text() == canonical_report_text,
                "canonical report changed after hard-link mirror rejection",
            )

            case_probe = root / "case-probe"
            case_probe.write_text("probe\n")
            case_insensitive = (root / "CASE-PROBE").exists()
            case_probe.unlink()
            upper_output = root / "Case-Alias.OUT"
            lower_output = root / "case-alias.out"
            result = run(
                simulator,
                "-o",
                upper_output,
                "-r",
                lower_output,
                valid,
            )
            if case_insensitive:
                require(
                    result.returncode != 0,
                    "case-only listing/raw aliases were accepted",
                )
                require(
                    not upper_output.exists() and not lower_output.exists(),
                    "case-alias rollback left a partial output",
                )
            else:
                require(
                    result.returncode == 0,
                    f"distinct case-sensitive output paths failed: {result.stderr}",
                )
            print("PASS input/output same-path protection")
        except Exception as exc:
            failures.append(str(exc))

        try:
            invalid = root / "missing-end.cir"
            invalid.write_text("Missing end\nV1 in 0 1\nR1 in 0 1k\n.op\n")
            protected_output = root / "protected.out"
            protected_output.write_text("keep me\n")
            result = run(simulator, "-o", protected_output, invalid)
            require(result.returncode != 0, "missing .end was accepted")
            require(
                protected_output.read_text() == "keep me\n",
                "failed parse truncated an existing output",
            )

            protected_output.write_text("keep after staged write failure\n")
            invalid_raw = root / "missing-directory" / "result.raw"
            result = run(
                simulator,
                "-o",
                protected_output,
                "-r",
                invalid_raw,
                valid,
            )
            require(result.returncode != 0, "invalid raw destination succeeded")
            require(
                protected_output.read_text() == "keep after staged write failure\n",
                "raw write failure replaced the existing listing",
            )
            print("PASS parse/write failure preserves existing output")
        except Exception as exc:
            failures.append(str(exc))

        try:
            unsolved = root / "unsolved.cir"
            unsolved.write_text(
                "Conflicting ideal sources\n"
                "V1 out 0 1\n"
                "V2 out 0 2\n"
                ".op\n"
                ".end\n"
            )
            protected_output = root / "solve-protected.out"
            protected_output.write_text("keep after solve failure\n")
            failure_bundle = root / "unsolved"
            failure_bundle.mkdir()
            bundled_listing = failure_bundle / "unsolved.out"
            bundled_raw = failure_bundle / "unsolved.raw"
            bundled_listing.write_text("keep bundled listing\n")
            bundled_raw.write_text("keep bundled rawfile\n")
            result = run(simulator, "-o", protected_output, unsolved)
            require(result.returncode != 0, "singular circuit unexpectedly solved")
            require(
                protected_output.read_text() == "keep after solve failure\n",
                "failed solve truncated an existing output",
            )
            require(
                bundled_listing.read_text() == "keep bundled listing\n" and
                bundled_raw.read_text() == "keep bundled rawfile\n",
                "failed solve replaced a previous successful artifact pair",
            )
            failure_error = (failure_bundle / "unsolved.err").read_text()
            failure_report = (failure_bundle / "unsolved.solve.txt").read_text()
            require(
                "Operating point analysis failed" in failure_error,
                "failed solve error log omitted the terminal diagnostic",
            )
            require(
                "Status: failed" in failure_report and
                "Method path: direct sparse linear solve failed" in failure_report and
                "Direct sparse linear solve:" in failure_report and
                "Failure reason:" in failure_report and
                "sparse linear system" in failure_report,
                "failed solve report omitted convergence diagnostics",
            )

            no_report_failure = root / "unsolved-no-report.cir"
            no_report_failure.write_text(unsolved.read_text())
            result = run(simulator, "-b", no_report_failure)
            require(
                result.returncode != 0,
                "initial debug-enabled singular circuit unexpectedly solved",
            )
            no_report_bundle = root / "unsolved-no-report"
            require(
                (
                    no_report_bundle / "unsolved-no-report.solve.txt"
                ).is_file(),
                "initial debug-enabled failure did not write a solve report",
            )
            result = run(
                simulator,
                "-b",
                "--debug",
                "false",
                no_report_failure,
            )
            require(
                result.returncode != 0,
                "debug=false singular circuit unexpectedly solved",
            )
            require(
                (no_report_bundle / "unsolved-no-report.err").is_file(),
                "debug=false failure did not preserve the error log",
            )
            require(
                not (
                    no_report_bundle / "unsolved-no-report.solve.txt"
                ).exists(),
                "debug=false failure did not remove the stale solve report",
            )
            print("PASS solve failure preserves existing output")
        except Exception as exc:
            failures.append(str(exc))

        try:
            trailing = root / "trailing-after-end.cir"
            trailing.write_text(
                "Trailing statement\nR1 in 0 1k\n.op\n.end\nR2 in 0 2k\n"
            )
            result = run(simulator, trailing)
            require(result.returncode != 0, "statement after .end was accepted")
            print("PASS .end last-statement validation")
        except Exception as exc:
            failures.append(str(exc))

        try:
            unsupported = root / "unsupported.cir"
            unsupported.write_text(
                "Unsupported directive\nV1 in 0 1\nR1 in 0 1k\n.ac dec 10 1 1meg\n.end\n"
            )
            result = run(simulator, unsupported)
            require(result.returncode != 0, "unsupported directive was ignored")
            require("Unsupported control directive" in result.stderr, "missing diagnostic")

            malformed_cases = [
                (
                    "element-assignment",
                    "Bad resistor value\nR1 in 0 nonsense=1k\n.op\n.end\n",
                    "Invalid SPICE number",
                ),
                (
                    "source-assignment",
                    "Bad source value\nV1 in 0 nonsense=2\nR1 in 0 1k\n.op\n.end\n",
                    "Invalid SPICE number",
                ),
                (
                    "malformed-number",
                    "Bad numeric token\nR1 in 0 1..2\n.op\n.end\n",
                    "Invalid SPICE number",
                ),
                (
                    "print-without-tran",
                    "Missing tran\nR1 in 0 1k\n.print tran time\n.end\n",
                    ".print tran requires a .tran analysis",
                ),
                (
                    "malformed-model",
                    "Bad model\n.model DMOD D IS\nD1 in 0 DMOD\nR1 in 0 1k\n.op\n.end\n",
                    "Malformed model parameter",
                ),
                (
                    "unsupported-model-parameter",
                    "Unknown model parameter\n.model DMOD D TYPO=1\n"
                    "D1 in 0 DMOD\nR1 in 0 1k\n.op\n.end\n",
                    "Unsupported model parameter",
                ),
                (
                    "invalid-model-level",
                    "Unsupported model level\n.model NMOD NMOS LEVEL=2\n"
                    "M1 out out 0 0 NMOD W=1u L=1u\nR1 out 0 1k\n"
                    ".op\n.end\n",
                    "Only MOSFET LEVEL=1",
                ),
                (
                    "invalid-model-domain",
                    "Bad model domain\n.model DMOD D N=-1\n"
                    "D1 in 0 DMOD\nR1 in 0 1k\n.op\n.end\n",
                    "must be positive",
                ),
                (
                    "invalid-mos-ic",
                    "Bad MOS IC\n.model NMOD NMOS LEVEL=3\n"
                    "M1 out out 0 0 NMOD W=1u L=1u IC=0,0\n"
                    "R1 out 0 1k\n.op\n.end\n",
                    "MOSFET IC requires VDS, VGS, and VBS values",
                ),
                (
                    "invalid-mos-multiplier",
                    "Bad MOS multiplier\n.model NMOD NMOS LEVEL=3\n"
                    "M1 out out 0 0 NMOD W=1u L=1u M=0\n"
                    "R1 out 0 1k\n.op\n.end\n",
                    "MOSFET M must be positive",
                ),
                (
                    "invalid-area",
                    "Bad area\n.model DMOD D IS=1e-14\nD1 in 0 DMOD 0\n"
                    "R1 in 0 1k\n.op\n.end\n",
                    "area must be positive",
                ),
                (
                    "empty-circuit",
                    "No elements\n.op\n.end\n",
                    "requires at least one element",
                ),
                (
                    "ground-only-circuit",
                    "No MNA unknowns\nR1 0 gnd 1k\n.op\n.end\n",
                    "requires at least one non-ground node",
                ),
            ]
            for name, netlist, diagnostic in malformed_cases:
                malformed = root / f"{name}.cir"
                malformed.write_text(netlist)
                result = run(simulator, malformed)
                require(result.returncode != 0, f"{name} netlist was accepted")
                require(
                    diagnostic in result.stderr,
                    f"{name} diagnostic missing: {result.stderr}",
                )
            print("PASS unsupported and malformed input diagnostics")
        except Exception as exc:
            failures.append(str(exc))

        try:
            result = run(simulator, "--help")
            require(result.returncode == 0, "--help returned failure")
            require("Usage:" in result.stdout, "--help output is missing usage")
            result = run(simulator, "-o", "-r", valid)
            require(
                result.returncode != 0,
                "-o incorrectly accepted another option as its filename",
            )
            print("PASS command-line help")
        except Exception as exc:
            failures.append(str(exc))

    if failures:
        for failure in failures:
            print(f"FAIL {failure}")
        print(f"I/O summary: {7 - len(failures)}/7 checks passed")
        return 1

    print("I/O summary: 7/7 checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
