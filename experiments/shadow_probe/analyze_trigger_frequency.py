#!/usr/bin/env python3
"""Summarize per-phase LLM trigger frequency from a shadow-probe run."""

import argparse
import csv
import pathlib
import statistics


def read_csv(path):
    with path.open(encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def number(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def write_csv(path, rows, preferred_fields):
    extras = sorted({key for row in rows for key in row} - set(preferred_fields))
    fields = list(preferred_fields) + extras
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def classify_interval(seconds, target):
    if seconds is None:
        return "unknown"
    if seconds < target * 0.75:
        return "too_frequent"
    if seconds <= target * 1.5:
        return "near_target"
    return "sparser_than_target"


def format_number(value, digits=1):
    return "-" if value is None else ("%.*f" % (digits, value))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("results_root")
    parser.add_argument("--target-seconds", type=float, default=120.0)
    args = parser.parse_args()
    root = pathlib.Path(args.results_root)
    trigger_path = root / "all_trigger_events.csv"
    phase_path = root / "all_phases.csv"
    if not trigger_path.is_file() or not phase_path.is_file():
        raise SystemExit(
            "missing all_trigger_events.csv or all_phases.csv under %s" % root
        )

    triggers = read_csv(trigger_path)
    phases = read_csv(phase_path)
    if triggers and not any(number(row.get("phase_elapsed_seconds")) is not None
                            for row in triggers):
        raise SystemExit(
            "trigger logs have no phase_elapsed_seconds; rebuild Count after "
            "the frequency-probe source change and rerun the shadow probe"
        )

    phase_by_key = {
        (row.get("problem", ""), row.get("run_id", ""), row.get("iteration", "")): row
        for row in phases
    }
    triggers_by_key = {}
    for row in triggers:
        key = (row.get("problem", ""), row.get("run_id", ""), row.get("iteration", ""))
        triggers_by_key.setdefault(key, []).append(row)

    interval_rows = []
    summary_rows = []
    all_intervals = []
    overall_reason_counts = {}

    all_keys = sorted(
        set(phase_by_key) | set(triggers_by_key),
        key=lambda key: (key[0], int(key[2]) if key[2].isdigit() else 10**9),
    )
    for key in all_keys:
        phase = phase_by_key.get(key, {})
        phase_triggers = triggers_by_key.get(key, [])
        phase_triggers.sort(
            key=lambda row: (
                number(row.get("phase_elapsed_seconds"))
                if number(row.get("phase_elapsed_seconds")) is not None
                else float("inf"),
                number(row.get("expansions")) or 0.0,
            )
        )

        previous_seconds = None
        previous_expansions = None
        phase_intervals = []
        phase_expansion_intervals = []
        reason_counts = {}
        first_trigger_seconds = None
        first_trigger_expansions = None

        for index, trigger in enumerate(phase_triggers, start=1):
            seconds = number(trigger.get("phase_elapsed_seconds"))
            expansions = number(trigger.get("expansions"))
            if index == 1:
                first_trigger_seconds = seconds
                first_trigger_expansions = expansions
            interval_seconds = (
                seconds - previous_seconds
                if seconds is not None and previous_seconds is not None
                else None
            )
            interval_expansions = (
                expansions - previous_expansions
                if expansions is not None and previous_expansions is not None
                else None
            )
            if interval_seconds is not None:
                phase_intervals.append(interval_seconds)
                all_intervals.append(interval_seconds)
            if interval_expansions is not None:
                phase_expansion_intervals.append(interval_expansions)

            reason = trigger.get("reason", "unknown")
            reason_counts[reason] = reason_counts.get(reason, 0) + 1
            overall_reason_counts[reason] = overall_reason_counts.get(reason, 0) + 1
            row = dict(trigger)
            row.update(
                {
                    "trigger_index_in_phase": index,
                    "interval_seconds": interval_seconds,
                    "interval_expansions": interval_expansions,
                    "target_seconds": args.target_seconds,
                    "frequency_class": classify_interval(
                        interval_seconds, args.target_seconds
                    ) if index > 1 else "first_trigger",
                }
            )
            interval_rows.append(row)
            previous_seconds = seconds
            previous_expansions = expansions

        phase_seconds = number(phase.get("phase_seconds"))
        summary = {
            "problem": key[0],
            "run_id": key[1],
            "iteration": key[2],
            "phase_seconds": phase_seconds,
            "base_expansions": phase.get("base_expansions", ""),
            "trigger_count": len(phase_triggers),
            "first_trigger_seconds": first_trigger_seconds,
            "first_trigger_expansions": first_trigger_expansions,
            "mean_inter_trigger_seconds": (
                statistics.mean(phase_intervals) if phase_intervals else None
            ),
            "median_inter_trigger_seconds": (
                statistics.median(phase_intervals) if phase_intervals else None
            ),
            "min_inter_trigger_seconds": min(phase_intervals) if phase_intervals else None,
            "max_inter_trigger_seconds": max(phase_intervals) if phase_intervals else None,
            "mean_inter_trigger_expansions": (
                statistics.mean(phase_expansion_intervals)
                if phase_expansion_intervals else None
            ),
            "calls_per_minute": (
                len(phase_triggers) * 60.0 / phase_seconds
                if phase_seconds and phase_seconds > 0 else None
            ),
            "near_target_intervals": sum(
                classify_interval(value, args.target_seconds) == "near_target"
                for value in phase_intervals
            ),
            "too_frequent_intervals": sum(
                classify_interval(value, args.target_seconds) == "too_frequent"
                for value in phase_intervals
            ),
            "sparser_intervals": sum(
                classify_interval(value, args.target_seconds) == "sparser_than_target"
                for value in phase_intervals
            ),
        }
        for reason, count in reason_counts.items():
            summary["trigger_%s" % reason] = count
        summary_rows.append(summary)

    write_csv(
        root / "trigger_intervals.csv",
        interval_rows,
        [
            "problem", "scale", "run_id", "iteration",
            "trigger_index_in_phase", "request_id", "state", "reason",
            "g", "h", "expansions", "phase_elapsed_seconds",
            "interval_expansions", "interval_seconds", "target_seconds",
            "frequency_class", "comm_mode",
        ],
    )
    write_csv(
        root / "trigger_frequency_summary.csv",
        summary_rows,
        [
            "problem", "run_id", "iteration", "phase_seconds",
            "base_expansions", "trigger_count", "first_trigger_seconds",
            "first_trigger_expansions", "mean_inter_trigger_seconds",
            "median_inter_trigger_seconds", "min_inter_trigger_seconds",
            "max_inter_trigger_seconds", "mean_inter_trigger_expansions",
            "calls_per_minute", "near_target_intervals",
            "too_frequent_intervals", "sparser_intervals",
        ],
    )

    print()
    print("LLM 触发频率报告（phase 之间独立）")
    print(
        "目标相邻间隔：%.0f 秒；%.0f--%.0f 秒记为接近目标。"
        % (
            args.target_seconds,
            args.target_seconds * 0.75,
            args.target_seconds * 1.5,
        )
    )
    print("总触发：%d；可计算的相邻间隔：%d" % (len(triggers), len(all_intervals)))
    if all_intervals:
        print(
            "相邻间隔：mean=%s 秒，median=%s 秒，min=%s 秒，max=%s 秒"
            % (
                format_number(statistics.mean(all_intervals)),
                format_number(statistics.median(all_intervals)),
                format_number(min(all_intervals)),
                format_number(max(all_intervals)),
            )
        )
        classes = {
            name: sum(classify_interval(value, args.target_seconds) == name
                      for value in all_intervals)
            for name in ("too_frequent", "near_target", "sparser_than_target")
        }
        print(
            "区间分类：too_frequent=%d near_target=%d sparser=%d"
            % (
                classes["too_frequent"], classes["near_target"],
                classes["sparser_than_target"],
            )
        )
    print(
        "触发原因：%s"
        % (", ".join("%s=%d" % item for item in sorted(overall_reason_counts.items()))
           or "无触发")
    )
    print("逐次明细：%s" % (root / "trigger_intervals.csv"))
    print("逐 phase 汇总：%s" % (root / "trigger_frequency_summary.csv"))


if __name__ == "__main__":
    main()
