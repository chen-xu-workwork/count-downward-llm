# Baseline 日志分析与早期 LLM Pilot 对比

本文说明如何检查无 LLM baseline 的产物，以及如何与早期 18 题 LLM
pilot 做初步对比。早期 pilot 使用的是初期触发参数，因此只能用于诊断和形成
假设，不能代替后续使用当前参数的正式对照实验。

## 1. 先记录两个结果根目录

无 LLM baseline 通常位于：

```text
/root/autodl-tmp/count-results/depots-numeric-validation-original/baseline/<RUN_TAG>
```

早期 LLM pilot 位于：

```text
/root/autodl-tmp/count-results/depots-numeric-validation-original/qwen3_5-9b-global_step_350/pilot-live
```

比较前先保存两边的 `batch_config.json`。机器型号、代码 commit、并行数、时限、
模型 checkpoint 和触发参数也应一并记录。

## 2. 检查 baseline 是否完整

首先查看根目录的两个文件：

- `batch_config.json`：应列出本轮全部问题及实际运行参数；
- `batch_results.csv`：每题一行，`failed` 才表示执行故障；`plan_found`、
  `timeout`、`incomplete` 和 `unsolvable` 都是已经处理完成的结果。

完整验证集应有 161 个 `job_result.json`。可以执行：

```bash
find "$BASELINE_ROOT" -name job_result.json | wc -l
grep -c ',failed,' "$BASELINE_ROOT/batch_results.csv"
```

期望分别得到 `161` 和 `0`。如果数量不足，先使用同一结果目录和 `--resume`
补跑，不要把未完成题当成超时题。

baseline 还应满足纯净性检查：

- `job.json` 中 `mode` 为 `off`；
- `anytime/llm_requests.csv` 只有表头，没有请求记录；
- `anytime/phases.csv` 中请求、模型生成、可用样本和注入状态等 LLM 字段均为 0
  或空值。

## 3. 每道题主要看什么

每个题目目录中最重要的是：

| 文件 | 用途 |
|---|---|
| `job_result.json` | 最终状态、退出码、总墙钟时间、规模和问题路径 |
| `job.json` | 本题的完整命令、模式和已解析策略 |
| `anytime/incumbents.csv` | 每次严格改善的时间、cost、长度和累计展开数 |
| `anytime/phases.csv` | 每个 anytime phase 的搜索量、峰值内存和 LLM 统计 |
| `anytime/llm_requests.csv` | LLM 请求原因、状态、可用样本和注入状态；baseline 应为空 |
| `anytime/planner.log` | 排查异常、OOM、解析错误和搜索阶段切换 |
| `console.log` | 控制台、桥接或子进程级错误 |

从 `incumbents.csv` 提取以下指标：

- 是否在时限内找到方案；
- 第一行的 `elapsed_seconds`、`plan_cost` 和 `cumulative_expanded`，分别表示
  首解时间、首解 cost 和首解展开数；
- 最后一行的 `plan_cost`、`plan_length`，表示时限内最好方案；
- 行数，表示 incumbent 次数；
- 全部行按时间连接成阶梯线，即该题的 best-cost–time 曲线。

`incumbents.csv` 为空表示时限内没有找到可行解，不等于程序故障。最终判断仍以
`job_result.json` 和日志为准。

## 4. 与早期 LLM 结果如何对齐

不要使用目录前面的序号或 `_off`/`_live` 后缀匹配；应使用原始问题文件名，
例如 `problem_scale_30_id_33`。

早期 pilot 只有以下 18 题：

```text
scale 10: id 24, 39, 51, 91, 114
scale 20: id 2, 42, 56, 70, 88
scale 30: id 3, 26, 33, 44, 54
scale 40: id 24, 50, 62
```

因此第一轮对比只取 baseline 与这 18 题的交集。建议每题生成一行：

| 问题 | 两边是否求解 | baseline/LLM 首解时间 | baseline/LLM 首解展开数 | baseline/LLM 最终 cost | cost 差值 | LLM 请求/可用样本/注入状态 |
|---|---|---:|---:|---:|---:|---:|

其中 cost 越低越好。建议计算：

```text
cost 改善率 = (baseline_best_cost - llm_best_cost) / baseline_best_cost
首解加速比 = baseline_first_time / llm_first_time
首解展开加速比 = baseline_first_expanded / llm_first_expanded
```

如果某一侧没有找到解，应记录为“未求解”，不要用 0、无穷大或任意惩罚值混入
普通平均数。先单独报告覆盖率，再只在双方都求解的题上统计 cost 和首解比率；
聚合时优先报告中位数，并按规模分别统计。

## 5. 如何解读差异

优先观察三类结果：

1. **覆盖率**：LLM 是否让 baseline 未求解的题得到可行解，或反之。
2. **首解效率**：比较首解时间，同时查看首解展开数。前者反映实际体验，后者更接近
   搜索策略本身。
3. **anytime 质量**：比较同一时限下最终 cost，并逐题查看 best-cost–time 阶梯曲线，
   判断改善是否发生在 LLM 请求或状态注入之后。

对于 LLM 结果，再结合 `llm_requests.csv` 的 `reason`、`usable_sample_count`、
`inserted_states`、`finished_seconds`，以及 `phases.csv` 的累计 LLM 字段，判断：

- 请求是否真正返回了可用动作；
- 可用动作是否产生了新状态；
- incumbent 改善是否在请求完成后出现；
- 请求很多但没有注入，还是注入很多但没有改善。

这只能说明时间关联，不能单凭一次请求后的改善证明因果。正式结论仍需要当前参数的
live/off 成对实验和消融。

## 6. 公平性注意事项

早期 LLM pilot 的触发构成为 69 次父链停滞、39 次 global stall、2 次平台停滞，
明显代表旧参数，而不是当前平台检测器。它适合回答“早期完整链路是否可能带来收益”，
不适合评价当前三种触发机制的最终比例。

如果 baseline 与 LLM 运行在不同 CPU、不同并行数或不同系统负载下：

- 不应把 wall-clock 首解时间或单位时间展开数直接归因于 LLM；
- 首解展开数和 cost 可作为较稳健的初步比较，但仍会受搜索插入状态影响；
- 正式时间对比应在相同机器、相同并发策略下重跑同一批问题；
- 当前正式调度为 scale 10/20/30 八并行、scale 40 二并行。若旧 baseline 使用了
  其他并发数，应保留它作为算法覆盖率/cost 参考，并另补同配置的时间基线。

最终报告至少应分开给出：按规模的求解率、首解时间中位数、首解展开数中位数、
最终 cost 的成对差异，以及 LLM 请求/可用样本/注入状态数量。不要只比较所有题的
平均最终 cost，因为不同规模的绝对 cost 不在同一量级。
