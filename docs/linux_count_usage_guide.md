# 修改版 Count-Downward 的 Linux/AutoDL 使用手册

> 适用对象：将当前 Count-Downward 迁移到 AutoDL 或其他 Linux GPU 容器，运行 LLM-off 基线、真实 LLM 介入和批量 anytime 实验。  
> 当前状态：命令与依赖基于源码设计；最终 CUDA/vLLM 组合仍需在实际 AutoDL 镜像内验证。

## 1. 推荐的目录布局

以下路径只是示例，可替换为容器内的实际位置：

```text
/root/autodl-tmp/
├── planners/
│   └── Count-Downward/
├── data/
│   └── generated-pddl/depots-numeric-new/
│       ├── domain.pddl
│       └── problems/
├── models/
│   └── Qwen3.5-9B/
└── count-results/
```

后续命令使用：

```bash
export COUNT_PROJECT_ROOT=/root/autodl-tmp/planners/Count-Downward
export COUNT_DATA_ROOT=/root/autodl-tmp/data/generated-pddl/depots-numeric-new
export COUNT_MODEL_PATH=/root/autodl-tmp/models/Qwen3.5-9B
export COUNT_RESULTS_ROOT=/root/autodl-tmp/count-results
cd "$COUNT_PROJECT_ROOT"
```

不要在实验命令中混用 Windows `E:\...` 路径或 WSL `/mnt/e/...` 路径。manifest 中的路径也必须是容器内可见路径。

## 2. 环境需求

### 2.1 CPU 编译和基础运行环境

硬需求：

- x86-64 Linux；
- Bash；
- Python 3，建议 3.10 或 3.11；
- GNU `g++`/`gcc`、`make` 和 CMake；
- 足够的 CPU 内存和磁盘空间。

对 Debian/Ubuntu 类容器，缺少编译工具时可由有权限的用户安装：

```bash
apt-get update
apt-get install -y build-essential cmake make python3-dev
```

当前默认 `irhff` satisficing 配置在我们已有编译中不需要 CPLEX/OSI 后端。只有将来切换到依赖 LP 求解器的 Count 插件时，才需要额外安装 CPLEX 或 COIN-OR OSI/Clp 并设置 `DOWNWARD_CPLEX_ROOT`/`DOWNWARD_COIN_ROOT`。控制台向 `LD_LIBRARY_PATH` 添加的 `/opt/ibm/...` 和 `/opt/osi/...` 是兼容性默认路径；目录不存在本身不会影响当前 hFF 运行。

LLM 通信桥使用 C++ 标准线程。CMake 会声明标准 `Threads::Threads` 依赖，并在 Linux 上显式传入 `-pthread` 以兼容 Conda GCC 的混合 sysroot；不需要手工安装额外的 Python 包。如果旧源码在最终链接阶段报告 `undefined reference to pthread_create`，应更新源码并重新运行 CMake，而不是修改 LLM 参数。

### 2.2 Python 控制层

如果容器已有 PyPACE 的 Conda 环境，优先复用，不要随意覆盖其 CUDA/PyTorch 组合：

```bash
source /root/miniconda3/etc/profile.d/conda.sh
conda activate PyPACE_env
python --version
```

安装或检查 Count 控制层依赖：

```bash
cd "$COUNT_PROJECT_ROOT"
python -m pip install -r requirements/hybrid.txt
python -c "import aiohttp, unified_planning, pddl; print('hybrid dependencies: OK')"
```

`requirements/hybrid.txt` 包含：

- `aiohttp`：异步 OpenAI-compatible HTTP client；
- `unified-planning`：对 LLM 动作链做最长合法前缀验证；
- `pddl`：运行时 PDDL 描述转换。

### 2.3 GPU 和 vLLM

live 模式另外需要：

- NVIDIA GPU 和容器可见的驱动；
- 与驱动/CUDA 兼容的 PyTorch 和 vLLM；
- 本地模型目录或容器可访问的模型路径；
- 足够的 GPU 显存。

先检查而不要盲目重装：

```bash
nvidia-smi
python -c "import torch; print(torch.__version__, torch.version.cuda, torch.cuda.is_available())"
vllm --version
```

vLLM 没有写入 `requirements/hybrid.txt`，因为它与 CUDA/PyTorch/镜像版本强相关。如果 `PyPACE_env` 已能启动原有 vLLM，应直接使用该组合。

## 3. 在 Linux 上重新编译 Count

