# SPICE-like Circuit Simulator

这是一个基于 C++17 和 Eigen 的类 SPICE 电路仿真器。目前支持 DC operating point、基础 transient analysis 最小闭环，以及实验性的 pseudo-transient analysis (PTA) 路径，使用稀疏 MNA、SparseLU、Newton 迭代、步长限制和 source stepping 完成求解。瞬态分析以 Backward Euler 启动，并在具备足够已接受历史后采用可变步长 BDF2；BDF2 步使用严格的局部截断误差（LTE）估计控制步长。这里的 TRAN 是可运行的 MVP，不等同于完整 SPICE 瞬态引擎。

本项目实现的是明确受限的 SPICE 子集，并非完整 ngspice 替代品。I/O 层遵循常见 SPICE netlist、`.print` listing 和 ASCII rawfile 约定，未支持的控制卡或输出表达式会直接报错，不会静默忽略。

## PTA research-beta-v2.2 实验基线

`research-beta-v2.2` 是后续 PTA 实验采用的新基线，取代 `research-beta-v2.1`。本基线修复了 compound PTA 在 `fallback` 模式下提前扩展 MNA 的生命周期缺陷：普通 OP 和 source stepping 现在只使用物理电路的未知量与稀疏结构；二者均失败后，求解器才创建 PTA 伪元件、分配 PTA-only branch unknown、重建并重新绑定 MNA，然后迁移已有物理状态进入 PTA。由此，`fallback` 的普通求解阶段与 `--pta disabled` 使用同一数学系统，不再因未盖章的 PTA-only 未知量发生第一次稀疏分解失败。

此前的 `research-beta-v2` 和 `research-beta-v2.1` tag 保持不动，分别保留归一化 trust-region 初始版本及 seeded PTA 检查点恢复版本。所有 research tag 均视为不可移动的实验快照；研究结论必须记录所用 Git commit/tag、输入网表、完整配置、命令行及对应的 `.solve.txt` 与 `.pta.jsonl`。

该项目是可复现的研究原型，而不是生产级 SPICE 发布。建议对每个困难电路同时记录 ordinary、`--pta force` 和 `--pta fallback` 的结果，区分“普通路径自然失败后 PTA 恢复”与“普通路径因实现缺陷失败”。默认回归通过只能证明受覆盖范围内的实现一致性；跨拓扑、跨参数尺度的鲁棒性和性能结论仍须由后续实验建立。

### research-beta-v2.2 验证记录

本基线在本地 `-O3` 构建下通过完整 `make test`：PTA/TRAN 数值单元 243/243、core 81/81、config 71/71、I/O 7/7；OP 18/18（76 个参考值）、TRAN 18/18（863 个参考值）、MOS3 OP 4/4 与 TRAN 1/1、递归网表解析 48/48。困难 PTA fallback 专项回归也通过，并新增 compound PTA 端到端检查，保证普通 OP 成功时伪元件数保持为零。

UA741PFBx10 使用迁移后的无 `.pstran` 网表及等效 `fallback` 配置验证：`--pta disabled` 的 direct Newton 运行 6 次后因 trust-region 重试耗尽失败，source stepping 接受 13 步、拒绝 18 步后失败；`fallback` 的普通阶段得到完全相同的迭代轨迹，随后才扩展为 698 个 MNA 未知量和 235 个 PTA 伪元件，并由 PTA 以 470 个接受步、27 个拒绝步、60718 次局部 Newton 迭代收敛。该私有网表没有独立 ngspice 数值参考，因此这里只把它作为“普通路径自然失败、PTA 恢复”的可重放收敛证据，不据此宣称模型精度或性能普适性。

## 代码结构

- `src/app/`：程序入口和命令行解析。`main.cpp` 只负责编排配置、网表、求解与输出流程。
- `src/circuit/`：电路构建和求解调度；OP、TRAN、PTA 与公共 Newton 求解分别位于 `operating_point_solver.cpp`、`transient_solver.cpp`、`pta_solver.cpp` 和 `newton_solver.cpp`。
- `include/analysis/`：分析配置、积分策略和类型化诊断数据，不负责电路拓扑。
- `include/solver/`：MNA、Newton 步长和非线性电压限制等求解基础设施。
- `include/devices/`、`src/devices/`：器件接口与实现。MOSFET 实例配置放在 `devices/mosfet/`，Level-3 的 DC、charge 和 configuration 实现集中在 `devices/mosfet/level3/`。
- `include/models/`：模型卡与模型缓存；MOSFET Level-3 模型卡单独位于 `models/mosfet/level3_model_card.hpp`。
- `src/netlist/`、`include/netlist/`：词法读取、SPICE 数值/赋值语法、子电路展平，以及控制卡、模型和器件构造。
- `src/config/`：`config.json` 定位、严格 schema 解析、覆盖应用与 CLI `name=value` 解析；文件名直接体现 parser、applier 或 CLI 职责。
- `src/io/spice_output.cpp`：listing、ASCII rawfile 和 PTA diagnostic 格式化。
- `src/io/output_files.cpp`：结果路径校验、临时文件、原子提交与失败回滚。
- `src/io/solver_report.cpp`：输出电路规模、方法链、迭代/步长统计、有效配置和调参提示。
- `third_party/nlohmann/json.hpp`：随仓库固定版本的 header-only JSON 解析器。

项目自有 C++ 头文件统一使用 `.hpp`，实现文件统一使用 `.cpp`；项目内 include 均从 `include/` 根目录书写，避免目录移动后相对路径失效。这种分层使网表语法、器件模型、求解策略和文件事务可以独立演进。

## 已支持功能

### 器件

| 前缀 | 器件 | OP | TRAN |
| --- | --- | --- | --- |
| `R` | Resistor | 电导 stamp | 电导 stamp |
| `C` | Capacitor | 开路 | Backward Euler / BDF2 companion model |
| `L` | Inductor | 0 V 支路 | Backward Euler / BDF2 branch equation |
| `V` | Independent voltage source | MNA branch | 当前仅支持固定 DC 值 |
| `I` | Independent current source | RHS stamp | 当前仅支持固定 DC 值 |
| `D` | Diode | 指数模型与 Newton 线性化 | 静态非线性模型 |
| `Q` | NPN / PNP BJT | 简化 Ebers-Moll 风格模型 | 静态非线性模型 |
| `M` | NMOS / PMOS | 简化 Level-1 平方律模型 | 静态非线性模型 |

