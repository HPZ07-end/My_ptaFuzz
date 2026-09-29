# Complex regression-case provenance

The 36 netlists in this directory are deterministic, newly authored stress
decks. They are **topology adaptations**, not verbatim copies of upstream
files. Each `.cir` file keeps one source-family comment; all remaining physical
lines are model, source, device, analysis, output, or termination statements.

This distinction is necessary because most upstream examples use features that
this simulator does not yet parse: `.include`, global `.param`, controlled
sources, mutual inductors, `PULSE`/`SIN`/`PWL`, higher-order semiconductor
models, and simulator control scripts. The adapted decks flatten repeated
blocks, use only `R/C/L/V/I/D/Q/M`, retain simple `.model` cards, and turn a
fixed DC source plus `UIC` into a step at the first integration step. TRAN case
05 intentionally omits `UIC` to exercise operating-point initialization.

## Upstream topology families

| Family in the generator | Upstream material | Adaptation used here |
| --- | --- | --- |
| `xyce_rlc` | [Xyce regression suite](https://github.com/Xyce/Xyce_Regression), especially its capacitor and inductor regression families | RL/RLC sections, filter banks, power-distribution meshes, and multiple branch-current unknowns |
| `ngspice_rc` | [ngspice User's Manual](https://ngspice.sourceforge.io/docs/ngspice-manual.pdf) and [official examples](https://github.com/ngspice/ngspice/tree/master/examples) | Loaded RC ladders, distributed RC lines, interconnect meshes, and output-time controls |
| `ngspice_logic` | ngspice manual differential-pair, CMOS, NAND, and four-bit-adder examples | Flattened Level-1 CMOS NAND stress meshes and BJT differential banks; no claim is made that a generated deck is a complete upstream adder |
| `qucs_filter` | [Qucs-S examples](https://github.com/ra3xdh/qucs_s/tree/master/share/qucs_s/examples) and its RCL/LC-filter examples | Passive bridge, RCL ladder, LC-filter, and sensor/interconnect mesh families |
| `pyspice_power` | PySpice examples for [rectification](https://pyspice.fabrice-salvaire.fr/releases/v1.5/examples/diode/rectification.html), [buck conversion](https://pyspice.fabrice-salvaire.fr/releases/v1.4/examples/switched-power-supplies/buck-converter.html), and [relay flyback protection](https://pyspice.fabrice-salvaire.fr/releases/v1.4/examples/relay/relay.html) | Diode clamps, reservoir/filter banks, and mixed power-path meshes |

Sandia describes the Xyce suite as a functional and defect-regression suite
with more than 2,000 tests; see the [official regression-suite guide](https://xyce.sandia.gov/documentation-tutorials/running-the-xyce-regression-suite/).

## Complexity contract

OP and TRAN each contain 18 cases with the same distribution:

| Level | Cases | Required physical lines | Generated physical lines |
| --- | ---: | ---: | --- |
| 1 | 4 | 10-20 | 12, 14, 16, 19 |
| 2 | 4 | 20-40 | 24, 28, 34, 39 |
| 3 | 5 | 40-100 | 48, 60, 72, 84, 98 |
| 4 | 5 | more than 100 | 120, 150, 190, 240, 340 |

Thus the largest OP and TRAN decks are both 340 physical lines. Blank lines and
comment padding are rejected by the audit script.

Recreate and audit the cases with:

```sh
python3 -B tests/scripts/generate_complex_cases.py
make test-cases
```

Reference listings under `tests/references/` are independently generated with ngspice
46 rather than this project's solver:

```sh
make generate-standards
```

For TRAN references, the generator injects a fixed high-accuracy ngspice policy
into a temporary copy of each deck: `reltol=1e-8`, `vntol=1e-10`,
`abstol=1e-12`, `trtol=1`, and an internal maximum step of `TSTEP / 2000`.
The source netlists remain unchanged. ngspice data is then linearly resampled
onto each deck's requested `.tran` output grid.
For a `UIC` deck only, the explicit `t=0` row follows this project's documented
all-zero initial-sample convention; every reference value at `t>0` comes from
ngspice.
