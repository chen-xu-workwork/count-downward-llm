# PyPACE / Count 18 题验证 Pilot 最终执行报告

执行日期：2026-08-30（Asia/Shanghai）  
执行环境：AutoDL A800 80 GiB，`verl_env`  
执行结论：**18/18 `plan_found`，0 failure，批处理退出码 0**

## 1. 最终结论

正式 pilot 于 10:27:42 建立结果目录，于 15:59:32 写完最终结果，实际墙钟时间约 5 小时 31 分 50 秒。18 个问题均在各自时间窗内至少找到一个可行方案；所有 `job_result.json` 的 `return_code` 均为 0，`batch_results.csv` 没有错误字段。

`plan_found` 只表示找到了可行方案，不等同于证明最优。scale 20/30 题使用 1800 秒上限，scale 40 使用 3600 秒上限；多数中大规模题在保留 incumbent 后继续 anytime 搜索至时间窗结束。

正式结果保存在远端：

`/root/autodl-tmp/count-results/depots-numeric-validation-original/qwen3_5-9b-global_step_350/pilot-live`

远端结果总量约 5.5 MiB；执行结束后 vLLM、Count 搜索进程均已退出，GPU 无计算进程，8091 端口空闲。

## 2. 验证链路与代码调整

本次完成的最小调整：

1. 将共享 pilot 的默认模型路径从 checkpoint 根目录修正为真实 Hugging Face 目录：
   `/root/autodl-tmp/Qwen3_5-9B/dapo/data_260811_resume_193/global_step_350/actor/huggingface`。
2. 在 `VLLMService.start()` 创建的 vLLM 子进程环境中优先加入当前 conda 环境的 `lib`，解决 flash-attn 所需 `CXXABI_1.3.15` 与系统旧 `libstdc++` 的冲突；作用域仅限 vLLM 子进程。
3. 新增一项回归测试，验证 conda C++ 运行库只注入 vLLM 启动环境。
4. 修复 Count `release64` translator、preprocess 与 downward 构建产物。当前 IRHFF pilot 构建未链接存在机器级混用问题的 Coin/Clp LP solver。

没有升级或降级 vLLM、Torch、Transformers、flash-attn 等既有核心包；没有修改系统动态库。

## 3. 依赖与测试

关键版本：

- Python 3.12.13
- verl 0.9.0.dev0
- vLLM 0.20.2
- Torch 2.11.0
- Transformers 5.10.2
- flash-attn 2.8.3
- unified-planning 1.3.0
- pddl 0.4.8
- lark 1.2.2

为保留 vLLM 0.20.2 对 `lark==1.2.2` 的要求，`pddl 0.4.8` 使用 `--no-deps` 安装。尽管其包元数据声明 `lark<1.2.0`，实际已使用 lark 1.2.2 成功解析本项目真实 domain/problem；Unified Planning 也通过同一真实数据验证。

最终测试：

- Count 完整测试集：44/44 通过。
- Count-only smoke：`plan_found`，最终 cost 15。
- scale-10 live smoke：vLLM 启停及桥接通过。
- scale-20 live smoke：真实完成 1 次 LLM 请求、3 个可用 sample 和 rollout。
- `--resume`：正确跳过已完成任务，且全部完成时不会启动 vLLM。
- `git diff --check`：通过。

`pip check` 仍报告三项已知状态：

- `opencv-python-headless` 要求 NumPy 2，而环境为 NumPy 1.26.4（执行前已存在）。
- `decord 0.6.0` 的平台元数据警告（执行前已存在）。
- `pddl 0.4.8` 声明 lark `<1.2.0`，而保留的是 vLLM 所需 lark 1.2.2（本次有意选择，已用真实 PDDL 验证）。

裸 Python 直接 `import flash_attn` 仍会优先加载系统旧 `libstdc++` 并失败；正式运行入口通过 `VLLMService` 对 vLLM 子进程自动设置正确运行库，18 题完整执行已验证该修复。后续应继续使用共享入口，不应替换系统库或全局导出 `LD_LIBRARY_PATH`。

## 4. 正式结果汇总

| Scale | 题数 | plan_found | 请求 | 返回样本 | 可用样本 | 注入状态 | incumbent | 首个方案平均时间 | 最终 cost 均值 | 最终长度均值 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 10 | 5 | 5 | 0 | 0 | 0 | 0 | 26 | 0.05 s | 20.4 | 15.4 |
| 20 | 5 | 5 | 59 | 174 | 158 | 192 | 26 | 0.73 s | 52.8 | 29.2 |
| 30 | 5 | 5 | 32 | 87 | 80 | 565 | 17 | 525.96 s | 187.6 | 87.6 |
| 40 | 3 | 3 | 19 | 57 | 52 | 239 | 3 | 665.04 s | 295.0 | 105.0 |
| **合计** | **18** | **18** | **110** | **318** | **290** | **996** | **72** | **257.16 s** | **121.61** | **54.22** |

