#!/usr/bin/env bash

set -euo pipefail

# Keep this entry point minimal: all experiment policy lives in batch_console.

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/.." && pwd)"
cd "$project_root"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This runner is intended for a Linux container." >&2
    exit 1
fi

for command in python3; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Missing required command: $command" >&2
        exit 1
    fi
done

for path in \
    "$project_root/fast-downward.py" \
    "$project_root/builds/release64/bin/downward"; do
    if [[ ! -f "$path" ]]; then
        echo "Missing required file: $path" >&2
        exit 1
    fi
done

exec python3 -m hybrid_planner.batch_console "$@"
