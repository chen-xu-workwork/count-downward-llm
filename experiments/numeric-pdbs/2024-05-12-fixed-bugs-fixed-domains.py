#! /usr/bin/env python

import os
import shutil

import numeric_pdb_parser

import project
from lab.environments import TetralithEnvironment


REPO = project.get_repo_base()
BENCHMARKS_DIR = os.environ["NUMERIC_BENCHMARKS_TRANSFORMED"]

# If REVISION_CACHE is None, the default "./data/revision-cache/" is used.
REVISION_CACHE = os.environ.get("DOWNWARD_REVISION_CACHE")
SUITE = project.SUITE_NUMERIC

ENV = TetralithEnvironment(
    email="daniel.gnad@liu.se",
#    time_limit_per_task="24:00:00",
#    memory_per_cpu="8300M",
    extra_options="#SBATCH -A naiss2023-5-236", # parground
)

REVISION = "3e29d959920432e22710f7d5a96cca201153409e"

CONFIGS = [
    ("blind", ["--search", f"astar(blind())"]),
    ("hmax", ["--search", f"astar(iihmax())"]),
    ("lmcut", ["--search", f"astar(lmcutnumeric())"]),

]
for limit in [10000, 100000, 1000000, 10000000, 100000000]:
    CONFIGS.append((f"pdb-{int(limit / 1000)}k", ["--search", f"astar(numeric_pdb(pattern=greedy_numeric(), max_number_states={limit}))"]))

BUILD_OPTIONS = ["release64", "-j4"]
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
for config_nick, config in CONFIGS:
    exp.add_algorithm(
        config_nick,
        REPO,
        REVISION,
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


project.add_absolute_report(exp, attributes=ATTRIBUTES)

exp.run_steps()
