#! /usr/bin/env python

import os
import shutil

import numeric_pdb_parser

import project
from lab.environments import TetralithEnvironment
from lab.reports import Attribute, arithmetic_mean
from lab import tools

from downward.reports.scatter import ScatterPlotReport
from downward.reports.absolute import AbsoluteReport

REPO = project.get_repo_base()
BENCHMARKS_DIR_IPC = os.environ["NUMERIC_BENCHMARKS_IPC2023"]
BENCHMARKS_DIR_OTHERS = os.environ["NUMERIC_BENCHMARKS_OTHERS"]

ENV = TetralithEnvironment(
    email="markus.fritzsche@liu.se",
    memory_per_cpu="8300M",
    extra_options="#SBATCH -A naiss2024-5-404",  # parground
)

REVISIONS = [
    "4cbe00beacb66033b48f1489df3db948a7322ff8",
]


prefix1 = "astar(numeric_cpdbs(patterns=numeric_genetic(keep_parent_pointers=false, extend_abstract_state_space=false, collection_max_size=100000, max_pdb_size=10000000, max_number_pdb_states=100000"
prefix2 = "astar(numeric_ipdb(cache_estimates=false, max_time=900, keep_parent_pointers=false, extend_abstract_state_space=false, collection_max_size=1000000, max_pdb_size=1000000, max_number_pdb_states=10000"

prefix_list = [prefix2]

algos = {
    "BLL": "exploration_heuristic=BLIND, frontier_heuristic=LMCUT, failed_lookup_heuristic=LMCUT, ",
    "LLL": "exploration_heuristic=LMCUT, frontier_heuristic=LMCUT, failed_lookup_heuristic=LMCUT, ",
}

max_h_factor_list = [2.0, 5.0, 10.0]



configs = [
]

names = [
]

for prefix in prefix_list:
    for alg_name, alg_config in algos.items():
        for max_h_factor in max_h_factor_list:

            config_key = f"{alg_name}-prune={max_h_factor}"
            if config_key in names: 
                continue

            suffix = ")" if prefix == prefix1 else "" 
            config = f"{prefix}, max_h_factor={max_h_factor}, {alg_config} )){suffix}"
            configs.append(config)
            names.append(config_key)


configs = [["--search", config] for config in configs]

BUILD_OPTIONS = ["release64", "-j6"]
DRIVER_OPTIONS = [
    "--overall-time-limit",
    "30m",
    "--build",
    "release64",
    "--overall-memory-limit",
    "8G",
]

ATTRIBUTES = [
    "cost",
    "error",
    "run_dir",
    "total_time",
    "initial_h_value",
    "coverage",
    "evaluations",
    "expansions",
    "memory",
    "planner_time",
    "expansions_until_last_jump",
    "number_reached_goal_states",
    "number_abstract_states",
    "pdb_construction_time",
    "pdb_collection_construction_time",
    "ipdb_hillclimbing_time",
    "pdb_constructed",
    "res_task_construction_time",
    "number_failed_pdb_lookups",
    Attribute("score_planner_time", absolute=True, function=sum),
    Attribute("ratio_failed_lookups_per_pdb", absolute=True, function=arithmetic_mean),
]

exp = project.FastDownwardExperiment(environment=ENV)
for rev in REVISIONS:
    for i, (name, config) in enumerate(zip(names, configs)):
        exp.add_algorithm(
            f"{name}-{rev[:5]}",
            REPO,
            rev,
            config,
            build_options=BUILD_OPTIONS,
            driver_options=DRIVER_OPTIONS,
        )
        print(i, name, config)

SUITE_NUMERIC_IPC23_ALL_SAS = [
    suite + "_sas" for suite in project.SUITE_NUMERIC_IPC23_ALL_NO_0_COVERAGE
]
SUITE_NUMERIC_OTHERS_SAS = [suite + "_sas" for suite in project.SUITE_NUMERIC_OTHERS_NO_0_COVERAGE]
SUITE_NUMERIC_OTHERS_NEW_SAS = [
    suite + "_sas" for suite in project.SUITE_NUMERIC_OTHERS_NEW
]

exp.add_suite(BENCHMARKS_DIR_IPC, SUITE_NUMERIC_IPC23_ALL_SAS)
exp.add_suite(BENCHMARKS_DIR_OTHERS, SUITE_NUMERIC_OTHERS_SAS)
exp.add_suite(BENCHMARKS_DIR_OTHERS, SUITE_NUMERIC_OTHERS_NEW_SAS)

exp.add_parser(exp.EXITCODE_PARSER)
exp.add_parser(exp.TRANSLATOR_PARSER)
exp.add_parser(exp.SINGLE_SEARCH_PARSER)
exp.add_parser(exp.PLANNER_PARSER)
exp.add_parser(numeric_pdb_parser.get_parser())

exp.add_step("build", exp.build)
exp.add_step("start", exp.start_runs)
exp.add_step("parse", exp.parse)


exp.add_fetcher(name="fetch")

project.add_compress_and_delete_runs_step(exp)

# exp.add_fetcher(
#     "data/2025-01-20-astar-exploration-test-eval/",
#     name="fetch-base",
#     filter_algorithm=["num-ipdb-12fb9", "num-pdb-greedy-12fb9"],
#     merge=True,
# )
# # exp.add_fetcher("data/2025-01-21-hierarchical-pdbs-eval/", name="fetch-hier", filter_algorithm=["hier1-254f5", "hier2-254f5", "hier3-254f5", "hier4-254f5", "hier5-254f5"], merge=True)
# exp.add_fetcher(
#     "data/2025-01-21-astar-exploration-zero-cost-fix-eval/",
#     name="fetch-keep-pdb",
#     filter_algorithm=["num-ipdb-e1634", "num-pdb-greedy-e1634"],
#     merge=True,
# )


def add_failed_lookup_ratio(run):
    if "number_failed_pdb_lookups" in run:
        run["ratio_failed_lookups_per_pdb"] = (
            run["number_failed_pdb_lookups"] / run["evaluations"]
        )
    return run


def add_score_planner_time(run):
    run["score_planner_time"] = tools.compute_log_score(
        run["coverage"], run.get("planner_time"), lower_bound=1.0, upper_bound=1800.0
    )
    return run


filtered_algos = [f"{i}-{REVISIONS[0][:5]}" for i in range(len(configs))]


project.add_absolute_report(
    exp, attributes=ATTRIBUTES, filter=[add_failed_lookup_ratio, add_score_planner_time]
)

exp.add_report(
    AbsoluteReport(
        attributes=["coverage"], filter_algorithm=filtered_algos, format="html"
    ),
    name=f"{exp.name}-coverage",
)


# PLOTS

config_pairs = [
    ("1-8c5d7", "1-6035d"),
    # (f"lmcut-ipc-{REVISIONS[0][:5]}", f"num-ipdb-{REVISIONS[0][:5]}"),
    # (f"ipdb-{REVISIONS[0][:5]}", f"num-ipdb-{REVISIONS[0][:5]}"),
]


def remove_pn_domain(run):
    if run["domain"] == "pn-domain":
        return False
    return run


for alg1, alg2 in config_pairs:
    for attr in [
        "expansions_until_last_jump",
        "planner_time",
        "search_time",
        "evaluations",
    ]:
        exp.add_report(
            ScatterPlotReport(
                attributes=[attr],
                filter_algorithm=[alg1, alg2],
                filter=[remove_pn_domain],
                get_category=lambda r1, r2: r1["domain"],
                format="png",
                show_missing=True,
            ),
            name=f"scatterplot-{attr.replace('_', '-')}-{alg1}-vs-{alg2}",
        )


exp.run_steps()
