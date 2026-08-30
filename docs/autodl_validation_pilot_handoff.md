# Count + LLM 初期验证：运行交接说明

## 这次在测试什么

这还不是最终论文实验，而是正式实验前的一轮端到端确认。主要验证：

- Count 能正常求解原始 validation PDDL；
- 一个 vLLM 服务能够持续为多个规划问题提供推理；
- LLM 触发、请求、动作验证和 lazy rollout 整条链路能够工作；
- 30 规模及以下问题可以并行，40 规模问题会等待其他任务结束后单独运行；
- 结果记录完整，并且中断后能够按问题续跑。

脚本固定抽取 18 个 validation 问题：规模 10、20、30 各 5 个，规模 40 取 3 个。小问题时限为 30 分钟，40 规模时限为 1 小时。全部达到时限时，整批最长大约需要 7 小时，实际通常会更短。

## 怎样启动

建议在 `tmux` 中运行，避免关闭终端导致任务中断。

```bash
source /root/miniconda3/etc/profile.d/conda.sh
conda activate PyPACE_env

cd /root/autodl-tmp/count-downward-llm
git pull

# 启动前快速检查脚本语法
bash -n scripts/run_autodl_validation_pilot.sh

# 启动 live LLM 试跑
bash scripts/run_autodl_validation_pilot.sh
```

脚本已经固定好当前数据、模型、Count 和结果目录，不需要再编辑 manifest 或大批处理脚本。

## 正常情况下会看到什么

启动阶段应看到 vLLM 就绪：

```text
[COUNT-BATCH] launching one persistent vLLM: ...
[COUNT-BATCH] vLLM ready models=...
```

每题都会有清楚的开始和结束记录：

```text
[COUNT-BATCH] start job=... scale=... mode=live ...
[COUNT-BATCH] end job=... status=... seconds=...
```

运行较久的问题通常还会看到 LLM 触发、请求或 rollout 信息，以及找到新解时的 incumbent 信息。不是每一道题都必须触发 LLM。

以下结果都表示规划器正常完成了一次尝试：

- `plan_found`：找到了计划；
- `unsolvable`：证明无解；
- `timeout`：正常耗尽 30/60 分钟时限；
- `incomplete`：搜索正常结束但没有完成证明。

`timeout` 或没有找到首个解不等于框架故障。真正需要排查的是 `failed`、vLLM 无法就绪、持续通信失败，或者输出目录没有生成记录。如果所有 18 题都没有找到计划，或者长时间运行的问题完全没有 LLM 请求，也请把它当作异常现象记录下来。

## 结果在哪里

默认结果目录：

```text
/root/autodl-tmp/count-results/depots-numeric-validation-original/qwen3_5-9b-global_step_350/pilot-live
```

优先查看：

- `batch_results.csv`：18 道题的总状态和耗时；
- `vllm.log`：模型服务日志；
- `<job>/console.log`：单题完整控制台日志；
- `<job>/anytime/incumbents.csv`：cost-time 改善曲线；
- `<job>/anytime/llm_requests.csv`：LLM 触发和请求结果；
- `<job>/job_result.json`：该题已经处理完成的续跑标记。

## 中断后如何继续

直接执行完全相同的启动命令：

```bash
cd /root/autodl-tmp/count-downward-llm
bash scripts/run_autodl_validation_pilot.sh
```

已完成、已超时或正常 incomplete 的问题会显示 `resume skip` 并被跳过。断电时仍在运行的题会从头重跑；续跑是问题级的，不会恢复单题内部的搜索内存。

续跑时不要更换 `COUNT_RUN_TAG` 或结果目录。若要测试另一套模型、参数或 LLM-off 模式，应使用新的结果目录，避免与本轮结果混在一起。

## 如果出现异常

先不要删除或覆盖结果目录。请保留并反馈：

1. 终端最后约 100 行输出；
2. 根目录下的 `vllm.log` 和 `batch_results.csv`；
3. 对应失败任务的 `<job>/console.log` 与 `<job>/job.json`。

有这些文件，通常就可以判断是模型服务、通信、规划器还是资源限制问题。
