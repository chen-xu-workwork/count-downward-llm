#include "lazy_search.h"

#include "search_common.h"

#include "../action_chain_evaluator.h"
#include "../globals.h"
#include "../heuristic.h"
#include "../llm/llm_protocol.h"
#include "../llm/llm_trigger_monitor.h"
#include "../option_parser.h"
#include "../plugin.h"
#include "../successor_generator.h"

#include "../open_lists/open_list_factory.h"

#include "../utils/rng.h"
#include "../utils/planvis.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <sstream>
#include <thread>
#include <vector>

using namespace std;

namespace lazy_search {
static const int DEFAULT_LAZY_BOOST = 1000;

LazySearch::LazySearch(const Options &opts)
    : SearchEngine(opts),
      open_list(opts.get<shared_ptr<OpenListFactory>>("open")->
                create_edge_open_list()),
      reopen_closed_nodes(opts.get<bool>("reopen_closed")),
      randomize_successors(opts.get<bool>("randomize_successors")),
      preferred_successors_first(opts.get<bool>("preferred_successors_first")),
      llm_h_evaluator(opts.get<Heuristic *>("llm_h", nullptr)),
      current_state(g_initial_state()),
      current_predecessor_id(StateID::no_state),
      current_operator(nullptr),
      current_g(0),
      current_real_g(0),
      current_eval_context(current_state, 0, true, &statistics),
      current_origin(WorkOrigin::INITIAL),
      has_current_work(true),
      llm_trigger_monitor(new llm::TriggerMonitor()),
      llm_base_expansions(0),
      llm_ancestor_check_interval(max(
          1, llm::setting_int("ANCESTOR_CHECK_INTERVAL", 100000))),
      llm_ancestor_depth(max(
          1, llm::setting_int("ANCESTOR_DEPTH", 20))),
      llm_ancestor_min_depth(max(
          0, llm::setting_int("MIN_DEPTH", 30))),
      llm_ancestor_epsilon(max(
          0.0, llm::setting_float("ANCESTOR_H_EPSILON", 0.001))) {
    /*
      We initialize current_eval_context in such a way that the initial node
      counts as "preferred".
    */
}

LazySearch::~LazySearch() = default;

void LazySearch::set_anytime_iteration(int iteration) {
    SearchEngine::set_anytime_iteration(iteration);
    llm_trigger_monitor->set_anytime_iteration(iteration);
}

void LazySearch::set_pref_operator_heuristics(
    vector<Heuristic *> &heur) {
    preferred_operator_heuristics = heur;
}

void LazySearch::initialize() {
    cout << "Conducting lazy best first search, (real) bound = " << bound << endl;
    if (PLAN_VIS_LOG == latex_only) {
    	g_plan_logger->register_latex_var("x");
    	g_plan_logger->register_latex_var("y");
    }

    assert(open_list);
    set<Heuristic *> hset;
    open_list->get_involved_heuristics(hset);

    // Add heuristics that are used for preferred operators (in case they are
    // not also used in the open list).
    hset.insert(preferred_operator_heuristics.begin(),
                preferred_operator_heuristics.end());

    if (llm_h_evaluator)
        hset.insert(llm_h_evaluator);

    heuristics.assign(hset.begin(), hset.end());
    assert(!heuristics.empty());
    if (!llm_h_evaluator) {
        llm_h_evaluator = heuristics.front();
        if (llm_trigger_monitor->enabled() && heuristics.size() > 1) {
            cout << "[HYBRID-LLM-TRIGGER] warning"
                 << " reason=ambiguous_llm_h_evaluator"
                 << " detail=configure_llm_h" << endl;
        }
    }
    if (llm_trigger_monitor->enabled())
        llm_action_evaluator.reset(new ActionChainEvaluator());
    llm_trigger_monitor->start();
}

void LazySearch::get_successor_operators(vector<const GlobalOperator *> &ops) {
    assert(ops.empty());

    vector<const GlobalOperator *> all_operators;
    g_successor_generator->generate_applicable_ops(
        current_state, all_operators);

    vector<const GlobalOperator *> preferred_operators;
    for (Heuristic *heur : preferred_operator_heuristics) {
        if (!current_eval_context.is_heuristic_infinite(heur)) {
            vector<const GlobalOperator *> preferred =
                current_eval_context.get_preferred_operators(heur);
            preferred_operators.insert(
                preferred_operators.end(), preferred.begin(), preferred.end());
        }
    }

    if (randomize_successors) {
        g_rng()->shuffle(all_operators);
        // Note that preferred_operators can contain duplicates that are
        // only filtered out later, which gives operators "preferred
        // multiple times" a higher chance to be ordered early.
        g_rng()->shuffle(preferred_operators);
    }

    if (preferred_successors_first) {
        for (const GlobalOperator *op : preferred_operators) {
            if (!op->is_marked()) {
                ops.push_back(op);
                op->mark();
            }
        }

        for (const GlobalOperator *op : all_operators)
            if (!op->is_marked())
                ops.push_back(op);
    } else {
        for (const GlobalOperator *op : preferred_operators)
            if (!op->is_marked())
                op->mark();
        ops.swap(all_operators);
    }
}

size_t LazySearch::generate_successors() {
    vector<const GlobalOperator *> operators;
    get_successor_operators(operators);
    statistics.inc_generated(operators.size());
    if (llm_trigger_monitor->enabled() &&
        current_origin == WorkOrigin::LLM_ROLLOUT)
        llm_rollout_statistics.normal_edges_generated += operators.size();

    for (const GlobalOperator *op : operators) {
        ap_float new_g = current_g + get_adjusted_cost(*op);
        ap_float new_real_g = current_real_g + op->get_cost();
        bool is_preferred = op->is_marked();
        if (is_preferred)
            op->unmark();
        if (new_real_g < bound) {
            EvaluationContext new_eval_context(
                current_eval_context.get_cache(), new_g, is_preferred, nullptr);
            open_list->insert(new_eval_context, make_pair(current_state.get_id(), op));
        }
    }
    return operators.size();
}

ap_float LazySearch::current_llm_h() {
    if (current_eval_context.is_heuristic_infinite(llm_h_evaluator))
        return numeric_limits<ap_float>::infinity();
    return current_eval_context.get_heuristic_value(llm_h_evaluator);
}

bool LazySearch::llm_ancestor_stagnant(
    const SearchNode &node, ap_float current_h) {
    int traversal_depth = max(llm_ancestor_depth, llm_ancestor_min_depth);
    StateID ancestor_id = node.get_parent_state_id();
    ap_float best_improvement = 0;
    int inspected = 0;
    while (ancestor_id != StateID::no_state && inspected < traversal_depth) {
        GlobalState ancestor_state = g_state_registry->lookup_state(ancestor_id);
        SearchNode ancestor_node = search_space.get_node(ancestor_state);
        if (ancestor_node.is_new() || ancestor_node.is_dead_end())
            return false;
        if (inspected < llm_ancestor_depth) {
            EvaluationContext ancestor_context(
                ancestor_state, ancestor_node.get_g(), false, nullptr);
            if (ancestor_context.is_heuristic_infinite(llm_h_evaluator))
                return false;
            ap_float ancestor_h =
                ancestor_context.get_heuristic_value(llm_h_evaluator);
            best_improvement = max(best_improvement, ancestor_h - current_h);
        }
        ancestor_id = ancestor_node.get_parent_state_id();
        ++inspected;
    }
    return inspected >= traversal_depth &&
           best_improvement <= llm_ancestor_epsilon;
}

llm::ExpansionOutcome LazySearch::process_current_work() {
    SearchNode node = search_space.get_node(current_state);
    bool reopen = reopen_closed_nodes && !node.is_new() &&
                  !node.is_dead_end() && (current_g < node.get_g());

    if (!node.is_new() && !reopen) {
        if (node.is_dead_end()) {
            return llm::ExpansionOutcome(
                llm::ExpansionKind::DEAD_END, current_state.get_id());
        }
        return llm::ExpansionOutcome(
            llm::ExpansionKind::DUPLICATE_EXISTING,
            current_state.get_id());
    }

    if (node.is_new() || reopen) {
        StateID dummy_id = current_predecessor_id;
        // HACK! HACK! we do this because SearchNode has no default/copy constructor
        if (dummy_id == StateID::no_state) {
            dummy_id = g_initial_state().get_id();
        }
        GlobalState parent_state = g_state_registry->lookup_state(dummy_id);

    	if (PLAN_VIS_LOG == latex_only) {
    		g_plan_logger->log_latex_explored(parent_state.get_numeric_state_vals_string());
    	}

        SearchNode parent_node = search_space.get_node(parent_state);

        if (current_operator) {
            for (Heuristic *heuristic : heuristics)
                heuristic->reach_state(parent_state, *current_operator, current_state);
        }
        statistics.inc_evaluated_states();
        if (!open_list->is_dead_end(current_eval_context)) {
            // TODO: Generalize code for using multiple heuristics.
            if (reopen) {
                node.reopen(parent_node, current_operator);
                statistics.inc_reopened();
            } else if (current_predecessor_id == StateID::no_state) {
                node.open_initial();
                if (search_progress.check_progress(current_eval_context))
                    print_checkpoint_line(current_g);
            } else {
                node.open(parent_node, current_operator);
            }
            node.close();
            if (check_goal_and_set_plan(current_state))
                return llm::ExpansionOutcome(
                    llm::ExpansionKind::GOAL, current_state.get_id());
            if (search_progress.check_progress(current_eval_context)) {
                print_checkpoint_line(current_g);
                reward_progress();
            }
            generate_successors();
            statistics.inc_expanded();

            if (llm_trigger_monitor->enabled()) {
                ap_float h = current_llm_h();
                if (current_origin == WorkOrigin::INITIAL) {
                    llm_trigger_monitor->record_rollout_expansion(h);
                    llm_trigger_monitor->maybe_request_initial(
                        current_state.get_id(), node.get_g(), h);
                } else if (current_origin == WorkOrigin::BASE_OPEN_LIST) {
                    ++llm_base_expansions;
                    bool ancestor_stagnant =
                        llm_base_expansions % llm_ancestor_check_interval == 0 &&
                        llm_ancestor_stagnant(node, h);
                    llm_trigger_monitor->record_base_expansion(
                        current_state.get_id(), node.get_g(), h,
                        ancestor_stagnant, !llm_bursts.empty());
                } else {
                    llm_trigger_monitor->record_rollout_expansion(h);
                }
            }
        } else {
            node.mark_as_dead_end();
            statistics.inc_dead_ends();
            if (current_predecessor_id == StateID::no_state)
                print_initial_h_values(current_eval_context);
            return llm::ExpansionOutcome(
                llm::ExpansionKind::DEAD_END, current_state.get_id());
        }
        if (current_predecessor_id == StateID::no_state) {
            print_initial_h_values(current_eval_context);
        }
    }
    return llm::ExpansionOutcome(
        reopen ? llm::ExpansionKind::REOPENED
               : llm::ExpansionKind::EXPANDED_NEW,
        current_state.get_id());
}

bool LazySearch::fetch_next_base_edge() {
    while (!open_list->empty()) {
        EdgeOpenListEntry next = open_list->remove_min();
        GlobalState predecessor =
            g_state_registry->lookup_state(next.first);
        if (violates_global_constraint(predecessor))
            continue;

        SearchNode predecessor_node = search_space.get_node(predecessor);
        if (predecessor_node.is_new() || predecessor_node.is_dead_end())
            continue;
        if (!next.second->is_applicable(predecessor)) {
            cout << "[HYBRID-LAZY] discarded stale inapplicable base edge"
                 << " predecessor=" << llm::state_id_label(next.first)
                 << " action=\"" << next.second->get_name() << "\"" << endl;
            continue;
        }

        current_predecessor_id = next.first;
        current_operator = next.second;
        current_state = g_state_registry->get_successor_state(
            predecessor, *current_operator);
        current_g = predecessor_node.get_g() +
            get_adjusted_cost(*current_operator);
        current_real_g = predecessor_node.get_real_g() +
            current_operator->get_cost();
        current_eval_context = EvaluationContext(
            current_state, current_g, true, &statistics);
        current_origin = WorkOrigin::BASE_OPEN_LIST;
        has_current_work = true;
        return true;
    }
    return false;
}

void LazySearch::abort_active_proposal(llm::ExpansionKind reason) {
    if (llm_bursts.empty() || llm_bursts.front().proposals.empty())
        return;
    llm::Burst &burst = llm_bursts.front();
    llm::Proposal &proposal = burst.proposals.front();
    burst.had_abort = true;
    ++llm_rollout_statistics.proposals_aborted;
    switch (reason) {
    case llm::ExpansionKind::INAPPLICABLE:
        ++llm_rollout_statistics.inapplicable_aborts;
        break;
    case llm::ExpansionKind::DEAD_END:
        ++llm_rollout_statistics.dead_end_aborts;
        break;
    case llm::ExpansionKind::BOUND_PRUNED:
        ++llm_rollout_statistics.bound_aborts;
        break;
    case llm::ExpansionKind::INVALID_PREDECESSOR:
        ++llm_rollout_statistics.invalid_predecessor_aborts;
        break;
    case llm::ExpansionKind::CYCLE:
        ++llm_rollout_statistics.cycle_aborts;
        break;
    case llm::ExpansionKind::STALE:
        ++llm_rollout_statistics.stale_proposals;
        break;
    default:
        break;
    }
    cout << "[HYBRID-LLM-ROLLOUT] event=proposal_aborted"
         << " request_id=" << proposal.request_id
         << " proposal_id=" << proposal.proposal_id
         << " cursor=" << proposal.cursor
         << " reason=" << static_cast<int>(reason) << endl;
    burst.proposals.pop_front();
    finish_active_burst_if_empty();
}

void LazySearch::finish_active_burst_if_empty() {
    if (llm_bursts.empty() || !llm_bursts.front().proposals.empty())
        return;
    llm::Burst &burst = llm_bursts.front();
    if (burst.started) {
        double seconds = chrono::duration_cast<chrono::duration<double>>(
            chrono::steady_clock::now() - burst.started_at).count();
        llm_rollout_statistics.burst_wall_seconds += seconds;
        if (burst.had_abort)
            ++llm_rollout_statistics.bursts_aborted;
        else
            ++llm_rollout_statistics.bursts_completed;
        cout << "[HYBRID-LLM-ROLLOUT] event=burst_finished"
             << " request_id=" << burst.request_id
             << " burst_id=" << burst.burst_id
             << " result=" << (burst.had_abort ? "aborted" : "completed")
             << " seconds=" << seconds << endl;
    }
    llm_bursts.pop_front();
}

bool LazySearch::prepare_next_rollout_edge() {
    if (llm_bursts.empty())
        return false;
    llm::Burst &burst = llm_bursts.front();
    if (!burst.started) {
        burst.started = true;
        burst.started_at = chrono::steady_clock::now();
        ++llm_rollout_statistics.bursts_started;
        cout << "[HYBRID-LLM-ROLLOUT] event=burst_started"
             << " request_id=" << burst.request_id
             << " burst_id=" << burst.burst_id
             << " proposals=" << burst.proposals.size()
             << " actions=" << burst.total_actions << endl;
    }
    if (burst.proposals.empty()) {
        finish_active_burst_if_empty();
        return false;
    }

    llm::Proposal &proposal = burst.proposals.front();
    if (proposal.run_id != llm::setting_string("RUN_ID", "standalone") ||
        proposal.iteration != get_anytime_iteration()) {
        abort_active_proposal(llm::ExpansionKind::STALE);
        return false;
    }
    if (proposal.cursor >= proposal.actions.size()) {
        ++llm_rollout_statistics.proposals_completed;
        burst.proposals.pop_front();
        finish_active_burst_if_empty();
        return false;
    }

    GlobalState predecessor =
        g_state_registry->lookup_state(proposal.anchor_state);
    SearchNode predecessor_node = search_space.get_node(predecessor);
    if (predecessor_node.is_new() || predecessor_node.is_dead_end() ||
        violates_global_constraint(predecessor)) {
        abort_active_proposal(llm::ExpansionKind::INVALID_PREDECESSOR);
        return false;
    }

    ActionResolution resolution = llm_action_evaluator->resolve_action(
        predecessor, proposal.actions[proposal.cursor]);
    if (resolution.status != ActionResolutionStatus::FOUND) {
        abort_active_proposal(llm::ExpansionKind::INAPPLICABLE);
        return false;
    }

    ap_float candidate_g = predecessor_node.get_g() +
        get_adjusted_cost(*resolution.op);
    ap_float candidate_real_g = predecessor_node.get_real_g() +
        resolution.op->get_cost();
    if (candidate_real_g >= bound) {
        abort_active_proposal(llm::ExpansionKind::BOUND_PRUNED);
        return false;
    }

    GlobalState successor = llm_action_evaluator->apply_action(
        predecessor, *resolution.op);
    if (proposal.visited_states.count(successor.get_id())) {
        abort_active_proposal(llm::ExpansionKind::CYCLE);
        return false;
    }

    current_predecessor_id = proposal.anchor_state;
    current_operator = resolution.op;
    current_state = successor;
    current_g = candidate_g;
    current_real_g = candidate_real_g;
    current_eval_context = EvaluationContext(
        current_state, current_g, true, &statistics);
    current_origin = WorkOrigin::LLM_ROLLOUT;
    has_current_work = true;
    cout << "[HYBRID-LLM-ROLLOUT] event=edge_prepared"
         << " request_id=" << proposal.request_id
         << " proposal_id=" << proposal.proposal_id
         << " cursor=" << proposal.cursor
         << " predecessor=" << llm::state_id_label(proposal.anchor_state)
         << " successor=" << llm::state_id_label(successor.get_id())
         << " action=\"" << resolution.op->get_name() << "\"" << endl;
    return true;
}

void LazySearch::handle_rollout_outcome(
    const llm::ExpansionOutcome &outcome) {
    if (llm_bursts.empty() || llm_bursts.front().proposals.empty())
        return;
    llm::Burst &burst = llm_bursts.front();
    llm::Proposal &proposal = burst.proposals.front();

    switch (outcome.kind) {
    case llm::ExpansionKind::EXPANDED_NEW:
        ++llm_rollout_statistics.states_new;
        break;
    case llm::ExpansionKind::REOPENED:
        ++llm_rollout_statistics.states_reopened;
        break;
    case llm::ExpansionKind::DUPLICATE_EXISTING:
        ++llm_rollout_statistics.states_duplicate;
        break;
    case llm::ExpansionKind::GOAL:
        break;
    default:
        abort_active_proposal(outcome.kind);
        return;
    }

    ++llm_rollout_statistics.actions_processed;
    proposal.anchor_state = outcome.actual_successor;
    proposal.visited_states.insert(outcome.actual_successor);
    ++proposal.cursor;
    cout << "[HYBRID-LLM-ROLLOUT] event=edge_processed"
         << " request_id=" << proposal.request_id
         << " proposal_id=" << proposal.proposal_id
         << " cursor=" << proposal.cursor
         << " state=" << llm::state_id_label(outcome.actual_successor)
         << " outcome=" << static_cast<int>(outcome.kind) << endl;

    if (proposal.cursor >= proposal.actions.size() ||
        outcome.kind == llm::ExpansionKind::GOAL) {
        ++llm_rollout_statistics.proposals_completed;
        burst.proposals.pop_front();
        finish_active_burst_if_empty();
    }
}

void LazySearch::accept_llm_response(const llm::Response &response) {
    string expected_run_id = llm::setting_string("RUN_ID", "standalone");
    if (response.run_id != expected_run_id ||
        response.iteration != get_anytime_iteration()) {
        llm_trigger_monitor->record_stale_response();
        ++llm_rollout_statistics.stale_proposals;
        return;
    }
    if (!response.transport_ok)
        return;

    llm::ParsedResponse parsed = llm::parse_response_body(response.body);
    bool usable_status = parsed.status == "ok" || parsed.status == "partial";
    if (!parsed.valid || !usable_status) {
        cout << "[HYBRID-LLM-ROLLOUT] event=response_rejected"
             << " request_id=" << response.request_id
             << " error=\"" << (parsed.valid ? parsed.status : parsed.error)
             << "\"" << endl;
        return;
    }
    if (static_cast<int>(llm_bursts.size()) >=
        llm_rollout_limits.max_queued_bursts) {
        ++llm_rollout_statistics.responses_rejected_budget;
        return;
    }

    llm::Burst burst;
    burst.burst_id = response.request_id;
    burst.request_id = response.request_id;
    burst.run_id = response.run_id;
    burst.iteration = response.iteration;
    burst.source_state = response.state_id;

    size_t response_requested_actions = 0;
    for (const vector<string> &chain : parsed.action_chains)
        response_requested_actions += chain.size();
    llm_rollout_statistics.actions_requested += response_requested_actions;

    int sample_index = 0;
    int accepted_actions = 0;
    for (const vector<string> &chain : parsed.action_chains) {
        if (chain.empty()) {
            ++sample_index;
            continue;
        }
        if (static_cast<int>(burst.proposals.size()) >=
            llm_rollout_limits.max_proposals_per_response) {
            break;
        }
        int remaining =
            llm_rollout_limits.max_actions_per_burst - accepted_actions;
        int accepted = min(
            static_cast<int>(chain.size()),
            min(llm_rollout_limits.max_actions_per_proposal, remaining));
        if (accepted <= 0)
            break;

        llm::Proposal proposal;
        ostringstream proposal_id;
        proposal_id << response.request_id << "-s" << sample_index;
        proposal.proposal_id = proposal_id.str();
        proposal.request_id = response.request_id;
        proposal.run_id = response.run_id;
        proposal.sample_index = sample_index;
        proposal.iteration = response.iteration;
        proposal.source_state = response.state_id;
        proposal.anchor_state = response.state_id;
        proposal.actions.assign(chain.begin(), chain.begin() + accepted);
        proposal.visited_states.insert(response.state_id);
        burst.proposals.push_back(proposal);
        accepted_actions += accepted;
        ++sample_index;
    }
    if (burst.proposals.empty())
        return;

    burst.total_actions = accepted_actions;
    llm_rollout_statistics.actions_prevalidated += accepted_actions;
    if (accepted_actions < static_cast<int>(response_requested_actions)) {
        // This counter describes response/burst hard-cap enforcement, not
        // Python's symbolic prefix filtering.
        ++llm_rollout_statistics.responses_rejected_budget;
    }
    llm_bursts.push_back(burst);
    llm_trigger_monitor->record_usable_response();
    cout << "[HYBRID-LLM-ROLLOUT] event=response_accepted"
         << " request_id=" << response.request_id
         << " proposals=" << burst.proposals.size()
         << " actions=" << accepted_actions << endl;
}

void LazySearch::poll_llm_responses() {
    vector<llm::Response> responses = llm_trigger_monitor->poll_completed();
    for (const llm::Response &response : responses)
        accept_llm_response(response);
}

SearchStatus LazySearch::schedule_next_work() {
    if (llm_trigger_monitor->enabled()) {
        poll_llm_responses();
        finish_active_burst_if_empty();
        if (!llm_bursts.empty()) {
            prepare_next_rollout_edge();
            return IN_PROGRESS;
        }
    }
    if (fetch_next_base_edge())
        return IN_PROGRESS;
    if (llm_trigger_monitor->enabled() &&
        llm_trigger_monitor->has_pending()) {
        this_thread::sleep_for(chrono::milliseconds(1));
        return IN_PROGRESS;
    }
    cout << "Completely explored state space -- no solution!" << endl;
    return FAILED;
}

SearchStatus LazySearch::step() {
    if (!has_current_work)
        return schedule_next_work();

    WorkOrigin processed_origin = current_origin;
    llm::ExpansionOutcome outcome = process_current_work();
    has_current_work = false;
    if (llm_trigger_monitor->enabled() &&
        processed_origin == WorkOrigin::LLM_ROLLOUT)
        handle_rollout_outcome(outcome);
    if (outcome.kind == llm::ExpansionKind::GOAL)
        return SOLVED;
    return schedule_next_work();
}

void LazySearch::reward_progress() {
    open_list->boost_preferred();
}

void LazySearch::print_checkpoint_line(ap_float g) const {
    cout << "[g=" << g << ", ";
    statistics.print_basic_statistics();
    cout << "]" << endl;
}

void LazySearch::print_statistics() const {
    statistics.print_detailed_statistics();
    search_space.print_statistics();
    if (llm_trigger_monitor->enabled()) {
        llm_rollout_statistics.print(get_anytime_iteration());
        llm_trigger_monitor->finalize_and_print();
    }
}


static void _add_succ_order_options(OptionParser &parser) {
    vector<string> options;
    parser.add_option<bool>(
        "randomize_successors",
        "randomize the order in which successors are generated",
        "false");
    parser.add_option<bool>(
        "preferred_successors_first",
        "consider preferred operators first",
        "false");
    parser.document_note(
        "Successor ordering",
        "When using randomize_successors=true and "
        "preferred_successors_first=true, randomization happens before "
        "preferred operators are moved to the front.");
}

static void _add_llm_options(OptionParser &parser) {
    parser.add_option<Heuristic *>(
        "llm_h",
        "heuristic used for Lazy LLM trigger bookkeeping and ancestor checks; "
        "defaults to the first heuristic involved in the Open List",
        OptionParser::NONE);
}

static SearchEngine *_parse(OptionParser &parser) {
    parser.document_synopsis("Lazy best-first search", "");
    parser.add_option<shared_ptr<OpenListFactory>>("open", "open list");
    parser.add_option<bool>("reopen_closed", "reopen closed nodes", "false");
    parser.add_list_option<Heuristic *>(
        "preferred",
        "use preferred operators of these heuristics", "[]");
    _add_succ_order_options(parser);
    _add_llm_options(parser);
    SearchEngine::add_options_to_parser(parser);
    Options opts = parser.parse();

    LazySearch *engine = nullptr;
    if (!parser.dry_run()) {
        engine = new LazySearch(opts);
        /*
          TODO: The following two lines look fishy. If they serve a
          purpose, shouldn't the constructor take care of this?
        */
        vector<Heuristic *> preferred_list = opts.get_list<Heuristic *>("preferred");
        engine->set_pref_operator_heuristics(preferred_list);
    }

    return engine;
}


static SearchEngine *_parse_greedy(OptionParser &parser) {
    parser.document_synopsis("Greedy search (lazy)", "");
    parser.document_note(
        "Open lists",
        "In most cases, lazy greedy best first search uses "
        "an alternation open list with one queue for each evaluator. "
        "If preferred operator heuristics are used, it adds an "
        "extra queue for each of these evaluators that includes "
        "only the nodes that are generated with a preferred operator. "
        "If only one evaluator and no preferred operator heuristic is used, "
        "the search does not use an alternation open list "
        "but a standard open list with only one queue.");
    parser.document_note(
        "Equivalent statements using general lazy search",
        "\n```\n--heuristic h2=eval2\n"
        "--search lazy_greedy([eval1, h2], preferred=h2, boost=100)\n```\n"
        "is equivalent to\n"
        "```\n--heuristic h1=eval1 --heuristic h2=eval2\n"
        "--search lazy(alt([single(h1), single(h1, pref_only=true), single(h2),\n"
        "                  single(h2, pref_only=true)], boost=100),\n"
        "              preferred=h2)\n```\n"
        "------------------------------------------------------------\n"
        "```\n--search lazy_greedy([eval1, eval2], boost=100)\n```\n"
        "is equivalent to\n"
        "```\n--search lazy(alt([single(eval1), single(eval2)], boost=100))\n```\n"
        "------------------------------------------------------------\n"
        "```\n--heuristic h1=eval1\n--search lazy_greedy(h1, preferred=h1)\n```\n"
        "is equivalent to\n"
        "```\n--heuristic h1=eval1\n"
        "--search lazy(alt([single(h1), single(h1, pref_only=true)], boost=1000),\n"
        "              preferred=h1)\n```\n"
        "------------------------------------------------------------\n"
        "```\n--search lazy_greedy(eval1)\n```\n"
        "is equivalent to\n"
        "```\n--search lazy(single(eval1))\n```\n",
        true);

    parser.add_list_option<ScalarEvaluator *>("evals", "scalar evaluators");
    parser.add_list_option<Heuristic *>(
        "preferred",
        "use preferred operators of these heuristics", "[]");
    parser.add_option<bool>("reopen_closed",
                            "reopen closed nodes", "false");
    parser.add_option<ap_float>(
        "boost",
        "boost value for alternation queues that are restricted "
        "to preferred operator nodes",
        OptionParser::to_str(DEFAULT_LAZY_BOOST));
    _add_succ_order_options(parser);
    _add_llm_options(parser);
    SearchEngine::add_options_to_parser(parser);
    Options opts = parser.parse();

    LazySearch *engine = 0;
    if (!parser.dry_run()) {
        opts.set("open", search_common::create_greedy_open_list_factory(opts));
        engine = new LazySearch(opts);
        // TODO: The following two lines look fishy. See similar comment in _parse.
        vector<Heuristic *> preferred_list = opts.get_list<Heuristic *>("preferred");
        engine->set_pref_operator_heuristics(preferred_list);
    }
    return engine;
}

static SearchEngine *_parse_weighted_astar(OptionParser &parser) {
    parser.document_synopsis(
        "(Weighted) A* search (lazy)",
        "Weighted A* is a special case of lazy best first search.");
    parser.document_note(
        "Open lists",
        "In the general case, it uses an alternation open list "
        "with one queue for each evaluator h that ranks the nodes "
        "by g + w * h. If preferred operator heuristics are used, "
        "it adds for each of the evaluators another such queue that "
        "only inserts nodes that are generated by preferred operators. "
        "In the special case with only one evaluator and no preferred "
        "operator heuristics, it uses a single queue that "
        "is ranked by g + w * h. ");
    parser.document_note(
        "Equivalent statements using general lazy search",
        "\n```\n--heuristic h1=eval1\n"
        "--search lazy_wastar([h1, eval2], w=2, preferred=h1,\n"
        "                     bound=100, boost=500)\n```\n"
        "is equivalent to\n"
        "```\n--heuristic h1=eval1 --heuristic h2=eval2\n"
        "--search lazy(alt([single(sum([g(), weight(h1, 2)])),\n"
        "                   single(sum([g(), weight(h1, 2)]), pref_only=true),\n"
        "                   single(sum([g(), weight(h2, 2)])),\n"
        "                   single(sum([g(), weight(h2, 2)]), pref_only=true)],\n"
        "                  boost=500),\n"
        "              preferred=h1, reopen_closed=true, bound=100)\n```\n"
        "------------------------------------------------------------\n"
        "```\n--search lazy_wastar([eval1, eval2], w=2, bound=100)\n```\n"
        "is equivalent to\n"
        "```\n--search lazy(alt([single(sum([g(), weight(eval1, 2)])),\n"
        "                   single(sum([g(), weight(eval2, 2)]))],\n"
        "                  boost=1000),\n"
        "              reopen_closed=true, bound=100)\n```\n"
        "------------------------------------------------------------\n"
        "```\n--search lazy_wastar([eval1, eval2], bound=100, boost=0)\n```\n"
        "is equivalent to\n"
        "```\n--search lazy(alt([single(sum([g(), eval1])),\n"
        "                   single(sum([g(), eval2]))])\n"
        "              reopen_closed=true, bound=100)\n```\n"
        "------------------------------------------------------------\n"
        "```\n--search lazy_wastar(eval1, w=2)\n```\n"
        "is equivalent to\n"
        "```\n--search lazy(single(sum([g(), weight(eval1, 2)])), reopen_closed=true)\n```\n",
        true);

    parser.add_list_option<ScalarEvaluator *>("evals", "scalar evaluators");
    parser.add_list_option<Heuristic *>(
        "preferred",
        "use preferred operators of these heuristics", "[]");
    parser.add_option<bool>("reopen_closed", "reopen closed nodes", "true");
    parser.add_option<ap_float>("boost",
                           "boost value for preferred operator open lists",
                           OptionParser::to_str(DEFAULT_LAZY_BOOST));
    parser.add_option<ap_float>("w", "heuristic weight", "1");
    _add_succ_order_options(parser);
    _add_llm_options(parser);
    SearchEngine::add_options_to_parser(parser);
    Options opts = parser.parse();

    opts.verify_list_non_empty<ScalarEvaluator *>("evals");

    LazySearch *engine = nullptr;
    if (!parser.dry_run()) {
        opts.set("open", search_common::create_wastar_open_list_factory(opts));
        engine = new LazySearch(opts);
        // TODO: The following two lines look fishy. See similar comment in _parse.
        vector<Heuristic *> preferred_list = opts.get_list<Heuristic *>("preferred");
        engine->set_pref_operator_heuristics(preferred_list);
    }
    return engine;
}

static Plugin<SearchEngine> _plugin("lazy", _parse);
static Plugin<SearchEngine> _plugin_greedy("lazy_greedy", _parse_greedy);
static Plugin<SearchEngine> _plugin_weighted_astar("lazy_wastar", _parse_weighted_astar);
}
