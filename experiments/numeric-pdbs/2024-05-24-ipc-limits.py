#! /usr/bin/env python

import os
import shutil

import numeric_pdb_parser

import project
from lab.environments import TetralithEnvironment

from downward.reports.scatter import ScatterPlotReport

REPO = project.get_repo_base()
BENCHMARKS_DIR = os.environ["NUMERIC_BENCHMARKS_TRANSFORMED"]

# If REVISION_CACHE is None, the default "./data/revision-cache/" is used.
REVISION_CACHE = os.environ.get("DOWNWARD_REVISION_CACHE")
SUITE = project.SUITE_NUMERIC

ENV = TetralithEnvironment(
    email="daniel.gnad@liu.se",
#    time_limit_per_task="24:00:00",
    memory_per_cpu="8300M",
    extra_options="#SBATCH -A naiss2023-5-236", # parground
)

REVISIONS = ["b2975deab8b7ddd498730d6bc3639c599ad9028e",
            ]

CONFIGS = [
    #("blind", ["--search", f"astar(blind())"]),
    #("hmax", ["--search", f"astar(iihmax())"]),
    #("lmcut", ["--search", f"astar(lmcutnumeric())"]),

]
for var_order, vo_name in [("GOAL_CG_LEVEL", "g_cg_l")]:
    for num_first, nf_name in [("false", ""), ("true", "-NF")]:
        for lim, lim_name in [(10000000, "10M"), (50000000, "50M"), (100000000, "100M"), (150000000, "150M")]:
            CONFIGS.append((f"pdb-{lim_name}{nf_name}-{vo_name}", ["--search", f"astar(numeric_pdb(pattern=greedy_numeric(numeric_variables_first={num_first}, variable_order_type={var_order}), max_number_states={lim}))"]))

BUILD_OPTIONS = ["release64", "-j4"]
DRIVER_OPTIONS = ["--overall-time-limit", "30m", "--build", "release64", "--overall-memory-limit", "8G"]

ATTRIBUTES = [
    "cost",
    "error",
    "run_dir",
    "search_start_time",
    "search_start_memory",
    "total_time",
    "initial_h_value_float",
    "coverage",
    "expansions",
    "memory",
    "planner_time",
    "expansions_until_last_jump",
    "number_reached_goal_states",
    "number_abstract_states",
    "pdb_construction_time",
]

exp = project.FastDownwardExperiment(environment=ENV, revision_cache=REVISION_CACHE)
for rev in REVISIONS:
    for config_nick, config in CONFIGS:
        exp.add_algorithm(
            f"{config_nick}-{rev[:5]}",
            REPO,
            rev,
            config,
            build_options=BUILD_OPTIONS,
            driver_options=DRIVER_OPTIONS,
        )

exp.add_suite(BENCHMARKS_DIR, SUITE)

exp.add_parser(exp.EXITCODE_PARSER)
exp.add_parser(exp.TRANSLATOR_PARSER)
exp.add_parser(exp.SINGLE_SEARCH_PARSER)
exp.add_parser(exp.PLANNER_PARSER)
exp.add_parser(numeric_pdb_parser.get_parser())

exp.add_step("build", exp.build)
exp.add_step("start", exp.start_runs)
exp.add_step("parse", exp.parse)


exp.add_fetcher(name="fetch")

exp.add_fetcher("data/2024-05-14-ipc-limits-eval/", name="fetch-baselines", merge=True, filter_algorithm=["blind", "hmax", "lmcut"])

project.add_absolute_report(exp, attributes=ATTRIBUTES)

config_pairs = [("blind", "pdb-50M-g_cg_l-b2975"), ("hmax", "pdb-50M-g_cg_l-b2975"), ("lmcut", "pdb-50M-g_cg_l-b2975")]

PLOT_FORMAT = "png"

for alg1, alg2 in config_pairs:
    exp.add_report(
        ScatterPlotReport(
            attributes=["expansions_until_last_jump"],
            filter_algorithm=[alg1, alg2],
            #filter_coverage=[1],
            get_category=lambda r1, r2 : r1["domain"],
            format=PLOT_FORMAT,
            show_missing=True,
        ),
        name=f"scatterplot-expansions-until-last-f-{alg1}-vs-{alg2}",
    )
    exp.add_report(
        ScatterPlotReport(
            attributes=["planner_time"],
            filter_algorithm=[alg1, alg2],
            #filter_coverage=[1],
            get_category=lambda r1, r2 : r1["domain"],
            format=PLOT_FORMAT,
            show_missing=True,
        ),
        name=f"scatterplot-runtime-{alg1}-vs-{alg2}",
    )

exp.run_steps()