Windows/WSL 产生的 `builds/release64` 不应直接当作 AutoDL 二进制使用，因为 CMake cache 包含编译器、库和绝对路径。最稳妥的做法是只上传源码，在容器中重新生成 build。当前正式实验使用 `irhff`，不依赖 LP/CPLEX；项目的标准构建配置会显式关闭可选 OSI 探测，避免容器中不完整的 COIN/CPLEX 安装误触发 `OsiCpxSolverInterface.hpp` 编译。

如果上传内容已经包含旧 build，可先将它改名保留：

```bash
cd "$COUNT_PROJECT_ROOT"
mv builds/release64 "builds/release64.pre-autodl-$(date +%Y%m%d-%H%M%S)"
python build.py release64 -j"$(nproc)"
```

如果没有旧 build，直接运行编译命令：

```bash
cd "$COUNT_PROJECT_ROOT"
python build.py release64 -j"$(nproc)"
```

期望结尾包含：

```text
[100%] Built target downward
Built configuration release64 successfully
```

确认二进制：

```bash
test -x builds/release64/bin/downward
builds/release64/bin/downward --help >/dev/null
echo "Count binary: OK"
```

这里不只构建 `downward` 单个目标，而是完整构建 translator、preprocessor 和搜索器，确保移走旧 build 后运行脚本所需的三个阶段都存在。

`Clock skew detected` 通常是上传/解压后的文件时间戳与容器时钟不一致。如果已看到 `Built target downward` 且二进制存在，可先继续功能验证；正式实验前建议仍使用容器内的新鲜 build 消除该警告。

## 4. 编译后的自动验收

先运行完整 Python 和 Linux 集成测试：

```bash
cd "$COUNT_PROJECT_ROOT"
python -m unittest discover -s tests -p 'test_*.py' -v
```

必须重点确认：

- `test_lazy_rollout_integration` 在 Linux 上应实际执行，不应因二进制缺失而 skip；
- mock LLM 能收到完整中间 `init`；
- 10 个测试动作被 rollout 处理；
- 日志出现 `burst_finished result=completed`；
- 普通后继边数大于 0；
- 最终能产生有效计划。

若只想单独运行该测试：

```bash
python -m unittest tests.test_lazy_rollout_integration -v
```

## 5. 先做 60 秒 LLM-off 冒烟测试

这一步不需要 GPU 或 vLLM，用于确认 domain/problem 路径、translator、preprocessor、新二进制和记录器能够连通：

```bash
cd "$COUNT_PROJECT_ROOT"
bash scripts/run_batch_linux.sh \
  "$COUNT_DATA_ROOT/domain.pddl" \
  "$COUNT_DATA_ROOT/problems/problem_scale_30_id_572.pddl" \
  --default-mode off \
  --small-time-limit 60 \
  --small-parallelism 1 \
  --output-dir "$COUNT_RESULTS_ROOT/smoke-off"
```

期望看到：

```text
[COUNT-BATCH] all jobs are off-mode; vLLM will not be started
[NLM-PY-CONSOLE] LLM disabled; launching near-native planner
[COUNT-BATCH] end ... status=plan_found|incomplete|timeout ...
```

并确认结果目录存在：

```text
smoke-off/
├── batch_config.json
├── batch_results.csv
└── <job-id>/
    ├── console.log
    ├── job.json
    └── anytime/
        ├── run.json
        ├── planner.log
        ├── phases.csv
        ├── incumbents.csv
        └── llm_requests.csv
```

off 运行中 `llm_requests.csv` 应只有表头，搜索日志不应出现 LLM bridge 或 rollout 启动信息。

## 6. 批处理 manifest

推荐在容器中为 live 和 off 正式实验分别保存 manifest。示例 live manifest：

```json
{
  "domain": "/root/autodl-tmp/data/generated-pddl/depots-numeric-new/domain.pddl",
  "problem_dir": "/root/autodl-tmp/data/generated-pddl/depots-numeric-new/problems",
  "default_mode": "live",
  "jobs": [
    {
      "id": "scale30-572-live",
      "problem": "problem_scale_30_id_572.pddl"
    },
    {
      "id": "scale30-806-live",
      "problem": "problem_scale_30_id_806.pddl"
    },
    {
      "id": "scale40-176-live",
      "problem": "problem_scale_40_id_176.pddl"
    },
    {
      "id": "scale40-222-live",
      "problem": "problem_scale_40_id_222.pddl"
    }
  ]
}
```

文件名如果不含 `scale_30`/`scale_40`，必须显式填写：

```json
{
  "id": "hard-instance-live",
  "problem": "hard-instance.pddl",
  "scale": 40
}
```

