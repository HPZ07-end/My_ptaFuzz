#!/usr/bin/env python3
"""Generate deterministic, parser-compatible OP and TRAN regression decks.

The upstream examples named below use a substantially larger SPICE language
than this project currently implements.  These decks therefore preserve their
topology *families* (ladders, filters, rectifiers, differential pairs and
flattened CMOS logic), while using only the primitive subset accepted by the
local parser.  No network access is needed when this script is run.

Only ``tests/cases/op/*.cir`` and ``tests/cases/tran/*.cir`` are replaced.
References are intentionally not generated here: they must come from an
explicitly chosen reference simulator or from a reviewed project baseline.
"""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


EXPECTED_PER_LEVEL = {1: 4, 2: 4, 3: 5, 4: 5}
LEVEL_RANGES = {
    1: (10, 20),
    2: (20, 40),
    3: (40, 100),
    4: (101, None),
}

SOURCE_FAMILIES = {
    "ngspice_rc": (
        "ngspice manual RC/transmission-line examples",
        "https://ngspice.sourceforge.io/docs/ngspice-manual.pdf",
    ),
    "ngspice_logic": (
        "ngspice manual differential-pair and four-bit NAND-adder examples",
        "https://ngspice.sourceforge.io/docs/ngspice-manual.pdf",
    ),
    "xyce_rlc": (
        "Xyce Regression RLC/filter test families",
        "https://github.com/Xyce/Xyce_Regression",
    ),
    "qucs_filter": (
        "Qucs-S RCL ladder and LC-filter example families",
        "https://github.com/ra3xdh/qucs_s",
    ),
    "pyspice_power": (
        "PySpice rectifier, buck and relay example families",
        "https://github.com/FabriceSalvaire/PySpice",
    ),
    "mixed": (
        "ngspice logic plus Xyce/Qucs-S RLC and PySpice power examples",
        "https://ngspice.sourceforge.io/docs/ngspice-manual.pdf",
    ),
}


@dataclass(frozen=True)
class CaseSpec:
    level: int
    index: int
    slug: str
    target_lines: int
    source_key: str
    flavor: str

    @property
    def filename(self) -> str:
        return f"level{self.level}_{self.index:02d}_{self.slug}.cir"


OP_SPECS = (
    CaseSpec(1, 1, "resistive_bridge_mesh", 12, "qucs_filter", "linear"),
    CaseSpec(1, 2, "loaded_resistor_ladder", 14, "ngspice_rc", "linear"),
    CaseSpec(1, 3, "diode_bias_clamp", 16, "pyspice_power", "diode"),
    CaseSpec(1, 4, "rl_supply_section", 19, "xyce_rlc", "rlc"),
    CaseSpec(2, 5, "rc_ladder_filter", 24, "ngspice_rc", "rc"),
    CaseSpec(2, 6, "diode_clamp_bank_dc", 28, "pyspice_power", "diode"),
    CaseSpec(2, 7, "bjt_differential_pair_mesh", 34, "ngspice_logic", "bjt"),
    CaseSpec(2, 8, "cmos_nand_mesh", 39, "ngspice_logic", "cmos"),
    CaseSpec(3, 9, "bridged_rc_mesh", 48, "qucs_filter", "rc"),
    CaseSpec(3, 10, "rlc_filter_bank", 60, "xyce_rlc", "rlc"),
    CaseSpec(3, 11, "diode_clamp_array", 72, "pyspice_power", "diode"),
    CaseSpec(3, 12, "bjt_differential_bank", 84, "ngspice_logic", "bjt"),
    CaseSpec(3, 13, "cmos_nand_logic_mesh", 98, "ngspice_logic", "cmos"),
    CaseSpec(4, 14, "resistive_sensor_matrix", 120, "qucs_filter", "linear"),
    CaseSpec(4, 15, "bjt_differential_stress_bank", 150, "ngspice_logic", "bjt"),
    CaseSpec(4, 16, "cmos_nand_stress_backplane", 190, "ngspice_logic", "cmos"),
    CaseSpec(4, 17, "mixed_rlc_clamp_backplane", 240, "xyce_rlc", "mixed"),
    CaseSpec(4, 18, "mixed_signal_stress_backplane", 340, "mixed", "mixed"),
)


