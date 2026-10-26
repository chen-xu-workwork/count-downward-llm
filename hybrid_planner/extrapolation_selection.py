"""Freeze a paired 45/50 Depots sample and batch manifests without editing PDDL.

The first invocation samples each scale uniformly without replacement. Later
invocations validate the original inputs and reuse ``selection.json``. Changing
the dataset, its contents, or sampling settings requires a new output directory.
"""

import argparse
import hashlib
import json
import os
import pathlib
import random
import re
import tempfile


SCALES = (45, 50)
SCHEMA_VERSION = 1
TIME_LIMIT_SECONDS = 3600
LIVE_REQUEST_BUDGET = 15
PROBLEM_PATTERN = re.compile(r"^problem_scale_(45|50)_id_([0-9]+)\.pddl$")


def _sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _atomic_json(path, payload):
    """Commit a complete JSON file, leaving no truncated destination on failure."""

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


def _scan_pool(problem_dir, sample_per_scale):
    pools = {str(scale): [] for scale in SCALES}
    seen_ids = {scale: set() for scale in SCALES}
    for path in problem_dir.iterdir():
        match = PROBLEM_PATTERN.fullmatch(path.name)
        if match is None or not path.is_file():
            continue
        scale, numeric_id = (int(value) for value in match.groups())
        if numeric_id in seen_ids[scale]:
            raise ValueError(
                "Duplicate numeric problem ID %d at scale %d" % (numeric_id, scale)
            )
        seen_ids[scale].add(numeric_id)
        pools[str(scale)].append(
            {"id": numeric_id, "name": path.name, "sha256": _sha256(path)}
        )
    result = {}
    for scale in SCALES:
        files = sorted(pools[str(scale)], key=lambda item: (item["id"], item["name"]))
        if len(files) < sample_per_scale:
            raise ValueError(
                "Scale %d has %d matching problems, fewer than requested %d"
                % (scale, len(files), sample_per_scale)
            )
        result[str(scale)] = {"count": len(files), "files": files}
    return result


def _validate_selected(selected, pool, sample_per_scale):
    if not isinstance(selected, list):
        raise _input_error("Existing selection has no valid selected list")
    expected_count = len(SCALES) * sample_per_scale
    if len(selected) != expected_count:
        raise _input_error("Existing selection has an unexpected selected count")
    validated = []
    for scale in SCALES:
        entries = [item for item in selected if isinstance(item, dict) and item.get("scale") == scale]
        if len(entries) != sample_per_scale:
            raise _input_error("Existing selection count changed for scale %d" % scale)
        available = {item["name"]: item for item in pool[str(scale)]["files"]}
        names = set()
        for item in entries:
            name = item.get("name")
            source = available.get(name)
            if source is None or item != dict(source, scale=scale) or name in names:
                raise _input_error("Existing selection contains an invalid or changed problem")
            names.add(name)
        validated.extend(sorted(entries, key=lambda item: (item["id"], item["name"])))
    if validated != selected:
        raise _input_error("Existing selection problem order changed")


def _manifest(selection, mode):
    dataset_root = pathlib.Path(selection["dataset_root"])
    return {
        "domain": str(dataset_root / "domain.pddl"),
        "problem_dir": str(dataset_root / "problems"),
        "default_mode": mode,
        "jobs": [
            {
                "id": "scale%d-id%d-%s" % (item["scale"], item["id"], mode),
                "problem": item["name"],
                "scale": item["scale"],
                "mode": mode,
                "time_limit_seconds": TIME_LIMIT_SECONDS,
                "max_requests_per_iteration": LIVE_REQUEST_BUDGET if mode == "live" else 0,
            }
            for item in selection["selected"]
        ],
    }


def prepare_selection(dataset_root, output_dir, sample_per_scale=20, sample_seed=20261025):
    """Freeze/reuse a paired sample and return its JSON-serializable metadata.

    Raises ``ValueError`` if an existing selection or manifest conflicts with
    the requested inputs. Missing manifests may be rebuilt from the frozen
    selection; existing manifests are validated and never silently overwritten.
    No source problem or domain file is modified.
    """

    if isinstance(sample_per_scale, bool) or not isinstance(sample_per_scale, int) or sample_per_scale < 1:
        raise ValueError("sample_per_scale must be a positive integer")
    if isinstance(sample_seed, bool) or not isinstance(sample_seed, int):
        raise ValueError("sample_seed must be an integer")
    dataset_root = pathlib.Path(dataset_root).expanduser().resolve()
    output_dir = pathlib.Path(output_dir).expanduser().resolve()
    domain = dataset_root / "domain.pddl"
    problem_dir = dataset_root / "problems"
    if not domain.is_file():
        raise ValueError("Dataset domain file does not exist: %s" % domain)
    if not problem_dir.is_dir():
        raise ValueError("Dataset problems directory does not exist: %s" % problem_dir)
    # Keep all outputs outside the input tree, including when invoked incorrectly.
    if output_dir == dataset_root or dataset_root in output_dir.parents:
        raise ValueError("output_dir must be outside dataset_root to preserve source data")

    manifest_paths = {
        "off": str(output_dir / "baseline_manifest.json"),
        "live": str(output_dir / "tracs_manifest.json"),
    }
    inputs = {
        "schema_version": SCHEMA_VERSION,
        "scales": list(SCALES),
        "sample_seed": sample_seed,
        "sample_per_scale": sample_per_scale,
        "dataset_root": str(dataset_root),
        "domain_sha256": _sha256(domain),
        "pool": _scan_pool(problem_dir, sample_per_scale),
        "manifest_paths": manifest_paths,
    }
    selection_path = output_dir / "selection.json"
    if selection_path.exists():
        selection = _read_json(selection_path)
        if not isinstance(selection, dict):
            raise _input_error("Existing selection is not a JSON object")
        for key, value in inputs.items():
            if selection.get(key) != value:
                raise _input_error("Existing selection does not match current %s" % key)
        _validate_selected(selection.get("selected"), inputs["pool"], sample_per_scale)
    else:
        generator = random.Random(sample_seed)
        selected = []
        for scale in SCALES:
            sample = generator.sample(inputs["pool"][str(scale)]["files"], sample_per_scale)
            selected.extend(
                dict(item, scale=scale)
                for item in sorted(sample, key=lambda item: (item["id"], item["name"]))
            )
        selection = dict(inputs, selected=selected)

    manifests = {mode: _manifest(selection, mode) for mode in ("off", "live")}
    # Validate both before writing anything, so an inconsistent mode cannot
    # overwrite its counterpart or create a new selection on top of old jobs.
    for mode, expected in manifests.items():
        path = pathlib.Path(manifest_paths[mode])
        if path.exists() and _read_json(path) != expected:
            raise _input_error("Existing %s manifest differs from the frozen selection" % mode)

    output_dir.mkdir(parents=True, exist_ok=True)
    if not selection_path.exists():
        _atomic_json(selection_path, selection)
    for mode, payload in manifests.items():
        path = pathlib.Path(manifest_paths[mode])
        if not path.exists():
            _atomic_json(path, payload)
    return selection


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset-root", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--sample-per-scale", type=int, default=20)
    parser.add_argument("--sample-seed", type=int, default=20261025)
    args = parser.parse_args(argv)
    try:
        selection = prepare_selection(
            args.dataset_root, args.output_dir, args.sample_per_scale, args.sample_seed
        )
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print(json.dumps(selection, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
