# Count-Downward LLM 迁移改动与 Lazy 机制总结

> 文档用途：作为后续修改论文方法、实现和实验章节的事实依据。  
> 代码状态：基于 2026-08-27 当前 Count-Downward 源码。  
> 重要区分：本文中的“已实现”不等于“已经实验证明有效”。

## 1. 一句话总结

我们将 NLM-CutPlan 中与求解器无关的 LLM 控制层迁移到 Count-Downward，同时不强行复制 Eager Search 的状态注入和平台判定方式，而是针对 Lazy Search 的 Edge OpenList 和延迟启发式计算语义，设计了：

1. **Lazy expansion-stream plateau detection**：只使用实际展开状态的真实 `h` 值，在展开流上通过窗口和 `h` 桶识别持续平台。
2. **Preemptive Bounded LLM Rollout**：将 LLM 生成的动作链作为低频、有界的抢占式 rollout，逐边复用 Count 原生 Lazy 搜索语义，并保留中间状态的全部普通后继边。

因此，更合适的论文表述不是“Lazy Search 无法使用 Eager 平台机制”，而是：

> We design search-semantics-aware intervention mechanisms for both eager and lazy search. The eager variant monitors state-frontier behavior, whereas the lazy variant detects persistent heuristic plateaus from the stream of states evaluated at expansion time.

## 2. 迁移后的整体系统

```text
Count anytime Lazy Search
        │
        ├── 真实展开状态的 h / g / 展开计数
        │          │
        │          ▼
        │   Lazy trigger monitor
        │   plateau / global stall / ancestor stagnation
        │          │ 异步请求
        │          ▼
        │   C++ HTTP bridge
        │          │
        │          ▼
        │   Python control plane
        │   state -> prompt -> vLLM -> parse -> validate
        │          │ 有效动作前缀
        │          ▼
        └── Preemptive bounded rollout
                   │
                   └── 结束后恢复原有 Edge OpenList
```

系统分为三层：

- **Python 控制层**：构造 prompt、连接 vLLM、并发采样、解析输出、验证动作前缀和记录实验数据。
- **C++ 通用 LLM 层**：管理触发、请求预算、异步 HTTP、回包生命周期、proposal/burst 和结构化统计。
- **Lazy 搜索适配层**：使用 Lazy 搜索自己的状态生命周期执行 LLM 路线，不另造一套搜索节点语义。

## 3. 从 NLM-CutPlan 复用或迁移的能力

### 3.1 中间状态到 prompt

- C++ 搜索器可将任意已注册状态序列化为完整 `(:init ...)`。
- 导出内容同时包含命题事实、数值 fluent 和在 SAS 转换中被消去的静态初始事实。
- 对“某个 predicate 的一些 grounded tuple 可变，另一些 tuple 实际不可变”的情况，翻译器会保留没有可达 delete effect 的初始 grounded fact。
- Python 用运行时 `init` 替换原问题初态，再用与训练数据一致的领域描述模板构造 system/user prompt。

这一部分的工程意义是：LLM 看到的不是原始问题初态，而是搜索实际到达的中间状态。

### 3.2 异步 LLM 请求

- C++ 搜索线程只提交请求，不在原地等待模型。
- HTTP worker 在后台执行通信；回包尚未到达时，Count 继续从基础 OpenList 搜索。
- Python 使用独立 asyncio 线程和共享 `aiohttp` 连接池，支持并发、QPS 限制、重试和总超时。
- 每个触发状态默认独立采样 3 条回答，而不是把一次生成复制三份。

### 3.3 响应解析和双层验证

- Python 通过 AST 读取顶层顺序 `action_Xxx(...)` 调用，不执行模型生成的 Python 代码。
- Unified Planning 从触发状态出发顺序模拟，只保留到第一个不可用动作之前的最长合法前缀。
- C++ 执行时再检查动作名、applicability、incumbent bound、父节点有效性和 proposal 内循环。
- Python 验证用于过滤明显无效输出；C++ 复核才是影响搜索前的最终安全边界。

### 3.4 Anytime 生命周期和数据记录

默认 satisficing 配置保留 Count 的核心搜索日程：

```text
Lazy Greedy (unit cost)
Lazy Greedy
Lazy WA* w=5
Lazy WA* w=3
Lazy WA* w=2
Lazy WA* w=1
```