`.model` 支持 `D`、`NPN`、`PNP`、`NMOS`、`PMOS`、`NCH`、`PCH`。当前求解方程使用的模型参数包括：

- Diode：`IS`, `N`, `VT`, `GMIN`、`RS`
- BJT：`IS`, `BF` / `BETA`, `BR`, `NF`, `NR`, `VT`, `GMIN`，以及 DC Gummel-Poon 子集 `RB`、`RC`、`RE`、`RBE`、`RCE`、`VA` / `VAF`、`VAR`、`IKF`、`IKR`、`ISE`、`ISC`、`NE`、`NC`
- MOSFET：`LEVEL=1` 的简化平方律；以及 ngspice `LEVEL=3` 的 DC channel-current 子集（有效几何、body effect、短沟道、迁移率退化、速度饱和、沟道长度调制及 `gm/gds/gmb`）
- 实例参数：`AREA`, `W`, `L`，以及 MOS3 的 `AD`、`AS`、`PD`、`PS`、`NRD`、`NRS`、`M`、`OFF`、`IC`、`TEMP`

`RS`、`RBE`、`RCE` 与 `RDS` 已落实到 DC / transient 的器件 stamp：Diode `RS` 是由外部阳极到本征二极管阳极的串联电阻（按 `AREA` 缩放）；BJT `RBE` / `RCE` 分别是本征 B-E / C-E 并联电阻（按 `AREA` 缩放）；Level-1 MOS 的 `RDS` 是按 `W/L` 缩放的 D-S 线性并联电导。MOS3 的 `TOX`、`LD`、`XL`、`WD`、`XW`、`UO`、`VTO`、`KP`、`GAMMA`、`PHI`、`NSUB`、`ETA`、`DELTA`、`THETA`、`VMAX`、`KAPPA` 已参与 DC channel stamp；`RD`/`RS`（或 `RSH*NRD/NRS`）会建立内部 D′/S′ 节点，B-D′/B-S′ 结电流按 `JS*AD/AS` 或 `IS` 参与 DC stamp，结电容、`CGSO`/`CGDO`/`CGBO` 重叠电容和 Meyer 本征栅电容均以瞬态 companion stamp 接入。Meyer 栅电荷和 B-D/B-S 结电荷在每个已接受时间点保存并积分；含 MOS3 的 UIC 使用 BE step-doubling，避免零初值伪造 BDF2 电荷历史。器件温度仍待实现。未知参数、非数值参数、非物理的负值以及非 `LEVEL=1` / `LEVEL=3` 的 MOSFET model 会在读取阶段明确报错。

### Netlist 读取规则

- 第一物理行作为 circuit title；`.title ...` 可以覆盖它。
- 元件名、模型名、节点名和控制卡大小写不敏感。
- `0` 是 ground；额外接受 `gnd` 作为别名。
- 整行注释使用 `*`；行尾注释支持 `;`、`$` 和 `//`。
- `#` 不是注释符，因为 SPICE branch vector 会使用 `v1#branch` 形式。
- `+` 开头的逻辑续行会拼接到上一条语句；孤立续行会报告文件名和行号。
- 支持 `.subckt name [pin...] [params: name=value ...]` / `.ends [name]` 与 `X` 子电路实例；解析时会递归展平为现有 primitive 元件。`X` 实例可用 `PARAMS:`（可省略）后的 `name=value` 覆盖默认参数；参数值支持 `{...}` 包裹的 `+`、`-`、`*`、`/`、`^` 表达式和对上层参数的引用，并会在展平时落实为数值 token。子电路当前支持 `R`、`C`、`L`、`V`、`I`、`D`、`Q`、`M`、嵌套 `X`，不支持子电路体内的控制卡。
- `.end` 必须存在、不接受参数，并且必须是最后一个非注释语句。
- 支持数值后缀 `a`, `f`, `p`, `n`, `u`, `m`, `mil`, `k`, `meg`, `g`, `t`；其中 `m` 表示 milli，mega 必须写成 `meg`。
- 独立源接受 `5`、`DC 5`、`DC=5`、`DC= 5` 和 `DC = 5`；当前不接受任意 `key=value` 代替 DC 值。
- 数值 token 会完整校验，`1..2`、`1k=2` 等畸形写法不会只读取前缀后继续运行。
- 网表必须至少包含一个元件和一个非 ground 节点，避免把零维 MNA 系统送入求解器。
- 当前支持的控制卡为 `.title`、`.model`、`.subckt` / `.ends`、`.op`、`.tran`、`.pstran`、`.nodeset`、`.ic`、`.option` / `.options`、`.print` 和 `.end`。其他 dot command 会明确报错。

### 分析与 `.print`

支持：

```spice
.op
.tran TSTEP TSTOP [TSTART [TMAX]] [UIC]
.options delmax=...
.pstran convval=... initstep=... minstep=... maxstep=... [tau=... vbe0=... kvgs0=... tauramp=...]
.print op v(node) v(node1,node2) i(device)
.print tran v(node) v(node1,node2) i(device)
.nodeset v(node[,reference])=value
.ic v(node[,reference])=value
```

