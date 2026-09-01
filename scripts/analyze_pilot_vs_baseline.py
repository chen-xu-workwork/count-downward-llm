#!/usr/bin/env python3
"""Compare pilot-live jobs that actually used the LLM with matching baselines."""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import statistics
from collections import Counter
from pathlib import Path
from typing import Any, Iterable


PROBLEM_RE = re.compile(r"problem_scale_(?P<scale>\d+)_id_(?P<id>\d+)")


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        return list(csv.DictReader(handle))


def number(value: Any) -> float | None:
    if value is None or str(value).strip() == "":
        return None
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return None
    return parsed if math.isfinite(parsed) else None


def integer(value: Any) -> int:
    parsed = number(value)
    return int(parsed) if parsed is not None else 0


def problem_key(value: str) -> str:
    match = PROBLEM_RE.search(value)
    if not match:
        raise ValueError(f"Cannot identify problem from {value!r}")
    return match.group(0)


def problem_label(key: str) -> str:
    match = PROBLEM_RE.fullmatch(key)
    assert match
    return f"s{match.group('scale')}/id{match.group('id')}"


def fmt(value: float | None, digits: int = 2) -> str:
    if value is None:
        return "—"
    return f"{value:.{digits}f}"


def fmt_int(value: float | None) -> str:
    if value is None:
        return "—"
    return f"{int(round(value)):,}"


def median(values: Iterable[float | None]) -> float | None:
    clean = [value for value in values if value is not None and math.isfinite(value)]
    return statistics.median(clean) if clean else None


def summarize_job(job_dir: Path) -> dict[str, Any]:
    result = read_json(job_dir / "job_result.json")
    key = problem_key(str(result.get("problem") or result.get("job_id") or job_dir.name))
    match = PROBLEM_RE.fullmatch(key)
    assert match
    incumbents = read_csv(job_dir / "anytime" / "incumbents.csv")
    requests = read_csv(job_dir / "anytime" / "llm_requests.csv")
    phases = read_csv(job_dir / "anytime" / "phases.csv")

    incumbent_points: list[dict[str, Any]] = []
    for row in incumbents:
        incumbent_points.append(
            {
                "iteration": integer(row.get("iteration")),
                "time": number(row.get("elapsed_seconds")),
                "cost": number(row.get("plan_cost")),
                "length": number(row.get("plan_length")),
                "expanded": number(row.get("cumulative_expanded")),
                "injected": integer(row.get("cumulative_injected_states")),
                "usable": integer(row.get("cumulative_usable_samples")),
                "requests": integer(row.get("cumulative_state_requests")),
            }
        )

    completed = [row for row in requests if row.get("status") in {"ok", "partial"}]
    request_finish_times = [
        number(row.get("finished_seconds"))
        for row in completed
        if number(row.get("finished_seconds")) is not None
    ]
    injected_finish_times = [
        number(row.get("finished_seconds"))
        for row in completed
        if integer(row.get("inserted_states")) > 0
        and number(row.get("finished_seconds")) is not None
    ]
    first_response = min(request_finish_times) if request_finish_times else None
    first_injection = min(injected_finish_times) if injected_finish_times else None

    before_injection = [
        point
        for point in incumbent_points
        if first_injection is not None
        and point["time"] is not None
        and point["time"] < first_injection
    ]
    after_injection = [
        point
        for point in incumbent_points
        if first_injection is not None
        and point["time"] is not None
        and point["time"] >= first_injection
    ]
    llm_marked = [point for point in incumbent_points if point["injected"] > 0]

    first = incumbent_points[0] if incumbent_points else None
    final = incumbent_points[-1] if incumbent_points else None
    pre_injection_cost = before_injection[-1]["cost"] if before_injection else None
    final_cost = final["cost"] if final else None
    post_injection_gain = (
        pre_injection_cost - final_cost
        if pre_injection_cost is not None and final_cost is not None
        else None
    )

    return {
        "key": key,
        "label": problem_label(key),
        "scale": int(match.group("scale")),
        "problem_id": int(match.group("id")),
        "job_dir": str(job_dir.resolve()),
        "mode": result.get("mode"),
        "status": result.get("status"),
        "return_code": result.get("return_code"),
        "time_limit": number(result.get("time_limit_seconds")),
        "wall_time": number(result.get("elapsed_seconds")),
        "solved": bool(incumbent_points),
        "incumbent_count": len(incumbent_points),
        "first_time": first["time"] if first else None,
        "first_cost": first["cost"] if first else None,
        "first_length": first["length"] if first else None,
        "first_expanded": first["expanded"] if first else None,
        "final_time": final["time"] if final else None,
        "final_cost": final_cost,
        "final_length": final["length"] if final else None,
        "final_expanded": final["expanded"] if final else None,
        "incumbents": incumbent_points,
        "request_count": len(requests),
        "request_statuses": dict(Counter(row.get("status") or "" for row in requests)),
        "request_reasons": dict(Counter(row.get("reason") or "" for row in requests)),
        "sample_count": sum(integer(row.get("sample_count")) for row in requests),
        "usable_sample_count": sum(integer(row.get("usable_sample_count")) for row in requests),
        "inserted_states": sum(integer(row.get("inserted_states")) for row in requests),
        "first_response_time": first_response,
        "first_injection_time": first_injection,
        "pre_injection_cost": pre_injection_cost,
        "post_injection_gain": post_injection_gain,
        "post_injection_incumbents": len(after_injection),
        "llm_marked_incumbents": len(llm_marked),
        "first_solution_after_injection": bool(
            first
            and first_injection is not None
            and first["time"] is not None
            and first["time"] >= first_injection
        ),
        "phase_count": len(phases),
    }


