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
BENCHMARKS_DIR_IPC_TRANSFORMED = os.environ["NUMERIC_BENCHMARKS_IPC2023_TRANSFORMED"]
BENCHMARKS_DIR_OTHERS = os.environ["NUMERIC_BENCHMARKS_OTHERS"]

# If REVISION_CACHE is None, the default "./data/revision-cache/" is used.
REVISION_CACHE = os.environ.get("DOWNWARD_REVISION_CACHE")

ENV = TetralithEnvironment(
    email="daniel.gnad@liu.se",
#    time_limit_per_task="24:00:00",
    memory_per_cpu="8300M",
#    extra_options="#SBATCH -A naiss2023-5-236", # parground
    extra_options="#SBATCH -A naiss2023-5-314", # Learning Dynamic Algorithms for Automated Planning
)

REVISIONS = ["a54afa7c9787ab1380e0337eaf574934564639e8"
            ]

CONFIGS = [
    ("blind", ["--search", f"astar(blind())"]),
    ("ipdb", ["--search", f"astar(ipdb(max_time=900))"]),
#    ("iihmax", ["--search", f"astar(iihmax())"]),
    ("lmcut-ipc", ["--search", f"astar(lmcutnumeric(use_second_order_simple=true, bound_iterations=10, ceiling_less_than_one=true))"]),
#    ("opcount-ipc", ["--search", f"astar(operatorcounting([lmcutnumeric_constraints(ceiling_less_than_one=true),state_equation_constraints],cplex,lp))"]),
#    ("hrmax", ["--search", f"astar(hrmax(restrict_achievers=true))"]),
#    ("subgoal", ["--search", "astar(hgen(cplex,lp))"]),
#    ("landmarks", ["--search", "astar(operatorcounting([lm_numeric],cplex,lp))"]),
]

for var_order, vo_name in [("GOAL_CG_LEVEL", "g_cg_l")]:
    for num_first, nf_name in [("false", "")]:
        for lim, lim_name in [(100000, "100k")]:
            CONFIGS.append((f"pdb-{lim_name}{nf_name}-{vo_name}", ["--search", f"astar(numeric_pdb(pattern=greedy_numeric(prefer_numeric_variables={num_first}, variable_order_type={var_order}, max_number_pdb_states={lim})))"]))

for num_vars in [2]:
    for limit in [100000]:#, 100000]:#, 1000000]:
        CONFIGS.append((f"cpdbs-sys{num_vars}-{int(limit / 1000)}k", ["--search", f"astar(numeric_cpdbs(patterns=numeric_systematic(pattern_max_size={num_vars}, max_number_pdb_states={limit})))"]))

for coll_name, coll_limit in [#("100k", 100000), ("1M", 1000000), 
                              ("20M", 20000000)
                              ]:
    for pdb_name, pdb_limit in [#("10k", 10000), 
                                ("100k", 100000), #("1M", 1000000)
                                ]:
        for max_time in [900]:
            if coll_limit > pdb_limit:
                CONFIGS.append((f"ipdb-{coll_name}-{pdb_name}-{max_time}", ["--search", f"astar(numeric_ipdb(max_time={max_time}, max_number_pdb_states={pdb_limit}, collection_max_size={coll_limit}))"]))


BUILD_OPTIONS = ["release64", "-j6"]
DRIVER_OPTIONS = ["--overall-time-limit", "30m", "--build", "release64", "--overall-memory-limit", "8G"]

ATTRIBUTES = [
    "cost",
    "error",
    "run_dir",
    "search_time",
    "total_time",
    "initial_h_value",
    "coverage",
    "expansions",
    "memory",
    "planner_time",
    "expansions_until_last_jump",
    "pdb_construction_time",
    "pdb_collection_construction_time",
    "ipdb_hillclimbing_time",
    "pdb_constructed",
    'dominance_pruning_time',
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


exp.add_suite(BENCHMARKS_DIR_IPC, project.SUITE_NUMERIC_IPC23_ALL)
exp.add_suite(BENCHMARKS_DIR_OTHERS, project.SUITE_NUMERIC_OTHERS)
exp.add_suite(BENCHMARKS_DIR_OTHERS, project.SUITE_NUMERIC_OTHERS_RES)

exp.add_parser(exp.EXITCODE_PARSER)
exp.add_parser(exp.TRANSLATOR_PARSER)
exp.add_parser(exp.SINGLE_SEARCH_PARSER)
exp.add_parser(exp.PLANNER_PARSER)
exp.add_parser(numeric_pdb_parser.get_parser())

exp.add_step("build", exp.build)
exp.add_step("start", exp.start_runs)
exp.add_step("parse", exp.parse)


exp.add_fetcher(name="fetch")


def filter_final_domains(run):
    domain = run["domain"]
    if domain in ["markettrader_test_10", "pn_domain", "depots-sym", "depots-unit", "satellite-unit"]:
        return False
    if domain.endswith("-sat"):
        return False
    if "minecraft" in domain:
        if any(f"{x}x{x}" in run["problem"] for x in [10, 35]):
            return False
    if domain in ["counters", "delivery", "drone", "expedition", "ext-plant-watering", "farmland-ipc23", "hydropower", "markettrader", "mprime", "pathwaysmetric", "rover-ipc23", "sailing-ipc23", "settlersnumeric", "sugar", "zenotravel-ipc23"]:
        run["domain"] = f"ipc23-{domain}"
    return run

project.add_absolute_report(exp, attributes=ATTRIBUTES, filter=[filter_final_domains])



# PLOTS

config_pairs = [(f"blind-{REVISIONS[0][:5]}", f"ipdb-20M-100k-900-{REVISIONS[0][:5]}"), (f"ipdb-{REVISIONS[0][:5]}", f"ipdb-20M-100k-900-{REVISIONS[0][:5]}"), (f"lmcut-ipc-{REVISIONS[0][:5]}", f"ipdb-20M-100k-900-{REVISIONS[0][:5]}")]

for alg1, alg2 in config_pairs:
    for attr in ["expansions_until_last_jump", "planner_time"]:
        exp.add_report(
            ScatterPlotReport(
                attributes=[attr],
                filter_algorithm=[alg1, alg2],
                filter=[filter_final_domains],
                get_category=lambda r1, r2 : r1["domain"],
                format="png",
                show_missing=attr in ["expansions_until_last_jump", "planner_time"],
                #relative=attr in ["initial_h_value", "cost"]
            ),
            name=f"scatterplot-{attr.replace('_', '-')}-{alg1}-vs-{alg2}",
        )


exp.run_steps()
