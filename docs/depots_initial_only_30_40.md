# Depots 30/40：仅在每轮开头调用 LLM 的对照实验

本入口只运行 Lazy 的 `initial-only` 对照，不重跑 baseline，也不重跑常规运行中触发 LLM 的实验。直接使用原验证集中规模 30 的 50 题和规模 40 的 13 题；规模 10、20 不会进入清单。数据仍由原目录读取，无需打包或复制子集。源码和已有的 Depots 提示手册通过 Git 同步即可。

## 实验定义

每个 anytime 搜索阶段均使用**原始问题的初始状态 s0**，不使用上一阶段搜索到的中间状态。Lazy 完成 s0 的初始化和第一次展开、生成常规后继后，进入准备期：重复提交初态请求，等待它们完成、报错或超时，再允许后续常规搜索和 LLM rollout 推进。准备期计入同一道题的时间预算。

| 设置 | 规模 30 | 规模 40 |
| --- | ---: | ---: |
| 题数 | 50 | 13 |
| 单题总时限 | 1800 秒 | 3600 秒 |
| 同时求解题数 | 8 | 4 |
| 每个 anytime 阶段初态请求组数 B | 10 | 15 |
| 每组采样数 K | 3 | 3 |
| 每阶段采样尝试上限 B×K | 30 | 45 |

沿用原规模策略，因此 40 规模为 15×3=45 次，30 规模为 10×3=30 次。每个求解进程最多并行 6 个生成任务（通常对应两组请求）。准备阶段结束后才逐批使用已收集的回复，避免一次发送全部回复超过桥接队列容量。三个运行中触发器（expansion plateau、global stall、ancestor stagnation）全部关闭；普通 `REQUEST_INITIAL` 单次请求开关也关闭，由专用策略负责整个准备流程。

完整解和经过验证的部分解均保留，`partial_only=0`。温度 0.7、top-p 0.9、max tokens 16384、单次 LLM 超时 300 秒、max retries 3、实验种子 101，沿用原实验设置。这里固定的是**每阶段采样预算上限**；不同策略可能进入不同数量的 anytime 阶段，超时/取消/重试也会影响实际消耗，所以不能直接声称全程实际调用数或 token 数完全一致。分析时应同时报告实际请求、完成/取消情况和准备期耗时。

## 容器中一键启动

在容器的 Count-Downward 仓库根目录执行：

```bash
bash scripts/start_depots_initial_only_30_40_tmux.sh
```

默认数据路径是 `/root/PyPACE/data/generated-pddl/depots-numeric-validation-original`。默认模型路径是 `/root/autodl-tmp/Qwen3_5-9B/dapo/data_260811_resume_193/global_step_350/actor/huggingface`。若容器路径不同，在**第一次启动前**覆盖：

```bash
COUNT_DATA_ROOT=/root/PyPACE/data \
COUNT_MODEL_PATH=/root/autodl-tmp/your-model/actor/huggingface \
COUNT_SMALL_PARALLELISM=8 \
COUNT_LARGE_PARALLELISM=4 \
bash scripts/start_depots_initial_only_30_40_tmux.sh
```

也可用 `COUNT_DATASET_ROOT` 直接指定包含 `domain.pddl` 和 `problems/` 的完整数据集目录；它优先于 `COUNT_DATA_ROOT` 推导路径。提示手册默认读取仓库自带的 `hybrid_planner/prompting/resources/depots/domain_NOassert.txt`，可用 `COUNT_PROMPT_DOMAIN_CODE` 指定另一路径。

若要前台运行：

```bash
bash scripts/run_depots_initial_only_30_40.sh
```

入口在容器中默认先执行 `python3 build.py release64 -j4` 增量编译，再检查二进制是否具备 `TRACS_INITIAL_ONLY_PREPARATION_V1` 能力标记，最后启动模型和实验。构建失败或二进制过旧会在模型启动前停止。`COUNT_BUILD_JOBS` 可调整编译并行数；已自行编译好时可设 `COUNT_AUTO_BUILD=0`，但仍会检查能力标记。运行期间不要在同一仓库重新编译或替换规划器。这里描述的是容器运行入口的行为，本次修改未在本地编译或执行实验。

