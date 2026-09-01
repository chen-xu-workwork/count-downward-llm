# 真实 Depots 问题的 Count anytime shadow probe

本实验运行 Count-Downward 的原生 hFF Lazy anytime 搜索，同时开启仅记录日志的
LLM 触发探针。探针不启动 HTTP 服务、不调用模型、不等待响应，也不创建 proposal
或 rollout，因此不会改变 OpenList、父链、bound 或后继生成。

唯一额外开销来自触发条件检查、稀疏的 ancestor 检查，以及触发时序列化当前状态
并打印日志。搜索决策和状态空间不变，但 wall-clock 并非数学意义上的零开销。若后续
做严格运行时间对比，仍应另外运行 `HYBRID_LLM_TRIGGER=0` 的配对基线；本实验主要
用于观察原生搜索决策下的 cost–time 曲线和潜在 LLM 介入点。

## 固定选择的四个问题

使用 `random.Random(20260827)` 从现有文件中固定抽取：

- `problem_scale_30_id_572.pddl`
- `problem_scale_30_id_806.pddl`
- `problem_scale_40_id_176.pddl`
- `problem_scale_40_id_222.pddl`

清单保存在 `selected_problems.txt`，避免每次运行重新抽样。

## 正式运行

在 WSL 中：

```bash
cd '/mnt/e/Python Projects/规划器/planners/Count-Downward'
bash experiments/shadow_probe/run_selected_depots_shadow.sh
```

默认每题搜索 1800 秒，四题顺序运行，因此搜索时间最多约两小时，另加 translator
和 preprocessor 时间。顺序运行是为了避免四个搜索进程争抢 CPU/内存，污染
cost–time 曲线。

也可以显式指定时间和输出目录：

```bash
bash experiments/shadow_probe/run_selected_depots_shadow.sh \
    1800 /tmp/depots-shadow-formal
```

先做一分钟短测：

```bash
bash experiments/shadow_probe/run_selected_depots_shadow.sh \
    60 /tmp/depots-shadow-smoke
```

运行单题：

```bash
bash experiments/shadow_probe/run_one_shadow.sh \
    '/mnt/e/Python Projects/PyPACE/data/generated-pddl/depots-numeric-new/problems/problem_scale_30_id_572.pddl' \
    /tmp/one-shadow-run 1800
```

脚本直接使用
`/home/professorxu/miniconda3/envs/PyPACE_env/bin/python`，不需要激活 Conda。
可以用 `COUNT_SHADOW_PYTHON` 覆盖。

## 搜索和触发配置

搜索配置与 Count 的 `nipc26-sat-hff` 核心阶段一致：两轮 Lazy Greedy，随后
Lazy WA* 权重 5、3、2、1；新 incumbent 作为下一阶段严格 bound。所有阶段共享
一个绝对 `max_time=1800` deadline。

探针使用：

```text
HYBRID_LLM_COMM_MODE=log
HYBRID_LLM_REQUEST_INITIAL=0
HYBRID_LLM_ENABLE_EXPANSION_PLATEAU=1
HYBRID_LLM_ENABLE_GLOBAL_STALL=1
HYBRID_LLM_ENABLE_ANCESTOR_STAGNATION=1
HYBRID_LLM_PLATEAU_WINDOW_EXPANSIONS=65536
HYBRID_LLM_PLATEAU_CONFIRM_WINDOWS=3
HYBRID_LLM_PLATEAU_RESET_WINDOWS=2
HYBRID_LLM_PLATEAU_MIN_BUCKET_EXPANSIONS=16384
HYBRID_LLM_PLATEAU_MIN_SINCE_REQUEST_EXPANSIONS=65536
HYBRID_LLM_PLATEAU_MIN_SHARE=0.25
HYBRID_LLM_PLATEAU_H_BUCKET_WIDTH=0.001
HYBRID_LLM_PLATEAU_PER_LAYER_REQUEST_GAP_EXPANSIONS=500000
HYBRID_LLM_STALL_EXPANSIONS=500000
HYBRID_LLM_MIN_REQUEST_GAP_EXPANSIONS=500000
HYBRID_LLM_ANCESTOR_CHECK_INTERVAL=100000
HYBRID_LLM_ANCESTOR_DEPTH=20
HYBRID_LLM_MIN_DEPTH=30
HYBRID_LLM_MAX_REQUESTS=10
```

Lazy expansion plateau 只统计基础搜索真正展开成功的状态 h，不读取继承父状态
上下文的 Edge OpenList key。每 65536 次有限基础扩展形成一个窗口；同一个 h 桶在
连续 3 个窗口中占比至少 0.25、至少出现 16384 次时，桶进入
`active/armed`。各桶独立累计证据；若同一窗口有多个已确认的合格桶，只选择其中
展开次数最多的桶作为下一次平台请求来源，并列时选择较低 h。确认窗口本身的最后
一个状态不会触发；后续第一个落入被选中桶且满足共享请求间隔和同桶冷却的真实
展开状态才成为请求源。LLM rollout 状态不进入窗口。

