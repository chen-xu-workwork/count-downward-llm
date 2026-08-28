#!/usr/bin/env bash
# Shared helpers for the human-readable Count-Downward acceptance tests.

set -euo pipefail

acceptance_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$acceptance_dir/../.." && pwd)"
build_root="${COUNT_BUILD_ROOT:-$project_root/builds/release64}"

translator="$build_root/bin/translate/translate.py"
preprocessor="$build_root/bin/preprocess"
downward="$build_root/bin/downward"
domain="$project_root/src/translate/regression-tests/issue34-domain.pddl"
problem="$project_root/src/translate/regression-tests/issue34-problem.pddl"
mock_server="$project_root/tests/mock_llm_server.py"

blue='\033[1;34m'
green='\033[1;32m'
red='\033[1;31m'
reset='\033[0m'

section() {
    printf '\n%b== %s ==%b\n' "$blue" "$1" "$reset"
}

pass() {
    printf '%bPASS%b  %s\n' "$green" "$reset" "$1"
}

fail() {
    printf '%bFAIL%b  %s\n' "$red" "$reset" "$1" >&2
    exit 1
}

require_file() {
    [[ -f "$1" ]] || fail "缺少文件：$1"
}

require_build() {
    require_file "$translator"
    require_file "$preprocessor"
    require_file "$downward"
}

new_workspace() {
    local label="$1"
    mktemp -d "${TMPDIR:-/tmp}/count-${label}.XXXXXX"
}

prepare_issue34() {
    local work_dir="$1"
    (
        cd "$work_dir"
        python3 "$translator" "$domain" "$problem" >translator.log 2>&1
        "$preprocessor" <output.sas >output 2>preprocessor.log
    )
    [[ -s "$work_dir/output" ]] || fail "翻译或预处理没有生成搜索输入"
}

require_log() {
    local pattern="$1"
    local log_file="$2"
    local explanation="$3"
    if ! grep -Eq "$pattern" "$log_file"; then
        printf '没有找到：%s\n日志位置：%s\n' "$pattern" "$log_file" >&2
        fail "$explanation"
    fi
}

reject_log() {
    local pattern="$1"
    local log_file="$2"
    local explanation="$3"
    if grep -Eq "$pattern" "$log_file"; then
        fail "$explanation"
    fi
}

last_rollout_stats() {
    grep '\[HYBRID-LLM-ROLLOUT-STATS\]' "$1" | tail -n 1
}

last_trigger_stats() {
    grep '\[HYBRID-LLM-TRIGGER-STATS\]' "$1" | tail -n 1
}

stat_value() {
    local line="$1"
    local key="$2"
    local token
    for token in $line; do
        case "$token" in
            "$key="*) printf '%s\n' "${token#*=}"; return 0 ;;
        esac
    done
    return 1
}

assert_eq() {
    [[ "$1" == "$2" ]] || fail "$3（实际=$1，期望=$2）"
}

assert_gt() {
    (( "$1" > "$2" )) || fail "$3（实际=$1，必须大于 $2）"
}

free_port() {
    python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'
}

print_artifacts() {
    printf '完整日志和计划保存在：%s\n' "$1"
}

show_evidence() {
    local log_file="$1"
    local pattern="$2"
    echo "关键证据："
    grep -E "$pattern" "$log_file" | sed 's/^/  /'
}

require_build
