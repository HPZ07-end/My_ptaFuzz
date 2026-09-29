# Remote ngspice validation bundle

This bundle contains self-contained DC `.op` netlists selected from the local
SpiceFuzz/PTA runs. Exact duplicate netlist contents were removed by SHA-256.

## What to test first

1. `netlists/priority_pta_required` (22 netlists)
   - PTA simulator: NR-only failed.
   - PTA simulator: source stepping without PTA also failed.
   - PTA simulator: NR+PTA converged.
   - These are the highest-priority candidates for the claim "stock ngspice
     does not converge, PTA converges".
2. `netlists/secondary_nr_fail` (129 netlists)
   - PTA simulator: NR-only failed.
   - Source stepping and NR+PTA converged.
   - Stock ngspice may recover some of these through its own fallback methods,
     so they are secondary candidates.
3. `netlists/controls_nr_converged` (10 netlists)
   - PTA simulator converged with ordinary NR.
   - Use these as positive controls. If most controls fail remotely, check the
     ngspice build, supported model syntax, working directory, and timeout.

`manifest.json` records the SHA-256, original local classification, reward,
source run, and duplicate occurrence count for every exported file.

## Run on the remote Linux server

Requirements: Python 3 and a stock `ngspice` executable. The runner uses only
the Python standard library.

```bash
unzip ngspice_remote_validation_20260929.zip
cd ngspice_remote_validation_20260929
python3 run_ngspice.py --ngspice "$(command -v ngspice)" --timeout 60
```

If ngspice is not on `PATH`, pass its absolute path:

```bash
python3 run_ngspice.py --ngspice /absolute/path/to/ngspice --timeout 60
```

Outputs are written under `ngspice_results/`:

- `summary.tsv`: convenient table for inspection.
- `summary.json`: structured result data.
- one directory per case containing the ngspice log, raw output, stdout, and
  stderr.

The desired remote result is `status=convergence_failure`. Do not count
`invalid`, `timeout`, or `simulator_error` as evidence that PTA improves
convergence. A successful final corpus should satisfy both:

- stock ngspice: `convergence_failure`;
- the bundled manifest: priority or secondary PTA-success classification.

