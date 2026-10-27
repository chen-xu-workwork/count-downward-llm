# Depots 模型到 Zenotravel 的迁移实验

使用现有 Depots checkpoint，在 Zenotravel 原始验证题集上运行完整 TRACS–Lazy，不训练模型，不运行新的 Off baseline。默认先从规模 30 的 45 题中固定抽取 20 题，从规模 40 的 15 题中固定抽取 10 题，再将缺少完整旧 baseline 记录的题在同规模内固定随机替换，最终仍为 30 题。替换只依据记录是否完整，不依据 baseline 解出与否。抽样 seed 为 `20261027`，搜索和生成 seed 为 `101`。

规模 30 每题 1800 秒、每个 anytime 阶段最多 10 次状态请求；规模 40 每题 3600 秒、每阶段最多 15 次状态请求。默认 6 并行，K=3，触发间隔倍率分别沿用 0.5、0.25。`llm_partial_only=0`，不会继承上一个 Partial-only 实验的过滤设置。

## 容器中一键启动

将补丁包中的新增模块、脚本、Zenotravel 领域资源和共享 `extrapolation` helper 依赖同步到现有 Count-Downward 项目根目录。不需要重新训练模型或编译 C++，也不要求将旧 baseline 结果放到容器。本地 Windows 上用于准备检查的结果目录不要复制到 Linux 续跑；部署源码和材料后，容器首次启动会用相同抽样 seed 重新生成同一题单。以下假设项目位于 `/root/autodl-tmp/count-downward-llm`，数据根目录为 `/root/PyPACE/data`：

```bash
cd /root/autodl-tmp/count-downward-llm

COUNT_RUNTIME_ENV_PREFIX="/root/miniconda3/envs/verl_env" \
COUNT_DATA_ROOT="/root/PyPACE/data" \
COUNT_MODEL_PATH="/root/autodl-tmp/Qwen3_5-9B/dapo/data_260811_resume_193/global_step_350/actor/huggingface" \
COUNT_RESULTS_DIR="/root/autodl-tmp/count-results/depots-to-zenotravel-30-40/seed_101" \
COUNT_TMUX_SESSION="count-depots-to-zenotravel-seed101" \
bash scripts/start_zenotravel_transfer_30_40_tmux.sh
```

只需将 `COUNT_DATA_ROOT` 改成容器实际的 `data` 目录，下面的结构保持：

```text
data/
  generated-pddl/zenotravel-numeric-validation-original/
    domain.pddl
    problems/problem_scale_30_id_*.pddl
    problems/problem_scale_40_id_*.pddl
  converted-python-style/zenotravel-numeric-segmented/validation/
    domain.txt
```

`COUNT_DATASET_ROOT` 可以单独指定 PDDL 题集根目录，`COUNT_PROMPT_DOMAIN_CODE` 可以单独指定 Zenotravel `domain.txt`。默认优先使用上述转换材料里的说明；缺失时由 Python 入口使用随项目携带的 Zenotravel 手册。题集的 PDDL domain 必须是 `zeno-travel`，规范化后应与随项目携带的 `hybrid_planner/prompting/resources/zenotravel/domain.pddl` 已审计 schema 匹配；手册会检查五种动作和十种状态查询接口。模型始终指向现有 Depots checkpoint。

入口只生成和运行 Live 题单，不含 Off 阶段。启动前会固定抽样和有效配置，避免错误使用 Depots 手册。默认使用 GPU `0`、vLLM 端口 `8091`，可通过 `COUNT_VLLM_GPUS` 和 `COUNT_VLLM_PORT` 修改。正式启动前会检查端口空闲，防止错连已有的同名模型服务。等其他实验结束后再运行，避免资源和端口争用。

## 查看与准备检查

```bash
tmux attach -t count-depots-to-zenotravel-seed101
# Ctrl-b，再按 d：离开界面，实验继续。

tail -f /root/autodl-tmp/count-results/depots-to-zenotravel-30-40/seed_101/tmux-run.log
```

如果不使用 tmux，把启动命令最后一行改为 `bash scripts/run_zenotravel_transfer_30_40.sh` 即可。

只用 Python 标准库核对题集、说明和抽样，不启动模型或搜索，也不执行 Unified Planning（UP）验证：

```bash
COUNT_RUNTIME_ENV_PREFIX="/root/miniconda3/envs/verl_env" \
COUNT_DATA_ROOT="/root/PyPACE/data" \
COUNT_RESULTS_DIR="/root/autodl-tmp/count-results/depots-to-zenotravel-30-40/seed_101" \
bash scripts/run_zenotravel_transfer_30_40.sh --prepare-only
```

