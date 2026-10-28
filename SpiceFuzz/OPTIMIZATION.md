# ngspice feedback optimization

## 1. 可信判定

`scripts/ngspice_testing.py` 为每个配置启动独立进程、使用独立目录，并在运行前移除该目录中旧的 raw/log。进程使用 `-n` 禁用用户 `.spiceinit`，在评估器自己的控制段中固定求解选项，强制输出 ASCII OP。

成功要求：正常退出、存在完整的单点实数 Operating Point、变量表和值数量一致、数值有限，且没有终止 OP 的错误。启动阶段出现 singular matrix 警告后成功恢复，不会被当作最终失败。语法错误、环境错误、超时、崩溃、无效输出和收敛失败分别记录。

日志记录阶段进入/成功/失败事件。source stepping 内部的 gmin 不会覆盖外层阶段；无法确定成功阶段时保存 `unknown`。迭代数缺失时使用缺失标记，不用零冒充观测值。`version` 输出和输入哈希进入评估记录。

## 2. ngspice NR/PTA 差分检测

四个独立配置的 `optran` 参数分别为：

- `ng_nr`: `1 0 0 0 0 0`，允许直接 OP，禁用外层 gmin/source/optran。
- `ng_gmin`: `1 1 0 0 0 0`，直接 OP + gmin。
- `ng_source`: `1 1 1 0 0 0`，直接 OP + gmin + source。
- `ng_full`: `1 1 1 10n 10u 0`，包括指定步长和时长的 optran。

固定 ngspice 选项：`itl1=100 itl2=50 itl4=10 reltol=1e-3 vntol=1e-6 abstol=1e-12 gmin=1e-12 temp=27 trtol=7 method=trap`。这些是评估协议，不由生成模型修改。需要研究其他预算时，应修改协议、另开实验，不与旧结果混用。

`nr_differential_candidate` 表示直接 NR 的收敛失败被已验证 PTA 恢复；这个标记尚未独立重放直接 NR。主目标 `authoritative_target` 则要求完整配置收敛失败、PTA 合格成功，并在独立进程中重放两者，确认 PTA 最终解一致。主目标重放失败时不给 +12。

source stepping 可能在内部调用 gmin，它不是一个完全不使用 gmin 的独立算法。