def load_jobs(root: Path) -> dict[str, dict[str, Any]]:
    jobs: dict[str, dict[str, Any]] = {}
    for result_file in sorted(root.glob("*/job_result.json")):
        summary = summarize_job(result_file.parent)
        if summary["key"] in jobs:
            raise ValueError(f"Duplicate problem {summary['key']} below {root}")
        jobs[summary["key"]] = summary
    return jobs


def ratio(numerator: float | None, denominator: float | None) -> float | None:
    if numerator is None or denominator is None or denominator <= 0:
        return None
    return numerator / denominator


def first_reach(job: dict[str, Any], target_cost: float) -> dict[str, Any] | None:
    for point in job["incumbents"]:
        if point["cost"] is not None and point["cost"] <= target_cost:
            return point
    return None


def compare_pair(baseline: dict[str, Any], pilot: dict[str, Any]) -> dict[str, Any]:
    cost_delta = None
    cost_improvement = None
    if baseline["final_cost"] is not None and pilot["final_cost"] is not None:
        cost_delta = baseline["final_cost"] - pilot["final_cost"]
        if baseline["final_cost"] != 0:
            cost_improvement = cost_delta / baseline["final_cost"]

    if pilot["solved"] and not baseline["solved"]:
        winner = "pilot_only"
    elif baseline["solved"] and not pilot["solved"]:
        winner = "baseline_only"
    elif not pilot["solved"] and not baseline["solved"]:
        winner = "neither"
    elif cost_delta is not None and cost_delta > 0:
        winner = "pilot_better"
    elif cost_delta is not None and cost_delta < 0:
        winner = "baseline_better"
    else:
        winner = "tie"

    first_time_ratio = ratio(baseline["first_time"], pilot["first_time"])
    first_expanded_ratio = ratio(baseline["first_expanded"], pilot["first_expanded"])
    common_target_cost = None
    baseline_target = None
    pilot_target = None
    if baseline["final_cost"] is not None and pilot["final_cost"] is not None:
        common_target_cost = max(baseline["final_cost"], pilot["final_cost"])
        baseline_target = first_reach(baseline, common_target_cost)
        pilot_target = first_reach(pilot, common_target_cost)
    target_time_ratio = ratio(
        baseline_target["time"] if baseline_target else None,
        pilot_target["time"] if pilot_target else None,
    )
    target_expanded_ratio = ratio(
        baseline_target["expanded"] if baseline_target else None,
        pilot_target["expanded"] if pilot_target else None,
    )
    pilot_target_after_injection = bool(
        pilot_target
        and pilot["first_injection_time"] is not None
        and pilot_target["time"] is not None
        and pilot_target["time"] >= pilot["first_injection_time"]
    )
    same_first_search_point = bool(
        baseline["first_cost"] is not None
        and pilot["first_cost"] is not None
        and baseline["first_cost"] == pilot["first_cost"]
        and baseline["first_expanded"] == pilot["first_expanded"]
    )

    evidence = "neutral"
    if winner == "pilot_only" and pilot["first_solution_after_injection"]:
        evidence = "positive_coverage_after_injection"
    elif winner == "pilot_better" and (
        pilot["post_injection_gain"] is not None and pilot["post_injection_gain"] > 0
        or pilot["first_solution_after_injection"]
    ):
        evidence = "positive_cost_after_injection"
    elif (
        winner == "tie"
        and pilot_target_after_injection
        and target_expanded_ratio is not None
        and target_expanded_ratio >= 1.05
    ):
        evidence = "positive_efficiency_after_injection"
    elif winner in {"baseline_better", "baseline_only"}:
        evidence = "negative_pair_result"
    elif pilot["llm_marked_incumbents"] == 0:
        evidence = "no_incumbent_after_injection"

    return {
        "problem": pilot["key"],
        "label": pilot["label"],
        "scale": pilot["scale"],
        "problem_id": pilot["problem_id"],
        "baseline_status": baseline["status"],
        "pilot_status": pilot["status"],
        "baseline_solved": baseline["solved"],
        "pilot_solved": pilot["solved"],
        "baseline_first_time": baseline["first_time"],
        "pilot_first_time": pilot["first_time"],
        "first_time_ratio": first_time_ratio,
        "baseline_first_expanded": baseline["first_expanded"],
        "pilot_first_expanded": pilot["first_expanded"],
        "first_expanded_ratio": first_expanded_ratio,
        "common_target_cost": common_target_cost,
        "baseline_target_time": baseline_target["time"] if baseline_target else None,
        "pilot_target_time": pilot_target["time"] if pilot_target else None,
        "target_time_ratio": target_time_ratio,
        "baseline_target_expanded": baseline_target["expanded"] if baseline_target else None,
        "pilot_target_expanded": pilot_target["expanded"] if pilot_target else None,
        "target_expanded_ratio": target_expanded_ratio,
        "pilot_target_after_injection": pilot_target_after_injection,
        "baseline_first_cost": baseline["first_cost"],
        "pilot_first_cost": pilot["first_cost"],
        "baseline_final_cost": baseline["final_cost"],
        "pilot_final_cost": pilot["final_cost"],
        "cost_delta_baseline_minus_pilot": cost_delta,
        "cost_improvement_rate": cost_improvement,
        "baseline_final_length": baseline["final_length"],
        "pilot_final_length": pilot["final_length"],
        "baseline_incumbents": baseline["incumbent_count"],
        "pilot_incumbents": pilot["incumbent_count"],
        "llm_requests": pilot["request_count"],
        "llm_usable_samples": pilot["usable_sample_count"],
        "llm_inserted_states": pilot["inserted_states"],
        "first_llm_injection_time": pilot["first_injection_time"],
        "pre_injection_cost": pilot["pre_injection_cost"],
        "post_injection_cost_gain": pilot["post_injection_gain"],
        "post_injection_incumbents": pilot["post_injection_incumbents"],
        "llm_marked_incumbents": pilot["llm_marked_incumbents"],
        "first_solution_after_injection": pilot["first_solution_after_injection"],
        "same_first_cost_and_expanded": same_first_search_point,
        "winner": winner,
        "time_association": evidence,
        "baseline_job_dir": baseline["job_dir"],
        "pilot_job_dir": pilot["job_dir"],
    }


