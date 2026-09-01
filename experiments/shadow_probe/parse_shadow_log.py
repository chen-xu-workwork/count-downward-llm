#!/usr/bin/env python3
"""Turn one Count shadow-probe planner log into analysis-ready records."""

import argparse
import csv
import json
import pathlib
import re
import shlex


MARKER_RE = re.compile(r"\[([A-Z0-9-]+)\]")


def parse_structured_line(line):
    match = MARKER_RE.search(line)
    if not match:
        return None, {}
    values = {}
    try:
        tokens = shlex.split(line[match.end() :].strip())
    except ValueError:
        tokens = []
    for token in tokens:
        if "=" in token:
            key, value = token.split("=", 1)
            values[key] = value
    return match.group(1), values


def numeric(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def write_csv(path, preferred_fields, rows):
    extra_fields = sorted(
        {key for row in rows for key in row} - set(preferred_fields)
    )
    fields = list(preferred_fields) + extra_fields
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--problem", required=True)
    parser.add_argument("--scale", required=True, type=int)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--time-limit", required=True, type=float)
    parser.add_argument("--planner-exit-code", required=True, type=int)
    parser.add_argument("--runner-wall-seconds", required=True, type=float)
    args = parser.parse_args()

    log_path = pathlib.Path(args.log)
    output_dir = pathlib.Path(args.out)
    output_dir.mkdir(parents=True, exist_ok=True)
    lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()

    phases = {}
    incumbents = []
    triggers = []
    plateau_events = []
    run_timeout = None
    response_events = 0

    def phase_row(iteration):
        row = phases.setdefault(str(iteration), {})
        row.update(
            {
                "problem": args.problem,
                "scale": args.scale,
                "run_id": args.run_id,
                "iteration": str(iteration),
            }
        )
        return row

    for line in lines:
        marker, values = parse_structured_line(line)
        if marker == "NLM-ANYTIME-PHASE-START":
            phase_row(values.get("iteration", "")).update(values)
        elif marker == "NLM-ANYTIME-PHASE-END":
            phase_row(values.get("iteration", "")).update(values)
        elif marker in {
            "HYBRID-LLM-TRIGGER-STATS",
            "HYBRID-LLM-ROLLOUT-STATS",
        }:
            phase_row(values.get("iteration", "")).update(values)
        elif marker == "NLM-ANYTIME-INCUMBENT":
            row = {
                "problem": args.problem,
                "scale": args.scale,
                "run_id": args.run_id,
            }
            row.update(values)
            incumbents.append(row)
        elif marker == "NLM-LLM-TRIGGER" and values.get("request_id"):
            row = {
                "problem": args.problem,
                "scale": args.scale,
                "run_id": args.run_id,
            }
            row.update(values)
            triggers.append(row)
        elif marker == "HYBRID-LLM-PLATEAU":
            row = {
                "problem": args.problem,
                "scale": args.scale,
                "run_id": args.run_id,
            }
            row.update(values)
            plateau_events.append(row)
        elif marker == "NLM-ANYTIME-RUN-TIMEOUT":
            run_timeout = dict(values)
        elif marker == "HYBRID-LLM-ROLLOUT" and values.get("event") in {
            "response_accepted",
            "burst_started",
            "edge_prepared",
            "edge_processed",
        }:
            response_events += 1

    if not phases:
        raise SystemExit("no anytime phase records found in %s" % log_path)

    phase_rows = [
        phases[key]
        for key in sorted(
            phases,
            key=lambda value: int(value) if value.isdigit() else 10**9,
        )
    ]
    purity_fields = [
        "pending_at_end",
        "responses_completed",
        "usable_responses",
        "llm_bursts_started",
        "llm_actions_requested",
        "llm_actions_prevalidated",
        "llm_actions_processed",
        "llm_states_new",
        "llm_states_reopened",
        "llm_states_duplicate",
    ]
    nonzero_probe_fields = []
    for row in phase_rows:
        for field in purity_fields:
            value = numeric(row.get(field, 0))
            if value not in (None, 0.0):
                nonzero_probe_fields.append(
                    {"iteration": row.get("iteration"), "field": field, "value": value}
                )
    pure_probe = not nonzero_probe_fields and response_events == 0

    costs = [numeric(row.get("plan_cost")) for row in incumbents]
    costs = [cost for cost in costs if cost is not None]
    first_time = numeric(incumbents[0].get("elapsed_seconds")) if incumbents else None
    last_time = numeric(incumbents[-1].get("elapsed_seconds")) if incumbents else None
    reason_counts = {}
    for row in triggers:
        reason = row.get("reason", "unknown")
        reason_counts[reason] = reason_counts.get(reason, 0) + 1

    summary = {
        "problem": args.problem,
        "scale": args.scale,
        "run_id": args.run_id,
        "probe_mode": "log",
        "search_time_limit_seconds": args.time_limit,
        "planner_exit_code": args.planner_exit_code,
        "runner_wall_seconds": args.runner_wall_seconds,
        "phase_count": len(phase_rows),
        "incumbent_count": len(incumbents),
        "first_solution_seconds": first_time,
        "first_solution_cost": costs[0] if costs else None,
        "last_incumbent_seconds": last_time,
        "best_cost": min(costs) if costs else None,
        "trigger_count": len(triggers),
        "trigger_reason_counts": reason_counts,
        "plateau_event_count": len(plateau_events),
        "run_timeout": run_timeout,
        "pure_probe": pure_probe,
        "response_or_rollout_event_count": response_events,
        "nonzero_probe_fields": nonzero_probe_fields,
    }

    write_csv(
        output_dir / "incumbents.csv",
        [
            "problem", "scale", "run_id", "iteration", "incumbent",
            "elapsed_seconds", "plan_cost", "plan_length", "plan_number",
            "cumulative_expanded", "cumulative_evaluated",
            "cumulative_generated", "cumulative_reopened",
        ],
        incumbents,
    )
    write_csv(
        output_dir / "trigger_events.csv",
        [
            "problem", "scale", "run_id", "iteration", "request_id",
            "state", "reason", "g", "h", "expansions",
            "phase_elapsed_seconds", "comm_mode",
        ],
        triggers,
    )
    write_csv(
        output_dir / "plateau_events.csv",
        [
            "problem", "scale", "run_id", "iteration", "event",
            "window", "expansions", "dominant_h", "dominant_count",
            "window_expansions", "share", "lower_share",
            "qualifying_buckets", "selected_h", "selected_count",
            "streak", "active", "armed", "h", "state", "reason",
        ],
        plateau_events,
    )
    write_csv(
        output_dir / "phases.csv",
        [
            "problem", "scale", "run_id", "iteration", "bound",
            "remaining_seconds", "result", "elapsed_seconds", "phase_seconds",
            "plan_cost", "plan_length", "phase_expanded", "phase_evaluated",
            "phase_generated", "phase_reopened", "base_expansions",
            "request_attempts", "requests_submitted", "pending_at_end",
            "responses_completed", "usable_responses", "llm_bursts_started",
            "llm_actions_processed", "llm_states_new",
        ],
        phase_rows,
    )
    (output_dir / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )

    print(
        "PARSED problem=%s phases=%d incumbents=%d triggers=%d best_cost=%s pure_probe=%s"
        % (
            args.problem,
            len(phase_rows),
            len(incumbents),
            len(triggers),
            summary["best_cost"],
            str(pure_probe).lower(),
        )
    )
    if not pure_probe:
        raise SystemExit("probe purity check failed; inspect summary.json")


if __name__ == "__main__":
    main()