配置依据：[ngspice 官方手册 11.3.5](https://ngspice.sourceforge.io/docs/ngspice-47-manual.pdf#page=331)；禁用 optran 的零步长/零时长写法参见[维护者说明](https://sourceforge.net/p/ngspice/discussion/133842/thread/06b6463c9e/)。

## 3. Reward

主目标使用：

`reward = base + target_bonus + difficulty_bonus + novelty_bonus + progress_bonus + cost_penalty`

- 已独立重放的完整恢复差分目标：`base=0`，`target_bonus=12`。
- ngspice 普通成功：`base=-3`。
- ngspice gmin 成功：`base=-2`。
- ngspice source/optran 成功：`base=-1`。
- 其他失败或成功阶段不明：`base=-3`；超时 `-4`；无效网表 `-6`。
- 困难度、新颖性和正向进展各不超过 `0.25`；进展也可为负。成本最多扣 `0.25`。
- shaping 要求 PTA 有合格稳态证据。任何非主目标的最终奖励最高为 `-0.25`。
- 环境错误照常归档，但 `trainable=false`，不更新策略或动作 reward EMA。

困难度主要来自恢复阶段，并加入按器件数量归一化的迭代工作量。新颖性使用两轮图颜色细化及有界参数特征，降低重命名、注释和文本重排影响；它是近似电路距离，不是完整图同构证明。

父子进展来自相同协议下的父子困难度差。这是 discount=0 的一步搜索启发式，不宣称长期策略不变性。不同批次的 reward 不能直接与旧内部实验 reward 比大小。

## 4. PTA 证据和共同输入

`scripts/solver_validation.py` 检查 raw、成功报告、实际求解方法、最终 PTA JSONL，以及最后一个被接受的稳态点。PTA 端点必须满足有效配置中的导数和 DC 残差阈值、Newton 收敛、源斜坡完成；raw 和最终轨迹的物理解还必须一致。

两端使用相同物理电路。评估副本去除 `.print` 输出选择及其续行，以输出全部未知量；不修改原始网表或器件参数。原始输入和评估输入分别保存哈希，归档保留评估副本。

共同网表验证在代码中执行指令白名单，拒绝 solver options、非 OP 分析及外部包含文件，不再只依赖提示词。默认冻结 `.nodeset`、`.ic`、器件 `IC=` 和 `OFF`；专门的初值变异动作默认不参与动作空间。使用 `--allow_initial_hint_mutations` 明确开启另一组初值实验。

这些检查不建立两端器件方程等价性，也不构成完整的拓扑适定性证明。输出始终记录 `model_equivalence=not_established`。正样本应解释为可重放的跨引擎恢复候选；算法优越性仍需模型对照或共同后端实验。

## 5. Critic 和训练归因

主循环先评估并缓存父网表。状态包含动作频率、直接观测 reward EMA、分器件参数尺度、半导体模型参数特征，以及父网表的 ngspice/PTA 反馈和缺失掩码。本次子网表的结果只进入标签，不提前写入决策状态。

critic 使用两层隐藏层、LayerNorm 和 Huber 值损失。按所选动作条件化的辅助头预测有效性、PTA 合格成功、主目标和 ngspice 成功阶段；没有正样本的批次不训练稀疏主目标头。暂未加入 GNN 或长期 episode。

动作概率严格使用：

`mu = (1-epsilon) * softmax((logits + count_bonus) / temperature) + epsilon / action_count`

每个决策保存采样温度、epsilon、计数 bonus 和实际 log probability。训练重建同一分布，使用有界概率比目标，并保留熵正则、优势标准化和梯度裁剪。完全随机采样时不会凭该动作更新 actor 偏好。这是一次批次更新的 clipped contextual-bandit 实现，不是完整多轮 PPO 训练器。

移除祖先 actor 回放和祖先动作 EMA 奖惩；谱系只参与种子权重调度。`--discount` 必须为 0。旧 `--lineage_negative_decay` 仅为命令兼容保留，不再影响训练。

## 运行

以下命令在 SpiceFuzz 目录运行。将 `NGSPICE_EXE` 替换为已安装的真实可执行文件路径；无需更改 PTA。

离线重放入口使用 Python 3.8 及以上的标准库；`SolverOutcome` 的可选类型使用 `typing.Optional`，不要求 Python 3.10 的 `| None` 标注。更旧的解释器会在导入项目模块前给出版本提示。完整强化学习搜索仍建议使用前述已验证的 Python 3.10 和匹配依赖环境。运行前用 `python3 --version` 检查实际解释器，不要假定 `python3` 指向虚拟环境或 Python 3.10。

单网表完整重放，不调用 DeepSeek：

```powershell
.\.venv\Scripts\python.exe -B evaluate_pta.py `
  --netlist .\result\20260928192428260329\20260928192428260329.cir `
  --pta_bin ..\PTA\Simulator\spice.exe `
  --ngspice_bin "NGSPICE_EXE" `
  --output_root .\test\ng_feedback_replay
```

省略 `--ngspice_bin` 时仅做本地 PTA 消融，不能报告为 ngspice 差分结果。

有限预算搜索，使用已有导出候选作为种子：

```powershell
.\.venv\Scripts\python.exe -B main_pta.py `
  --pta_bin ..\PTA\Simulator\spice.exe `
  --ngspice_bin "NGSPICE_EXE" `
  --seed_dir .\exports\ngspice_remote_validation_20260929\netlists\priority_pta_required `
  --max_candidates 100 --max_time 3600 --random_seed 0 `
  --result_root .\result\ng_feedback_run1 `
  --pta_output_root .\test\ng_feedback_run1 `
  --workdir .\netlist_pta\ng_feedback_run1
```

DeepSeek 凭据沿用环境变量 `DEEPSEEK_API_KEY`。运行元数据排除该密钥。每次选择新的结果目录；已有 `run.json` 时拒绝覆盖。`data/seed_netlist/pta` 当前为空，因此示例使用现有导出种子。

普通候选会运行四个本地配置和四个 ngspice 配置；主目标另加两次独立重放。首次访问父网表也有一次评估成本。运行时间上限在候选边界检查，不能中断已经开始的完整评估；单个进程仍受 `--maxtime` 限制。

归档新增：`run.json`、逐案例 `evaluation.json`、`training.jsonl`、`policy.pt`、`summary.json`。summary 分别记录目标命中次数及按原始输入 SHA256 去重的目标数；这不等于按拓扑家族去重。policy checkpoint 保存模型和优化器状态，但本次未实现自动恢复训练入口。旧模型权重因输入维度和网络结构变化，不应直接加载。

测试：

```powershell
.\.venv\Scripts\python.exe -B -m unittest discover -s tests -v
```

测试不调用外部模型 API；临时文件位于 SpiceFuzz/test 并自动清理。有现成 Windows PTA 程序时，额外执行真实普通 OP 和 PTA 恢复测试；没有该程序时跳过这两个测试。

尚需真实 ngspice 与固定预算搜索实验，才能衡量目标命中率是否提高。阶段判定、训练归因修正和本地测试通过不等于已经证明搜索效果提升。
