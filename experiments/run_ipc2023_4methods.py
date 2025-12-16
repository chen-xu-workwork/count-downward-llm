#! /usr/bin/env python3

"""Run 4 configs on IPC2023 numeric domains and write a comparison CSV.

Configs (per domain/problem):
- domain_abstraction, restricted_task = 0
- domain_abstraction, restricted_task = 1
- lmcutnumeric,     restricted_task = 0
- lmcutnumeric,     restricted_task = 1

The output CSV matches the column layout of
`ipc2023_restricted_compare.csv`:

    domain,problem,config,restricted_task,solved,
    plan_length,plan_cost,expanded,total_time_s,exit_code

We use a 30-second overall time limit per run via the driver option
"--overall-time-limit 30s".
"""

import csv
import os
import subprocess
import sys
from pathlib import Path
from typing import Dict, Tuple


REPO = Path(__file__).resolve().parents[1]


# IPC 2023 numeric domains (simple & restricted) as in
# experiments/numeric-pdbs/project.py :: SUITE_NUMERIC_IPC23_ALL
DOMAINS = [
    "counters",
    "delivery",
    "drone",
    "expedition",
    "ext-plant-watering",
    "farmland-ipc23",
    "hydropower",
    "markettrader",
    "mprime",
    "pathwaysmetric",
    "rover-ipc23",
    "sailing-ipc23",
    "settlersnumeric",
    "sugar",
    "zenotravel-ipc23",
]


# First three problems per domain.
PROBLEMS = ["pfile1.pddl", "pfile2.pddl", "pfile3.pddl"]


def get_bench_root() -> Path:
    """Return benchmark root for IPC2023 numeric domains.

    Prefer the environment variable if set, otherwise fall back to the
    default path used on the local machine.
    """

    env = os.environ.get("NUMERIC_BENCHMARKS_IPC2023") or os.environ.get(
        "NUMERIC_BENCHMARKS_IPC"
    )
    if env:
        return Path(env)
    # Fallback to the path used in this workspace.
    return Path("/home/derillsith/benchmarks/ipc2023")


def build_command(
    domain_file: Path,
    problem_file: Path,
    config: str,
    restricted_task: int,
) -> Tuple[str, list]:
    """Return a tuple (config_name, argv) for subprocess.run.

    config is either "domain_abstraction" or "lmcutnumeric".
    restricted_task is 0 or 1 (for --restricted-task-transformation).
    """

    extra = []
    if restricted_task:
        extra = ["--restricted-task-transformation"]

    if config == "domain_abstraction":
        search_str = (
            "astar(domain_abstraction("
            "domain_abstraction_generator="
            "domain_abstraction_cegar("
            "max_abstraction_size=infinity,"
            "max_time=900,"
            "use_wildcard_plans=false,"
            "flaw_treatment=one_split_per_atom,"
            "numeric_split_strategy=exclusion)))"
        )
    elif config == "lmcutnumeric":
        search_str = (
            "astar(lmcutnumeric("
            "use_second_order_simple=true,"
            "bound_iterations=10,"
            "ceiling_less_than_one=true))"
        )
    else:
        raise ValueError(f"Unknown config: {config}")

    argv = [
        sys.executable,
        str(REPO / "fast-downward.py"),
        "--build",
        "release64",
        "--overall-time-limit",
        "30s",
        *extra,
        str(domain_file),
        str(problem_file),
        "--search",
        search_str,
    ]
    return config, argv


def parse_metrics(output: str) -> Dict[str, str]:
    """Extract metrics from a Fast Downward log.

    Returns a dict with keys: solved, plan_length, plan_cost,
    expanded, total_time_s (all as strings, possibly empty).
    """

    solved = "0"
    if "Solution found!" in output or "Plan length:" in output:
        solved = "1"

    plan_length = ""
    plan_cost = ""
    expanded = ""
    total_time_s = ""

    import re

    m = re.search(r"Plan length: (\d+)", output)
    if m:
        plan_length = m.group(1)

    m = re.search(r"Plan cost: ([0-9.]+)", output)
    if m:
        plan_cost = m.group(1)

    m = re.search(r"Expanded (\d+) state", output)
    if m:
        expanded = m.group(1)

    m = re.search(r"Total time: ([0-9.]+)s", output)
    if m:
        total_time_s = m.group(1)

    return {
        "solved": solved,
        "plan_length": plan_length,
        "plan_cost": plan_cost,
        "expanded": expanded,
        "total_time_s": total_time_s,
    }


def main() -> None:
    bench_root = get_bench_root()
    out_csv = REPO / "ipc2023_restricted_compare_new.csv"
    logs_dir = REPO / "test_results" / "ipc2023_4methods_logs"
    logs_dir.mkdir(parents=True, exist_ok=True)

    print(f"Benchmark root: {bench_root}")
    print(f"Writing CSV to: {out_csv}")
    print(f"Writing logs to: {logs_dir}")

    fieldnames = [
        "domain",
        "problem",
        "config",
        "restricted_task",
        "solved",
        "plan_length",
        "plan_cost",
        "expanded",
        "total_time_s",
        "exit_code",
    ]

    rows = []

    # (config_name, restricted_flag)
    configs = [
        ("domain_abstraction", 0),
        ("domain_abstraction", 1),
        ("lmcutnumeric", 0),
        ("lmcutnumeric", 1),
    ]

    for domain in DOMAINS:
        domdir = bench_root / domain
        for problem in PROBLEMS:
            problem_file = domdir / problem
            domain_file = domdir / "domain.pddl"
            if not problem_file.exists():
                print(f"[SKIP] Missing problem {problem_file}")
                continue
            if not domain_file.exists():
                print(f"[SKIP] Missing domain {domain_file}")
                continue

            for config_name, restricted_flag in configs:
                cfg, argv = build_command(
                    domain_file, problem_file, config_name, restricted_flag
                )

                print(f"[RUN] {domain} {problem} cfg={cfg} restricted={restricted_flag}")
                log_path = (
                    logs_dir
                    / f"{domain}_{problem.replace('.pddl', '')}_{cfg}_r{restricted_flag}.log"
                )

                try:
                    completed = subprocess.run(
                        argv,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT,
                        text=True,
                        cwd=str(REPO),
                        timeout=40.0,
                    )
                    output = completed.stdout
                    exit_code = completed.returncode
                except subprocess.TimeoutExpired as exc:
                    output = exc.stdout or b""
                    exit_code = 999

                # Normalize output to text and save log regardless of success.
                if isinstance(output, bytes):
                    output = output.decode("utf-8", errors="replace")
                log_path.write_text(output or "", encoding="utf-8")

                metrics = parse_metrics(output or "")
                row = {
                    "domain": domain,
                    "problem": problem,
                    "config": cfg,
                    "restricted_task": str(restricted_flag),
                    "solved": metrics["solved"],
                    "plan_length": metrics["plan_length"],
                    "plan_cost": metrics["plan_cost"],
                    "expanded": metrics["expanded"],
                    "total_time_s": metrics["total_time_s"],
                    "exit_code": str(exit_code),
                }
                rows.append(row)

    with out_csv.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)

    print(f"Wrote {len(rows)} rows to {out_csv}")


if __name__ == "__main__":
    main()
