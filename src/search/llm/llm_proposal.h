#ifndef LLM_LLM_PROPOSAL_H
#define LLM_LLM_PROPOSAL_H

#include "../state_id.h"

#include <chrono>
#include <cstddef>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

namespace llm {

enum class ExpansionKind {
    EXPANDED_NEW,
    REOPENED,
    DUPLICATE_EXISTING,
    INAPPLICABLE,
    DEAD_END,
    BOUND_PRUNED,
    INVALID_PREDECESSOR,
    CYCLE,
    STALE,
    GOAL
};

struct ExpansionOutcome {
    ExpansionKind kind;
    StateID actual_successor;

    ExpansionOutcome(
        ExpansionKind kind_, StateID successor_ = StateID::no_state)
        : kind(kind_), actual_successor(successor_) {
    }
};

struct Proposal {
    std::string proposal_id;
    std::string request_id;
    std::string run_id;
    int sample_index;
    int iteration;
    StateID source_state;
    StateID anchor_state;
    std::vector<std::string> actions;
    std::size_t cursor;
    std::unordered_set<StateID> visited_states;

    Proposal();
};

struct Burst {
    std::string burst_id;
    std::string request_id;
    std::string run_id;
    int iteration;
    StateID source_state;
    std::deque<Proposal> proposals;
    std::size_t total_actions;
    std::chrono::steady_clock::time_point accepted_at;
    std::chrono::steady_clock::time_point started_at;
    bool started;
    bool had_abort;

    Burst();
};

struct RolloutLimits {
    int max_proposals_per_response;
    int max_actions_per_proposal;
    int max_actions_per_burst;
    int max_queued_bursts;

    RolloutLimits();
};

class RolloutStatistics {
public:
    std::size_t bursts_started;
    std::size_t bursts_completed;
    std::size_t bursts_aborted;
    std::size_t actions_requested;
    std::size_t actions_prevalidated;
    std::size_t actions_processed;
    std::size_t states_new;
    std::size_t states_reopened;
    std::size_t states_duplicate;
    std::size_t dead_end_aborts;
    std::size_t bound_aborts;
    std::size_t cycle_aborts;
    std::size_t inapplicable_aborts;
    std::size_t invalid_predecessor_aborts;
    std::size_t stale_proposals;
    std::size_t normal_edges_generated;
    std::size_t responses_rejected_budget;
    std::size_t proposals_completed;
    std::size_t proposals_aborted;
    double burst_wall_seconds;

    RolloutStatistics();
    void print(int iteration) const;
};

}

#endif