TRAN_SPECS = (
    CaseSpec(1, 1, "rc_step_ladder", 12, "ngspice_rc", "rc"),
    CaseSpec(1, 2, "rl_step_section", 14, "xyce_rlc", "rlc"),
    CaseSpec(1, 3, "rcl_lowpass_step", 16, "qucs_filter", "rlc"),
    CaseSpec(1, 4, "diode_rc_charge", 19, "pyspice_power", "diode"),
    CaseSpec(2, 5, "rc_op_initialized_ladder", 24, "ngspice_rc", "rc"),
    CaseSpec(2, 6, "rlc_tstart_filter", 28, "xyce_rlc", "rlc"),
    CaseSpec(2, 7, "diode_reservoir_bank", 34, "pyspice_power", "diode"),
    CaseSpec(2, 8, "cmos_nand_load", 39, "ngspice_logic", "cmos"),
    CaseSpec(3, 9, "distributed_rc_line", 48, "ngspice_rc", "rc"),
    CaseSpec(3, 10, "multisection_lc_filter", 60, "qucs_filter", "rlc"),
    CaseSpec(3, 11, "diode_rc_stress_bank", 72, "pyspice_power", "diode"),
    CaseSpec(3, 12, "bjt_differential_transient", 84, "ngspice_logic", "bjt"),
    CaseSpec(3, 13, "cmos_nand_delay_mesh", 98, "ngspice_logic", "cmos"),
    CaseSpec(4, 14, "rc_interconnect_matrix", 120, "ngspice_rc", "rc"),
    CaseSpec(4, 15, "diode_rc_filter_bank", 150, "pyspice_power", "diode"),
    CaseSpec(4, 16, "cmos_nand_delay_backplane", 190, "ngspice_logic", "cmos"),
    CaseSpec(4, 17, "rlc_power_distribution", 240, "xyce_rlc", "rlc"),
    CaseSpec(4, 18, "mixed_signal_power_backplane", 340, "mixed", "mixed"),
)


class Deck:
    """Small helper that keeps generated element names unique and readable."""

    def __init__(self, title: str, source_key: str) -> None:
        family, url = SOURCE_FAMILIES[source_key]
        self.lines = [
            title,
            f"* Stress-mesh topology family adapted from {family}; primitive-only subset: {url}",
        ]
        self._counters: Counter[str] = Counter()
        self._device_names: set[str] = set()

    def model(self, name: str, kind: str, parameters: str) -> None:
        self.lines.append(f".model {name} {kind} {parameters}")

    def named(self, name: str, *fields: object) -> str:
        key = name.lower()
        if key in self._device_names:
            raise ValueError(f"duplicate generated device name: {name}")
        self._device_names.add(key)
        self.lines.append(" ".join((name, *(str(field) for field in fields))))
        return name

    def auto(self, prefix: str, *fields: object) -> str:
        self._counters[prefix] += 1
        name = f"{prefix}{self._counters[prefix]:03d}"
        return self.named(name, *fields)


@dataclass
class FillState:
    root: str
    output: str
    nodes: list[str]
    flavor: str
    branches: list[str]


def _seed_linear(deck: Deck, flavor: str, transient: bool) -> FillState:
    drive = "3.3" if transient else "2.5"
    deck.named("VDRV", "in", "0", "DC", drive)
    deck.named("RCORE1", "in", "out", "1.2k")
    deck.named("RCORE2", "out", "0", "2.7k")
    if flavor == "rc":
        deck.named("CCORE", "out", "0", "12n")
    return FillState("in", "out", ["in", "out"], flavor, ["VDRV"])


def _seed_rlc(deck: Deck, transient: bool) -> FillState:
    deck.named("VDRV", "in", "0", "DC", "3.3" if transient else "2.5")
    deck.named("RCORE1", "in", "lmid", "68")
    deck.named("LCORE", "lmid", "out", "120u")
    deck.named("CCORE", "out", "0", "22n")
    deck.named("RCORE2", "out", "0", "1.8k")
    return FillState(
        "in", "out", ["in", "lmid", "out"], "rlc", ["VDRV", "LCORE"]
    )