- `v(node1,node2)` 在输出层计算差分电压，不改变 MNA 方程。
- transient listing 自动把 `time` 放在第一列；`.print tran time ...` 也可以接受。
- 多条相同分析类型的 `.print` 会按出现顺序合并并去重。
- 没有 `.print` 时，默认输出所有非 ground 节点电压和所有 branch unknown 电流。
- `i(device)` 当前只适用于具有 branch unknown 的器件，即独立电压源和电感；请求其他器件电流会得到明确错误。
- 没有分析卡时默认执行 `.op`。同时存在 `.op` 与 `.tran` 时依次输出两个分析块。
- `.tran` 未指定 `UIC` 时先求 operating point；`.nodeset` 是该 OP 的初值提示，`.ic` 与器件 `IC=` 是较强的初值提示。指定 `UIC` 时不求 OP，而以 `.ic` 和 C/L/BJT/MOS 的 `IC=` 构造 t=0 状态；没有显式初值时才使用全零 MNA 状态。矛盾的显式电压条件会报错。
- `.option DELMAX=value`（也接受 `.options`）是 HSPICE 兼容的内部时间步长硬上限，数值接受 SPICE 后缀。ngspice 的标准、可移植写法是 `.tran` 的第四个 `TMAX` 参数；两者同时出现时取更小者，确保每个内部积分步都不超过任一上限。该限制也应用于 `.pstran` 的伪时间步，且不改变 `.op`。ngspice 当前可接受 `DELMAX` 这个非标准 option 名称，但不将其列为通用 `.options` 变量；本程序刻意实现其 HSPICE 语义，而非静默忽略。
- `.pstran` 启用 PTA operating-point 求解，接受不区分大小写的 `convval`、`initstep`、`minstep`、`maxstep`、`tau`、`vbe0`、`kvgs0`、`tauramp` 参数，且可使用 `key=value`、`key = value` 或 `key= value` 写法。`convval` 映射为 PTA 的导数和 DC 残差阈值，三个 step 参数映射为 PTA 步长边界；`tau` 启用复合伪元件，`tauramp` 控制独立源斜坡。`vbe0` 和 `kvgs0` 分别为 BJT `VBE` 与 MOS `VGS` 的初始、极性归一化 Newton 限幅器种子；MOS 同时以 `VGD=0` 建立同一初始限幅状态。它们只约束第一轮非线性更新幅度，不写入或钳位任何共享电路节点；对 PMOS，`kvgs0` 仍使用正的器件本征 `VGS` 约定。`.pstran` 与 `--pta` 或 `pta.mode` 同时指定时会静默保留网表的 `force` 模式。
- `TSTEP` 控制输出间隔，`TSTART` 控制开始保存的时间，`TMAX` 限制内部积分步长。当前未指定 `TMAX` 时内部最大步长使用 `TSTEP`；输出时间点也会强制成为积分点，因此与 ngspice 的默认自适应步长策略不同。
- 每次瞬态分析在 BDF2 严格 LTE 历史尚不足时使用 Backward Euler step-doubling；随后在新步长不大于前一步两倍时使用可变步长 BDF2。严格 LTE 由候选点与三个已接受状态构成的非均匀网格三阶差商给出导数缺陷，再以收敛端点的 MNA Jacobian 投影成状态误差；电压未知量使用 `voltage_absolute_tolerance`，branch-current 未知量使用 `current_absolute_tolerance`，并结合 `relative_tolerance` 归一化。BDF2 步长按三次根误差律缩放，并对增长幅度施加保守上限。超过误差预算或 Newton 未收敛的步会回滚并缩小后重试；内部积分点仍不会越过输出时间点。

### 实验性 PTA

命令行可选择 OP 求解路径：

```sh
./spice --pta disabled input.cir  # 默认：Newton + source stepping
./spice --pta force input.cir     # 只使用 PTA
./spice --pta fallback input.cir  # 常规 OP 失败后尝试 PTA

# 使用 SPICE 数值后缀覆盖 PTA 配置；可重复指定
./spice --pta force \
  --pta-option initial-step=1n \
  --pta-option maximum-steps=20000 \
  --pta-option successful-step-scale=1.5 \
  --pta-option medium-oscillation-ratio=0.5 \
  --pta-option heavy-oscillation-ratio=1.0 \
  --pta-option include-diodes=true \
  input.cir
```

`--pta-option name=value` 只可与 `--pta force` 或 `--pta fallback` 一起使用。数值选项接受与网表相同的 SPICE 后缀；布尔选项 `include-mos-bulk` 与 `include-diodes` 接受 `true` / `false` 或 `1` / `0`。可用名称为：

- 时间与收敛：`initial-step`、`minimum-step`、`maximum-step`、`maximum-steps`、`derivative-tolerance`、`derivative-relative-tolerance`、`derivative-voltage-absolute-tolerance`、`derivative-current-absolute-tolerance`、`dc-residual-tolerance`、`dc-residual-relative-tolerance`、`dc-voltage-absolute-tolerance`、`dc-current-absolute-tolerance`
- 伪元件：`initial-node-capacitance`、`minimum-node-capacitance`、`maximum-node-capacitance`、`current-source-capacitance`、`voltage-source-inductance`
- 非线性限幅器种子：`initial-mos-vgs`、`initial-bjt-vbe`（均可设为 `null` 以清空）
- 自适应规则：`failed-step-scale`、`successful-step-scale`、`capacitance-grow-scale`、`small-oscillation-scale`、`medium-oscillation-scale`、`heavy-oscillation-scale`、`medium-oscillation-ratio`、`heavy-oscillation-ratio`
- 放置开关：`include-mos-bulk`、`include-diodes`

同一 PTA 选项不可重复指定；所有覆盖值会在建模前统一执行配置校验，非法范围或相互矛盾的边界会以命令行错误退出。

