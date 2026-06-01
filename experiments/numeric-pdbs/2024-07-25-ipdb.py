#! /usr/bin/env python

import os
import shutil

import numeric_pdb_parser

import project
from lab.environments import TetralithEnvironment

from downward.reports.scatter import ScatterPlotReport

REPO = project.get_repo_base()
BENCHMARKS_DIR_TRANS = os.environ["NUMERIC_BENCHMARKS_TRANSFORMED"]
BENCHMARKS_DIR_IPC = os.environ["NUMERIC_BENCHMARKS_IPC2023"]

# If REVISION_CACHE is None, the default "./data/revision-cache/" is used.
REVISION_CACHE = os.environ.get("DOWNWARD_REVISION_CACHE")

ENV = TetralithEnvironment(
    email="daniel.gnad@liu.se",
#    time_limit_per_task="24:00:00",
#    memory_per_cpu="8300M",
#    extra_options="#SBATCH -A naiss2023-5-236", # parground
    extra_options="#SBATCH -A naiss2023-5-314", # Learning Dynamic Algorithms for Automated Planning
)

REVISIONS = ["01dc61f8d42c724ec38d8f3639cf9099092a0161"
            ]

CONFIGS = [
    ("blind", ["--search", f"astar(blind())"]),
    #("hmax", ["--search", f"astar(iihmax())"]),
    ("lmcut", ["--search", f"astar(lmcutnumeric())"]),

]
for var_order, vo_name in [("GOAL_CG_LEVEL", "g_cg_l")]:
    for num_first, nf_name in [("false", "")]:
        for lim, lim_name in [(10000000, "10M")]:
            CONFIGS.append((f"pdb-{lim_name}{nf_name}-{vo_name}", ["--search", f"astar(numeric_pdb(pattern=greedy_numeric(prefer_numeric_variables={num_first}, variable_order_type={var_order}, max_number_pdb_states={lim})))"]))

for num_vars in [1,2,3]:
    for limit in [1000, 10000, 100000]:
        CONFIGS.append((f"cpdbs-sys{num_vars}-{int(limit / 1000)}k", ["--search", f"astar(numeric_cpdbs(patterns=numeric_systematic(pattern_max_size={num_vars}, max_number_pdb_states={limit})))"]))

CONFIGS.append((f"ipdb", ["--search", f"astar(numeric_ipdb(min_improvement=1, max_time=150, max_number_pdb_states=100000))"]))


BUILD_OPTIONS = ["release64", "-j6"]
DRIVER_OPTIONS = ["--overall-time-limit", "5m", "--build", "release64"]

ATTRIBUTES = [
    "cost",
    "error",
    "run_dir",
    "search_start_time",
    "search_start_memory",
    "total_time",
    "initial_h_value",
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


exp.add_suite(BENCHMARKS_DIR_TRANS, project.SUITE_NUMERIC_TRANSFORMED)
exp.add_suite(BENCHMARKS_DIR_IPC, project.SUITE_NUMERIC_IPC23)

exp.add_parser(exp.EXITCODE_PARSER)
exp.add_parser(exp.TRANSLATOR_PARSER)
exp.add_parser(exp.SINGLE_SEARCH_PARSER)
exp.add_parser(exp.PLANNER_PARSER)
exp.add_parser(numeric_pdb_parser.get_parser())

exp.add_step("build", exp.build)
exp.add_step("start", exp.start_runs)
exp.add_step("parse", exp.parse)


exp.add_fetcher(name="fetch")

project.add_absolute_report(exp, attributes=ATTRIBUTES)

config_pairs = [("blind", "pdb-10M-g_cg_l"), ("hmax", "pdb-10M-g_cg_l"), ("lmcut", "pdb-10M-g_cg_l")]

for alg1, alg2 in config_pairs:
    exp.add_report(
        ScatterPlotReport(
            attributes=["expansions_until_last_jump"],
            filter_algorithm=[alg1, alg2],
            #filter_coverage=[1],
            get_category=lambda r1, r2 : r1["domain"],
            format="png",
            show_missing=False,
        ),
        name=f"scatterplot-expansions-until-last-f-{alg1}-vs-{alg2}",
    )
    exp.add_report(
        ScatterPlotReport(
            attributes=["planner_time"],
            filter_algorithm=[alg1, alg2],
            #filter_coverage=[1],
            get_category=lambda r1, r2 : r1["domain"],
            format="png",
            show_missing=False,
        ),
        name=f"scatterplot-runtime-{alg1}-vs-{alg2}",
    )

exp.run_steps()
