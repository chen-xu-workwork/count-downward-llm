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
    email="daniel.gnad@liu.se",
    memory_per_cpu="8300M",
    extra_options="#SBATCH -A naiss2024-5-404", # parground
)

REVISIONS = [#"12fb9f81b03c2fea6d66c1b73e6db1cc1b8c98da",
             "43751b126ea14e826507e0216e67e65d7320096c", # astar-exploration
             ]

CONFIGS = [
    ("num-pdb-greedy", ["--search", "astar(numeric_pdb(pattern=greedy_numeric()))"]),
    #("num-cpdbs-sys", ["--search", "astar(numeric_cpdbs(patterns=numeric_systematic()))"]),
    ("num-ipdb", ["--search", "astar(numeric_ipdb(max_time=150))"]),

    ("hier1", ["--search", "astar(numeric_ipdb(max_time=150, extend_abstract_state_space=0))"]),
    ("hier2", ["--search", "astar(numeric_ipdb(max_time=150, extend_abstract_state_space=1, extension_h0_until_goal=-1,  extension_h1_until_goal=-1,  f_layer_offset_ratio=0.1))"]),
    ("hier3", ["--search", "astar(numeric_ipdb(max_time=150, extend_abstract_state_space=1, extension_h0_until_goal=100, extension_h1_until_goal=-1,  f_layer_offset_ratio=0.1))"]),
    ("hier4", ["--search", "astar(numeric_ipdb(max_time=150, extend_abstract_state_space=1, extension_h0_until_goal=-1,  extension_h1_until_goal=100, f_layer_offset_ratio=0.1))"]),
    ("hier5", ["--search", "astar(numeric_ipdb(max_time=150, extend_abstract_state_space=1, extension_h0_until_goal=100, extension_h1_until_goal=100, f_layer_offset_ratio=0.1))"]),

]


BUILD_OPTIONS = ["release64", "-j6"]
DRIVER_OPTIONS = ["--overall-time-limit", "5m", "--build", "release64", "--overall-memory-limit", "8G"]

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
exp.add_suite(BENCHMARKS_DIR_OTHERS, project.SUITE_NUMERIC_OTHERS_NEW)

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

exp.add_fetcher("data/2025-01-20-astar-exploration-test-eval/", name="fetch-base", filter_algorithm=["num-ipdb-12fb9", "num-pdb-greedy-12fb9"], merge=True)
#exp.add_fetcher("data/2025-01-21-hierarchical-pdbs-eval/", name="fetch-hier", filter_algorithm=["hier1-254f5", "hier2-254f5", "hier3-254f5", "hier4-254f5", "hier5-254f5"], merge=True)
exp.add_fetcher("data/2025-01-21-astar-exploration-zero-cost-fix-eval/", name="fetch-keep-pdb", filter_algorithm=["num-ipdb-e1634", "num-pdb-greedy-e1634"], merge=True)

def add_failed_lookup_ratio(run):
    if "number_failed_pdb_lookups" in run:
        run["ratio_failed_lookups_per_pdb"] = run["number_failed_pdb_lookups"] / run["evaluations"]
    return run

def add_score_planner_time(run):
    run["score_planner_time"] = tools.compute_log_score(
            run["coverage"], run.get("planner_time"), lower_bound=1.0, upper_bound=1800.0
        )
    return run

project.add_absolute_report(exp, attributes=ATTRIBUTES, filter=[add_failed_lookup_ratio, add_score_planner_time])

exp.add_report(AbsoluteReport(attributes=["coverage"], 
                              filter_algorithm=[f"{config}-{REVISIONS[0][:5]}" for config in ["blind", "iihmax", "hrmax", "landmarks", "opcount-ipc", "lmcut-ipc", "ipdb", "num-pdb-greedy", "num-cpdbs-sys", "num-ipdb"]],
                              format="html"), 
               name=f"{exp.name}-coverage")


# PLOTS

config_pairs = [(f"num-ipdb-e1634", f"num-ipdb-c811d"), 
                #(f"lmcut-ipc-{REVISIONS[0][:5]}", f"num-ipdb-{REVISIONS[0][:5]}"),
                #(f"ipdb-{REVISIONS[0][:5]}", f"num-ipdb-{REVISIONS[0][:5]}"),
                ]

def remove_pn_domain(run):
    if run["domain"] == "pn-domain":
        return False
    return run

for alg1, alg2 in config_pairs:
    for attr in ["expansions_until_last_jump", "planner_time", "search_time", "evaluations"]:
        exp.add_report(
            ScatterPlotReport(
                attributes=[attr],
                filter_algorithm=[alg1, alg2],
                filter=[remove_pn_domain],
                get_category=lambda r1, r2 : r1["domain"],
                format="png",
                show_missing=True,
            ),
            name=f"scatterplot-{attr.replace('_', '-')}-{alg1}-vs-{alg2}",
        )


exp.run_steps()
