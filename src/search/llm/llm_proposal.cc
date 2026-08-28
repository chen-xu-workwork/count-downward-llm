#include "llm_proposal.h"

#include "llm_protocol.h"

#include <algorithm>
#include <iostream>

using namespace std;

namespace llm {

Proposal::Proposal()
    : sample_index(0),
      iteration(1),
      source_state(StateID::no_state),
      anchor_state(StateID::no_state),
      cursor(0) {
}

Burst::Burst()
    : iteration(1),
      source_state(StateID::no_state),
      total_actions(0),
      accepted_at(chrono::steady_clock::now()),
      started_at(accepted_at),
      started(false),
      had_abort(false) {
}

RolloutLimits::RolloutLimits()
    : max_proposals_per_response(max(
          1, setting_int("MAX_PROPOSALS_PER_RESPONSE", 8))),
      max_actions_per_proposal(max(
          1, setting_int("MAX_ACTIONS_PER_PROPOSAL", 100))),
      max_actions_per_burst(max(
          1, setting_int("MAX_BURST_ACTIONS", 100))),
      max_queued_bursts(max(
          1, setting_int("MAX_QUEUED_BURSTS", 8))) {
}

RolloutStatistics::RolloutStatistics()
    : bursts_started(0),
      bursts_completed(0),
      bursts_aborted(0),
      actions_requested(0),
      actions_prevalidated(0),
      actions_processed(0),
      states_new(0),
      states_reopened(0),
      states_duplicate(0),
      dead_end_aborts(0),
      bound_aborts(0),
      cycle_aborts(0),
      inapplicable_aborts(0),
      invalid_predecessor_aborts(0),
      stale_proposals(0),
      normal_edges_generated(0),
      responses_rejected_budget(0),
      proposals_completed(0),
      proposals_aborted(0),
      burst_wall_seconds(0.0) {
}

void RolloutStatistics::print(int iteration) const {
    cout << "[HYBRID-LLM-ROLLOUT-STATS]"
         << " iteration=" << iteration
         << " llm_bursts_started=" << bursts_started
         << " llm_bursts_completed=" << bursts_completed
         << " llm_bursts_aborted=" << bursts_aborted
         << " llm_actions_requested=" << actions_requested
         << " llm_actions_prevalidated=" << actions_prevalidated
         << " llm_actions_processed=" << actions_processed
         << " llm_states_new=" << states_new
         << " llm_states_reopened=" << states_reopened
         << " llm_states_duplicate=" << states_duplicate
         << " llm_dead_end_aborts=" << dead_end_aborts
         << " llm_bound_aborts=" << bound_aborts
         << " llm_cycle_aborts=" << cycle_aborts
         << " llm_inapplicable_aborts=" << inapplicable_aborts
         << " llm_invalid_predecessor_aborts="
         << invalid_predecessor_aborts
         << " llm_stale_proposals=" << stale_proposals
         << " llm_normal_edges_generated=" << normal_edges_generated
         << " llm_responses_rejected_budget="
         << responses_rejected_budget
         << " llm_proposals_completed=" << proposals_completed
         << " llm_proposals_aborted=" << proposals_aborted
         << " llm_burst_wall_seconds=" << burst_wall_seconds
         << endl;
}

}