沿用先前入口的环境选择：优先当前含 vLLM 的 Conda 环境，其次 `/root/miniconda3/envs/verl_env`；可用 `COUNT_RUNTIME_ENV_PREFIX` 指定。`COUNT_RUNTIME_LIB_DIR` 可指定包含兼容 `libstdc++.so.6` 的库目录，解决 `CXXABI_1.3.15` 问题。GPU 默认 `0`，端口默认 `8091`，分别可用 `COUNT_VLLM_GPUS`、`COUNT_VLLM_PORT` 修改。

`COUNT_BUILD` 仅接受 `build_configs.py` 中的构建配置名（默认 `release64`），不接受二进制目录路径。

## 启动前只生成清单

```bash
bash scripts/run_depots_initial_only_30_40.sh --prepare-only
```

这一步检查域、题数和提示手册，冻结清单、参数及源码哈希，不编译、不加载模型、不运行规划器。应在最终准备运行的容器与环境中准备；冻结文件含绝对路径和 Python 路径，不应把本地 prepare-only 生成的结果目录搬进容器继续使用。

默认结果目录：

```text
/root/autodl-tmp/count-results/depots-initial-only-30-40/seed_101/
  selection.json                63 道原题及 SHA-256
  manifest_initial_only.json    每题规模、预算与时限
  initial_only_config.json      策略、模型路径、提示手册、源码哈希等
  command.json                  实际批处理命令
  comparison_keys.json          对齐已有 adaptive/baseline 的题名及哈希
  execution_fingerprint.json    实际规划器二进制与模型描述文件哈希
  pipeline_status.json          构建、预检、运行、完成/中断/失败状态
  prompt_preflight/             30/40 各一题的初态提示词检查产物
  initial_only_live/            批处理汇总、每题日志和运行产物
  tmux-run.log                  tmux 入口的完整日志
```

可用 `COUNT_RESULTS_DIR` 指定不同输出目录。首次运行后会锁定题集、参数、并发、源码等；变化会拒绝续跑，避免混合不同实验。切换模型、策略、并发或源码时请选新输出目录。编译并行数和是否跳过构建仅控制启动，不改变实验参数，允许在续跑时调整。

## 查看、停止与续跑

```bash
tmux attach -t count-depots-initial-only-seed-101
```

按 `Ctrl-b` 再按 `d` 只退出查看，实验继续运行。在会话内按 `Ctrl-c` 可停止当前流水线；退出前等待其清理子进程。再次执行相同启动命令会检查已完成题目的策略、种子与参数，只运行未完成或失败的题；正常到达单题时限的结果保留为已完成记录。全部 63 题已有完整记录时，会直接结束，不再编译或启动模型。同一结果目录的并行重复启动会被锁拒绝。

`COUNT_PARALLELISM` 可以统一设置两档并发，单独的 `COUNT_SMALL_PARALLELISM` / `COUNT_LARGE_PARALLELISM` 优先。例如统一 2 并发应在首次运行时设置 `COUNT_PARALLELISM=2`；如 shell 曾导出单档变量，需同步调整或取消它们。两档分阶段执行，不同时占用 8+4 个求解槽。

按默认时限和 8/4 并发，单纯求解的保守排程预算为 `ceil(50/8)×0.5 + ceil(13/4)×1 = 7.5` 小时。编译、模型启动、翻译/清理与进程切换有额外开销，因此这不是严格的关机上限；定时关机需另留余量。准备期的 LLM 等待已包含在单题时限内。

## 用于论文时的解释范围

这是“相同每阶段采样预算上限下，初态集中调用与搜索中调用”的位置/时机对照；它没有复现 Silver 等人全部提示词、训练与搜索实现，因此应避免直接标作对该方法的完整复现。应与同题、同模型、同种子和同样推理配置的原 adaptive 结果做配对分析，并沿用已有 baseline 作为参考。主要看覆盖率、方案质量和总耗时，同时核对实际 LLM 用量及准备期占比。