PTA 在 MNA pattern 固化前加入人工伪元件：独立电压源 branch 上的伪电感、独立电流源两端的伪电容，以及晶体管节点到地的伪电容。`fallback` 模式先只构建物理 MNA；普通 OP 与 source stepping 均失败后，才创建伪元件、扩展未知量、重建稀疏结构并迁移已有物理状态，因此其普通求解路径与 `disabled` 模式一致。其伪时间迭代复用现有的 Backward Euler / 受步长比限制的 BDF2 `TransientIntegrator`；每一步以归一化 BDF 导数和归一化原始 OP 残差共同判定稳态。导数指标为 `h*|dx/dt| / (abstol + reltol*scale)`；残差指标为 `|Ax-b| / (abstol + reltol*max(|Ax|, |b|))`。二者都会区分节点 KCL 行的电流绝对容差与电压源支路行的电压绝对容差，`derivative-tolerance` 和 `dc-residual-tolerance` 是对应的无量纲阈值。使用 `--pta-diagnostics` 可将 PTA 是否实际执行、收敛指标、局部增容次数、节点降容次数和最小步长恢复次数写至 stderr，并同步保存在结果目录的 `.err` 中；每次尝试还会记录伪时间区间、步长、BE/BDF2 阶数、Newton 迭代/阻尼次数、导数/残差，以及缩步、最小步局部增容重启和振荡降容的决定与原因。同一人类可读轨迹也写入 `.solve.txt`。带 BJT/MOS limiter seed 的 PTA 在任一伪时间点耗尽 trust-region 重试时，会从同一检查点重新执行其非单调 Newton 策略；该恢复仍失败才缩小伪时间步，随后才可能在最小步长按 PTA KCL 残差选择一个仍有余量的节点伪电容并重启积分历史。未使用 limiter seed 的 PTA 保持 trust-region 路径，以保留连续跟踪到的稳定支路。每个成功步后，伪时间步会按 `successful-step-scale` 增长，同时仍受最大步长和 BDF2 步长比限制；节点电压变化反向时会按相邻步变化幅度比降低该节点伪电容，两个 ratio 参数分别划分小/中及中/重振荡。

该功能仍处于实验阶段。自适应规则具有单元测试，并有一条端到端夹具覆盖“最小步长失败 → 按残差选择单节点增容 → 重启 → 恢复收敛”路径及后续节点降容。当前 Force 与 Fallback OP 回归覆盖 18 个网表、76 个输出值，并包含一个多稳态困难锁存器。归一化导数收敛已避免固定绝对阈值随伪时间步和未知量量级失真的问题；其默认容差与困难非线性电路的鲁棒性仍需更广泛的基准验证。因此 `force` 与 `fallback` 可用于回归和实验，但暂不视为生产求解保证。

## 输出格式

每次非 `--parse-only` 运行都会创建一个去掉网表最后扩展名的同名目录，并生成 `.out`、`.raw`、`.err` 以及默认启用的 `.solve.txt` artifact bundle；实际执行 PTA 时还会生成独立的 `.pta.jsonl` 轨迹。例如输入 `circuits/amplifier.cir`：

```text
circuits/
  amplifier.cir
  amplifier/
    amplifier.out
    amplifier.raw
    amplifier.err
    amplifier.solve.txt
    amplifier.pta.jsonl  # 仅 PTA 实际执行时
```

使用 `--output-root results` 时，目录改为 `results/amplifier/`，其中的文件名保持不变。`--parse-only` 只校验输入，不创建结果目录。`-b` / `--batch` 禁止向 stdout 回显 listing，但不影响 artifact。位置输出参数、`-o` 和 `-r` 仍可用于额外生成兼容的 listing/rawfile 镜像；规范结果始终写入上述同名目录。

### SPICE listing

同名 `.out` 始终生成；非 batch 且未指定额外 listing 镜像时也会回显到 stdout。`.print` 决定 listing 的变量和顺序：

```text
Circuit: Level 1 - RC step with UIC

Transient Analysis
No. of Data Rows : 11

----------------------------------------------------------------------------------------
Index   time                v(in)               v(out)              v1#branch
----------------------------------------------------------------------------------------
       0    0.0000000000e+00    0.0000000000e+00    0.0000000000e+00    0.0000000000e+00
       1    1.0000000000e-04    5.0000000000e+00    4.5454545455e-01   -4.5454545455e-03
```

branch current 在 listing 中使用 ngspice 常见的 `device#branch` 列名。

### ASCII rawfile

同名 `.raw` 始终生成。rawfile 不受 `.print` 过滤，包含全部节点电压和全部 branch unknown 电流；瞬态的第一个 variable 固定为 `time`。

```text
Title: Level 1 - RC step with UIC
Date: Tue Jul 14 12:00:00 2026
Plotname: Transient Analysis
Flags: real
No. Variables: 4
No. Points: 11
Variables:
	0	time	time
	1	v(in)	voltage
	2	v(out)	voltage
	3	i(v1)	current
Values:
 0	0.000000000000000e+00
	0.000000000000000e+00
	0.000000000000000e+00
	0.000000000000000e+00
```

### 错误日志与求解报告

- `.err` 保存本次运行写到 stderr 的内容；成功且未请求额外诊断时通常为空。诊断仍会同步显示在终端。
- `.solve.txt` 默认启用，保存运行状态、总墙钟时间、电路器件构成、节点与 MNA 规模、当前解幅值范围、实际求解方法链、每阶段迭代/阻尼/失败原因、source stepping 每次尝试、PTA 每个伪时间步及收敛指标、TRAN 接受/拒绝步统计、CPU/墙钟用时、最终生效配置和基于诊断指标生成的调参观察项。使用 `--debug false` 或配置文件根字段 `"debug": false` 会关闭本次运行的报告写入，并在提交本次 artifact 时移除该 bundle 中已有的旧 `.solve.txt`；`.out`、`.raw` 和 `.err` 仍照常生成。
- `.pta.jsonl` 仅在 PTA 实际执行时写出，且不受 `debug` 开关影响。第一行保存 schema、输入路径、配置来源、完整 PTA/ Newton 配置快照与 FNV-1a 哈希；随后每行对应一次 PTA 尝试；最后一行记录结束状态、聚合统计以及最终节点电压和 branch current。未执行 PTA 的运行会在同一事务中移除该 bundle 中可能残留的旧轨迹。
- Fallback 报告会保留完整链路，例如 `direct Newton failed -> source stepping failed -> adaptive PTA succeeded`，不会只保留最后一次 PTA 结果。

输出数值使用 classic locale、科学计数法，并拒绝写出 NaN/Inf。成功运行先在内存中生成结果，再将 `.out/.raw/.err`、PTA 实际执行时的 `.pta.jsonl` 和（启用 debug 时）`.solve.txt` 作为一组事务提交；关闭 debug 时，同一事务会移除旧 `.solve.txt`。任一暂存、替换或移除失败都会回滚。解析、构建或求解失败时只更新 `.err`、PTA 实际执行时的轨迹和（启用时）`.solve.txt`，或在关闭 debug 时移除旧报告；已有的有效 `.out/.raw` 保持不变。input、五个规范 artifact 和兼容 listing/raw 镜像不能通过规范路径、符号链接或硬链接相互指向同一文件。

