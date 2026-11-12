"""Full Depots 30/40 initial-state requests concurrent with native Lazy search."""

import json
import pathlib
import sys

from . import initial_only


EXPERIMENT = "depots_initial_only_async_lazy_30_40"
PROTOCOL_VERSION = "initial-only-async-v1"
SYNC_VERSION = "iteration-lifecycle-v1"
ASYNC_SYNC_VERSION = "iteration-lifecycle-async-v2"
DEFAULT_OUTPUT = "/root/autodl-tmp/count-results/depots-initial-only-async-30-40/seed_101"
PROTECTED_SERIAL_ROOTS = (
    "/root/autodl-tmp/count-results/depots-initial-only-30-40",
    "/root/autodl-tmp/count-results/depots-initial-only-rerun-11",
)


def validate_async_output(output):
    """Check before mkdir/lock; serial runs and nested experiment dirs stay intact."""
    output = pathlib.Path(output).expanduser().resolve()
    for root in PROTECTED_SERIAL_ROOTS:
        protected = pathlib.Path(root).resolve()
        if output == protected or protected in output.parents or output in protected.parents:
            raise ValueError("async output must be separate from preserved serial experiments")
    for candidate in (output, *output.parents):
        config_path = candidate / "initial_only_config.json"
        if not config_path.is_file():
            continue
        if candidate != output:
            raise ValueError("async output cannot be nested inside an existing experiment")
        config = json.loads(config_path.read_text(encoding="utf-8-sig"))
        if config.get("experiment") != EXPERIMENT or config.get("llm_initial_async") is not True:
            raise ValueError("existing output uses a different initial-only protocol; choose a new async directory")
    return output


def configure_async_experiment(config):
    from . import anytime
    if getattr(anytime, "ITERATION_SYNC_VERSION", None) != SYNC_VERSION:
        raise ValueError("async initial-only requires the iteration-lifecycle fix; sync the complete checkout")
    if getattr(anytime, "ITERATION_ASYNC_SYNC_VERSION", None) != ASYNC_SYNC_VERSION:
        raise ValueError("async initial-only requires immediate old-phase cancellation; sync the complete checkout")
    config.update(
        experiment=EXPERIMENT, llm_initial_async=True,
        initial_async_protocol=PROTOCOL_VERSION, iteration_sync_version=SYNC_VERSION,
        iteration_async_sync_version=ASYNC_SYNC_VERSION,
        preparation_policy="initial_s0_queue_drained_as_slots_free_while_native_search_continues",
        stale_response_policy="cancel_or_discard_at_phase_end_never_apply_to_next_phase",
        budget_interpretation="per_iteration_sampling_cap_actual_calls_can_be_lower_when_phase_ends",
        comparison_policy="new_full_63_job_async_experiment_preserve_all_serial_results",
    )


def build_argument_parser():
    parser = initial_only.build_argument_parser()
    parser.description = __doc__
    parser.epilog = "This entry never builds or runs tests. Compile the selected build separately before starting."
    parser.set_defaults(output_dir=DEFAULT_OUTPUT, llm_initial_async=True, skip_build=True)
    return parser


def validate_existing_binary(plan, args):
    """Read existing build files only; do not execute a compiler, planner or test."""
    initial_only.console.validate_initial_only_binary(plan.project_root, args.build)
    initial_only.console.validate_initial_only_async_binary(plan.project_root, args.build)
    build_root = plan.project_root / "builds" / args.build
    for relative in ("bin/preprocess", "bin/translate/translate.py"):
        required = build_root / relative
        if not required.is_file():
            raise ValueError(
                "missing compiled planner component: %s; compile this checkout separately "
                "with python3 build.py %s -j4 before starting the experiment" % (required, args.build)
            )


def main(argv=None):
    return initial_only.main(argv, parser=build_argument_parser())


if __name__ == "__main__":
    sys.exit(main())