- 后续 phase 接收当前 incumbent 作为严格 bound。
- 所有 phase 共享同一个绝对 wall-clock deadline，不会为每个 phase 重新获得完整时限。
- LLM 请求预算是 **phase 独立**的，新 phase 使用新的 monitor 和预算，避免前期耗尽机会。
- `run_id + iteration + request_id` 标识一次请求的生命周期。phase 结束后会取消或丢弃迟到的模型结果。
- 每个 phase 结束后释放完整的 SearchSpace、OpenList 和 bridge，防止多轮 anytime 对象累积。
- 保存 incumbent 之后才停止当前 phase 的 bridge，避免在超时边界丢失已找到的计划。

## 4. Lazy 专用创新一：展开流启发式平台检测

### 4.1 为什么 Eager 实现不能原样复制

Eager Search 的 OpenList 通常保存已经评价过的状态，因此可以直接利用候选状态自身的启发式值观察 frontier。

Count Lazy Search 的 OpenList 条目是 `<predecessor StateID, operator>`。该边入队时，后继状态尚未物化，OpenList key 主要沿用父状态的 EvaluationContext。如果把这个 key 当成后继状态的真实 `h`，会产生系统性误判。

我们因此将“是否卡在某个 `h` 平台”改写为：

> 在一段基础搜索展开流中，是否有一个或多个真实 `h` 桶长期占据较高比例？

### 4.2 观测对象

检测器只统计：

- 由基础 Edge OpenList 选中；
- 完成实际状态物化和启发式评价；
- 成功作为基础搜索状态展开；
- `llm_h` 为有限值的状态。

LLM rollout 产生的展开不进入平台窗口，从而避免 LLM 自己制造的路径反过来制造新的平台触发。不过，rollout 如果发现更低的 `h`，仍会更新全局 best-so-far `h` 并重置 global stall 计数。

### 4.3 窗口和 `h` 桶逻辑

当前默认参数为：

| 参数 | 默认值 | 语义 |
|---|---:|---|
| `PLATEAU_WINDOW_EXPANSIONS` | 65536 | 每个不重叠窗口的基础展开数 |
| `PLATEAU_H_BUCKET_WIDTH` | 0.001 | `h` 量化宽度 |
| `PLATEAU_MIN_BUCKET_EXPANSIONS` | 16384 | 某桶在单窗口内的最少样本数 |
| `PLATEAU_MIN_SHARE` | 0.25 | 某桶在窗口中的最小占比（含 25%） |
| `PLATEAU_CONFIRM_WINDOWS` | 3 | 激活前需要连续合格的窗口数 |
| `PLATEAU_RESET_WINDOWS` | 2 | 失活前需要连续不合格的窗口数 |
| `PLATEAU_MIN_SINCE_REQUEST_EXPANSIONS` | 65536 | 已触发平台桶重新 armed 前所需的同桶证据 |
| `PLATEAU_PER_LAYER_REQUEST_GAP_EXPANSIONS` | 500000 | 同一 `h` 桶两次请求的最小展开间隔 |
| 最多活跃桶位 | 16 | 固定空间上限，满时优先淘汰久未出现的 inactive 桶 |

各桶独立累计连续窗口证据：某个桶需要在 3 个连续窗口中同时满足最少样本数和最小占比，才进入 `active + armed`。若同一窗口内有多个已确认且合格的平台桶，检测器只选择展开数最多的桶作为下一次平台请求来源；并列时选择较低的 `h` 桶。未被选中的合格桶仍保留自己的连续证据，不会因为另一个桶更活跃而被清零。检测器在记录当前展开到窗口之前先检查已有 active 桶，因此“刚好完成第三个确认窗口的状态”不会被立即发送；之后第一个落入被选中桶且满足全局冷却和预算的状态，才成为 LLM 请求源。

### 4.4 这个设计保留的核心哲学

- 检测的仍然是“搜索长时间被困在某个启发式高度”，而不是简单的定时请求。
- 展开数窗口比 wall-clock 窗口更少受其他系统负载影响，但不同问题的单次展开成本不同，所以它不等价于固定时间间隔。
- 不使用较低 `h` 桶占比作为否决条件，因此相邻或多个高频桶可以同时积累平台证据；窗口日志仍记录 `lower_share` 作为分析指标。
- 多桶仲裁只决定下一次请求来源，不干扰每个桶自己的连续窗口确认。

## 5. 其他两类触发与统一仲裁

### 5.1 Global stall