## 构建与运行

依赖：

- C++17 编译器
- Eigen3
- Python 3（测试脚本）
- Make
- ngspice 46（仅在重新生成独立参考结果时需要；普通构建与 `make test` 不需要）

macOS 可安装 Eigen：

```sh
brew install eigen
```

构建：

```sh
make
```

运行并在网表旁生成同名结果目录：

```sh
./spice tests/cases/op/level1_01_resistive_bridge_mesh.cir
./spice -b tests/cases/op/level1_01_resistive_bridge_mesh.cir
./spice -b --output-root results \
  tests/cases/tran/level1_01_rc_step_ladder.cir
./spice --parse-only tests/private/UA741PFBx10.sp
```

额外生成旧式平铺镜像：

```sh
./spice -b -o result.out -r result.raw tests/cases/tran/level1_01_rc_step_ladder.cir
```

### 配置文件发现与校验

程序可读取名为 `config.json` 的 JSON 配置文件，用于注入 OP、PTA 和 TRAN 的求解参数，并控制是否输出详细求解报告。不存在配置文件时，全部默认值与此前保持一致。网表控制卡中显式给出的同名参数始终优先于配置文件。

默认会从进程的当前工作目录开始查找 `config.json`，再逐级查找父目录；默认最多向上查找 8 个父目录。找到最近的文件后停止搜索。注意搜索起点是启动 `spice` 时的工作目录，而不是网表文件所在的目录。

```sh
# 自动搜索，并打印实际使用的配置文件（写到 stderr）
./spice --print-config-path tests/cases/op/level1_01_resistive_bridge_mesh.cir

# 只读取指定配置文件；指定路径不存在或不可读会以参数错误退出
./spice --config ./config.json \
  --print-config-path \
  tests/cases/op/level1_01_resistive_bridge_mesh.cir

# 只检查当前工作目录，不查询父目录
./spice --config-search-depth 0 \
  --print-config-path \
  --parse-only tests/private/UA741PFBx10.sp
```

- `--config <path>`：只读取该路径，不再自动搜索。相对路径相对于当前工作目录。
- `--config-search-depth <N>`：自动搜索时最多向父目录移动 `N` 次；`0` 表示只检查当前工作目录。
- `--print-config-path`：向 stderr 输出实际选中的配置文件；未找到时输出内建默认值正在生效。
- 自动搜索未找到配置文件不是错误；显式路径不存在、路径不是普通文件、JSON 非法或 JSON 根节点不是对象时，程序以退出码 2 失败。

配置根节点必须包含 `schema_version: 1`。物理量既可以写 JSON 数值，也可以写 SPICE 数值字符串（如 `"2n"`、`"100u"`）。例如：

```json
{
  "schema_version": 1,
  "debug": true,
  "op": {
    "newton": {
      "maximum_iterations": 1000,
      "tolerance": "1n",
      "relative_tolerance": 0.0,
      "voltage_absolute_tolerance": "1n",
      "current_absolute_tolerance": "1n",
      "normalized_update_tolerance": 1.0,
      "normalized_residual_tolerance": 1.0,
      "maximum_backtracks": 8,
      "backtrack_scale": 0.5,
      "sufficient_decrease": 0.0001,
      "maximum_solution_step": 1.0
    },
    "source_stepping": {
      "enabled": true,
      "initial_step": 0.1,
      "maximum_step": 0.25,
      "minimum_step": "100u",
      "growth_factor": 1.5,
      "failure_scale": 0.5
    }
  },
  "pta": {
    "mode": "fallback",
    "newton": {"maximum_iterations": 1000},
    "initial_step": "1n",
    "maximum_steps": 10000
  },
  "tran": {
    "output_interval": "1n",
    "stop_time": "100n",
    "solver": {
      "newton": {"tolerance": "1n"},
      "relative_tolerance": 0.0001
    }
  }
}
```

允许的字段如下；未知字段、错误类型、无穷数和非法 SPICE 数值都会以配置错误退出。

- `debug`：布尔值，默认 `true`。控制是否写出同名 `.solve.txt` 报告；命令行 `--debug true|false` 的优先级更高。
- `op.newton`：`maximum_iterations`、`relative_tolerance`、`voltage_absolute_tolerance`、`current_absolute_tolerance`、`normalized_update_tolerance`、`normalized_residual_tolerance`、`maximum_backtracks`、`backtrack_scale`、`sufficient_decrease`、`maximum_solution_step`、`maximum_consecutive_non_monotone_steps`、`maximum_non_monotone_residual_growth`，以及所有 `trust_region_*` 字段和 `maximum_trust_region_retries`。`tolerance` 保留为兼容字段；若设置且未分别设置电压/电流绝对容差，它会同时设定二者。OP、TRAN 与 PTA 默认使用归一化 trust-region：在同一线性化点比较预测与实际残差下降，并据其比值接受/拒绝候选、缩小或扩大半径。OP/TRAN 的初始半径为 `0` 时自动采用首个 raw-step-limited Newton 方向的归一化步长；PTA 默认使用保守的有限半径 `1e8`，避免首个伪元件 stamp 的 branch-current 更新将半径放大。`trust_region_enabled=false` 可恢复原先的 Armijo 回溯与受控非单调策略。带 BJT/MOS limiter seed 的 PTA 在任一伪时间点耗尽重试后，会从同一试探伪时间点执行一次 PTA 非单调恢复；该恢复仍失败才会进入 PTA 的伪时间缩步或残差定向增容。独立导数/DC 残差判据始终位于这些局部 Newton 接受机制之外。
- `op.source_stepping`：`enabled`、`initial_step`、`maximum_step`、`minimum_step`、`growth_factor`、`failure_scale`。
- `pta.newton`：与 `op.newton` 相同。`pta` 还支持 `mode`、`initial_step`、`minimum_step`、`maximum_step`、`maximum_steps`、所有 `derivative_*` 与 `dc_*` 容差、`initial_node_capacitance`、`minimum_node_capacitance`、`maximum_node_capacitance`、`current_source_capacitance`、`voltage_source_inductance`、`compound_time_constant`、`compound_initial_resistance`、`compound_initial_conductance`、`source_ramp_time`、`initial_mos_vgs`、`initial_bjt_vbe`、所有振荡/电容缩放字段，以及 `include_mos_bulk`、`include_diodes`。
- `tran`：`enabled`、`output_interval`、`stop_time`、`output_start_time`、`maximum_step`、`use_initial_conditions`。
- `tran.solver.newton`：与 `op.newton` 相同。`tran.solver` 还支持 `relative_tolerance`、`voltage_absolute_tolerance`、`current_absolute_tolerance`、`minimum_step`、`safety_factor`、`minimum_scale`、`maximum_scale`、`convergence_failure_scale` 和 `maximum_rejects`。