也可对单个任务覆盖：

```json
{
  "problem": "problem_scale_40_id_176.pddl",
  "mode": "live",
  "time_limit_seconds": 3600,
  "max_requests_per_iteration": 15
}
```

正式论文实验建议使用固定的全局策略，不要根据单题结果事后调整时限和预算。

仓库已提供可编辑的模板：

```text
experiments/batch/example_manifest.json
```

## 7. 由批处理器启动并长期持有 vLLM

这是正式 live 实验的推荐方式。在一个 batch 内，vLLM 只启动一次，一直保留到最后一个问题结束：

```bash
cd "$COUNT_PROJECT_ROOT"
bash scripts/run_batch_linux.sh \
  --manifest /root/autodl-tmp/config/count-live.json \
  --output-dir "$COUNT_RESULTS_ROOT/live-run-001" \
  --vllm-model-path "$COUNT_MODEL_PATH" \
  --llm-model Qwen3.5-9B \
  --vllm-gpus 0 \
  --vllm-tensor-parallel-size 1 \
  --vllm-gpu-memory-utilization 0.90 \
  --vllm-max-model-len 32768 \
  --small-parallelism 8 \
  --large-parallelism 2
```

启动顺序为：

1. 启动一个 `vllm serve`；
2. 轮询 `/v1/models`，直到 `Qwen3.5-9B` 就绪；
3. 按 manifest 启动规划问题；
4. 30 及以下问题最多 8 个并行；
5. 遇到 40 规模问题时，先等已提交的小问题全部结束，再最多并行运行 2 题；
6. 批次全部结束后关闭由控制台创建的 vLLM。

默认每个搜索器的 LLM concurrency 为 6，每个状态 3 个 sample。八个 30 规模 live 搜索并行时，理论上可向同一 vLLM 提交最多 48 个 generation slot，实际排队和 batching 由 vLLM 决定。如果显存压力或尾延迟过高，优先降低：

```bash
--small-parallelism 4
--large-parallelism 1
--llm-max-concurrency 3
```

多 GPU 张量并行示例：

```bash
--vllm-gpus 0,1 --vllm-tensor-parallel-size 2
```

`--llm-model` 必须与 vLLM 暴露的 served model name 一致；否则就绪检查会持续等待并最终超时。

## 8. 连接独立长期运行的 vLLM

如果希望在多个 batch 之间也不停模型服务，可在 `tmux` 或平台托管进程中单独启动：

```bash
CUDA_VISIBLE_DEVICES=0 vllm serve "$COUNT_MODEL_PATH" \
  --served-model-name Qwen3.5-9B \
  --host 127.0.0.1 \
  --port 8091 \
  --tensor-parallel-size 1 \
  --gpu-memory-utilization 0.90 \
  --max-model-len 32768 \
  --dtype bfloat16 \
  --trust-remote-code
```

等待模型启动后，批处理器使用：

```bash
bash scripts/run_batch_linux.sh \
  --manifest /root/autodl-tmp/config/count-live.json \
  --output-dir "$COUNT_RESULTS_ROOT/live-external-001" \
  --external-vllm \
  --vllm-base-url http://127.0.0.1:8091/v1 \
  --llm-model Qwen3.5-9B \
  --small-parallelism 8 \
  --large-parallelism 2
```

`--external-vllm` 模式下，批处理器会检查服务就绪，但不会启动或关闭它。`--vllm-base-url` 应包含 `/v1`。

## 9. 运行正式 LLM-off 基线

为避免 GPU 服务和 CPU 竞争污染单题时间，建议将 off 基线作为独立的全 off batch，并使用单搜索器：

```bash
bash scripts/run_batch_linux.sh \
  --manifest /root/autodl-tmp/config/count-off.json \
  --output-dir "$COUNT_RESULTS_ROOT/off-run-001" \
  --small-parallelism 1 \
  --large-parallelism 1
```

全 off manifest 使用：

```json
{
  "domain": "/root/autodl-tmp/data/generated-pddl/depots-numeric-new/domain.pddl",
  "problem_dir": "/root/autodl-tmp/data/generated-pddl/depots-numeric-new/problems",
  "default_mode": "off",
  "jobs": [
    "problem_scale_30_id_572.pddl",
    "problem_scale_30_id_806.pddl",
    "problem_scale_40_id_176.pddl",
    "problem_scale_40_id_222.pddl"
  ]
}
```

在这种情况下：

