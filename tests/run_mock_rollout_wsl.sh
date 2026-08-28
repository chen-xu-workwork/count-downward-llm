#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
port="${HYBRID_TEST_PORT:-18765}"

cd "$project_root"
python3 builds/release64/bin/translate/translate.py \
    src/translate/regression-tests/issue34-domain.pddl \
    src/translate/regression-tests/issue34-problem.pddl \
    >/dev/null
builds/release64/bin/preprocess < output.sas >/dev/null

python3 tests/mock_llm_server.py \
    --port "$port" \
    --require-init-substring '(connect a1 a2 s12)' \
    --require-init-substring '(on b11 a2)' \
    --action '(pop-start s12 b11 a1 a2 b0 gasoleo lco)' \
    --action '(pop-end s12 a1 a2 b5 b0)' \
    --action '(pop-start s12 b2 a1 a2 b11 rat-a gasoleo)' \
    --action '(pop-end s12 a1 a2 b0 b11)' \
    --action '(push-start s12 b6 a1 a2 b11 rat-a gasoleo)' \
    --action '(push-end s12 a1 a2 b2 b11)' \
    --action '(push-start s12 b0 a1 a2 b6 lco rat-a)' \
    --action '(push-end s12 a1 a2 b11 b6)' \
    --action '(push-start s12 b1 a1 a2 b0 gasoleo lco)' \
    --action '(push-end s12 a1 a2 b6 b0)' &
server_pid=$!
cleanup() {
    if kill -0 "$server_pid" 2>/dev/null; then
        kill "$server_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT
sleep 0.2

env \
    NLM_LLM_TRIGGER=1 \
    NLM_LLM_COMM_MODE=http \
    NLM_LLM_HTTP_HOST=127.0.0.1 \
    NLM_LLM_HTTP_PORT="$port" \
    NLM_LLM_HTTP_PATH=/llm/request \
    NLM_LLM_HTTP_TIMEOUT_MS=5000 \
    NLM_LLM_HTTP_WORKERS=1 \
    NLM_LLM_MAX_PENDING=1 \
    NLM_LLM_MAX_REQUESTS=1 \
    NLM_LLM_REQUEST_INITIAL=1 \
    NLM_LLM_ENABLE_ANCESTOR_STAGNATION=0 \
    NLM_LLM_ENABLE_GLOBAL_STALL=0 \
    NLM_LLM_RUN_ID=lazy-integration \
    builds/release64/bin/downward \
        --heuristic 'hff=irhff(cost_type=one)' \
        --search 'lazy_greedy(hff,preferred=hff,cost_type=one,reopen_closed=false,llm_h=hff,max_time=30)' \
        --internal-plan-file tests/mock_rollout.plan \
        < output

wait "$server_pid"
trap - EXIT