### 命令行分析参数覆盖

在读取并应用 `config.json` 后，可以使用下列可重复的选项覆盖任意分析参数：

```sh
--op-option <name=value>
--pta-option <name=value>
--tran-option <name=value>
```

`name` 与 JSON 字段路径完全对应，但省略最外层的 `op`、`pta` 或 `tran`。命令行中字段名可使用下划线或连字符，且不区分大小写；例如 JSON 的 `op.newton.maximum_iterations` 可写为 `newton.maximum-iterations=200` 或 `newton.maximum_iterations=200`。

```sh
# 覆盖 OP 的 Newton 与 source stepping 参数
./spice --config config.json \
  --op-option newton.maximum-iterations=200 \
  --op-option source-stepping.enabled=false \
  tests/cases/op/level1_01_resistive_bridge_mesh.cir

# --pta 选择 PTA 模式；其余 pta 字段使用 --pta-option
./spice --pta fallback \
  --pta-option newton.tolerance=10n \
  --pta-option compound-time-constant=10n \
  --pta-option include-diodes=true \
  tests/cases/op/level1_01_resistive_bridge_mesh.cir

# 覆盖或创建 TRAN 分析，并调整瞬态求解器参数
./spice --tran-option output-interval=1n \
  --tran-option stop-time=100n \
  --tran-option solver.maximum-rejects=20 \
  --tran-option solver.newton.maximum-iterations=500 \
  tests/cases/op/level1_01_resistive_bridge_mesh.cir
```

`--op-option` 支持 `newton.*` 与 `source-stepping.*` 的所有字段。`--pta-option` 支持 `pta` 的全部字段，PTA 模式则继续使用现有的 `--pta disabled|force|fallback`；`initial-mos-vgs=null` 与 `initial-bjt-vbe=null` 可清空相应的可选种子。`--tran-option` 支持 `tran` 顶层字段与 `solver.*` 的全部字段，`enabled=false` 可禁用瞬态分析，其他 TRAN 字段会在不存在 `.tran` 时创建一个瞬态分析配置；新建配置仍必须最终提供有效的 `output-interval` 与 `stop-time`。所有数值均支持 SPICE 单位后缀（如 `1n`、`10u`），布尔值接受 `true`/`false` 或 `1`/`0`。

覆盖优先级为“内建默认值 < `config.json` < 显式 CLI 分析参数 < 网表控制卡”。配置文件与命令行的同名值相互冲突时仍由命令行优先；但只要 `.cir` / `.sp` 中已显式设置该参数，程序便静默保留网表值，不输出警告或错误。

`.solve.txt` 的输出开关独立于分析参数：内建默认值为 `true`，配置根字段 `debug` 可调整默认值，`--debug true|false` 最终覆盖配置文件；网表控制卡不会改变它。

目前受保护的网表控制字段包括 `.tran` 的 `TSTEP`、`TSTOP`、显式 `TSTART`、`TMAX` 和 `UIC`，`.options DELMAX`，以及 `.pstran` 中显式给出的 `convval`、`initstep`、`minstep`、`maxstep`、`tau`、`vbe0`、`kvgs0`、`tauramp` 与 PTA 模式。未由网表给出的 OP 参数、TRAN 求解器参数及 PTA 其他参数仍可由配置文件或命令行注入。`.pstran` 始终强制 PTA 模式，因此与 `--pta` 或 `pta.mode` 冲突时会静默保留 `force`。网表的 `TMAX` / `DELMAX` 是硬上限，外部的 `tran.maximum_step` 或 `--tran-option maximum-step=...` 不会改变它。网表含 `.tran` 时，外部 `enabled=false` 也不会禁用该分析；没有 `.tran` 时，`enabled: false` 仍可禁用由外部配置创建的瞬态分析。

查看命令行帮助：

```sh
./spice --help
```

## 自动测试

测试与参考结果按分析类型分离：

```text
tests/
  cases/
    op/          18 个 operating-point netlist
    tran/        18 个 transient netlist
  references/
    op/          18 个由 ngspice 独立生成的 OP listing reference
    tran/        18 个由 ngspice 独立生成并重采样的 TRAN listing reference
  output/
    op/<case>/   每个 OP 网表的 .out / .raw / .err / .solve.txt
    tran/<case>/ 每个 TRAN 网表的 .out / .raw / .err / .solve.txt
    pta/<mode>/<case>/  PTA OP 回归的四个 artifact
    pta/hard-op/<mode>/<case>/  困难 PTA 用例的 ordinary / force / fallback artifact
    private/<case>/  私有网表求解后的四个 artifact
```

运行默认全部回归：

```sh
make test
```

`make test-unit` 可单独运行瞬态/PTA 数值单元测试（当前含 227 项检查，其中覆盖均匀与非均匀网格 BDF2 LTE 缺陷、Jacobian 投影后的误差归一化、三次根控制律，以及 PTA trust-region 耗尽后的检查点恢复），`make test-core` 可单独运行命令行和 SPICE 字符串工具单元测试，`make test-config` 可单独运行配置模块单元测试和配置 CLI 端到端测试。

