# SpiceFuzz

The code and dataset for the paper "SpiceFuzz: LLM-Based Fuzzing for SPICE Circuit Simulator Tools Bug Detection", implemented in Pytorch. SpiceFuzz obtains circuits from the official website of the SPICE simulator and processes them to obtain a seed netlist library. Then, SpiceFuzz uses reinforcement learning to select mutations and constructs prompts with mutation rules and syntactic patterns. DeepSeek performs the requested mutations to obtain test netlists. Finally, the differences in results are discovered through differential testing on different solvers, and feedback is given to reinforcement learning or DeepSeek based on the simulation results.

## 1. Project Structure
- `data/`
	- `data/raw_data/ : circuits obtained from the official website of the SPICE simulator`
	- `data/seed_netlist/ : seed netlists processed by data_preparation.py`
- `result/ : results after testing `
- `scripts/`
	- `mutations.py : get mutation rules and syntax forms`
	- `reinforcement_learning.py : A2C algorithm`
	- `differential_testing.py : differential testing`
	- `extract_error.py : extract error information from log file`
- `data_preparation.py : extract netlist files from raw data `
- `main.py : a file for start`

## 2. Environment Setup

The code is tested with Python 3.10. All dependencies are listed in [requirements.txt](requirements.txt). And please ensure that the Ngspice or LTspice path is configured in the environment variable.

Configure DeepSeek before running a fuzzing entry point:

```powershell
$env:DEEPSEEK_API_KEY = "replace-with-your-real-key"
$env:DEEPSEEK_BASE_URL = "https://api.deepseek.com"
$env:DEEPSEEK_MODEL = "deepseek-flash"
```

`DEEPSEEK_BASE_URL` and `DEEPSEEK_MODEL` already have these defaults, so only
`DEEPSEEK_API_KEY` is required. The command-line equivalents are
`--deepseek_api_key`, `--deepseek_base_url`, and `--deepseek_model`.

## 3. Data preparation

The raw data is stored in the "data/raw_data" folder. Before starting SpiceFuzz, the compressed files should be extracted to the corresponding folder. Then start the data preparation code using the following command:
```
python data_preparation.py --raw_data_path ./data/raw_data/ngspice (regression) test circuits
```
Similar parameters can be configured through command above or directly modified at the beginning of the code. At the same time, you can also obtain the latest snapshot from the official website of [Ngspice](https://sourceforge.net/p/ngspice/ngspice/ci/master/tree/tests/) and [LTspice](https://www.analog.com/cn/resources/design-tools-and-calculators/ltspice-simulator/lt-spice-demo-circuits.html) and follow the steps above.

## 4. Usage

After obtaining the available netlist files through the previous step, you can then start SpiceFuzz using the following command：
```
python main.py --seed_dir ./data/seed_netlist/ngspice (regression) test circuits
```
Similar parameters can be configured through command above or directly modified at the beginning of the code. Finally, the results classified by symptoms can be viewed in the "result" folder.

### PTA-directed DC convergence fuzzing

The default PTA objective treats stock ngspice as the authoritative baseline.
A target is counted only when ngspice reports a convergence failure and PTA
reaches a steady-state solution. `--ngspice_bin` is therefore required:

```bash
python3 main_pta.py \
  --objective ngspice_differential \
  --ngspice_bin /absolute/path/to/ngspice \
  --pta_bin /absolute/path/to/PTA/Simulator/spice \
  --seed_dir ./data/seed_netlist/pta \
  --result_root ./result/pta_remote \
  --pta_output_root ./test/pta_remote \
  --workdir ./netlist_pta_remote
```

`pta_internal` is available only for diagnosing PTA's internal ablations. Its
results must not be reported as evidence that stock ngspice fails:

```bash
python3 main_pta.py --objective pta_internal --pta_bin /path/to/spice
```

In `ngspice_differential` mode, ngspice NR/gmin/source success receives a
negative reward. Only an ngspice convergence failure recovered by PTA receives
the differential, PTA-stage, and novelty bonuses. Signed rewards are also
propagated through the selected seed's actual mutation lineage; negative
feedback uses a shorter decay than positive target credit. Retained seeds keep
their ancestral state/action decisions, which are replayed as an independent
zero-discount A2C batch when a descendant is evaluated. True targets are always
retained with a higher sampling weight; gmin/source frontier candidates are
retained probabilistically to preserve exploration without flooding the pool.

## Citation

If you find the code useful, please cite our paper.
```
********
```

## Contact

For any questions, please drop an email to ********.