- 跟踪当前 phase 中观测到的最低 `h`，它是 **best-so-far heuristic value**，不是已知全局最优值。
- 只有超过 `max(0.001, 0.005 * scale)` 的下降才算有意义的新进展。
- 连续 500000 次基础展开没有有意义改善时，global stall 成为候选触发。
- 同一次 stall 只请求一次，直到发现新的 best-so-far `h` 才重置。

### 5.2 Ancestor stagnation

- 每 100000 次基础展开采样检查一次，不在每个状态上遍历父链。
- 当前节点需要至少有 30 层有效父链。
- 实际比较最近 20 个祖先的真实 `h`。如果当前状态相对这些祖先没有超过 0.001 的改善，则认为父链停滞。
- 相比最初的试验参数，检查间隔、比较深度和最低深度都更严格，用于降低该规则过度主导触发的情况。

### 5.3 触发优先级和公共限制

当前仲裁顺序为：

1. expansion plateau；
2. global stall；
3. ancestor stagnation。

如果平台候选和 global stall 同时成立，只发送一次，原因记为 `expansion_plateau+global_stall`。三类规则共享：

- 500000 次基础展开的最小请求间隔；
- 同一状态在同一 phase 中不重复请求；
- 同时在途请求上限；
- 每 phase 独立的请求预算；
- LLM burst 正在执行时不嵌套新触发。

单题控制台默认每 phase 最多 10 次请求；批处理器对 40 规模问题将其设为 15。这是实验资源策略，不应当作搜索算法本身的普遍常数。

## 6. Lazy 专用创新二：抢占式有界 LLM Rollout

### 6.1 核心语义

在模型回包到达之前，基础 Lazy Search 一直正常运行。当一个仍属于当前 phase 的有效回包到达时：

1. 每个合法 sample 成为一条独立 proposal，并从同一请求源状态开始。
2. 这些 proposal 构成一个有界 burst。
3. burst 期间暂时不从基础 Edge OpenList 选择下一条边，而是按顺序处理 LLM 建议动作。
4. 每个动作只在真正通过搜索器处理后才移动 proposal cursor。
5. burst 完成或终止后，立即恢复原有 Edge OpenList。

这不是把整条路线当作一个不可分割的宏动作，也不是把未经 Lazy 评价的 StateID 直接塞入 OpenList。

### 6.2 与 Count 原生语义的复用

每条 LLM 边都复用或遵守：

- Count 的 StateRegistry 去重；
- `g` 和真实路径代价；
- 严格 `candidate_real_g < incumbent_bound`；
- 启发式评价和 dead-end 识别；
- `NEW / REOPENED / DUPLICATE_EXISTING` 节点处理；
- 目标检测、父指针和最终计划回溯；
- 对每个新展开或 reopen 的中间状态调用原生 `generate_successors()`。

最后一点很重要：LLM 选定的后续只是短时获得更高调度优先级，中间状态的其他适用动作仍生成为普通边并保留在基础 OpenList 中。因此，在 burst 有限、基础 OpenList 最终恢复的前提下，该机制不因 LLM 路线而主动删除基础搜索分支。

### 6.3 有界和终止条件

默认硬上限为：

- 每个回包最多 8 条 proposal；
- 每条 proposal 最多 100 个动作；
- 每个 burst 所有 proposal 合计最多 100 个动作；
- 最多缓存 8 个 burst。

以下情况会终止当前 proposal：

- 未知或当前不可用动作；
- 父状态无效、是 dead end 或违反全局约束；
- 候选路径代价不再严格低于 incumbent bound；
- proposal 内访问到已访问 StateID；
- `run_id` 或 anytime iteration 已经过期；
- 遇到 dead end 或全局 wall-clock deadline。

对已存在的非 dead-end 状态，proposal 可以以该实际 StateID 重新锚定并继续；是否 reopen 仍由 Count 的原生配置和路径代价决定。

## 7. 严格 LLM-off 和 Shadow Probe

当前实现区分两类对照：

### 7.1 `off`：近乎原生性能基线

- 同时强制当前 `HYBRID_LLM_TRIGGER=0` 和兼容的 `NLM_LLM_TRIGGER=0`；
- 不启动每题 HTTP bridge、LLM client 或 prompt/validator；
- 不序列化触发状态；
- C++ Lazy 热路径不计算平台桶、global stall、父链或回包轮询；
- 不分配 LLM action evaluator。

`off` 仍使用我们修改后的 Lazy/anytime 代码并写实验日志，因此准确表述应为 **near-native Count baseline**，而不是与上游二进制逐指令完全相同。如果论文需要证明我们的通用 anytime 改造本身也没有性能影响，还应对照一份未修改的上游 Count 二进制。