def _seed_diode(deck: Deck, transient: bool) -> FillState:
    deck.model("DMOD", "D", "IS=2e-14 N=1.03 GMIN=1e-10")
    deck.named("VDRV", "in", "0", "DC", "2.4" if transient else "1.8")
    deck.named("RCORE1", "in", "out", "820")
    deck.named("DCORE1", "out", "0", "DMOD")
    deck.named("DCORE2", "0", "out", "DMOD", "AREA=1.5")
    deck.named("RCORE2", "out", "0", "4.7k")
    if transient:
        deck.named("CCORE", "out", "0", "47n")
    return FillState("in", "out", ["in", "out"], "diode", ["VDRV"])


def _seed_bjt(deck: Deck, transient: bool) -> FillState:
    deck.model("NPNMOD", "NPN", "IS=1e-16 BF=120 BR=2 NF=1 NR=1 GMIN=1e-10")
    deck.named("VCC", "vcc", "0", "DC", "5")
    deck.named("VEE", "vee", "0", "DC", "-3")
    deck.named("VINP", "inp", "0", "DC", "0.04" if transient else "0.02")
    deck.named("VINN", "inn", "0", "DC", "0")
    deck.named("QCORE1", "out", "inp", "tail", "NPNMOD")
    deck.named("QCORE2", "qc2", "inn", "tail", "NPNMOD", "AREA=1.1")
    deck.named("RCORE1", "vcc", "out", "3.9k")
    deck.named("RCORE2", "vcc", "qc2", "3.9k")
    deck.named("RTAIL", "tail", "vee", "1.8k")
    deck.named("ROUT", "out", "0", "220k")
    if transient:
        deck.named("COUT", "out", "0", "18n")
    return FillState(
        "vcc", "out", ["vcc", "out", "qc2", "tail"], "bjt", ["VCC"]
    )


def _seed_cmos(deck: Deck, transient: bool) -> FillState:
    deck.model("NMOD", "NMOS", "LEVEL=1 VTO=0.7 KP=80u LAMBDA=0.02 GMIN=1e-10")
    deck.model("PMOD", "PMOS", "LEVEL=1 VTO=-0.7 KP=40u LAMBDA=0.02 GMIN=1e-10")
    deck.named("VDD", "vdd", "0", "DC", "5")
    deck.named("VA", "a", "0", "DC", "0.4")
    deck.named("VB", "b", "0", "DC", "3.1" if transient else "2.7")
    deck.named("MCOREP1", "out", "a", "vdd", "vdd", "PMOD", "W=20u", "L=1u")
    deck.named("MCOREP2", "out", "b", "vdd", "vdd", "PMOD", "W=20u", "L=1u")
    deck.named("MCOREN1", "out", "a", "stack", "0", "NMOD", "W=12u", "L=1u")
    deck.named("MCOREN2", "stack", "b", "0", "0", "NMOD", "W=12u", "L=1u")
    deck.named("RSTACK", "stack", "0", "1meg")
    deck.named("ROUT", "out", "0", "220k")
    if transient:
        deck.named("COUT", "out", "0", "20n")
    return FillState(
        "vdd",
        "out",
        ["vdd", "a", "b", "stack", "out"],
        "cmos",
        ["VDD"],
    )


def _seed_mixed(deck: Deck, transient: bool) -> FillState:
    deck.model("DMOD", "D", "IS=2e-14 N=1.04 GMIN=1e-10")
    deck.model("NPNMOD", "NPN", "IS=1e-16 BF=100 BR=2 GMIN=1e-10")
    deck.model("NMOD", "NMOS", "LEVEL=1 VTO=0.7 KP=70u LAMBDA=0.015 GMIN=1e-10")
    deck.model("PMOD", "PMOS", "LEVEL=1 VTO=-0.7 KP=35u LAMBDA=0.015 GMIN=1e-10")
    deck.named("VDD", "vdd", "0", "DC", "5")
    deck.named("VBIAS", "bias", "0", "DC", "0.62")
    deck.named("VIN", "in", "0", "DC", "1.7")
    deck.named("RFEED", "vdd", "rail", "47")
    deck.named("LFEED", "rail", "supply", "80u")
    deck.named("MPASS", "out", "in", "supply", "supply", "PMOD", "W=24u", "L=1u")
    deck.named("MNLOAD", "out", "bias", "0", "0", "NMOD", "W=10u", "L=1u")
    deck.named("QMON", "sense", "bias", "0", "NPNMOD")
    deck.named("DCLAMP", "sense", "0", "DMOD")
    deck.named("RSENSE", "out", "sense", "2.2k")
    deck.named("ROUT", "out", "0", "100k")
    deck.named("COUT", "out", "0", "33n")
    return FillState(
        "supply",
        "out",
        ["vdd", "rail", "supply", "out", "sense"],
        "mixed",
        ["VDD", "LFEED"],
    )