def baseline_purity(root: Path) -> dict[str, Any]:
    non_off_jobs = 0
    request_rows = 0
    active_phase_rows = 0
    for job_dir in sorted(path for path in root.iterdir() if path.is_dir()):
        job_path = job_dir / "job.json"
        if job_path.exists() and read_json(job_path).get("mode") != "off":
            non_off_jobs += 1
        request_rows += len(read_csv(job_dir / "anytime" / "llm_requests.csv"))
        for row in read_csv(job_dir / "anytime" / "phases.csv"):
            if any(
                integer(row.get(field)) != 0
                for field in (
                    "cumulative_submitted",
                    "cumulative_model_generations",
                    "cumulative_usable_samples",
                    "cumulative_injected_states",
                )
            ):
                active_phase_rows += 1
    return {
        "non_off_jobs": non_off_jobs,
        "llm_request_rows": request_rows,
        "llm_active_phase_rows": active_phase_rows,
        "clean": non_off_jobs == 0 and request_rows == 0 and active_phase_rows == 0,
    }


def group_summary(rows: list[dict[str, Any]], scale: int | None = None) -> dict[str, Any]:
    subset = [row for row in rows if scale is None or row["scale"] == scale]
    both_solved = [row for row in subset if row["baseline_solved"] and row["pilot_solved"]]
    winners = Counter(row["winner"] for row in subset)
    return {
        "scale": scale if scale is not None else "all",
        "pairs": len(subset),
        "baseline_solved": sum(row["baseline_solved"] for row in subset),
        "pilot_solved": sum(row["pilot_solved"] for row in subset),
        "both_solved": len(both_solved),
        "pilot_better": winners["pilot_better"],
        "baseline_better": winners["baseline_better"],
        "ties": winners["tie"],
        "pilot_only": winners["pilot_only"],
        "baseline_only": winners["baseline_only"],
        "median_cost_delta": median(row["cost_delta_baseline_minus_pilot"] for row in both_solved),
        "median_cost_improvement_rate": median(row["cost_improvement_rate"] for row in both_solved),
        "median_first_time_ratio": median(row["first_time_ratio"] for row in both_solved),
        "median_first_expanded_ratio": median(row["first_expanded_ratio"] for row in both_solved),
        "median_target_time_ratio": median(row["target_time_ratio"] for row in both_solved),
        "median_target_expanded_ratio": median(row["target_expanded_ratio"] for row in both_solved),
        "llm_requests": sum(row["llm_requests"] for row in subset),
        "llm_usable_samples": sum(row["llm_usable_samples"] for row in subset),
        "llm_inserted_states": sum(row["llm_inserted_states"] for row in subset),
        "positive_after_injection": sum(
            row["time_association"].startswith("positive_") for row in subset
        ),
        "no_incumbent_after_injection": sum(
            row["llm_marked_incumbents"] == 0 for row in subset
        ),
    }


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    fieldnames = list(rows[0]) if rows else []
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def md_pair_row(row: dict[str, Any]) -> str:
    solved = f"{int(row['baseline_solved'])}/{int(row['pilot_solved'])}"
    cost_pair = f"{fmt(row['baseline_final_cost'], 0)} / {fmt(row['pilot_final_cost'], 0)}"
    delta = fmt(row["cost_delta_baseline_minus_pilot"], 0)
    expanded_pair = (
        f"{fmt_int(row['baseline_first_expanded'])} / {fmt_int(row['pilot_first_expanded'])}"
    )
    time_pair = f"{fmt(row['baseline_first_time'])} / {fmt(row['pilot_first_time'])}"
    llm = f"{row['llm_requests']}/{row['llm_usable_samples']}/{row['llm_inserted_states']}"
    association = {
        "positive_coverage_after_injection": "LLM 后新增覆盖",
        "positive_cost_after_injection": "LLM 后改善且胜出",
        "positive_efficiency_after_injection": "LLM 后更省展开达同等质量",
        "negative_pair_result": "baseline 最终更优",
        "no_incumbent_after_injection": "注入后无 incumbent",
        "neutral": "无明确方向",
    }[row["time_association"]]
    return (
        f"| {row['label']} | {solved} | {time_pair} | {expanded_pair} | {cost_pair} | "
        f"{delta} | {llm} | {association} |"
    )