### 7.2 Shadow probe：不介入的触发频率观测

Shadow probe 保留 trigger monitor 和结构化日志，但只用 log 通信模式，不连接 LLM、不接收回包、不创建 rollout。它适合标定触发频率和观察原始 cost-time 曲线，但因为仍执行触发统计与日志，不应当作严格的运行时基线。

## 8. 批量实验控制台

新的 `hybrid_planner.batch_console` 提供以下能力：

- 在整个批次开始时启动一次 vLLM，所有 live 问题复用该服务；
- 每个问题仍使用独立 Python console、HTTP bridge、随机空闲本地端口、phase registry 和结果目录；
- `scale <= 30`：默认时限 1800 秒、每 phase 10 次 LLM 请求，当前最多 8 个搜索器并行共享 vLLM；
- `scale > 30`：默认时限 3600 秒、每 phase 15 次 LLM 请求，当前同组最多并行 2 题；进入大规模组前仍等待小规模组全部结束；
- 同一 manifest 可以混合 `live` 和 `off`；全 `off` 批次根本不启动 vLLM；
- 达到设定时限记为正常 `timeout` 实验结果，不误判为控制器故障。

该调度策略是当前 Depots 实验的资源配置，而不是 Lazy LLM 搜索的算法定义。

## 9. 实验产物和可观测性

每个问题会产生：

- `run.json`：路径、模式、搜索配置、时限、预算和终止元数据；
- `planner.log`：translator、preprocessor、anytime、trigger 和 rollout 的完整日志；
- `phases.csv`：每 phase 的展开量、触发、模型 sample、rollout 和资源统计；
- `incumbents.csv`：首个解和每次严格 cost 改善的时间点，可直接用于画 best-cost–time 阶梯曲线；
- `llm_requests.csv`：请求来源、原因、`g/h`、模型耗时、sample 可用性、执行动作和 abort 统计；
- `sas_plan.*`：各次 incumbent 计划。

批处理层另外生成 `batch_config.json`、`batch_results.csv`、共享 `vllm.log` 和每题 `console.log/job.json`。

## 10. 论文中建议使用的术语

| 概念 | 建议英文名称 | 避免的说法 |
|---|---|---|
| 按搜索语义区分机制 | search-semantics-aware LLM intervention | 一个方法无修改适用所有搜索器 |
| Lazy 平台检测 | expansion-stream heuristic plateau detection | frontier plateau detection |
| Eager 平台机制 | state-frontier plateau detection | Lazy 无法实现平台检测 |
| LLM 动作链介入 | preemptive bounded LLM rollout | direct state injection / macro action |
| 搜索观测到的最低 h | best-so-far observed heuristic value | globally optimal h |
| 不启用 LLM 的基线 | near-native LLM-off baseline | pristine upstream Count |
| 不连接 LLM 的触发日志 | shadow trigger probe | zero-overhead baseline |

## 11. 可直接改写进论文的方法表述

### 11.1 Eager/Lazy 通用性

> Rather than imposing a single intervention rule on different search architectures, we instantiate the same high-level principle—detecting sustained heuristic stagnation—according to each search engine's evaluation semantics. For eager search, evaluated states in the frontier expose their own heuristic values. For lazy search, frontier entries represent predecessor–operator pairs and do not yet carry the successor's exact heuristic value. We therefore detect lazy plateaus from the stream of states evaluated during native expansion.

### 11.2 Lazy 平台机制

> The lazy detector partitions finite heuristic values into bounded-width buckets and summarizes native base-search expansions in fixed-size windows. A bucket is activated only after it accounts for a sufficient fraction and absolute number of expansions across several consecutive windows, while only a limited fraction of expansions reaches lower heuristic buckets. Once activated, the first subsequent native expansion in that bucket that satisfies the shared cooldown becomes an LLM query state. Expansions caused by an LLM rollout are excluded from plateau evidence.

### 11.3 LLM rollout

> Model inference is asynchronous: the base search continues until a response is available. A validated response is then executed as a bounded preemptive rollout. Each proposed action is rechecked and processed through the native lazy-search state transition, duplicate detection, cost-bound, dead-end, goal, and successor-generation logic. Ordinary successors of newly expanded intermediate states remain in the original edge open list, which resumes after the finite rollout terminates.

### 11.4 Anytime 预算

