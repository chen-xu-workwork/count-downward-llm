#include "llm_trigger_monitor.h"

#include "../global_state.h"
#include "../state_registry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>

using namespace std;

namespace llm {

struct TriggerMonitor::Config {
    bool enabled;
    bool request_initial;
    bool enable_ancestor_stagnation;
    bool enable_expansion_plateau;
    bool enable_global_stall;
    int stall_expansions;
    int min_request_gap_expansions;
    int max_pending;
    int max_requests;
    ap_float h_abs_epsilon;
    ap_float h_relative_epsilon;
    int plateau_window_expansions;
    int plateau_confirm_windows;
    int plateau_reset_windows;
    int plateau_min_bucket_expansions;
    int plateau_min_since_request_expansions;
    int plateau_per_layer_request_gap_expansions;
    ap_float plateau_min_share;
    ap_float plateau_h_bucket_width;

    Config()
        : enabled(setting_enabled("TRIGGER")),
          request_initial(setting_enabled("REQUEST_INITIAL")),
          enable_ancestor_stagnation(
              setting_enabled("ENABLE_ANCESTOR_STAGNATION", true)),
          enable_expansion_plateau(
              setting_enabled("ENABLE_EXPANSION_PLATEAU", true)),
          enable_global_stall(
              setting_enabled("ENABLE_GLOBAL_STALL", true)),
          stall_expansions(max(1, setting_int("STALL_EXPANSIONS", 500000))),
          min_request_gap_expansions(max(
              0, setting_int("MIN_REQUEST_GAP_EXPANSIONS", 500000))),
          max_pending(max(0, setting_int("MAX_PENDING", 0))),
          max_requests(max(0, setting_int("MAX_REQUESTS", 10))),
          h_abs_epsilon(max(
              0.0, setting_float("H_EPSILON", 0.001))),
          h_relative_epsilon(max(
              0.0, setting_float("H_RELATIVE_EPSILON", 0.005))),
          plateau_window_expansions(max(
              1, setting_int("PLATEAU_WINDOW_EXPANSIONS", 65536))),
          plateau_confirm_windows(max(
              1, setting_int("PLATEAU_CONFIRM_WINDOWS", 3))),
          plateau_reset_windows(max(
              1, setting_int("PLATEAU_RESET_WINDOWS", 2))),
          plateau_min_bucket_expansions(max(
              1, setting_int("PLATEAU_MIN_BUCKET_EXPANSIONS", 16384))),
          plateau_min_since_request_expansions(max(
              1, setting_int(
                  "PLATEAU_MIN_SINCE_REQUEST_EXPANSIONS", 65536))),
          plateau_per_layer_request_gap_expansions(max(
              min_request_gap_expansions,
              setting_int(
                  "PLATEAU_PER_LAYER_REQUEST_GAP_EXPANSIONS", 500000))),
          plateau_min_share(min(
              1.0, max(0.0, setting_float("PLATEAU_MIN_SHARE", 0.25)))),
          plateau_h_bucket_width(max(
              0.000001,
              setting_float("PLATEAU_H_BUCKET_WIDTH", 0.001))) {
    }
};

struct TriggerMonitor::PlateauState {
    static const int MAX_LAYERS = 16;

    struct Layer {
        bool occupied;
        int64_t key;
        ap_float h;
        int current_expansions;
        int expansions_since_request;
        int last_seen_expansion;
        int last_request_expansion;
        int qualifying_streak;
        int miss_streak;
        bool active;
        bool armed;
        bool selected;
        StateID representative_state;
        ap_float representative_g;

        Layer()
            : occupied(false),
              key(0),
              h(0),
              current_expansions(0),
              expansions_since_request(0),
              last_seen_expansion(-1),
              last_request_expansion(-1),
              qualifying_streak(0),
              miss_streak(0),
              active(false),
              armed(false),
              selected(false),
              representative_state(StateID::no_state),
              representative_g(0) {
        }
    };

    array<Layer, MAX_LAYERS> layers;
    int expansions_in_window;
    int window_index;
    size_t windows_analyzed;
    size_t activations;
    size_t deactivations;
    size_t requests_submitted;
    size_t candidates_deferred;
    size_t layer_evictions;

