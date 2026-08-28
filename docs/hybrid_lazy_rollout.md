# LLM-guided lazy rollout

Count-Downward can run its native lazy search together with asynchronous action
chains proposed by an LLM. The planner preserves the normal lazy-search
frontier: an accepted chain is processed one edge at a time, and every new or
reopened intermediate state also generates its ordinary successors. While a
chain is active, it temporarily owns the next search step; after it finishes or
is rejected, the existing open lists resume unchanged.

The Python control plane is `hybrid_planner/console.py`. Its default schedule
uses Count-Downward's `irhff(cost_type=one)` heuristic in an anytime sequence of
lazy greedy and weighted A* phases. Use `--llm-mode off` for the same schedule
with LLM requests disabled, `--llm-mode replay` for deterministic recorded
responses, or `--llm-mode live` for an HTTP model backend. Run `python -m
hybrid_planner.console --help` for the complete command-line interface.

The C++ bridge reads `HYBRID_LLM_*` environment variables and retains the
legacy `NLM_LLM_*` names as fallbacks. Important safety limits include the
number of proposals per response, actions per proposal, actions per burst, and
queued bursts. The Python side applies the same response-size bounds before a
response reaches the search process.

Each request is scoped to one anytime phase. Phase changes cancel or discard
late work, and all phases share one absolute wall-clock deadline. Structured
`HYBRID-LLM-TRIGGER-STATS` and `HYBRID-LLM-ROLLOUT-STATS` records are written to
the normal run artifacts by `hybrid_planner.anytime`. The current request
budget is 10 per phase. A shared 500000-base-expansion request gap, calibrated
from the initial Depots shadow runs to roughly two minutes, applies regardless
of which trigger rule wins arbitration.

## Lazy expansion-stream plateau detection

Lazy edge entries carry the evaluation context of their predecessor, so their
keys are not treated as the successor state's heuristic value. Instead, the
Lazy-specific plateau detector observes only finite states that native base
search actually expands, after their real `llm_h` value has been computed.

Expansions are grouped into fixed-size windows and quantized h buckets. A
bucket becomes active when it dominates a configured share of several
consecutive windows and only a limited share of expansions escape below it.
Activation arms the bucket; the first later base expansion in the same bucket
that satisfies the shared request gap and per-layer cooldown becomes the LLM
request source. The state that completes the confirmation window is never used
for that activation's request. Rollout expansions are excluded from these
windows.

The main settings are `ENABLE_EXPANSION_PLATEAU`,
`PLATEAU_WINDOW_EXPANSIONS`, `PLATEAU_CONFIRM_WINDOWS`,
`PLATEAU_RESET_WINDOWS`, `PLATEAU_MIN_BUCKET_EXPANSIONS`,
`PLATEAU_MIN_SHARE`, `PLATEAU_MAX_LOWER_SHARE`,
`PLATEAU_H_BUCKET_WIDTH`, and
`PLATEAU_PER_LAYER_REQUEST_GAP_EXPANSIONS`. Structured
`HYBRID-LLM-PLATEAU` records expose each window and lifecycle transition for
shadow-run calibration.

The current calibrated defaults use 65536 base expansions per window, three
consecutive qualifying windows, a minimum bucket share of 0.3, and at least
16384 observations from that bucket. Two consecutive misses deactivate a
layer. In expansion units, this increases confirmation evidence from 24576 to
196608 and deactivation evidence from 16384 to 131072, bringing detector
timescales closer to the shared 500000-expansion LLM request interval. The
detector retains at most 16 h buckets; inactive least-recently-seen buckets are
evicted first, and eviction counts remain visible in trigger statistics.

Global h stall remains configured at 500000 base expansions. Ancestor
stagnation is sampled every 100000 base expansions, requires a parent chain at
least 30 states deep, and compares the real h values of the nearest 20
ancestors. These stricter ancestor defaults reduce the dominance observed in
the first shadow experiment without changing the global-stall rule.

Run the Python tests with:

```text
python -m unittest discover -s tests -v
```

On Linux or WSL, `tests/test_lazy_rollout_integration.py` additionally launches
the compiled search binary against a one-shot mock LLM server. It verifies that
a multi-action chain is requested, processed, interleaved with native successor
generation, and still produces a valid plan.