- vLLM 不会启动或被访问；
- 30 规模时限仍是 1800 秒；
- 40 规模时限仍是 3600 秒；
- LLM 请求预算强制为 0；
- 每题实际顺序运行，更适合与 live 或 upstream Count 做 wall-clock 对比。

控制台支持混合 live/off manifest，但这主要用于功能检查和方便运行。如果 off 与 live 同时运行，off 的时间会包含 CPU/内存竞争，不应当作干净的单题性能基线。

## 10. 当前默认调度和预算

| 问题类别 | 搜索时限 | 并行度 | 每 anytime phase LLM 请求预算 |
|---|---:|---:|---:|
| `scale <= 30` | 1800 s | 8，同规模组内并行 | 10 |
| `scale > 30` | 3600 s | 2，同规模组内并行 | 15 |
| all-off 性能基线 | 按 scale | 建议 1 | 0 |

这些参数可用以下选项改写：

```text
--small-scale-max
--small-parallelism
--large-parallelism
--small-time-limit
--large-time-limit
--small-max-requests
--large-max-requests
```

注意：请求预算是 **每个 anytime phase** 的预算，不是整道问题的总预算。

## 11. 实验过程中应看到的终端信息

vLLM 生命周期：

```text
[COUNT-BATCH] launching one persistent vLLM: ...
[COUNT-BATCH] waiting for vLLM at http://127.0.0.1:8091/v1
[COUNT-BATCH] vLLM ready models=Qwen3.5-9B
```

每题启停：

```text
[COUNT-BATCH] start job=... scale=30 mode=live limit=1800s budget=10
[COUNT-BATCH] end job=... status=plan_found|incomplete|timeout code=... seconds=...
```

Anytime 与更优解：

```text
[NLM-ANYTIME-PHASE-START] ...
[NLM-ANYTIME-INCUMBENT] ... plan_cost=...
[NLM-ANYTIME-PHASE-END] ...
[NLM-ANYTIME-RUN-TIMEOUT] ...
```

触发和 rollout：

```text
[HYBRID-LLM-PLATEAU] event=window|activated|rearmed|deactivated|request_submitted ...
[HYBRID-LLM-BRIDGE] submitted ...
[NLM-PY-CONSOLE] model request started ...
[HYBRID-LLM-ROLLOUT] event=response_accepted|burst_started|edge_processed|proposal_aborted|burst_finished ...
[HYBRID-LLM-TRIGGER-STATS] ...
[HYBRID-LLM-ROLLOUT-STATS] ...
```

并行的 30 规模问题的每行日志前面会加 job ID，避免两个搜索器的输出混淆。

## 12. 结果检查

### 12.1 批处理总表

```bash
python - <<'PY'
import csv
from pathlib import Path

path = Path("/root/autodl-tmp/count-results/live-run-001/batch_results.csv")
for row in csv.DictReader(path.open(encoding="utf-8")):
    print(row["job_id"], row["status"], row["elapsed_seconds"], row["return_code"])
PY
```

`status` 含义：

- `plan_found`：规划器产生了解；
- `unsolvable`：规划器证明无解；
- `incomplete`：搜索未完成，但不是控制器故障；
- `timeout`：正常达到 30/60 分钟限制；
- `failed`：配置、进程、内存或其他非预期故障，需检查 `console.log`。

### 12.2 Cost-time 曲线

每题：

```text
<batch>/<job-id>/anytime/incumbents.csv
```

每行是首解或一次严格 cost 改善，核心列为：

```text
elapsed_seconds, plan_cost, plan_length, iteration,
cumulative_expanded, cumulative_state_requests,
cumulative_model_generations, cumulative_injected_states
```

`incumbents.csv` 为空表示时限内没有找到第一个可行解，不一定是运行故障。

### 12.3 LLM 有效性

检查：

```text
<batch>/<job-id>/anytime/llm_requests.csv
<batch>/<job-id>/anytime/phases.csv
```

建议关注：

- `reason`：三类触发的构成；
- `sample_count` / `usable_sample_count`；
- `model_wall_seconds`；
- `applied_actions` / `inserted_states` / `rollout_aborts`；
- `transport_failures` / `stale_responses`；
- `llm_bursts_started/completed/aborted`；
- 各类 dead-end、bound、cycle 和 inapplicable abort。

## 13. 环境变量和参数覆盖

C++ 优先读取 `HYBRID_LLM_*`，如果不存在才读取兼容的 `NLM_LLM_*`。新实验建议使用 `HYBRID_LLM_*`。

例如，临时只启用平台触发：

