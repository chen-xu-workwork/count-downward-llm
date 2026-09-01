#!/usr/bin/env python3
"""Aggregate the four per-problem shadow-probe result directories."""

import argparse
import csv
import json
import pathlib


def write_rows(path, rows, preferred_fields=()):
    extras = sorted({key for row in rows for key in row} - set(preferred_fields))
    fields = list(preferred_fields) + extras
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def read_csv(path):
    if not path.is_file():
        return []
    with path.open(encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("results_root")
    args = parser.parse_args()
    root = pathlib.Path(args.results_root)

    summaries = []
    incumbents = []
    triggers = []
    plateau_events = []
    phases = []
    for summary_path in sorted(root.glob("*/summary.json")):
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        reason_counts = summary.pop("trigger_reason_counts", {})
        timeout = summary.pop("run_timeout", None) or {}
        summary.pop("nonzero_probe_fields", None)
        summary["result_directory"] = str(summary_path.parent)
        for key, value in timeout.items():
            summary["timeout_%s" % key] = value
        for reason, count in reason_counts.items():
            summary["trigger_%s" % reason] = count
        summaries.append(summary)
        incumbents.extend(read_csv(summary_path.parent / "incumbents.csv"))
        triggers.extend(read_csv(summary_path.parent / "trigger_events.csv"))
        plateau_events.extend(
            read_csv(summary_path.parent / "plateau_events.csv")
        )
        phases.extend(read_csv(summary_path.parent / "phases.csv"))

    if not summaries:
        raise SystemExit("no per-problem summary.json files under %s" % root)

    write_rows(
        root / "all_runs_summary.csv",
        summaries,
        [
            "problem", "scale", "run_id", "pure_probe", "planner_exit_code",
            "phase_count", "incumbent_count", "first_solution_seconds",
            "first_solution_cost", "last_incumbent_seconds", "best_cost",
            "trigger_count", "search_time_limit_seconds", "result_directory",
            "runner_wall_seconds", "timeout_elapsed_seconds",
            "timeout_completed_iterations", "timeout_best_bound",
        ],
    )
    write_rows(
        root / "all_incumbents.csv",
        incumbents,
        [
            "problem", "scale", "run_id", "iteration", "incumbent",
            "elapsed_seconds", "plan_cost", "plan_length", "plan_number",
            "cumulative_expanded", "cumulative_evaluated",
            "cumulative_generated", "cumulative_reopened",
        ],
    )
    write_rows(
        root / "all_trigger_events.csv",
        triggers,
        [
            "problem", "scale", "run_id", "iteration", "request_id",
            "state", "reason", "g", "h", "expansions",
            "phase_elapsed_seconds", "comm_mode",
        ],
    )
    write_rows(
        root / "all_plateau_events.csv",
        plateau_events,
        [
            "problem", "scale", "run_id", "iteration", "event",
            "window", "expansions", "dominant_h", "dominant_count",
            "window_expansions", "share", "lower_share",
            "qualifying_buckets", "selected_h", "selected_count",
            "streak", "active", "armed", "h", "state", "reason",
        ],
    )
    write_rows(
        root / "all_phases.csv",
        phases,
        [
            "problem", "scale", "run_id", "iteration", "bound", "result",
            "elapsed_seconds", "phase_seconds", "plan_cost", "plan_length",
            "phase_expanded", "phase_evaluated", "phase_generated",
            "requests_submitted", "pending_at_end", "responses_completed",
            "llm_actions_processed",
        ],
    )
    print(
        "AGGREGATED runs=%d incumbents=%d triggers=%d plateau_events=%d root=%s"
        % (
            len(summaries), len(incumbents), len(triggers),
            len(plateau_events), root,
        )
    )


if __name__ == "__main__":
    main()