在同一个容器中，prepare-only 和正式运行应使用相同 Python 环境、模型路径、题集、手册、seed 和结果目录，复用冻结题单。它不要求模型、vLLM 或规划器二进制已经就位。正式运行会在启动模型和求解之前，按每个规模选择一题构建实际提示词，并使用 UP 解析与校验，检查目标领域通路。

## 结果与预检文件

结果根目录保存以下内容：

- `selection.json`：冻结抽样、候选池及文件哈希，`replacements` 记录因 baseline 记录缺失而发生的替换。
- `transfer_manifest.json`：仅含 Live 任务的题单与每题预算。
- `transfer_config.json`：有效参数、模型路径和领域资源等配置。
- `comparison_keys.json`：原始题目文件名、规模和哈希，供离线匹配旧 baseline。
- `command.json`：实际批处理启动命令。
- `transfer_live/`：批次结果、逐题日志、计划及 anytime 记录。
- `prompt_preflight/` 和 `prompt_preflight.json`：正式启动前每个规模一题的实际提示词材料与 UP 校验结果，不调用模型。

这些文件区分了题单准备和正式运行前的通路校验。仅完成 `--prepare-only` 不等于已经通过 UP 校验。

## 抽样、续跑和旧 baseline 对照

默认抽样 30=20、40=10，可通过 `COUNT_SAMPLE_30`、`COUNT_SAMPLE_40` 与 `COUNT_SAMPLE_SEED` 更改。更改抽样、checkpoint、手册、seed 或其他实验参数时应使用新结果目录。仅匹配 30/40 原始题，不会加入 10/20 或增强中间状态题。

抽样分两步：先按固定 seed 从完整题池抽取 20+10 题；只对缺少完整旧 baseline 记录的题，从同规模、未选中且已有完整记录的候选题中固定随机补入。当前默认题单只替换一题，另外 29 题保持不变。已保存的未解或正常超时结果仍属于有效 baseline 记录，不会因为未解而被排除。

补丁包携带 `hybrid_planner/prompting/resources/zenotravel/baseline_availability.json`，保存已核对的 baseline 记录可用名单。入口会核对该名单并复现替换，默认最终 30 题全部可与旧 baseline 配对；容器中不需要复制原 baseline 文件。替换过程会保存在 `selection.json` 的 `replacements` 字段中。

中断后用相同变量、相同结果目录重新启动。正常已完成的题跳过，失败或中断的题重跑；单题搜索不能从中间状态恢复。不要将旧 baseline 的结果文件复制进这次 Live 目录。

沿用已有 Zenotravel baseline 时，可使用 `comparison_keys.json` 按原始题目文件名逐题匹配，并核对规模与题目哈希，只统计最终冻结的这 30 道题。历史归档的 `batch_results.csv` 若只有表头，可从各题的 `job_result.json` 和 `anytime/` 读取已有结果。必须核对 domain/题目版本、时限和统计截止口径。旧 baseline 若来自不同硬件或不同并行配置，时间比较应明确说明，不能据此声称严格的同硬件加速。本入口不要求旧 baseline 文件在线，不会重跑 baseline，也不会按 baseline 成败筛题。

## 耗时与环境

现有批处理器先跑完规模 30，再进入规模 40，两个规模之间有等待边界。默认 6 并行，20 道 30 规模题的搜索预算最多约 `ceil(20/6) × 0.5 = 2` 小时；10 道 40 规模题约 `ceil(10/6) × 1 = 2` 小时，合计约 4 小时，另加模型加载、题目转换和保存开销。这不是进程级硬截止，脚本结束后也不会自动关闭租用实例。

可用 `COUNT_PARALLELISM` 调整两个规模的并行数。高级设置有 `COUNT_EXPERIMENT_SEED`、`COUNT_LLM_MODEL_NAME`、`COUNT_PLANNER_PYTHON`、`COUNT_BUILD`、`COUNT_RUNTIME_ENV_PREFIX`、`COUNT_RUNTIME_LIB_DIR` 和 `NLM_VLLM_EXECUTABLE`。运行环境默认优先使用已激活且具有 vLLM 的 Conda 环境，其次尝试 `/root/miniconda3/envs/verl_env`；tmux 会显式传递本次环境，避免使用旧 server 保存的路径。