研究 PTA 时，默认门槛之外还应运行：

```sh
make pta-force-standard
make pta-fallback-standard
make test-pta-hard-op
make test-private
```

前两项将标准 OP 网表与 ngspice 参考比较；困难锁存器检查所选多稳态支路；私有网表仅检查求解状态和规范 artifact，不构成额外的默认发布门槛。

`make test-op` 与 `make test-tran` 会在每个网表执行后输出一条
`TIME <analysis> <case> <milliseconds> PASS/FAIL`，并输出该分析组的总墙钟时间。单例时间覆盖 simulator 子进程启动、解析、建模、求解以及四个 artifact 写出；rawfile 校验和 ngspice 对照时间不包含在其中，便于 PTA 前后比较求解端到端开销。

PTA Force OP 回归与参考精度比较可一并运行：

```sh
make pta-force-standard
```

当前该套件覆盖 18 个 OP 网表和 76 个输出值。它是 PTA 的基础回归门槛，不替代更大规模的困难非线性电路基准。

另有一条专门的困难 OP 基准：带显式 q-high 初态的弱偏置交叉耦合 CMOS 锁存器。
该脚本保留 legacy Newton（包括自动 source stepping）预期失败的对照，同时验证
Force/Fallback PTA 的局部 trust-region 均能落到与 ngspice 46 独立参考一致的
`q-high` 稳定支路。
该锁存器有多个 DC 解，因此参考值用于验证所选稳定支路，而不宣称解唯一：

```sh
make test-pta-hard-op
```

这是 legacy-Newton/PTA 的求解器分流基准，不属于默认 `make test` 的永久发布门槛。

也可以分别运行或只比较已有结果：

```sh
make test-io
make test-core
make test-unit
make test-cases
make test-op
make test-tran
make test-netlists  # 递归解析 tests/ 下已实现器件的 .cir / .sp，不执行求解
make test-private   # 求解 tests/private/ 并写入 tests/output/private/；复杂网表可能耗时较长
make pta            # disabled / force / fallback 三种 PTA 路径对照 ngspice OP reference
make test-pta-hard-op  # 多稳态 CMOS 锁存器的 PTA 分流基准
make compare
make compare-op
make compare-tran
```

`make test` 会完成以下检查：

1. 构建 simulator，并运行瞬态/PTA、命令行、SPICE 字符串工具和配置单元测试。
2. 使用 `tests/scripts/test_io.py` 检查 SPICE 注释、续行、大小写、严格数值/model/实例参数、`.end`、混合 OP/TRAN 输出、同名结果目录、成功/失败求解报告、事务式文件替换、硬链接保护和 CLI。
3. 对每个 netlist 生成独立子目录以及 `.out`、`.raw`、`.err` 和默认启用的 `.solve.txt` artifact，并校验报告状态与分析段落及 debug 开关行为。
4. 使用 `tests/scripts/validate_raw.py` 校验 rawfile header、变量数量、点数、有限数值和瞬态时间单调性，并把 raw 数据与同次 listing 逐点、逐变量交叉核对。
5. 使用 `tests/scripts/compare_spice.py` 解析标准与实际 `Index` 表格，并按绝对误差加相对误差比较。
6. 使用 `--parse-only` 递归读取 `tests/` 下全部 `.cir` / `.sp`，覆盖 `tests/private/` 的遗留 Level-3 MOS 模型卡与层次化子电路；此检查只验证语法、实例、模型和分析参数的读取与校验，不要求求解收敛。

`make test-cases` 是独立的网表复杂度审计：检查两组各 18 个网表的命名、数量、分析类型、物理行数、有效语句以及最大案例规模；它不属于 `make test` 的常规回归流程。

默认容差：

```make
OP_ABS_TOL    ?= 5e-4
OP_REL_TOL    ?= 1e-4
TRAN_ABS_TOL  ?= 1e-4
TRAN_REL_TOL  ?= 1e-3
TIME_ABS_TOL  ?= 1e-15
```

判定公式：

```text
|actual - expected| <= absolute_tolerance + relative_tolerance * |expected|
```

详细显示每一个比较值：

```sh
make test OP_COMPARE_FLAGS=--verbose TRAN_COMPARE_FLAGS=--verbose
```

OP 与 TRAN 各有 18 个由上游开源电路库拓扑改编的压力用例，Level 分布均为 `4/4/5/5`：Level 1 为 10-20 行，Level 2 为 20-40 行，Level 3 为 40-100 行，Level 4 大于 100 行；两组最大案例均为 340 个物理行。案例不是上游文件的原样复制，而是展平并约束到当前解析器所支持的 primitive-only 子集。详细来源、适配规则和逐级行数见 [`tests/cases/SOURCES.md`](tests/cases/SOURCES.md)。

测试用例与参考值可分别复现：

```sh
python3 -B tests/scripts/generate_complex_cases.py
make test-cases
make generate-standards  # 需要 ngspice
```

`tests/references/` 不使用本项目求解结果自我生成。OP 参考值直接来自 ngspice 46；生成 TRAN 参考时，脚本会向临时网表注入固定的高精度 ngspice 设置：`reltol=1e-8`、`vntol=1e-10`、`abstol=1e-12`、`trtol=1`，并将内部最大步长限制为 `TSTEP / 2000`。随后将 ngspice 结果线性重采样到网表要求的输出时间网格，原始测试网表不会被修改。对于未提供显式初值的 `UIC` 网表，仅显式 `t=0` 行按本项目的全零采样约定处理，所有 `t>0` 数据均来自 ngspice；带 `.ic` 或器件 `IC=` 的新研究用例应单独保存其 ngspice 初态与比对规则。

MOS Level-3 的独立回归资产位于 [`tests/cases/mos3/`](tests/cases/mos3/)。这些 fixture 直接适配自 ngspice 官方 SourceForge 仓库和 bug #481；使用 `make generate-mos3-standards` 以 ngspice 46 重建参考输出。`make test-mos3` 验证当前四个官方 OP case 和一个 UIC 瞬态 case，并纳入默认 `make test`。

