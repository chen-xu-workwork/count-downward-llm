# 真实中间状态 PDDL 与 prompt 验证

这个测试固定使用 validation 集中的
`problem_scale_20_id_2.pddl`，不调用真实 LLM，也不修改状态导出、PDDL
覆盖或 prompt 转换逻辑。

测试分三步：

1. 临时降低 global-stall 阈值，在 lazy 搜索展开到真实中间状态后触发一次 mock 请求；
2. 保存该状态的完整 runtime PDDL、system/user prompt，并用 Unified Planning
   解析 PDDL、创建模拟器初始状态；
3. 将保存的 runtime PDDL 当成独立问题，在 LLM 完全关闭的情况下交给 Count
   求解。找到计划才算测试通过。

唯一增加的运行时代码是调试记录中的 `runtime_problem` 保存字段；原有处理结果
直接落盘，没有增加修补或重新生成逻辑。

## 在本机 WSL 中运行

```bash
conda activate PyPACE_env

cd "/mnt/e/Python Projects/规划器/planners/Count-Downward"

bash -n scripts/run_scale20_intermediate_state_validation.sh
bash scripts/run_scale20_intermediate_state_validation.sh
```

脚本会自动识别本机数据目录：

```text
/mnt/e/Python Projects/PyPACE/data/generated-pddl/depots-numeric-validation-original
```

WSL 结果默认保存在：

```text
experiments/intermediate_state_validation/results/<问题与时间戳>/
```

整个流程只使用 mock 请求、PDDL/Unified Planning 校验和 LLM-off Count，不启动
vLLM，不需要 GPU，也不需要重新编译当前已编译好的 Count。

捕获阶段默认最多 120 秒，原生再求解阶段默认最多 1800 秒。可临时覆盖：

```bash
COUNT_INTERMEDIATE_CAPTURE_SECONDS=300 \
COUNT_INTERMEDIATE_NATIVE_SECONDS=1800 \
bash scripts/run_scale20_intermediate_state_validation.sh
```

如需在其他机器运行，可以通过 `COUNT_VALIDATION_DATASET_ROOT` 和
`COUNT_INTERMEDIATE_OUTPUT_DIR` 覆盖数据及输出路径。AutoDL 的 `/root/PyPACE/...`
路径仍作为非 WSL 环境的默认回退。

如果捕获阶段已经成功、但后续校验因为环境或脚本问题中断，可以复用原目录，
无需重新搜索：

```bash
COUNT_INTERMEDIATE_OUTPUT_DIR="<已有的完整结果目录>" \
COUNT_INTERMEDIATE_REUSE_CAPTURE=1 \
bash scripts/run_scale20_intermediate_state_validation.sh
```

成功时结尾会出现：

```text
[INTERMEDIATE-STATE-TEST] PASS
```

## 人工审阅文件

脚本会打印本次带时间戳的 `review` 目录。重点查看：

- `intermediate_init.pddl`：Count 直接导出的中间状态 `:init`；
- `intermediate_problem.pddl`：替换初态后的完整新问题；
- `original_problem.pddl`：用于对照的原问题；
- `system_prompt.txt`、`user_prompt.txt`：实际交付模型的两段 prompt；
- `problem_description.txt`：中间 PDDL 转换出的任务描述；
- `validation_report.json`：非初态、PDDL 解析和 prompt 嵌入检查；
- `native_solver.log`、`native_sas_plan`：原生 Count 再求解的证据。

若捕获时搜索本身提前结束，只要已经生成唯一 request 记录，测试仍会继续；最终
判定以保存内容通过校验且原生 Count 找到计划为准。
