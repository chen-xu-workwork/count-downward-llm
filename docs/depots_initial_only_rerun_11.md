# Initial-only 轮次同步修复：仅补跑 11 道题

这次补跑针对原 63 题日志中确认的“新轮次 HTTP 请求先于 Python 消费轮次开始日志，被误判为过期”问题。固定名单为：

- 30 规模：54、316、463、481、486、718、729、737、752、989（10 题）。
- 40 规模：140（1 题）。

每题从原始初态完整重跑，重新分配原有 1800 / 3600 秒时限；不从旧运行的第二或第三阶段接续。保持原 `job_id`、实验种子 101、每轮 10 / 15 组 × 3 样本、模型和提示配置。求解并发上限仍为 8 / 4，但本批只有一道 40 规模题，40 阶段实际同时运行一道。因此它不复现原批次的完整并发负载。

旧结果只读。默认旧目录：

```text
/root/autodl-tmp/count-results/depots-initial-only-30-40/seed_101
```

默认补跑结果写入独立目录：

```text
/root/autodl-tmp/count-results/depots-initial-only-rerun-11/seed_101
```

入口会拒绝新旧目录相同、互相嵌套或经过符号链接指向重叠位置的情况。旧结果目录需要保留原来的配置、选题清单、执行指纹，以及各题的 `job.json`、`job_result.json`、`anytime/run.json`；11 道题还需要 `anytime/llm_requests.csv`。启动时校验原任务和域/提示手册哈希、推理参数、种子及已确认的误拒绝请求记录。旧结果目录可以迁移，按题名与内容哈希对应，不要求旧日志记录的容器绝对路径仍存在。模型描述文件指纹也会核对；这不能代替完整权重哈希，请继续使用原 Depots checkpoint。

## 容器一键启动

Git 同步代码后，在容器仓库根目录执行：

```bash
bash scripts/start_depots_initial_only_rerun_11_tmux.sh
```

流程依次为：校验旧结果与输入 → 增量编译 → 定向回归测试 → 提示预检 → 加载模型 → 仅补跑 11 题。测试使用模拟回复，不加载模型，不消耗 GPU 推理资源。测试失败立即停止，不开始实验。`COUNT_AUTO_BUILD=0` 可跳过已完成的编译，但仍校验二进制能力并执行定向测试。

路径需要调整时，在第一次启动前设置：

```bash
COUNT_ORIGINAL_RESULTS_DIR=/root/autodl-tmp/count-results/depots-initial-only-30-40/seed_101 \
COUNT_RERUN_RESULTS_DIR=/root/autodl-tmp/count-results/depots-initial-only-rerun-11/seed_101 \
COUNT_DATA_ROOT=/root/PyPACE/data \
COUNT_MODEL_PATH=/root/autodl-tmp/Qwen3_5-9B/dapo/data_260811_resume_193/global_step_350/actor/huggingface \
bash scripts/start_depots_initial_only_rerun_11_tmux.sh
```

新入口使用 `COUNT_RERUN_RESULTS_DIR`，忽略旧入口可能遗留的 `COUNT_RESULTS_DIR`。shell 入口不接受 `--output-dir` / `--rerun-from` 路径覆盖，请使用上面两个环境变量，确保 tmux 日志和实际产物写到同一新目录。前台入口是 `bash scripts/run_depots_initial_only_rerun_11.sh`。

若只检查并生成清单，不编译、不运行测试、不加载模型：

```bash
bash scripts/run_depots_initial_only_rerun_11.sh --prepare-only
```

准备操作也需要旧产物、数据和模型描述文件，但不会读取模型权重。

## 运行记录和续跑

```bash
tmux attach -t count-depots-initial-only-rerun11-seed-101
```

`Ctrl-b` 再按 `d` 退出查看，实验继续；会话内 `Ctrl-c` 停止并清理子进程。再次执行相同入口只续跑新目录中未完成或失败的题，绝不把旧目录的“已完成”标志当成补跑完成。11 题全部完成后再启动会直接结束，不重新构建、测试或加载模型。

新目录中的 `replacement_manifest.json` 记录 52 道保留旧结果和 11 道使用新结果的分析映射，以及旧结果与请求记录的哈希。它只是一份映射，不会合并、移动、覆盖或删除旧产物。后续汇总应等 11 道补跑全部完成后使用这份映射，不能取新旧结果中更好的那个来替换。

`test_command.json` / `test_result.json` 记录本次容器测试；`initial_only_config.json` 记录修复版本、源码哈希与旧实验来源。其它文件布局沿用原入口，题目结果在 `initial_only_live/` 下。配置或源码变化会拒绝复用同一个补跑目录，以免混合版本。

按默认并发和时限，纯求解排程预算为 `ceil(10/8)×0.5 + ceil(1/4)×1 = 2` 小时，编译、测试、模型启动、翻译与清理另有开销，因此不能直接按两小时定时关机。

## 手动编译和测试

入口会自动完成这些步骤。若需要单独检查，在同一容器环境的仓库根目录执行：

```bash
python3 build.py release64 -j4
COUNT_BUILD_ROOT="$PWD/builds/release64" python3 -m unittest \
  tests.test_anytime tests.test_iteration_sync tests.test_iteration_sync_http \
  tests.test_initial_only_control tests.test_initial_only_integration \
  tests.test_initial_only_rerun -v
```

应确认没有失败，native 集成测试实际执行而不是 `skipped`。本次修改只做静态审阅，本地没有编译或运行测试/实验。
