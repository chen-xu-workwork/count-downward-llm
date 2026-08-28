# Count-Downward LLM/Lazy 迁移验收脚本

这些脚本不是性能 benchmark，而是小规模、确定性的功能验收。它们使用仓库自带的
`issue34` PDDL 问题和一次性本地 mock HTTP 服务，不调用真实 LLM，也不需要 API
密钥。每个脚本只验证一个核心性质，成功时打印绿色 `PASS`，失败时以非零状态退出。

## 运行方法

先在 WSL 中进入 Count-Downward 仓库，并确保 `release64` 已经编译：

```bash
cd '/mnt/e/Python Projects/规划器/planners/Count-Downward'
bash tests/acceptance/run_all.sh
```

也可以单独运行任意脚本，例如：

```bash
bash tests/acceptance/02_complete_lazy_rollout.sh
```

脚本在 `/tmp/count-...` 下建立彼此隔离的工作目录，并在结束时打印目录位置。完整的
`search.log`、mock 服务日志和生成的计划都保留在那里，便于复核。不要并行运行这些
脚本；总入口始终按顺序运行。

如果构建目录不是 `builds/release64`，可设置：

```bash
COUNT_BUILD_ROOT=/path/to/build bash tests/acceptance/run_all.sh
```

## 七项测试分别证明什么

| 脚本 | 核心功能 | 成功时应看到的关键证据 |
|---|---|---|
| `01_llm_off_baseline.sh` | LLM 关闭时不破坏 Count 原生 Lazy 搜索 | `Solution found.`、`Plan cost: 18`、没有请求/通信事件且 rollout 动作数为 0 |
| `02_complete_lazy_rollout.sh` | 完整状态协议、逐边抢占式 rollout、重复状态 re-anchor、普通分支保留 | mock 收到 10 步请求、`result=completed`、处理 10 步、`llm_normal_edges_generated > 0` |
| `03_invalid_proposal_fallback.sh` | 未知/不适用动作不能污染搜索，失败后恢复基础 OpenList | `proposal_aborted reason=3`，随后仍有 `Solution found.` |
| `04_rollout_budget_cap.sh` | 响应级动作硬预算限制抢占长度 | 模型返回 10 步，但 accepted/prevalidated/processed 都只有 3 |
| `05_pending_request_and_bound.sh` | OpenList 暂空但请求在途时不提前失败；严格 bound 仍生效 | 等待延迟响应后出现 `proposal_aborted reason=5`，然后才报告无解 |
| `06_anytime_bound_and_deadline.sh` | incumbent 改善、下一 phase 严格 bound、所有 phase 共用绝对 deadline | 代价严格下降、第二 phase 的 `bound=18`、最后出现 `NLM-ANYTIME-RUN-TIMEOUT` |
| `07_lazy_expansion_plateau.sh` | Lazy 用真实展开状态 h 连续确认平台，并请求确认后的首个同桶状态 | 第 2 次扩展出现 `event=activated`，请求发生在第 3 次扩展且 `reason=expansion_plateau` |

`reason=3` 是 `INAPPLICABLE`，`reason=5` 是 `BOUND_PRUNED`。这些整数来自 C++
结构化日志；脚本同时检查具名统计字段，避免只依赖数字含义。

## 不在这些脚本范围内的内容

- 真实模型的生成质量、GPU/vLLM 部署和网络稳定性；
- 多 benchmark、多随机种子的论文性能实验；
- 与 NLM-CutPlan 的性能优劣比较。

这些验收脚本证明的是搜索接入和安全边界正确，不代表 LLM 能提高所有问题的规划性能。