    PlateauState()
        : expansions_in_window(0),
          window_index(0),
          windows_analyzed(0),
          activations(0),
          deactivations(0),
          requests_submitted(0),
          candidates_deferred(0),
          layer_evictions(0) {
    }
};

TriggerMonitor::PendingInfo::PendingInfo()
    : submitted_at(chrono::steady_clock::now()),
      expansions_at_submit(0) {
}

TriggerMonitor::PendingInfo::PendingInfo(
    int expansions, const string &reason_)
    : submitted_at(chrono::steady_clock::now()),
      expansions_at_submit(expansions),
      reason(reason_) {
}

TriggerMonitor::TriggerMonitor()
    : config(new Config()),
      plateau(new PlateauState()),
      run_id(setting_string("RUN_ID", "standalone")),
      anytime_iteration(1),
      next_request_id(0),
      base_expansions(0),
      expansions_since_best_h(0),
      last_request_expansion(-1),
      best_h(numeric_limits<ap_float>::infinity()),
      have_best_h(false),
      global_stall_requested(false),
      started(false),
      statistics_printed(false),
      phase_started_at(chrono::steady_clock::now()),
      request_attempts(0),
      requests_submitted(0),
      requests_rejected_duplicate(0),
      requests_rejected_pending_limit(0),
      requests_rejected_request_limit(0),
      requests_rejected_spacing(0),
      requests_rejected_burst(0),
      requests_rejected_bridge(0),
      responses_completed(0),
      response_transport_failures(0),
      stale_responses(0),
      usable_responses(0),
      max_pending_observed(0),
      total_response_seconds(0.0),
      max_response_seconds(0.0) {
}

TriggerMonitor::~TriggerMonitor() {
    finalize_and_print();
    delete plateau;
    delete config;
}

bool TriggerMonitor::enabled() const {
    return config->enabled;
}

void TriggerMonitor::start() {
    if (!started) {
        started = true;
        phase_started_at = chrono::steady_clock::now();
        if (!enabled())
            return;
        bridge.start();
        if (setting_enabled("ENABLE_FRONTIER_PLATEAU", false)) {
            cout << "[HYBRID-LLM-TRIGGER] frontier_plateau_disabled"
                 << " reason=lazy_edge_keys_are_parent_context" << endl;
        }
        cout << "[HYBRID-LLM-PLATEAU] event=configured"
             << " enabled=" << (config->enable_expansion_plateau ? 1 : 0)
             << " window_expansions="
             << config->plateau_window_expansions
             << " confirm_windows=" << config->plateau_confirm_windows
             << " reset_windows=" << config->plateau_reset_windows
             << " min_bucket_expansions="
             << config->plateau_min_bucket_expansions
             << " min_share=" << config->plateau_min_share
             << " candidate_policy=busiest_qualifying_bucket"
             << " bucket_width=" << config->plateau_h_bucket_width
             << " per_layer_gap="
             << config->plateau_per_layer_request_gap_expansions
             << endl;
    }
}

void TriggerMonitor::set_anytime_iteration(int iteration) {
    anytime_iteration = max(1, iteration);
}

bool TriggerMonitor::meaningfully_improves(ap_float current_h) const {
    if (!have_best_h)
        return true;
    ap_float scale = max(fabs(best_h), fabs(current_h));
    ap_float threshold = max(
        config->h_abs_epsilon, config->h_relative_epsilon * scale);
    return best_h - current_h > threshold;
}

bool TriggerMonitor::request_slot_available(bool bypass_spacing) const {
    if (!enabled())
        return false;
    if (config->max_requests > 0 &&
        static_cast<int>(requests_submitted) >= config->max_requests) {
        return false;
    }
    if (!bypass_spacing && last_request_expansion >= 0 &&
        base_expansions - last_request_expansion <
            config->min_request_gap_expansions) {
        return false;
    }
    if (config->max_pending > 0 &&
        static_cast<int>(pending_requests.size()) >= config->max_pending) {
        return false;
    }
    return true;
}

bool TriggerMonitor::request_state(
    StateID state_id, const string &reason,
    ap_float g, ap_float h, bool bypass_spacing) {
    ++request_attempts;
    if (!enabled() || state_id == StateID::no_state)
        return false;
    if (config->max_requests > 0 &&
        static_cast<int>(requests_submitted) >= config->max_requests) {
        ++requests_rejected_request_limit;
        return false;
    }
    if (!bypass_spacing && last_request_expansion >= 0 &&
        base_expansions - last_request_expansion <
            config->min_request_gap_expansions) {
        ++requests_rejected_spacing;
        return false;
    }
    if (requested_states.count(state_id)) {
        ++requests_rejected_duplicate;
        return false;
    }
    if (config->max_pending > 0 &&
        static_cast<int>(pending_requests.size()) >= config->max_pending) {
        ++requests_rejected_pending_limit;
        return false;
    }

    Request request;
    ostringstream request_id;
    request_id << run_id << "-p" << anytime_iteration << "-"
               << state_id.hash() << "-" << next_request_id++;
    request.request_id = request_id.str();
    request.run_id = run_id;
    request.iteration = anytime_iteration;
    request.state_id = state_id;
    request.state_index = state_id.hash();
    request.state_label = state_id_label(state_id);
    request.problem_id = setting_string("PROBLEM_ID", "");
    if (request.problem_id.empty()) {
        const char *legacy_problem_id = getenv("NLM_PROBLEM_ID");
        if (legacy_problem_id)
            request.problem_id = legacy_problem_id;
    }
    request.reason = reason;
    request.g = g;
    request.h = h;
    request.search_expansions = base_expansions;
    request.phase_elapsed_seconds =
        chrono::duration_cast<chrono::duration<double>>(
            chrono::steady_clock::now() - phase_started_at).count();
    request.init =
        g_state_registry->lookup_state(state_id).get_pddl_init_string();

    if (!bridge.submit(request)) {
        ++requests_rejected_bridge;
        return false;
    }
    ++requests_submitted;
    last_request_expansion = base_expansions;
    requested_states.insert(state_id);
    if (bridge.expects_response()) {
        pending_requests.emplace(
            request.request_id, PendingInfo(base_expansions, reason));
        max_pending_observed = max(
            max_pending_observed, pending_requests.size());
    }
    return true;
}

void TriggerMonitor::maybe_request_initial(
    StateID state_id, ap_float g, ap_float h) {
    if (config->request_initial)
        request_state(state_id, "initial", g, h, true);
}

bool TriggerMonitor::maybe_request_expansion_plateau(
    StateID state_id, ap_float g, ap_float h,
    bool global_stall_eligible, bool &candidate_present) {
    candidate_present = false;
    if (!config->enable_expansion_plateau ||
        !isfinite(static_cast<double>(h))) {
        return false;
    }

    int64_t key = static_cast<int64_t>(
        llround(h / config->plateau_h_bucket_width));
    PlateauState::Layer *layer = nullptr;
    for (PlateauState::Layer &candidate : plateau->layers) {
        if (candidate.occupied && candidate.key == key) {
            layer = &candidate;
            break;
        }
    }
    if (!layer || !layer->active || !layer->armed || !layer->selected)
        return false;
    if (layer->last_request_expansion >= 0 &&
        base_expansions - layer->last_request_expansion <
            config->plateau_per_layer_request_gap_expansions) {
        return false;
    }

    candidate_present = true;
    if (!request_slot_available(false)) {
        ++plateau->candidates_deferred;
        return false;
    }

    string reason = global_stall_eligible
        ? "expansion_plateau+global_stall"
        : "expansion_plateau";
    if (!request_state(state_id, reason, g, h, false))
        return false;

    layer->armed = false;
    layer->last_request_expansion = base_expansions;
    layer->expansions_since_request = 0;
    ++plateau->requests_submitted;
    if (global_stall_eligible)
        global_stall_requested = true;
    cout << "[HYBRID-LLM-PLATEAU] event=request_submitted"
         << " iteration=" << anytime_iteration
         << " window=" << plateau->window_index
         << " h=" << h
         << " state=" << state_id_label(state_id)
         << " expansions=" << base_expansions
         << " reason=" << reason << endl;
    return true;
}

void TriggerMonitor::record_expansion_plateau_observation(
    StateID state_id, ap_float g, ap_float h) {
    if (!config->enable_expansion_plateau ||
        !isfinite(static_cast<double>(h))) {
        return;
    }

    int64_t key = static_cast<int64_t>(
        llround(h / config->plateau_h_bucket_width));
    PlateauState::Layer *layer = nullptr;
    for (PlateauState::Layer &candidate : plateau->layers) {
        if (candidate.occupied && candidate.key == key) {
            layer = &candidate;
            break;
        }
    }
    if (!layer) {
        for (PlateauState::Layer &candidate : plateau->layers) {
            if (!candidate.occupied) {
                layer = &candidate;
                break;
            }
        }
    }
    if (!layer) {
        for (PlateauState::Layer &candidate : plateau->layers) {
            if (!layer ||
                (!candidate.active && layer->active) ||
                (candidate.active == layer->active &&
                 candidate.last_seen_expansion < layer->last_seen_expansion)) {
                layer = &candidate;
            }
        }
        if (layer->active)
            ++plateau->deactivations;
        *layer = PlateauState::Layer();
        ++plateau->layer_evictions;
    }
    if (!layer->occupied) {
        layer->occupied = true;
        layer->key = key;
        layer->h = h;
    }

    ++layer->current_expansions;
    ++layer->expansions_since_request;
    layer->last_seen_expansion = base_expansions;
    layer->representative_state = state_id;
    layer->representative_g = g;
    ++plateau->expansions_in_window;

    if (plateau->expansions_in_window <
        config->plateau_window_expansions) {
        return;
    }

    ++plateau->window_index;
    ++plateau->windows_analyzed;
    PlateauState::Layer *dominant = nullptr;
    PlateauState::Layer *selected = nullptr;
    int qualifying_buckets = 0;
    for (PlateauState::Layer &candidate : plateau->layers) {
        candidate.selected = false;
        if (candidate.occupied &&
            (!dominant || candidate.current_expansions >
                              dominant->current_expansions ||
             (candidate.current_expansions == dominant->current_expansions &&
              candidate.key < dominant->key))) {
            dominant = &candidate;
        }
    }

    for (PlateauState::Layer &candidate : plateau->layers) {
        if (!candidate.occupied)
            continue;
        int lower_expansions = 0;
        for (const PlateauState::Layer &other : plateau->layers) {
            if (other.occupied && other.key < candidate.key)
                lower_expansions += other.current_expansions;
        }
        ap_float share = static_cast<ap_float>(candidate.current_expansions) /
            plateau->expansions_in_window;
        ap_float lower_share = static_cast<ap_float>(lower_expansions) /
            plateau->expansions_in_window;
        bool qualifies =
            candidate.current_expansions >=
                config->plateau_min_bucket_expansions &&
            share >= config->plateau_min_share;

        if (qualifies) {
            ++qualifying_buckets;
            ++candidate.qualifying_streak;
            candidate.miss_streak = 0;
            if (!candidate.active &&
                candidate.qualifying_streak >=
                    config->plateau_confirm_windows) {
                candidate.active = true;
                candidate.armed = true;
                ++plateau->activations;
                cout << "[HYBRID-LLM-PLATEAU] event=activated"
                     << " iteration=" << anytime_iteration
                     << " window=" << plateau->window_index
                     << " h=" << candidate.h
                     << " share=" << share
                     << " lower_share=" << lower_share
                     << " streak=" << candidate.qualifying_streak
                     << " expansions=" << base_expansions << endl;
            } else if (candidate.active && !candidate.armed &&
                       candidate.expansions_since_request >=
                           config->plateau_min_since_request_expansions) {
                candidate.armed = true;
                cout << "[HYBRID-LLM-PLATEAU] event=rearmed"
                     << " iteration=" << anytime_iteration
                     << " window=" << plateau->window_index
                     << " h=" << candidate.h
                     << " evidence_expansions="
                     << candidate.expansions_since_request
                     << " expansions=" << base_expansions << endl;
            }
            if (candidate.active &&
                (!selected || candidate.current_expansions >
                                  selected->current_expansions ||
                 (candidate.current_expansions ==
                      selected->current_expansions &&
                  candidate.key < selected->key))) {
                selected = &candidate;
            }
        } else {
            candidate.qualifying_streak = 0;
            ++candidate.miss_streak;
            if (candidate.active &&
                candidate.miss_streak >= config->plateau_reset_windows) {
                candidate.active = false;
                candidate.armed = false;
                candidate.expansions_since_request = 0;
                ++plateau->deactivations;
                cout << "[HYBRID-LLM-PLATEAU] event=deactivated"
                     << " iteration=" << anytime_iteration
                     << " window=" << plateau->window_index
                     << " h=" << candidate.h
                     << " share=" << share
                     << " lower_share=" << lower_share
                     << " expansions=" << base_expansions << endl;
            }
        }
    }

    if (selected)
        selected->selected = true;

    if (dominant) {
        int dominant_lower_expansions = 0;
        for (const PlateauState::Layer &other : plateau->layers) {
            if (other.occupied && other.key < dominant->key)
                dominant_lower_expansions += other.current_expansions;
        }
        ap_float dominant_share =
            static_cast<ap_float>(dominant->current_expansions) /
            plateau->expansions_in_window;
        ap_float dominant_lower_share =
            static_cast<ap_float>(dominant_lower_expansions) /
            plateau->expansions_in_window;
        cout << "[HYBRID-LLM-PLATEAU] event=window"
             << " iteration=" << anytime_iteration
             << " window=" << plateau->window_index
             << " expansions=" << base_expansions
             << " window_expansions=" << plateau->expansions_in_window
             << " dominant_h=" << dominant->h
             << " dominant_count=" << dominant->current_expansions
             << " share=" << dominant_share
             << " lower_share=" << dominant_lower_share
             << " qualifying_buckets=" << qualifying_buckets
             << " selected_h="
             << (selected ? selected->h : -1)
             << " selected_count="
             << (selected ? selected->current_expansions : 0)
             << " streak=" << dominant->qualifying_streak
             << " active=" << (dominant->active ? 1 : 0)
             << " armed=" << (dominant->armed ? 1 : 0)
             << endl;
    }

    for (PlateauState::Layer &candidate : plateau->layers)
        candidate.current_expansions = 0;
    plateau->expansions_in_window = 0;
}

void TriggerMonitor::record_base_expansion(
    StateID state_id, ap_float g, ap_float h,
    bool ancestor_stagnant, bool burst_active) {
    ++base_expansions;
    if (meaningfully_improves(h)) {
        best_h = h;
        have_best_h = true;
        expansions_since_best_h = 0;
        global_stall_requested = false;
    } else {
        ++expansions_since_best_h;
    }

    if (burst_active) {
        ++requests_rejected_burst;
        record_expansion_plateau_observation(state_id, g, h);
        return;
    }

    bool global_stall_eligible =
        config->enable_global_stall && !global_stall_requested &&
        expansions_since_best_h >= config->stall_expansions;
    bool plateau_candidate = false;
    bool plateau_submitted = maybe_request_expansion_plateau(
        state_id, g, h, global_stall_eligible, plateau_candidate);

    if (!plateau_submitted && !plateau_candidate) {
        if (global_stall_eligible && request_slot_available(false)) {
            if (request_state(state_id, "global_stall", g, h, false))
                global_stall_requested = true;
        } else if (config->enable_ancestor_stagnation &&
                   ancestor_stagnant && request_slot_available(false)) {
            request_state(state_id, "ancestor_stagnation", g, h, false);
        }
    }
    record_expansion_plateau_observation(state_id, g, h);
}

void TriggerMonitor::record_rollout_expansion(ap_float h) {
    if (meaningfully_improves(h)) {
        best_h = h;
        have_best_h = true;
        expansions_since_best_h = 0;
        global_stall_requested = false;
    }
}

vector<Response> TriggerMonitor::poll_completed() {
    vector<Response> responses = bridge.poll_completed();
    for (const Response &response : responses) {
        ++responses_completed;
        auto pending = pending_requests.find(response.request_id);
        if (pending != pending_requests.end()) {
            double seconds = chrono::duration_cast<chrono::duration<double>>(
                chrono::steady_clock::now() - pending->second.submitted_at).count();
            total_response_seconds += seconds;
            max_response_seconds = max(max_response_seconds, seconds);
            pending_requests.erase(pending);
        }
        if (!response.transport_ok)
            ++response_transport_failures;
    }
    return responses;
}

bool TriggerMonitor::has_pending() const {
    return !pending_requests.empty();
}

void TriggerMonitor::record_usable_response() {
    ++usable_responses;
}

void TriggerMonitor::record_stale_response() {
    ++stale_responses;
}

void TriggerMonitor::finalize_and_print() {
    if (statistics_printed)
        return;
    statistics_printed = true;
    bridge.stop();
    if (!enabled())
        return;
    cout << "[HYBRID-LLM-TRIGGER-STATS]"
         << " iteration=" << anytime_iteration
         << " base_expansions=" << base_expansions
         << " request_attempts=" << request_attempts
         << " requests_submitted=" << requests_submitted
         << " pending_at_end=" << pending_requests.size()
         << " max_pending=" << max_pending_observed
         << " rejected_duplicate=" << requests_rejected_duplicate
         << " rejected_pending_limit=" << requests_rejected_pending_limit
         << " rejected_request_limit=" << requests_rejected_request_limit
         << " rejected_spacing=" << requests_rejected_spacing
         << " rejected_burst=" << requests_rejected_burst
         << " rejected_bridge=" << requests_rejected_bridge
         << " responses_completed=" << responses_completed
         << " transport_failures=" << response_transport_failures
         << " stale_responses=" << stale_responses
         << " usable_responses=" << usable_responses
         << " plateau_windows=" << plateau->windows_analyzed
         << " plateau_activations=" << plateau->activations
         << " plateau_deactivations=" << plateau->deactivations
         << " plateau_requests=" << plateau->requests_submitted
         << " plateau_candidates_deferred="
         << plateau->candidates_deferred
         << " plateau_layer_evictions=" << plateau->layer_evictions
         << " avg_response_seconds="
         << (responses_completed
                 ? total_response_seconds / responses_completed : 0.0)
         << " max_response_seconds=" << max_response_seconds
         << " queued_discarded=" << bridge.queued_discarded()
         << " active_cancelled=" << bridge.active_cancelled()
         << " completed_unconsumed=" << bridge.completed_unconsumed()
         << endl;
}

}
