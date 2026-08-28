#ifndef SEARCH_ENGINES_LAZY_SEARCH_H
#define SEARCH_ENGINES_LAZY_SEARCH_H

#include "../evaluation_context.h"
#include "../global_state.h"
#include "../scalar_evaluator.h"
#include "../search_engine.h"
#include "../search_progress.h"
#include "../search_space.h"

#include "../llm/llm_proposal.h"

#include "../open_lists/open_list.h"

#include <memory>
#include <deque>
#include <vector>

class GlobalOperator;
class Heuristic;
class ActionChainEvaluator;

namespace llm {
class TriggerMonitor;
struct Response;
}

namespace options {
class Options;
}

namespace lazy_search {
class LazySearch : public SearchEngine {
protected:
    enum class WorkOrigin { INITIAL, BASE_OPEN_LIST, LLM_ROLLOUT };

    std::unique_ptr<EdgeOpenList> open_list;

    // Search behavior parameters
    bool reopen_closed_nodes; // whether to reopen closed nodes upon finding lower g paths
    bool randomize_successors;
    bool preferred_successors_first;

    std::vector<Heuristic *> heuristics;
    std::vector<Heuristic *> preferred_operator_heuristics;
    std::vector<Heuristic *> estimate_heuristics;
    Heuristic *llm_h_evaluator;

    GlobalState current_state;
    StateID current_predecessor_id;
    const GlobalOperator *current_operator;
    ap_float current_g;
    ap_float current_real_g;
    EvaluationContext current_eval_context;
    WorkOrigin current_origin;
    bool has_current_work;

    std::unique_ptr<ActionChainEvaluator> llm_action_evaluator;
    std::unique_ptr<llm::TriggerMonitor> llm_trigger_monitor;
    llm::RolloutLimits llm_rollout_limits;
    llm::RolloutStatistics llm_rollout_statistics;
    std::deque<llm::Burst> llm_bursts;
    int llm_base_expansions;
    int llm_ancestor_check_interval;
    int llm_ancestor_depth;
    int llm_ancestor_min_depth;
    ap_float llm_ancestor_epsilon;

    virtual void initialize() override;
    virtual SearchStatus step() override;

    std::size_t generate_successors();
    llm::ExpansionOutcome process_current_work();
    bool fetch_next_base_edge();
    bool prepare_next_rollout_edge();
    SearchStatus schedule_next_work();

    void poll_llm_responses();
    void accept_llm_response(const llm::Response &response);
    void handle_rollout_outcome(const llm::ExpansionOutcome &outcome);
    void abort_active_proposal(llm::ExpansionKind reason);
    void finish_active_burst_if_empty();
    bool llm_ancestor_stagnant(const SearchNode &node, ap_float current_h);
    ap_float current_llm_h();

    void reward_progress();

    void get_successor_operators(std::vector<const GlobalOperator *> &ops);

    // TODO: Move into SearchEngine?
    void print_checkpoint_line(ap_float g) const;

public:
    explicit LazySearch(const options::Options &opts);
    virtual ~LazySearch() override;

    void set_pref_operator_heuristics(std::vector<Heuristic *> &heur);
    virtual void set_anytime_iteration(int iteration) override;

    virtual void print_statistics() const override;
};
}

#endif
