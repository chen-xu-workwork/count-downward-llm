# Depots 30/40：初态 LLM 与 Lazy 搜索异步运行

这是一个新的全量 initial-only 对照，使用原验证集 30 规模的 50 题和 40 规模的 13 题。每个 anytime 阶段从原始初态 `s0` 建立请求队列；按可用槽位逐组发送，原生搜索同时继续。收到有效回复后在当前阶段使用其候选动作，不等整批回复收齐。

当前阶段找到新解或结束时，取消该阶段尚未完成的本地请求和模型任务；晚到的回复丢弃，不注入下一阶段。新轮次请求也可先于延迟的日志通知确认轮次切换，并触发旧任务取消。服务端如何释放推理资源取决于其取消处理，不能保证 GPU 请求瞬间终止。下一阶段再次从原始初态建立自己的请求队列。只向初态提问，三个运行中状态触发器仍关闭；完整解与部分解都保留。

| 设置 | 30 规模 | 40 规模 |
| --- | ---: | ---: |
| 题数 | 50 | 13 |
| 求解并发上限 | 8 | 4 |
| 单题总时限 | 1800 秒 | 3600 秒 |
| 每轮初态请求组上限 B | 10 | 15 |
| 每组采样 K | 3 | 3 |
| 每轮采样预算上限 B×K | 30 | 45 |

沿用种子 101、原 Depots 模型、温度 0.7、top-p 0.9、max tokens 16384、单次推理超时 300 秒和 max retries 3。每个求解进程最多并行 6 个生成任务，通常对应两组初态请求在途。这里的 30 / 45 是**每轮上限**；如果搜索先结束本轮，队列可能尚未发完，已发出的请求也可能取消。统计应报告实际提交、模型生成、完成/取消和采用情况，不应声称每轮一定完成 30 / 45 次，或全程实际调用/token 消耗相同。

## 独立启动入口

Git 同步后，先在容器仓库根目录单独编译本次代码：

```bash
python3 build.py release64 -j4
```

编译成功后启动实验：

```bash
bash scripts/start_depots_initial_only_async_30_40_tmux.sh
```

默认结果目录为：

```text
/root/autodl-tmp/count-results/depots-initial-only-async-30-40/seed_101
```

需要覆盖路径时，在首次启动前设置：

```bash
COUNT_ASYNC_RESULTS_DIR=/root/autodl-tmp/count-results/depots-initial-only-async-30-40/seed_101 \
COUNT_DATA_ROOT=/root/PyPACE/data \
COUNT_MODEL_PATH=/root/autodl-tmp/Qwen3_5-9B/dapo/data_260811_resume_193/global_step_350/actor/huggingface \
bash scripts/start_depots_initial_only_async_30_40_tmux.sh
```

新入口使用专用 `COUNT_ASYNC_RESULTS_DIR`，忽略旧脚本可能遗留的 `COUNT_RESULTS_DIR`，并在写日志前检查输出目录不与旧串行实验重叠。shell 入口不接受 `--output-dir` 覆盖，请使用该环境变量。前台入口是 `bash scripts/run_depots_initial_only_async_30_40.sh`。

入口只启动实验，**不会自动编译，也不会自动运行测试**。启动顺序是冻结题目及参数、检查已有二进制的 `TRACS_INITIAL_ONLY_ASYNC_V1` 能力和所需文件、检查运行路径和提示词，然后加载模型开始实验。缺少编译产物或仍是旧二进制时会在加载模型前停止，请先单独编译。继承或显式设置 `COUNT_AUTO_BUILD=1` 都不能开启此入口的编译；`COUNT_BUILD` 只选择已经编译好的构建版本。

只准备清单、不编译、不运行测试、不加载模型：

```bash
bash scripts/run_depots_initial_only_async_30_40.sh --prepare-only
```

正常启动与准备操作需使用同一容器路径及配置。配置和源码哈希会冻结，修改后需新输出目录。

## 查看及续跑

```bash
tmux attach -t count-depots-initial-only-async-seed-101
```

`Ctrl-b` 再按 `d` 退出查看；会话内 `Ctrl-c` 停止并清理当前流程。再次运行相同入口只续跑此异步目录中未完成或失败的题。全部 63 题已有完整记录时直接结束，不再加载模型。

产物布局沿用 `selection.json`、`initial_only_config.json`、`manifest_initial_only.json`、`initial_only_live/`。配置记录 `llm_initial_async=true` 与 `initial-only-async-v1` 协议；批处理、作业和单题记录也区分异步与串行，禁止相互续跑。

日志中的 `initial_async_seconds` 表示异步初态请求窗口的持续时间，它与搜索重叠，不是串行等待时间，不能从总运行时间中直接扣除来估计搜索耗时。

旧 `depots-initial-only-30-40` 是“每轮先收齐再继续搜索”的串行协议，旧 `rerun_11` 入口只用于修复那份串行结果中的 11 道题。两者都不能作为这次异步全量实验的断点或替换结果。旧数据和入口保留；本次全量结果写入新目录，不自动合并旧产物。相同 job ID 与种子尽量保持采样标识一致，但实际调用量会随搜索和并发变化。

## 可选手工测试

测试与实验入口分开。需要检查时，在容器原有运行环境中完成上述编译，再手工执行以下定向测试；这些测试使用模拟回复，不加载 LLM 模型，其中 native 集成测试会运行小型规划任务：

```bash
COUNT_BUILD_ROOT="$PWD/builds/release64" python3 -m unittest \
  tests.test_anytime tests.test_iteration_sync tests.test_iteration_sync_http \
  tests.test_initial_only_control tests.test_initial_only_integration \
  tests.test_initial_only_rerun tests.test_initial_only_async \
  tests.test_initial_only_async_integration -v
```

确认 native 集成测试没有 `skipped`。测试不会自动启动这 63 道题的实验；测试通过后仍需执行前面的实验启动命令。

2026-11-12 本地验证：Windows / Python 3.13.5 下运行了 10 个纯 Python 测试模块，共 113 项，112 项通过；1 项真实 PDDL 提示与校验测试因缺少 `pddl` / `unified-planning` 依赖跳过。两份启动脚本通过 Bash 语法检查。本地未编译、未运行 native 集成测试、未加载模型或启动实验。