```bash
export HYBRID_LLM_ENABLE_EXPANSION_PLATEAU=1
export HYBRID_LLM_ENABLE_GLOBAL_STALL=0
export HYBRID_LLM_ENABLE_ANCESTOR_STAGNATION=0
bash scripts/run_batch_linux.sh ...
```

重要规则：

- 显式 `--llm-mode live/off` 的通信模式、端口、run ID 和 trigger 开关会同时写入新旧两组变量，不受父环境残留值影响；
- batch 的每题请求预算也会同时写入两组前缀；
- 其他算法参数可通过环境变量覆盖控制台默认值；
- 正式实验前应把实际参数写入运行记录，避免 Conda/容器残留变量影响复现。

## 14. 常见问题

### 14.1 vLLM 一直停在 waiting for ready

检查：

```bash
curl http://127.0.0.1:8091/v1/models
```

常见原因：

- vLLM 进程已因显存不足退出；
- `--llm-model` 与 served model name 不一致；
- 端口被占用；
- `--vllm-base-url` 缺少 `/v1`；
- 模型路径不完整或无读取权限。

由批处理器启动的服务日志位于：

```text
<batch-output>/vllm.log
```

### 14.2 CUDA OOM 或 vLLM 请求积压

按以下顺序调整：

1. 将 `--small-parallelism 8` 降为 4，或将 `--large-parallelism 2` 降为 1；
2. 将 `--llm-max-concurrency 6` 降为 3；
3. 适当降低 `--vllm-max-model-len`；
4. 调整 `--vllm-gpu-memory-utilization`；
5. 有多张 GPU 时使用 tensor parallel。

不建议在不记录的情况下随意降低 sample 数，因为这会改变算法实际获得的候选路线数。

### 14.3 规划问题时限内没有首解

这在当前 30/40 规模 Depots 问题上可能是正常结果。先查看：

- `batch_results.csv` 是否为 `timeout`/`incomplete` 而不是 `failed`；
- `planner.log` 是否持续有展开计数；
- `phases.csv` 是否有完整 phase 记录；
- `incumbents.csv` 是否为空。

40 规模问题默认已给予 3600 秒，不需要额外改动。

### 14.4 `failed` 而不是 `timeout`

首先检查：

```text
<batch>/<job-id>/console.log
<batch>/<job-id>/anytime/planner.log
```

重点搜索：

```text
Traceback
error
missing
CUDA
out of memory
parse
validation
```

### 14.5 Ctrl-C 中止

批处理器收到 Ctrl-C 后会终止当前搜索子进程，然后关闭自己启动的 vLLM。已写入的每题日志仍保留，但批次会被视为未完整。

## 15. 正式实验前的检查清单

- [ ] 在 AutoDL Linux 容器中干净编译 `release64`。
- [ ] `test_lazy_rollout_integration` 实际执行并通过。
- [ ] 一次 60 秒 all-off 冒烟测试通过。
- [ ] 一次 `--small-max-requests 1` 的 live 冒烟测试能收到回包并启动 rollout。
- [ ] vLLM served model name 与 `--llm-model` 一致。
- [ ] manifest 内全部路径在容器中存在。
- [ ] 30 规模实际可以并行 8 个且不发生 CPU/内存/GPU OOM。
- [ ] 40 规模可以并行 2 个，且开始时终端中没有小规模 active job。
- [ ] off 和 live 使用独立输出目录。
- [ ] 保存 `nvidia-smi`、Python/PyTorch/vLLM 版本、`pip freeze`、代码 commit/diff 和完整启动命令。
- [ ] 每个批次结束后检查 `batch_results.csv`，不只看终端最后一行。

## 16. 快速命令索引

查看全部参数：

```bash
python -m hybrid_planner.batch_console --help
python -m hybrid_planner.console --help
```

全 off：

```bash
bash scripts/run_batch_linux.sh --manifest count-off.json \
  --output-dir "$COUNT_RESULTS_ROOT/off-run" --small-parallelism 1
```

批处理器自己启动 vLLM：

```bash
bash scripts/run_batch_linux.sh --manifest count-live.json \
  --output-dir "$COUNT_RESULTS_ROOT/live-run" \
  --vllm-model-path "$COUNT_MODEL_PATH" --vllm-gpus 0
```

连接已运行 vLLM：

```bash
bash scripts/run_batch_linux.sh --manifest count-live.json \
  --output-dir "$COUNT_RESULTS_ROOT/live-external" \
  --external-vllm --vllm-base-url http://127.0.0.1:8091/v1
```

详细的算法与论文表述参考见：

```text
docs/count_llm_changes_and_innovations.md
```