这组平台参数来自首个频率复测的校准：原 8192 窗口在该问题上约只有 1--3 秒，
而共享 LLM 间隔约为 500000 次扩展。新设置把连续确认覆盖量从 24576 提高到
196608 次扩展，把连续两个坏窗口的失活证据从 16384 提高到 131072 次扩展；
同时将单桶 share 从“绝对多数”0.5 降到 0.25。这样平台需要更长时间持续，
但允许 Lazy 展开流中同一 h 与邻近 h 状态交错出现。较低 h 桶的占比继续写入
日志用于诊断，但不再否决其他桶。检测器最多保留 16 个 h 桶；满载时优先
淘汰最久未出现的 inactive 桶，淘汰次数记录在 `plateau_layer_evictions` 中。

触发仲裁顺序为 expansion plateau、global stall、ancestor stagnation。平台与 global
同时成立时合并为一次 `expansion_plateau+global_stall` 请求。

四题初测的加权搜索速度约为每秒 4080 次基础扩展，因此 500000 次扩展约对应
122 秒。共享请求间隔由此设为 500000，作为“约两分钟一次 LLM 机会”的机器无关
近似。预算仍然按 anytime phase 独立计算，每个 phase 最多 10 次。父链检测仍每
100000 次扩展检查一次，但比较深度由 10 提高到 20，最低状态深度由 20 提高到
30，以降低它在所有可用请求机会中长期占满的概率。global stall 仍保持 500000；
expansion plateau 则使用上文基于首次频率复测重新校准的长窗口参数。

这些阈值都可通过同名的 `COUNT_SHADOW_*` 变量覆盖。例如把全局停滞阈值临时
降到 100000：

```bash
COUNT_SHADOW_STALL_EXPANSIONS=100000 \
    bash experiments/shadow_probe/run_selected_depots_shadow.sh 1800
```

## 结果文件

每个问题目录包含：

- `planner.log`：translator、preprocessor、搜索、anytime 和 probe 原始日志；
- `sas_plan.*`：各次 incumbent 计划；
- `incumbents.csv`：原始 cost–time 曲线点；
- `trigger_events.csv`：每次潜在介入的 phase、state、reason、g、h、expansions，
  以及 phase 内真实触发时间 `phase_elapsed_seconds`；
- `plateau_events.csv`：平台窗口、激活/失活、重新 armed 和请求事件；
- `phases.csv`：每阶段搜索、trigger 和零 rollout 汇总；
- `summary.json`：单题摘要和 pure-probe 自动检查；
- `run_config.txt`：本次使用的完整关键配置。

批处理根目录还会生成：

- `all_runs_summary.csv`
- `all_incumbents.csv`
- `all_trigger_events.csv`
- `all_plateau_events.csv`
- `all_phases.csv`

## 两分钟频率复测

重新编译后，在 WSL 中运行：

```bash
cd '/mnt/e/Python Projects/规划器/planners/Count-Downward'
bash experiments/shadow_probe/run_trigger_frequency_probe.sh
```

它仍然顺序运行固定的两个 30 规模和两个 40 规模问题，每题默认 1800 秒，完全
不连接 LLM。除上述新默认值外不使用人为降低的触发阈值。也可指定时限和输出目录：

```bash
bash experiments/shadow_probe/run_trigger_frequency_probe.sh \
    1800 /tmp/count-trigger-frequency
```

除了原有结果，它还生成：

- `trigger_intervals.csv`：每次触发的实际秒数、与上一次触发的秒数/扩展数间隔、
  以及 `too_frequent/near_target/sparser_than_target` 分类；
- `trigger_frequency_summary.csv`：逐问题、逐 phase 的首次触发时间、平均/中位
  相邻间隔、每分钟请求数和各触发 reason 数量。

频率分类只评估同一 phase 内的相邻触发，不把 phase 起点到第一次触发算作相邻
间隔。默认把 90--180 秒视为接近 120 秒目标；第一次触发时间仍单独报告，便于
判断某个规则是否过早取得首次机会。

`elapsed_seconds` 是 Count anytime 搜索开始后的时间，不包含 translator/preprocessor。
`incumbents.csv` 每一行代表首次解或一次严格 cost 改善，可直接绘制阶梯状
best-cost–time 曲线。

每个单题结束时解析器会强制检查：

```text
pending_at_end = 0
responses_completed = 0
usable_responses = 0
llm_bursts_started = 0
llm_actions_processed = 0
llm_states_new/reopened/duplicate = 0
```

任何 HTTP 响应或 rollout 事件都会令 `pure_probe=false` 并使脚本失败，从而避免
把发生过实际介入的运行误当成原生曲线。