> Request budgets are reset for every anytime phase, preventing early phases from exhausting all model opportunities before later, tighter-bound phases begin. All phases nevertheless share one absolute wall-clock deadline, and responses from a completed phase are cancelled or rejected using run and iteration identifiers.

## 12. 当前可以与不可以直接宣称的结论

### 12.1 可以由代码直接支持的事实

- 系统支持 Eager 原型与 Lazy Count 两种不同搜索语义下的专用接入方式。
- Lazy 平台只使用实际展开时已计算的真实 `h`。
- LLM 请求在 C++ 搜索主线程上是异步的。
- LLM 动作链是有界的，且逐动作复核。
- 中间状态的普通后继会进入原有 Edge OpenList，burst 结束后恢复基础搜索。
- 每 phase 有独立请求预算，所有 phase 共享全局时限。
- `off` 模式不执行 LLM 触发分析或通信。

### 12.2 必须由实验支持后才能宣称

- 平台检测能够稳定选中比随机状态更有价值的介入点。
- LLM rollout 能够提高覆盖率、首解速度或 anytime 解质量。
- 当前 65536/3/0.25/500000 等参数在 Depots 以外也有效。
- 30 规模并行 8 个搜索器、40 规模并行 2 个搜索器且请求预算 15 是通用最优配置。
- 该平台检测是首个或唯一的相关方法。此类新颖性表述还需相关工作检索。

## 13. 建议实验和消融

至少保留以下对照：

1. 未修改 upstream Count；
2. 当前 `off` near-native Count；
3. shadow probe：开启触发分析但不请求 LLM；
4. 完整 live 系统；
5. 只使用 global stall；
6. 只使用 ancestor stagnation；
7. 只使用 expansion plateau；
8. 相同请求频率的随机或周期性介入点；
9. 相同长度预算的随机合法 rollout，用于区分“额外展开”与“LLM 路线质量”。

建议同时报告：覆盖率、首解时间、给定时限的 best cost、anytime area/score、请求数、模型 generation 数、有效 sample 比例、真正执行动作数、rollout abort 原因和总 GPU/CPU 成本。

## 14. 已知边界和论文中应主动说明的事项

- 当前平台参数是根据少量 Depots 问题的 shadow 数据初步标定的，不是理论最优参数。
- 固定展开数保证的是搜索工作量间隔，不是严格的“每两分钟”。大规模问题单次展开更贵，因而相同 500000 展开的 wall time 可能更长。
- 异步 LLM 回包的实际到达时机会影响接下来的扩展顺序，因此 live 实验不会像纯确定性搜索那样完全可重现。
- “保留基础分支”表示 LLM rollout 不主动删除普通后继，不应在没有额外证明时扩展为对所有搜索配置的完备性或最优性保证。
- Count 的当前实验定位是 numeric satisficing/anytime 搜索，不应把结论自动外推到所有数值规划子赛道或所有搜索器。

## 15. 代码地图

| 功能 | 主要文件 |
|---|---|
| Lazy 平台、global/ancestor trigger | `src/search/llm/llm_trigger_monitor.cc` |
| 异步 C++ HTTP bridge | `src/search/llm/llm_bridge.cc` |
| proposal/burst 和 rollout 统计 | `src/search/llm/llm_proposal.cc/.h` |
| Lazy 抢占 rollout 与原生边处理 | `src/search/search_engines/lazy_search.cc/.h` |
| 动作解析和 C++ 符号执行 | `src/search/action_chain_evaluator.cc/.h` |
| 中间状态 PDDL init 导出 | `src/search/global_state.cc` |
| 静态 grounded fact 保留 | `src/translate/grounded_static_facts.py` |
| Anytime deadline、bound、incumbent 日志 | `src/search/search_engine.*`, `src/search/search_engines/iterated_search.*` |
| Prompt、vLLM、前缀验证、单题控制 | `hybrid_planner/` |
| 一次 vLLM 服务的批量调度 | `hybrid_planner/batch_console.py` |
| 自动化验证 | `tests/` |

## 16. 当前验证状态

- Python 配置、prompt、验证、LLM client、anytime 记录、batch policy 和 vLLM 生命周期共 42 项自动化测试在当前环境通过，7 项按平台条件跳过。
- Linux/WSL 上有编译二进制时，`test_lazy_rollout_integration.py` 会用 mock LLM 验证多动作 rollout、普通后继保留和最终计划。
- 最新的 LLM-off 热路径短路和批处理控制台尚需在最终 AutoDL Linux 容器中重新编译并做一次完整验收。