可用样本比例为 290/318，约 91.2%。请求状态为：88 `partial`、18 `ok`、4 `stale_iteration`。`partial` 表示样本中至少有一部分动作链可用，并不表示请求失败；整个 pilot 的 transport failure 为 0。触发原因包括 69 次 `ancestor_stagnation`、39 次 `global_stall` 和 2 次 `expansion_plateau`。

模型生成启动总数为 330；它高于请求表中的 318 个返回样本，是因为 4 个跨 iteration 变旧的请求已启动生成但结果记为 `stale_iteration`。

## 5. 每题结果

| # | 问题 | 耗时 | incumbent | 首个方案 | 初始 cost | 最终 cost | 长度 | 请求 | 可用样本 | 注入状态 |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | s10/id24 | 6.2 s | 6 | 0.22 s | 57 | 15 | 17 | 0 | 0 | 0 |
| 2 | s10/id39 | 0.7 s | 5 | 0.00 s | 44 | 24 | 15 | 0 | 0 | 0 |
| 3 | s10/id51 | 0.7 s | 1 | 0.00 s | 23 | 23 | 10 | 0 | 0 | 0 |
| 4 | s10/id91 | 6.8 s | 4 | 0.00 s | 44 | 24 | 14 | 0 | 0 | 0 |
| 5 | s10/id114 | 3.2 s | 10 | 0.01 s | 80 | 16 | 21 | 0 | 0 | 0 |
| 6 | s20/id2 | 1804.7 s | 6 | 0.49 s | 291 | 57 | 31 | 15 | 39 | 84 |
| 7 | s20/id42 | 1802.9 s | 1 | 0.17 s | 98 | 98 | 36 | 10 | 23 | 8 |
| 8 | s20/id56 | 1803.5 s | 7 | 2.67 s | 204 | 37 | 29 | 14 | 39 | 67 |
| 9 | s20/id70 | 1803.3 s | 6 | 0.28 s | 149 | 37 | 31 | 10 | 28 | 19 |
| 10 | s20/id88 | 1802.8 s | 6 | 0.04 s | 86 | 35 | 19 | 10 | 29 | 14 |
| 11 | s30/id3 | 1802.1 s | 1 | 668.71 s | 284 | 284 | 114 | 5 | 12 | 82 |
| 12 | s30/id26 | 1801.8 s | 1 | 1569.47 s | 437 | 437 | 172 | 11 | 27 | 174 |
| 13 | s30/id33 | 1802.8 s | 5 | 81.65 s | 568 | 88 | 63 | 6 | 15 | 123 |
| 14 | s30/id44 | 1805.5 s | 5 | 115.45 s | 508 | 71 | 58 | 5 | 15 | 122 |
| 15 | s30/id54 | 1802.8 s | 5 | 194.54 s | 163 | 58 | 31 | 5 | 11 | 64 |
| 16 | s40/id24 | 3602.5 s | 1 | 591.12 s | 402 | 402 | 114 | 8 | 24 | 84 |
| 17 | s40/id50 | 3603.0 s | 1 | 404.32 s | 257 | 257 | 104 | 5 | 12 | 48 |
| 18 | s40/id62 | 3603.1 s | 1 | 999.68 s | 226 | 226 | 97 | 6 | 16 | 107 |

## 6. 资源与运行观察

- vLLM 常驻时显存约 73.25 GiB，没有 OOM。
- planner 记录的最大峰值内存为 10,951,424 KiB，约 10.45 GiB。
- scale-10 问题在触发阈值前即完成，因此没有 LLM 请求；这符合保守触发策略。
- scale-20/30 可并行 2 题；scale-40 按 exclusive 策略串行执行。
- 18 题结果目录完整包含 `job_result.json`、plan、`incumbents.csv`、`phases.csv`、`llm_requests.csv` 与 planner/console 日志。

## 7. 协作状态与后续建议

PyPACE 保留了协作者原有的未提交修改：

`src/verl_DAPO/9B_8xA800_train.bash`

Count 仓仅有本次明确改动：

- `hybrid_planner/llm/vllm_service.py`
- `scripts/run_autodl_validation_pilot.sh`
- `tests/test_vllm_service.py`

本次没有创建提交，也没有执行 pull、reset 或 checkout。

建议后续：

1. 由代码维护者审阅并提交上述 3 个 Count 文件，避免多人环境中的未提交改动丢失。
2. 若需要比较 LLM 增益，应使用相同 18 题、相同时间窗再运行 `COUNT_RUN_MODE=off` 基线；本次仅能确认 live 链路稳定和方案覆盖率，不能单独归因 LLM 的收益。
3. 进一步分析应比较 first/final cost、time-to-first-plan、expanded/generated、LLM 注入状态与 off 基线，而不能只看 18/18 覆盖率。
4. 保留 `--resume` 与独立 run tag，避免覆盖本次正式结果。