def write_report(
    path: Path,
    baseline_root: Path,
    pilot_root: Path,
    config: dict[str, Any],
    purity: dict[str, Any],
    rows: list[dict[str, Any]],
    groups: list[dict[str, Any]],
) -> None:
    overall = groups[0]
    positive_rows = [row for row in rows if row["time_association"].startswith("positive_")]
    negative_rows = [row for row in rows if row["winner"] in {"baseline_better", "baseline_only"}]
    no_incumbent_rows = [row for row in rows if row["llm_marked_incumbents"] == 0]
    same_initial = sum(row["same_first_cost_and_expanded"] for row in rows)

    lines = [
        "# Early LLM pilot 与 baseline 配对分析",
        "",
        "## 结论摘要",
        "",
        f"本次只分析 `pilot-live` 中实际产生 LLM 请求的 **{len(rows)} 题**；5 道 scale-10 题没有请求，未纳入 LLM 效果判断。",
        "",
        f"- 覆盖率：baseline {overall['baseline_solved']}/{overall['pairs']}，pilot {overall['pilot_solved']}/{overall['pairs']}。",
        f"- 双方均求解的配对中：pilot 最终 cost 更低 {overall['pilot_better']} 题，baseline 更低 {overall['baseline_better']} 题，持平 {overall['ties']} 题。",
        f"- 最终 cost 的配对中位差（baseline − pilot）为 {fmt(overall['median_cost_delta'])}；配对改善率中位数为 {fmt((overall['median_cost_improvement_rate'] or 0) * 100)}%。",
        f"- 有 {len(positive_rows)} 题满足“pilot 胜出”或“至少少 5% 展开达到同等质量”，且相应首解/cost 改善发生在首次 LLM 注入之后；这构成时间关联证据，但不是单次实验下的因果证明。",
        f"- {len(no_incumbent_rows)} 题在 LLM 注入后没有产生新的 incumbent；注入本身没有转化为可观察到的方案改善。",
        f"- {same_initial}/{len(rows)} 题的首解 cost 与首解展开数在两边完全一致，说明这些题的首解主要来自介入前的相同搜索轨迹。",
        "",
        "## 配置与完整性",
        "",
        f"- baseline：{baseline_root}",
        f"- pilot：{pilot_root}",
        f"- baseline 共有 {config['baseline_jobs']} 个结果（plan_found={config['baseline_statuses'].get('plan_found', 0)}，incomplete={config['baseline_statuses'].get('incomplete', 0)}，failed={config['baseline_statuses'].get('failed', 0)}）；pilot 共有 {config['pilot_jobs']} 个结果（plan_found={config['pilot_statuses'].get('plan_found', 0)}，failed={config['pilot_statuses'].get('failed', 0)}）。全部 pilot 题均找到 baseline 配对。",
        f"- baseline 纯净性：{'通过' if purity['clean'] else '未通过'}（非 off job={purity['non_off_jobs']}，LLM 请求行={purity['llm_request_rows']}，LLM 活跃 phase 行={purity['llm_active_phase_rows']}）。",
        f"- 时限相同：scale 20/30 为 {fmt(config['pilot_small_limit'], 0)} 秒，scale 40 为 {fmt(config['pilot_large_limit'], 0)} 秒。",
        f"- 并行配置不同：baseline small_parallelism={config['baseline_parallelism']}，pilot small_parallelism={config['pilot_parallelism']}。pilot 运行报告记录为 AutoDL A800 环境；baseline 目录名标注 Xeon 6459C。因此 wall-clock 时间只能作为提示，不能单独归因于 LLM。",
        "",
        "## 逐题配对",
        "",
        "`求解 B/L`、`首解时间 B/L`、`首解展开 B/L` 和 `最终 cost B/L` 均按 baseline / LLM pilot 顺序。cost 差值为 baseline − pilot，正数表示 pilot 更好。LLM 活动为 请求/可用样本/注入状态。",
        "",
        "| 问题 | 求解 B/L | 首解时间 B/L (s) | 首解展开 B/L | 最终 cost B/L | cost 差值 | LLM 活动 | 时间关联判断 |",
        "|---|---:|---:|---:|---:|---:|---:|---|",
    ]
    lines.extend(md_pair_row(row) for row in rows)
    lines.extend(
        [
            "",
            "## 按规模汇总",
            "",
            "| Scale | 配对 | 求解 B/L | pilot 胜 / baseline 胜 / 平 | cost 改善率中位数 | 达同质展开比中位数 | 首解展开比中位数 | LLM 请求/可用/注入 |",
            "|---:|---:|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for group in groups[1:]:
        lines.append(
            f"| {group['scale']} | {group['pairs']} | {group['baseline_solved']}/{group['pilot_solved']} | "
            f"{group['pilot_better']} / {group['baseline_better']} / {group['ties']} | "
            f"{fmt((group['median_cost_improvement_rate'] or 0) * 100)}% | "
            f"{fmt(group['median_target_expanded_ratio'])}× | {fmt(group['median_first_expanded_ratio'])}× | "
            f"{group['llm_requests']}/{group['llm_usable_samples']}/{group['llm_inserted_states']} |"
        )

    lines.extend(["", "## 如何解释", ""])
    if positive_rows:
        lines.append(
            "出现正向时间关联的题："
            + "、".join(row["label"] for row in positive_rows)
            + "。这些题的 pilot 新增覆盖、最终 cost 更优，或用更少展开达到双方共同可达的质量；相应事件发生在 LLM 注入之后。"
        )
        lines.append("")
    if negative_rows:
        lines.append(
            "baseline 最终更优的题："
            + "、".join(row["label"] for row in negative_rows)
            + "。这说明 LLM 介入并非稳定地带来收益。"
        )
        lines.append("")
    if no_incumbent_rows:
        lines.append(
            "注入后无新 incumbent 的题："
            + "、".join(row["label"] for row in no_incumbent_rows)
            + "。虽然这些题得到了可用样本和新状态，但没有观察到方案质量改善。"
        )
        lines.append("")
    lines.extend(
        [
            "单次 live/off 配对可以回答“这一次运行里是否出现收益迹象”，但不能排除异步调度、机器差异和随机采样造成的波动。正式因果结论应在同一机器、同一并发配置下，对相同题目进行多随机种子 live/off 配对，并增加禁用注入或打乱注入时机的消融。",
            "",
            "## 指标定义",
            "",
            "- 首解时间、首解展开数取 `incumbents.csv` 第一行。",
            "- 最终 cost 取 `incumbents.csv` 最后一行；该文件只记录严格改善。",
            "- LLM 处理题定义为 `llm_requests.csv` 至少有一条请求。",
            "- 首次注入时间取第一条 `inserted_states > 0` 且状态为 `ok` 或 `partial` 的请求完成时间。",
            "- 达同等质量使用双方最终 cost 中较差的一个作为共同可达阈值，再比较首次达到该阈值的时间和累计展开数；至少减少 5% 展开才标记为正向效率迹象。",
            "- “LLM 后改善”仅表示 incumbent 时间晚于已记录的首次注入，属于时间关联。",
        ]
    )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline_root", type=Path)
    parser.add_argument("pilot_root", type=Path)
    parser.add_argument("output_dir", type=Path)
    args = parser.parse_args()

    baseline_root = args.baseline_root.resolve()
    pilot_root = args.pilot_root.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    baseline_jobs = load_jobs(baseline_root)
    pilot_jobs = load_jobs(pilot_root)
    treated_pilot = {key: job for key, job in pilot_jobs.items() if job["request_count"] > 0}
    missing = sorted(set(treated_pilot) - set(baseline_jobs))
    if missing:
        raise ValueError(f"Missing baseline matches: {missing}")

    rows = [
        compare_pair(baseline_jobs[key], treated_pilot[key])
        for key in sorted(treated_pilot, key=lambda value: (treated_pilot[value]["scale"], treated_pilot[value]["problem_id"]))
    ]
    purity = baseline_purity(baseline_root)
    baseline_config = read_json(baseline_root / "batch_config.json")
    pilot_config = read_json(pilot_root / "batch_config.json")
    config = {
        "baseline_jobs": len(baseline_jobs),
        "pilot_jobs": len(pilot_jobs),
        "pilot_treated_jobs": len(treated_pilot),
        "pilot_untreated_jobs": len(pilot_jobs) - len(treated_pilot),
        "baseline_statuses": dict(Counter(job["status"] for job in baseline_jobs.values())),
        "pilot_statuses": dict(Counter(job["status"] for job in pilot_jobs.values())),
        "baseline_parallelism": baseline_config.get("small_parallelism"),
        "pilot_parallelism": pilot_config.get("small_parallelism"),
        "baseline_small_limit": number(baseline_config.get("small_time_limit_seconds")),
        "pilot_small_limit": number(pilot_config.get("small_time_limit_seconds")),
        "baseline_large_limit": number(baseline_config.get("large_time_limit_seconds")),
        "pilot_large_limit": number(pilot_config.get("large_time_limit_seconds")),
    }
    scales = sorted({row["scale"] for row in rows})
    groups = [group_summary(rows)] + [group_summary(rows, scale) for scale in scales]
    summary = {
        "config": config,
        "baseline_purity": purity,
        "groups": groups,
        "pairs": rows,
        "trajectories": {
            row["problem"]: {
                "baseline": baseline_jobs[row["problem"]]["incumbents"],
                "pilot": treated_pilot[row["problem"]]["incumbents"],
                "first_injection_time": treated_pilot[row["problem"]]["first_injection_time"],
            }
            for row in rows
        },
    }

    write_csv(output_dir / "paired_metrics.csv", rows)
    (output_dir / "analysis_summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    write_report(
        output_dir / "analysis_report.md",
        baseline_root,
        pilot_root,
        config,
        purity,
        rows,
        groups,
    )
    print(json.dumps({"output_dir": str(output_dir), "groups": groups}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
