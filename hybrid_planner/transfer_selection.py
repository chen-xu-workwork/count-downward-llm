"""Freeze a live-only Zenotravel 30/40 sample for a Depots-model transfer run.

Sampling uses only problem identifiers and a fixed random seed, never solver
outcomes. An optional baseline-availability list replaces only initially drawn
problems lacking baseline records. Original PDDL files remain in place. Reentry
validates the complete 30/40 pool and reuses the same selection and manifest.
"""

import argparse
import hashlib
import json
import os
import pathlib
import random
import re
import tempfile


SCALES = (30, 40)
TARGET_DOMAIN = "zeno-travel"
SCHEMA_VERSION = 1
PROBLEM_PATTERN = re.compile(r"^problem_scale_(30|40)_id_([0-9]+)\.pddl$")
DOMAIN_PATTERN = re.compile(r"\(\s*define\s*\(\s*domain\s+([^\s()]+)\s*\)", re.I)
PROBLEM_DOMAIN_PATTERN = re.compile(r"\(\s*:domain\s+([^\s()]+)\s*\)", re.I)
TIME_LIMITS = {30: 1800, 40: 3600}
REQUEST_BUDGETS = {30: 10, 40: 15}


def _sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _atomic_json(path, payload):
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=path.name + ".", suffix=".tmp", dir=str(path.parent)
    )
    temporary_path = pathlib.Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(payload, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_path, path)
    finally:
        if temporary_path.exists():
            temporary_path.unlink()


