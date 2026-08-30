#!/usr/bin/env python3
"""Validate and unpack one real Count intermediate-state prompt record."""

import argparse
import json
import pathlib
import shutil
import sys

from hybrid_planner.prompting.builder import replace_problem_init
from hybrid_planner.validation.response_processor import (
    UnifiedPlanningPrefixValidator,
)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def write_text(path, text):
    pathlib.Path(path).write_text(str(text), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Check that a captured non-initial Count state forms a complete "
            "PDDL problem and unpack the exact prompts for human review."
        )
    )
    parser.add_argument("--record", required=True)
    parser.add_argument("--domain", required=True)
    parser.add_argument("--original-problem", required=True)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()

    record_path = pathlib.Path(args.record).resolve()
    domain_path = pathlib.Path(args.domain).resolve()
    original_problem_path = pathlib.Path(args.original_problem).resolve()
    output_dir = pathlib.Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    record = json.loads(record_path.read_text(encoding="utf-8"))
    original_problem = original_problem_path.read_text(encoding="utf-8")
    init_text = str(record.get("init", ""))
    runtime_problem = str(record.get("runtime_problem", ""))
    system_prompt = str(record.get("system", ""))
    user_prompt = str(record.get("user", ""))
    problem_description = str(record.get("problem_description", ""))

    require(init_text.strip(), "captured init is empty")
    require(runtime_problem.strip(), "runtime_problem was not saved")
    require(system_prompt.strip(), "system prompt is empty")
    require(user_prompt.strip(), "user prompt is empty")
    require(problem_description.strip(), "problem description is empty")
    require(record.get("reason") != "initial", "captured request is the initial state")
    require(float(record.get("g", 0)) > 0, "captured state does not have g > 0")

    rebuilt_problem = replace_problem_init(original_problem, init_text)
    require(
        rebuilt_problem == runtime_problem,
        "saved runtime PDDL differs from the existing init-overlay implementation",
    )
    require(
        runtime_problem != original_problem,
        "runtime PDDL is unchanged from the original problem",
    )
    require(
        problem_description in user_prompt,
        "the exact translated problem description is absent from the user prompt",
    )

    # This combines the saved runtime problem with the real domain, parses it
    # through Unified Planning and instantiates a sequential simulator state.
    validation = UnifiedPlanningPrefixValidator(domain_path).validate(
        runtime_problem,
        (),
    )

    shutil.copy2(domain_path, output_dir / "domain.pddl")
    shutil.copy2(original_problem_path, output_dir / "original_problem.pddl")
    shutil.copy2(record_path, output_dir / "request_record.json")
    write_text(output_dir / "intermediate_init.pddl", init_text)
    write_text(output_dir / "intermediate_problem.pddl", runtime_problem)
    write_text(output_dir / "system_prompt.txt", system_prompt)
    write_text(output_dir / "user_prompt.txt", user_prompt)
    write_text(output_dir / "problem_description.txt", problem_description)

    report = {
        "source_record": str(record_path),
        "problem_id": record.get("problem_id"),
        "request_id": record.get("request_id"),
        "state_id": record.get("state_id"),
        "state_label": record.get("state_label"),
        "reason": record.get("reason"),
        "g": record.get("g"),
        "h": record.get("h"),
        "checks": {
            "non_initial_request": True,
            "g_is_positive": True,
            "runtime_problem_matches_existing_overlay_logic": True,
            "runtime_problem_differs_from_original": True,
            "unified_planning_parse_and_simulator": True,
            "problem_description_embedded_in_user_prompt": True,
        },
        "runtime_state_is_goal": validation.goal_reached,
    }
    write_text(
        output_dir / "validation_report.json",
        json.dumps(report, ensure_ascii=False, indent=2) + "\n",
    )

    print("Validated captured intermediate state: %s" % record.get("request_id"))
    print("Review artifacts: %s" % output_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
