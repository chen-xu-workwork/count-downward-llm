#ifndef LLM_LLM_TRIGGER_MONITOR_H
#define LLM_LLM_TRIGGER_MONITOR_H

#include "llm_bridge.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace llm {

class TriggerMonitor {
    struct Config;
    struct PendingInfo {
        std::chrono::steady_clock::time_point submitted_at;
        int expansions_at_submit;
        std::string reason;

        PendingInfo();
        PendingInfo(int expansions, const std::string &reason_);
    };
    struct PlateauState;

    Config *config;
    PlateauState *plateau;
    Bridge bridge;
    std::string run_id;
    int anytime_iteration;
    int next_request_id;
    int base_expansions;
    int expansions_since_best_h;
    int last_request_expansion;
    ap_float best_h;
    bool have_best_h;
    bool global_stall_requested;
    bool started;
    bool statistics_printed;
    std::chrono::steady_clock::time_point phase_started_at;

    std::unordered_set<StateID> requested_states;
    std::unordered_map<std::string, PendingInfo> pending_requests;

    std::size_t request_attempts;
    std::size_t requests_submitted;
    std::size_t requests_rejected_duplicate;
    std::size_t requests_rejected_pending_limit;
    std::size_t requests_rejected_request_limit;
    std::size_t requests_rejected_spacing;
    std::size_t requests_rejected_burst;
    std::size_t requests_rejected_bridge;
    std::size_t responses_completed;
    std::size_t response_transport_failures;
    std::size_t stale_responses;
    std::size_t usable_responses;
    std::size_t max_pending_observed;
    double total_response_seconds;
    double max_response_seconds;

    bool request_state(
        StateID state_id, const std::string &reason,
        ap_float g, ap_float h, bool bypass_spacing);
    bool request_slot_available(bool bypass_spacing) const;
    bool meaningfully_improves(ap_float current_h) const;
    bool maybe_request_expansion_plateau(
        StateID state_id, ap_float g, ap_float h,
        bool global_stall_eligible, bool &candidate_present);
    void record_expansion_plateau_observation(
        StateID state_id, ap_float g, ap_float h);

public:
    TriggerMonitor();
    ~TriggerMonitor();

    TriggerMonitor(const TriggerMonitor &) = delete;
    TriggerMonitor &operator=(const TriggerMonitor &) = delete;

    bool enabled() const;
    void start();
    void set_anytime_iteration(int iteration);

    void maybe_request_initial(StateID state_id, ap_float g, ap_float h);
    void record_base_expansion(
        StateID state_id, ap_float g, ap_float h,
        bool ancestor_stagnant, bool burst_active);
    void record_rollout_expansion(ap_float h);

    std::vector<Response> poll_completed();
    bool has_pending() const;
    void record_usable_response();
    void record_stale_response();
    void finalize_and_print();
};

}

#endif