def _read_json(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise ValueError("Cannot read %s: %s" % (path, exc)) from exc


def _input_error(message):
    return ValueError(message + "; use a new output directory for changed inputs")


def _pddl_without_comments(path):
    return re.sub(r";[^\r\n]*", "", path.read_text(encoding="utf-8-sig"))


def read_domain_name(path):
    """Read the declared PDDL domain name, normalized for case."""
    path = pathlib.Path(path)
    match = DOMAIN_PATTERN.search(_pddl_without_comments(path))
    if match is None:
        raise ValueError("Cannot read a PDDL domain declaration from %s" % path)
    return match.group(1).lower()


def _scan_pool(problem_dir, counts):
    pools = {str(scale): [] for scale in SCALES}
    seen_ids = {scale: set() for scale in SCALES}
    for path in problem_dir.iterdir():
        match = PROBLEM_PATTERN.fullmatch(path.name)
        if match is None or not path.is_file():
            continue
        scale, numeric_id = (int(value) for value in match.groups())
        if numeric_id in seen_ids[scale]:
            raise ValueError("Duplicate numeric problem ID %d at scale %d" % (numeric_id, scale))
        seen_ids[scale].add(numeric_id)
        declared = PROBLEM_DOMAIN_PATTERN.search(_pddl_without_comments(path))
        if declared is None or declared.group(1).lower() != TARGET_DOMAIN:
            raise ValueError("Problem %s must declare domain %s" % (path.name, TARGET_DOMAIN))
        pools[str(scale)].append({"id": numeric_id, "name": path.name, "sha256": _sha256(path)})
    result = {}
    for scale in SCALES:
        files = sorted(pools[str(scale)], key=lambda item: (item["id"], item["name"]))
        if len(files) < counts[scale]:
            raise ValueError(
                "Scale %d has %d matching problems, fewer than requested %d"
                % (scale, len(files), counts[scale])
            )
        result[str(scale)] = {"count": len(files), "files": files}
    return result


def _manifest(selection):
    dataset_root = pathlib.Path(selection["dataset_root"])
    return {
        "domain": str(dataset_root / "domain.pddl"),
        "problem_dir": str(dataset_root / "problems"),
        "default_mode": "live",
        "jobs": [
            {
                "id": "scale%d-id%d-live" % (item["scale"], item["id"]),
                "problem": item["name"],
                "scale": item["scale"],
                "mode": "live",
                "time_limit_seconds": TIME_LIMITS[item["scale"]],
                "max_requests_per_iteration": REQUEST_BUDGETS[item["scale"]],
            }
            for item in selection["selected"]
        ],
    }


def _baseline_availability(value):
    if not isinstance(value, dict):
        raise ValueError("baseline_availability must be a JSON object")
    if isinstance(value.get("schema_version"), bool) or value.get("schema_version") != 1:
        raise ValueError("baseline_availability requires schema_version=1")
    if value.get("target_domain") != TARGET_DOMAIN:
        raise ValueError("baseline_availability target_domain must be %s" % TARGET_DOMAIN)
    if not isinstance(value.get("baseline_archive"), str) or not value["baseline_archive"].strip():
        raise ValueError("baseline_availability requires a baseline_archive name")
    names = value.get("eligible_problem_names")
    if not isinstance(names, list) or any(not isinstance(name, str) or not name for name in names):
        raise ValueError("baseline_availability eligible_problem_names must be a list of problem names")
    if len(names) != len(set(names)):
        raise ValueError("baseline_availability contains duplicate eligible_problem_names")
    try:
        # Copy JSON values so subsequent caller mutations cannot alter a plan.
        metadata = json.loads(json.dumps(value, ensure_ascii=False, allow_nan=False))
    except (TypeError, ValueError) as exc:
        raise ValueError("baseline_availability must be JSON serializable") from exc
    return metadata, set(names)


def prepare_selection(dataset_root, output_dir, sample_30=20, sample_40=10, sample_seed=20261027,
                      *, baseline_availability=None):
    """Create or validate the frozen sample and its single live manifest.

    All input and existing-output checks precede writes. Changing a selected or
    unselected source problem, domain, seed, count, path, baseline-availability
    metadata or saved manifest requires a new output directory. Missing manifests
    can be rebuilt exactly. Availability is based on complete baseline records,
    irrespective of whether those runs found a plan; solver outcomes are unused.
    Callers that execute experiments should hold their experiment-directory
    process lock while preparing the selection.
    """
    counts = {30: sample_30, 40: sample_40}
    for scale, count in counts.items():
        if isinstance(count, bool) or not isinstance(count, int) or count < 1:
            raise ValueError("sample_%d must be a positive integer" % scale)
    if isinstance(sample_seed, bool) or not isinstance(sample_seed, int):
        raise ValueError("sample_seed must be an integer")
    availability, eligible_names = (None, None)
    if baseline_availability is not None:
        availability, eligible_names = _baseline_availability(baseline_availability)
    dataset_root = pathlib.Path(dataset_root).expanduser().resolve()
    output_dir = pathlib.Path(output_dir).expanduser().resolve()
    domain = dataset_root / "domain.pddl"
    problem_dir = dataset_root / "problems"
    if not domain.is_file():
        raise ValueError("Dataset domain file does not exist: %s" % domain)
    if not problem_dir.is_dir():
        raise ValueError("Dataset problems directory does not exist: %s" % problem_dir)
    if output_dir == dataset_root or dataset_root in output_dir.parents:
        raise ValueError("output_dir must be outside dataset_root to preserve source data")
    domain_name = read_domain_name(domain)
    if domain_name != TARGET_DOMAIN:
        raise ValueError("Expected target domain %s, found %s" % (TARGET_DOMAIN, domain_name))

    manifest_path = output_dir / "transfer_manifest.json"
    inputs = {
        "schema_version": SCHEMA_VERSION,
        "scales": list(SCALES),
        "target_domain": domain_name,
        "sample_seed": sample_seed,
        "sample_30": sample_30,
        "sample_40": sample_40,
        "dataset_root": str(dataset_root),
        "domain_sha256": _sha256(domain),
        "pool": _scan_pool(problem_dir, counts),
        "manifest_path": str(manifest_path),
    }
    generator = random.Random(sample_seed)
    selected = []
    replacements = []
    eligible_counts = {}
    for scale in SCALES:
        pool = inputs["pool"][str(scale)]["files"]
        sample = sorted(generator.sample(pool, counts[scale]), key=lambda item: (item["id"], item["name"]))
        if eligible_names is not None:
            eligible_pool = [item for item in pool if item["name"] in eligible_names]
            eligible_counts[str(scale)] = len(eligible_pool)
            if len(eligible_pool) < counts[scale]:
                raise ValueError(
                    "Scale %d has %d problems with baseline records, fewer than requested %d"
                    % (scale, len(eligible_pool), counts[scale])
                )
            drawn_names = {item["name"] for item in sample}
            missing = [item for item in sample if item["name"] not in eligible_names]
            candidates = [item for item in eligible_pool if item["name"] not in drawn_names]
            added = random.Random(sample_seed + scale).sample(candidates, len(missing))
            sample = [item for item in sample if item["name"] in eligible_names] + added
            replacements.extend(
                {"scale": scale, "removed": removed["name"], "added": replacement["name"],
                 "reason": "missing_baseline_record"}
                for removed, replacement in zip(missing, added)
            )
        selected.extend(
            dict(item, scale=scale)
            for item in sorted(sample, key=lambda item: (item["id"], item["name"]))
        )
    if availability is not None:
        inputs["baseline_availability"] = availability
        inputs["baseline_eligible_counts"] = eligible_counts
        inputs["replacements"] = replacements
    selection = dict(inputs, selected=selected)
    selection_path = output_dir / "selection.json"
    if selection_path.exists() and _read_json(selection_path) != selection:
        raise _input_error("Existing selection differs from current inputs or deterministic sample")
    manifest = _manifest(selection)
    if manifest_path.exists() and _read_json(manifest_path) != manifest:
        raise _input_error("Existing live manifest differs from the frozen selection")

    output_dir.mkdir(parents=True, exist_ok=True)
    if not selection_path.exists():
        _atomic_json(selection_path, selection)
    if not manifest_path.exists():
        _atomic_json(manifest_path, manifest)
    return selection


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset-root", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--sample-30", type=int, default=20)
    parser.add_argument("--sample-40", type=int, default=10)
    parser.add_argument("--sample-seed", type=int, default=20261027)
    parser.add_argument("--baseline-availability", help="JSON list of problems with complete baseline records")
    args = parser.parse_args(argv)
    try:
        availability = _read_json(pathlib.Path(args.baseline_availability)) if args.baseline_availability else None
        selection = prepare_selection(args.dataset_root, args.output_dir, args.sample_30, args.sample_40,
                                      args.sample_seed, baseline_availability=availability)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print(json.dumps(selection, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
