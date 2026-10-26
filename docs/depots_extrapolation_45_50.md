# Depots 45/50 完整运行补充实验

本入口固定使用 Lazy，45、50 规模各抽 20 题，共 40 题。两个方法共用同一清单；先跑完 Count Downward（Off），再跑完整 TRACS–Lazy（Live）。每个阶段 6 并行，每题全部 anytime 搜索阶段共享 3600 秒。规模 45/50 沿用规模 40 的触发间隔倍率 0.25、每阶段最多 15 次请求、每次采样 3 条回复。本次不再全量运行 80 题。

这是完整回复实验，始终 `llm_partial_only=0`；此前 Partial-only 实验的 `COUNT_LLM_PARTIAL_ONLY=1` 在本入口无效。抽样种子默认 `20261025`，搜索/生成实验种子默认 `101`。模型保持冻结。

## 容器中启动

先把新增的模块、脚本和相关修改同步到容器项目目录。无需重新编译 C++。等其他批次结束后，再单独启动这一批，避免 CPU、内存和模型服务争用。

```bash
source /root/miniconda3/etc/profile.d/conda.sh
conda activate verl_env
cd /root/autodl-tmp/count-downward-llm

COUNT_MODEL_PATH="/root/autodl-tmp/Qwen3_5-9B/dapo/data_260811_resume_193/global_step_350/actor/huggingface" \
COUNT_DATASET_ROOT="/root/PyPACE/data/generated-pddl/depots-numeric-45and50" \
COUNT_RESULTS_DIR="/root/autodl-tmp/count-results/depots45-50-extrapolation/seed_101" \
COUNT_TMUX_SESSION="count-depots45-50-seed101" \
bash scripts/start_depots_extrapolation_45_50_tmux.sh
```

`COUNT_MODEL_PATH` 指向 Hugging Face 权重目录。`COUNT_DATASET_ROOT` 应包含 `domain.pddl` 和 `problems/`；将本地 `E:\Python Projects\PyPACE\data\generated-pddl\depots-numeric-45and50` 上传到这个容器路径即可。只匹配 45/50 题，不会纳入其他规模。

该入口创建 tmux 后立即返回，批次继续在后台运行；Off 阶段结束后，Live 阶段自动启动本地 vLLM，结束时释放其拥有的服务。默认 GPU 是 `0`、端口 `8091`。已有同端口服务时请先确认没有冲突。

查看运行与退出查看：

```bash
tmux attach -t count-depots45-50-seed101
# Ctrl-b，再按 d：离开界面，实验继续。
tail -f /root/autodl-tmp/count-results/depots45-50-extrapolation/seed_101/tmux-run.log
```

不使用 tmux 时，改为 `bash scripts/run_depots_extrapolation_45_50.sh`，其余变量相同。脚本执行期间终端保持连接。

## 启动前只检查方案

`--prepare-only` 仅生成或核对固定抽样、有效配置和两个阶段的命令，不启动搜索、不加载模型。只需题集和 Python，模型权重、vLLM 与规划器二进制尚未就位也可使用。

```bash
COUNT_DATASET_ROOT="/root/PyPACE/data/generated-pddl/depots-numeric-45and50" \
COUNT_RESULTS_DIR="/root/autodl-tmp/count-results/depots45-50-extrapolation/seed_101" \
bash scripts/run_depots_extrapolation_45_50.sh --prepare-only
```

正式运行使用完全相同的配置和结果目录，复用已经保存的清单。请核对每个规模恰有 20 题、总计 40 题、并行数 6、时限 3600 秒及 Partial-only 关闭。

准备与正式运行应使用同一 Python 环境；可用 `COUNT_RUNTIME_ENV_PREFIX` 明确指定。入口会锁定题集内容、抽样、参数、模型路径与 Python 源码，正式执行时额外记录规划器二进制及模型配置文件的指纹；模型权重本身不做全量哈希，请保持该目录内的 checkpoint 不变。

## 中断与续跑

进程中断或容器重启后，恢复原环境，用相同变量、相同结果目录重新启动。已完成的题可跳过，未完成的题重新处理；中断的单题搜索不能从中间状态恢复。不要删除已有清单或将不同题集、模型、seed、抽样配置混入这个目录。更改实验配置时使用新的结果目录。

默认依次运行两个阶段。如需分开运行，可设置 `COUNT_EXTRAPOLATION_STAGE=off` 或 `COUNT_EXTRAPOLATION_STAGE=live`；两者都必须使用相同配置和目录，默认值为 `both`。不要将两个阶段同时启动。

Live 只有在同目录的 Off 题目全部正常结束后才会开始；正常达到时限或搜索未完成也算一次已完成的尝试。程序异常、失败或缺失结果会阻止进入 Live，重新启动时会重跑这些题。同一结果目录设有进程锁，重复启动会直接报错。

## 可配置项与耗时

常用变量：`COUNT_MODEL_PATH`、`COUNT_DATASET_ROOT`、`COUNT_RESULTS_DIR`、`COUNT_EXPERIMENT_SEED`、`COUNT_VLLM_GPUS`、`COUNT_LLM_MODEL_NAME`、`COUNT_TMUX_SESSION`。抽样可通过 `COUNT_SAMPLE_PER_SCALE` 与 `COUNT_SAMPLE_SEED` 调整，默认分别为 `20` 和 `20261025`；改变这些值时应另设结果目录。

运行环境默认优先使用已激活且具有 vLLM 的 Conda 环境，其次尝试 `/root/miniconda3/envs/verl_env`。可用 `COUNT_RUNTIME_ENV_PREFIX` 明确指定环境，用 `COUNT_RUNTIME_LIB_DIR` 指定兼容 `CXXABI_1.3.15` 的运行库目录；tmux 会显式继承本次命令的环境，避免复用旧 server 环境。高级选项有 `COUNT_PLANNER_PYTHON`、`COUNT_BUILD`、`COUNT_PROMPT_DOMAIN_CODE`、`COUNT_VLLM_PORT` 与 `NLM_VLLM_EXECUTABLE`。

40 题在同一队列中排队，45/50 不分成两批。6 并行、每题 1 小时，单个方法的搜索预算约为 `ceil(40/6) = 7` 小时，两个方法合计约 14 小时；另加题目转换、模型加载和保存开销。程序异常或转换长时间卡住可能超出此估算，14 小时不是进程级总时限。需要定时关机时应留额外余量，并从实际启动时间起算；运行结束后仍需通过租用平台关闭实例，退出脚本本身不会关机。