def _seed(deck: Deck, flavor: str, transient: bool) -> FillState:
    if flavor in {"linear", "rc"}:
        return _seed_linear(deck, flavor, transient)
    if flavor == "rlc":
        return _seed_rlc(deck, transient)
    if flavor == "diode":
        return _seed_diode(deck, transient)
    if flavor == "bjt":
        return _seed_bjt(deck, transient)
    if flavor == "cmos":
        return _seed_cmos(deck, transient)
    if flavor == "mixed":
        return _seed_mixed(deck, transient)
    raise ValueError(f"unknown topology flavor: {flavor}")


def _older_node(state: FillState, distance: int = 3) -> str:
    if len(state.nodes) <= distance:
        return state.root
    return state.nodes[-distance]


def _add_mesh_line(deck: Deck, state: FillState, step: int) -> None:
    """Add one real element while keeping every introduced node DC-connected."""

    phase = step % 8
    current = state.nodes[-1]

    if phase == 0:
        node = f"n{len(state.nodes):03d}"
        deck.auto("RF", current, node, f"{180 + 17 * (step % 19)}")
        state.nodes.append(node)
        return
    if phase == 1:
        deck.auto("RG", current, "0", f"{12 + step % 23}k")
        return
    if phase == 2:
        if state.flavor in {"rc", "rlc", "mixed"}:
            deck.auto("CF", current, "0", f"{2 + step % 13}n")
        else:
            deck.auto("RB", current, _older_node(state), f"{3 + step % 11}k")
        return
    if phase == 3:
        deck.auto("RB", current, _older_node(state, 5), f"{5 + step % 17}k")
        return
    if phase == 4 and state.flavor in {"rlc", "mixed"}:
        node = f"l{len(state.nodes):03d}"
        deck.auto("LF", current, node, f"{30 + 5 * (step % 17)}u")
        state.nodes.append(node)
        return
    if phase == 5 and state.flavor in {"diode", "mixed"}:
        # Reverse-oriented clamps keep the large regressions numerically mild.
        deck.auto("DF", "0", current, "DMOD", f"AREA={1 + step % 3}")
        return
    if phase == 6 and state.flavor in {"cmos", "mixed"}:
        deck.auto("MF", current, "0", "0", "0", "NMOD", "W=8u", "L=1u")
        return
    if phase == 7 and state.flavor == "bjt":
        deck.auto("QF", current, "0", "0", "NPNMOD", "AREA=1")
        return

    # Linear fallbacks are intentional: they provide a DC path around every
    # reactive/nonlinear branch and improve conditioning of the large decks.
    deck.auto("RX", current, state.root, f"{18 + step % 29}k")


def _fill(deck: Deck, state: FillState, count: int) -> None:
    if count < 0:
        raise ValueError("case core is larger than its requested line budget")
    for step in range(count):
        _add_mesh_line(deck, state, step)


def _tran_directive(spec: CaseSpec) -> str:
    # Case 5 deliberately exercises OP initialization; all other decks use a
    # fixed DC source with UIC, which is a step from the zero initial solution.
    if spec.index == 5:
        return ".tran 1u 10u 0 500n"
    if spec.index in {6, 10, 14}:
        return ".tran 1u 12u 2u 500n UIC"
    if spec.index in {2, 7, 11, 15, 18}:
        return ".tran 2u 12u 0 1u UIC"
    if spec.level == 4:
        return ".tran 2u 10u 0 2u UIC"
    return ".tran 1u 10u 0 1u UIC"