## 目录结构

```text
include/
  analysis/      分析计划、求解诊断、瞬态配置、stamp 上下文与积分器
  app/           命令行接口
  circuit/       Circuit 与 NodeMap 公共接口
  config/        配置加载、类型化覆盖及参数优先级接口
  devices/       器件定义、OP / TRAN stamp 与 MOSFET Level-3 子模块
  io/            SPICE 格式化、求解报告与事务式 artifact 接口
  models/        .model 参数存储及模型卡配置
  netlist/       网表读取、SPICE 词法、层次展开与 Parser 接口
  solver/        Eigen 稀疏 MNA、Newton 步长与非线性电压限制
  utils/         与 SPICE 语法无关的通用字符串工具
src/
  app/           应用入口与命令行解析
  circuit/       电路构建，以及 OP / TRAN / PTA / Newton 分工实现
  config/        配置加载、schema parser、override applier 与 CLI 覆盖
  devices/       非 header-only 的器件公共实现
  io/            SPICE 格式化、求解报告与原子文件事务
  netlist/       网表读取、子电路展开与语义解析
tests/
  cases/         OP / TRAN netlist 与 SOURCES.md
  references/    ngspice 独立参考 listing
  scripts/       用例生成、参考生成与回归校验脚本
  output/        按网表分目录的测试 artifact（不纳入版本控制）
```

项目内头文件统一使用 `.hpp` 并相对于 `include/` 引用，文件名统一使用 snake_case；实现文件使用 `.cpp` 并放入对应职责目录。Makefile 递归收集 `src/` 下的实现文件，因此后续增加子模块不会静默漏编译。默认构建使用 `-O3`；调试时可用 `make OPT_FLAGS=-O0` 覆盖。

## 当前限制

- 不支持 `PULSE`、`SIN`、`PWL` 等时变独立源，因此瞬态阶跃测试使用 `UIC` 和固定 DC 源构造 t=0 激励。
- 瞬态使用首步 Backward Euler 与受步长比限制的可变步长 BDF2；BDF2 使用基于三阶差商、动态器件导数残差和 MNA Jacobian 投影的严格 LTE 估计，启动阶段仍使用 BE step-doubling。严格残差目前覆盖独立 `C`、`L` 以及当前 MOS3 companion charge 模型的端点切线电容；尚未实现事件断点对齐、高于 BDF2 的积分公式，或完整半导体电荷模型的 LTE 残差。
- `.nodeset V(node)=value` 作为 OP 的 Newton 初值提示，不会固定最终解；`.ic V(node[,reference])=value` 在 `UIC` 下构造 t=0 状态、在非 `UIC` 分析中作为较强初值提示。电容 `IC=<V>`、电感 `IC=<A>`、BJT `IC=<VBE>,<VCE>` 和 MOS `IC=<VDS>,<VGS>,<VBS>` 同样参与该初值构造；矛盾的显式电压条件会报错。当前尚未实现带器件方程约束的一致初值求解，浮动的差分 IC 会以其中一端为 0 V 的确定性规范选择表示。
- Newton 同时检查按未知量量纲归一化的更新量和在候选解重新 stamp 后得到的非线性残差；节点电压使用电压绝对容差，branch current 使用电流绝对容差，而 KCL 残差行的量纲相反。OP、TRAN 与 PTA 默认使用自适应 trust-region：在同一线性化点比较固定行权重下的预测/实际残差下降，按一致性比值接受或拒绝候选并调整半径；拒绝会在该线性化点重试。关闭 trust-region 后恢复 Armijo 回溯与受配置次数/残差增长上限约束的完整受限步。带 limiter seed 的 PTA 在任一伪时间点耗尽 trust-region 重试后，会从同一试探状态运行一次独立的非单调恢复；两者均失败时才缩小伪时间步或在最小步增长残差选定的伪电容。无 seed 的 PTA 保持 trust-region 连续跟踪，以避免局部恢复跳转至另一稳定支路。瞬态失败仍会缩小时间步并在上一个已接受状态重试；PTA 的独立导数/DC 残差判据保持不变。
- PTA 已具备伪元件 stamp、BE/BDF2 伪时间推进、失败缩步，以及最小步失败时按伪系统 KCL 残差选取单个节点增容；成功步后仍按逐节点振荡降容。导数与 DC 残差判据已经归一化。局部 trust-region 的半径仅约束同一伪时间点内的 Newton 更新，而带 limiter seed 的检查点恢复允许其拒绝模型时交由 PTA 的非单调局部策略复核；它们都不替代 PTA 的步长、增容与稳定判据。当前 18 个 OP / 76 个输出值的 Force 与 Fallback 回归及一个多稳态困难锁存器构成研究测试基线，但默认容差、跨模型鲁棒性和性能结论仍需通过更广泛的困难非线性电路基准验证。因此它适合作为 PTA 算法研究的可追溯测试版，而不作为生产级 SPICE 求解保证。
- 不支持 `.include`、`.lib`、全局 `.param`、`.temp`、`.save`。
- 不支持受控源 `E/F/G/H`、行为源、AC/noise 分析。
- 二极管、BJT 和 MOSFET 仍是有限子集。MOS3 已实现 DC channel current、`RD`/`RS`、`RSH*NRD/NRS`、B-D′/B-S′ 结电流及其耗尽结电容、`CGSO`/`CGDO`/`CGBO` 重叠电容，以及带已接受 Qgs/Qgd/Qgb 和结电荷历史的瞬态 companion；含 MOS3 的 UIC 使用 BE step-doubling。器件温度尚未实现。
- 电阻、电容、二极管、BJT、MOSFET 的器件电流尚不能通过 `.print i(...)` 输出。

## SPICE 格式参考

I/O 兼容规则主要参考：

- [ngspice User's Manual](https://ngspice.sourceforge.io/docs/ngspice-manual.pdf)
- [ngspice ASCII rawfile writer source](https://sourceforge.net/p/ngspice/ngspice/ci/master/tree/src/frontend/rawfile.c)

本项目对未实现的语法保持显式报错，并在本文档中标明与 ngspice 的差异。