def _footer(spec: CaseSpec, analysis: str, state: FillState) -> list[str]:
    node_candidates = [state.root, state.output]
    if state.nodes:
        node_candidates.extend(
            state.nodes[index]
            for index in (
                len(state.nodes) // 3,
                (2 * len(state.nodes)) // 3,
                len(state.nodes) - 1,
            )
        )
    selected_nodes = list(dict.fromkeys(node_candidates))
    quantities = [f"v({node})" for node in selected_nodes]
    quantities.extend(f"i({branch})" for branch in state.branches)

    if analysis == "op":
        directive = ".op"
        printed = ".print op " + " ".join(quantities)
    else:
        directive = _tran_directive(spec)
        printed = ".print tran time " + " ".join(quantities)
    return [directive, printed, ".end"]


def _build_case(spec: CaseSpec, analysis: str) -> list[str]:
    title = (
        f"{analysis.upper()} level {spec.level} case {spec.index:02d}: "
        f"{spec.slug.replace('_', ' ')}"
    )
    deck = Deck(title, spec.source_key)
    state = _seed(deck, spec.flavor, transient=analysis == "tran")
    footer = _footer(spec, analysis, state)
    _fill(deck, state, spec.target_lines - len(deck.lines) - len(footer))
    deck.lines.extend(footer)

    if len(deck.lines) != spec.target_lines:
        raise AssertionError(
            f"{spec.filename}: expected {spec.target_lines} lines, "
            f"generated {len(deck.lines)}"
        )
    if any(not line or line != line.strip() for line in deck.lines):
        raise AssertionError(f"{spec.filename}: blank or padded physical line")
    if sum(line.startswith("*") for line in deck.lines) != 1:
        raise AssertionError(f"{spec.filename}: comments must not pad the line tier")
    return deck.lines


def _in_level_range(level: int, lines: int) -> bool:
    lower, upper = LEVEL_RANGES[level]
    return lines >= lower and (upper is None or lines <= upper)


def _validate_specs(specs: Iterable[CaseSpec], analysis: str) -> tuple[CaseSpec, ...]:
    result = tuple(specs)
    if len(result) != 18:
        raise AssertionError(f"{analysis}: expected 18 specifications")
    if Counter(spec.level for spec in result) != Counter(EXPECTED_PER_LEVEL):
        raise AssertionError(f"{analysis}: invalid level distribution")
    if sorted(spec.index for spec in result) != list(range(1, 19)):
        raise AssertionError(f"{analysis}: case indices must be 01 through 18")
    if len({spec.filename.lower() for spec in result}) != len(result):
        raise AssertionError(f"{analysis}: duplicate case filename")
    for spec in result:
        if not _in_level_range(spec.level, spec.target_lines):
            raise AssertionError(
                f"{analysis}/{spec.filename}: {spec.target_lines} lines do not "
                f"match level {spec.level}"
            )
    if max(spec.target_lines for spec in result) <= 300:
        raise AssertionError(f"{analysis}: most complex case must exceed 300 lines")
    return result


def _render_all() -> dict[tuple[str, str], str]:
    rendered: dict[tuple[str, str], str] = {}
    for analysis, raw_specs in (("op", OP_SPECS), ("tran", TRAN_SPECS)):
        for spec in _validate_specs(raw_specs, analysis):
            lines = _build_case(spec, analysis)
            rendered[(analysis, spec.filename)] = "\n".join(lines) + "\n"
    if Counter(analysis for analysis, _ in rendered) != Counter(op=18, tran=18):
        raise AssertionError("generator must produce exactly 18 OP and 18 TRAN decks")
    return rendered


def generate(repo_root: Path) -> None:
    # Render and validate everything before touching the existing case tree.
    rendered = _render_all()
    testcase_root = repo_root / "tests" / "cases"
    for analysis in ("op", "tran"):
        directory = testcase_root / analysis
        directory.mkdir(parents=True, exist_ok=True)
        for old_case in directory.glob("*.cir"):
            old_case.unlink()
        for (kind, filename), text in sorted(rendered.items()):
            if kind == analysis:
                (directory / filename).write_text(text, encoding="utf-8")

    print("Generated 18 OP and 18 TRAN cases under", testcase_root)
    print("Level distribution per analysis: 4 / 4 / 5 / 5")
    print("Largest case per analysis: 340 physical lines")


def _parse_args() -> argparse.Namespace:
    default_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=default_root,
        help="repository root containing tests/cases/ (default: repository root)",
    )
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    generate(args.root.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
