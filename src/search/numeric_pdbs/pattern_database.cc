#include "pattern_database.h"

#include "match_tree.h"
#include "numeric_condition.h"
#include "numeric_helper.h"
#include "numeric_task_proxy.h"

#include "../priority_queue.h"

#include "../tasks/projected_task.h"

#include "../utils/logging.h"
#include "../utils/math.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <optional>

using namespace std;
using namespace numeric_condition;
using namespace numeric_pdb_helper;

namespace numeric_pdbs {
AbstractOperator::AbstractOperator(const vector<pair<int, int>> &prev_pairs,
                                   const vector<pair<int, int>> &pre_pairs,
                                   const vector<pair<int, int>> &eff_pairs,
                                   int op_id,
                                   ap_float cost,
                                   const vector<size_t> &hash_multipliers,
                                   bool regression)
    : op_id(op_id),
      cost(cost),
      preconditions(prev_pairs) {

    if (regression){
        // preconditions are prevail + effects
        preconditions.insert(preconditions.end(),
                             eff_pairs.begin(),
                             eff_pairs.end());
    } else {
        // preconditions are prevail + pre
        preconditions.insert(preconditions.end(),
                             pre_pairs.begin(),
                             pre_pairs.end());
    }
    // Sort preconditions for MatchTree construction.
    sort(preconditions.begin(), preconditions.end());
    for (size_t i = 1; i < preconditions.size(); ++i) {
        assert(preconditions[i].first !=
               preconditions[i - 1].first);
    }
    hash_effect = 0;
    assert(pre_pairs.size() == eff_pairs.size());
    const vector<pair<int, int>> *_pre;
    const vector<pair<int, int>> *_eff;
    if (regression){
        _pre = &eff_pairs;
        _eff = &pre_pairs;
    } else {
        _pre = &pre_pairs;
        _eff = &eff_pairs;
    }
    for (size_t i = 0; i < pre_pairs.size(); ++i) {
        int var = pre_pairs[i].first;
        assert(var == eff_pairs[i].first);
        int old_val = (*_pre)[i].second;
        int new_val = (*_eff)[i].second;
        assert(new_val != -1);
        size_t effect = (new_val - old_val) * hash_multipliers[var];
        hash_effect += effect;
    }
}

void AbstractOperator::dump(const Pattern &pattern,
                            const NumericTaskProxy &task_proxy) const {
    cout << "AbstractOperator:" << endl;
    cout << "Preconditions:" << endl;
    for (size_t i = 0; i < preconditions.size(); ++i) {
        int var_id = preconditions[i].first;
        int val = preconditions[i].second;
        cout << "Variable: " << var_id << " (True name: "
             << task_proxy.get_variables()[pattern.regular[var_id]].get_name()
             << ", Index: " << i << ") Value: " << val << endl;
    }
    cout << "Hash effect:" << hash_effect << endl;
}


PatternDatabase::PatternDatabase(
        const shared_ptr<NumericTaskProxy> &task_proxy,
        const Pattern &pattern,
        size_t max_number_states,
        bool extend_abstract_state_space,
        bool need_goal,
        double f_layer_offset_ratio,
        InnerHeuristic search_h,
        InnerHeuristic frontier_h,
        InnerHeuristic failed_lookup_h,
        const vector<ap_float> &operator_costs,
        bool dump)
        : task_proxy(task_proxy),
          pattern(pattern),
          exploration_h(search_h),
          frontier_h(frontier_h),
          failed_lookup_h(failed_lookup_h),
          extend_abstract_state_space(extend_abstract_state_space),
          need_goal(need_goal),
          f_layer_offset_ratio(f_layer_offset_ratio),
          min_action_cost(numeric_limits<ap_float>::max()),
          exhausted_abstract_state_space(false) {

    assert(operator_costs.empty() ||
           operator_costs.size() == task_proxy->get_operators().size());
    assert(utils::is_sorted_unique(pattern.regular));
    assert(utils::is_sorted_unique(pattern.numeric));

    if (extend_abstract_state_space){
        extend_abstract_state_space = false;
        cout << "WARNING: extension of abstract state space currently not implemented." << endl;
    }
    
    utils::Timer timer;
    prop_hash_multipliers.reserve(pattern.regular.size());
    size_t domain_size_product = 1;
    for (int pattern_var_id : pattern.regular) {
        prop_hash_multipliers.push_back(domain_size_product);
        VariableProxy var = task_proxy->get_variables()[pattern_var_id];
        if (utils::is_product_within_limit(domain_size_product, var.get_domain_size(),
                                           numeric_limits<int>::max())) {
            domain_size_product *= var.get_domain_size();
        } else {
            cerr << "Given pattern is too large only on propositional variables! (Overflow occured): " << endl;
            cerr << pattern << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        }
    }
    
    if (pattern.numeric.empty()){
        create_pdb_propositional(domain_size_product, operator_costs);
    } else {
        create_pdb(max_number_states, std::nullopt, operator_costs, dump);
    }
    if (dump)
        cout << "PDB construction time: " << timer << endl;
}

void PatternDatabase::construct_inner_heuristics(size_t max_number_states,
                                                 const vector<int> &variable_to_index,
                                                 const vector<ap_float> &operator_costs) {
    if (exploration_h == InnerHeuristic::LMCUT ||
        frontier_h == InnerHeuristic::LMCUT ||
        failed_lookup_h == InnerHeuristic::LMCUT) {

        assert(!inner_h_task);
        inner_h_task = make_shared<tasks::ProjectedTask>(task_proxy->get_task(), pattern, task_proxy);

        assert(!lmc);
        lmc = make_unique<lm_cut_numeric_heuristic::LandmarkCutNumericHeuristic>(inner_h_task);

        lmc->initialize();
    }
    if (exploration_h == InnerHeuristic::PDB ||
        frontier_h == InnerHeuristic::PDB ||
        failed_lookup_h == InnerHeuristic::PDB) {

        assert(!pdb);

        if (pattern.regular.size() + pattern.numeric.size() > 1) {
            Pattern new_pattern;
            for (const auto &num_goal: task_proxy->get_numeric_goals()) {
                if (num_variable_to_index[num_goal.get_var_id()] != -1) {
                    new_pattern.numeric.push_back(num_goal.get_var_id());
                }
            }
            for (const auto &goal : task_proxy->get_propositional_goals()){
                int var = goal.get_variable().get_id();
                if (variable_to_index[var] != -1){
                    new_pattern.regular.push_back(var);
                }
            }
            if (new_pattern.numeric.size() == pattern.numeric.size() && new_pattern.regular.size() == pattern.regular.size()) {
                if (new_pattern.numeric.size() > 1){
                    new_pattern.numeric.resize(pattern.numeric.size() / 2);
                }
                if (new_pattern.regular.size() > 1){
                    new_pattern.regular.resize(pattern.regular.size() / 2);
                }
                if (new_pattern.numeric.size() == pattern.numeric.size() && new_pattern.regular.size() == pattern.regular.size()) {
                    // both parts of pattern have exactly one variable, need to clear one of them
                    new_pattern.regular.clear();
                }
            }
            sort(new_pattern.regular.begin(), new_pattern.regular.end());
            sort(new_pattern.numeric.begin(), new_pattern.numeric.end());
            cout << "Inner pattern: " << new_pattern.regular << new_pattern.numeric << endl; // TODO remove this

            pdb = std::make_unique<PatternDatabase>(
                    task_proxy,
                    new_pattern,
                    max((size_t) 1000, max_number_states / 10), // TODO don't hard-code the limit
                    extend_abstract_state_space,
                    true, // TODO make this an option
                    f_layer_offset_ratio,
                    InnerHeuristic::BLIND,
                    InnerHeuristic::BLIND,
                    InnerHeuristic::BLIND,
                    operator_costs,
                    false);
        } else {
            cout << "WARNING: no variables in inner pattern, fall back to blind" << endl;
            if (exploration_h == InnerHeuristic::PDB){
                exploration_h = InnerHeuristic::BLIND;
            }
            if (frontier_h == InnerHeuristic::PDB){
                frontier_h = InnerHeuristic::BLIND;
            }
            if (failed_lookup_h == InnerHeuristic::PDB){
                failed_lookup_h = InnerHeuristic::BLIND;
            }
        }
    }
}

std::pair<bool, ap_float> PatternDatabase::compute_heuristic(const State &state) {
    return get_value(state);
}

std::pair<bool, ap_float> PatternDatabase::compute_heuristic(const NumericState &state) {
    return get_value(state);
}

void PatternDatabase::multiply_out(
    int pos, int op_id, ap_float cost,
    vector<pair<int, int>> &prev_pairs,
    vector<pair<int, int>> &pre_pairs,
    vector<pair<int, int>> &eff_pairs,
    const vector<pair<int, int>> &effects_without_pre,
    vector<AbstractOperator> &operators,
    bool regression) {

    if (pos == static_cast<int>(effects_without_pre.size())) {
        // All effects without precondition have been checked: insert op.
        if (!eff_pairs.empty()) {
            operators.emplace_back(prev_pairs, pre_pairs, eff_pairs, op_id, cost,
                                   prop_hash_multipliers, regression);
        }
    } else {
        // For each possible value for the current variable, build an
        // abstract operator.
        int var_id = effects_without_pre[pos].first;
        int eff = effects_without_pre[pos].second;
        VariableProxy var = task_proxy->get_variables()[pattern.regular[var_id]];
        for (int i = 0; i < var.get_domain_size(); ++i) {
            if (i != eff) {
                pre_pairs.emplace_back(var_id, i);
                eff_pairs.emplace_back(var_id, eff);
            } else {
                prev_pairs.emplace_back(var_id, i);
            }
            multiply_out(pos + 1, op_id, cost, prev_pairs, pre_pairs, eff_pairs,
                         effects_without_pre, operators, regression);
            if (i != eff) {
                pre_pairs.pop_back();
                eff_pairs.pop_back();
            } else {
                prev_pairs.pop_back();
            }
        }
    }
}

void PatternDatabase::build_abstract_operators(
    const NumericOperatorProxy &op, ap_float cost,
    const std::vector<int> &variable_to_index,
    vector<AbstractOperator> &operators,
    bool regression) {

    // All variable value pairs that are a prevail condition
    vector<pair<int, int>> prev_pairs;
    // All variable value pairs that are a precondition (value != -1)
    vector<pair<int, int>> pre_pairs;
    // All variable value pairs that are an effect
    vector<pair<int, int>> eff_pairs;
    // All variable value pairs that are a precondition (value = -1)
    vector<pair<int, int>> effects_without_pre;

    size_t num_vars = task_proxy->get_variables().size();
    vector<bool> has_precond_and_effect_on_var(num_vars, false);
    vector<bool> has_precondition_on_var(num_vars, false);

    for (FactProxy pre : op.get_propositional_preconditions())
        has_precondition_on_var[pre.get_variable().get_id()] = true;

    for (EffectProxy eff : op.get_propositional_effects()) {
        int var_id = eff.get_fact().get_variable().get_id();
        int pattern_var_id = variable_to_index[var_id];
        if (pattern_var_id != -1) { // variable occurs in pattern
            int val = eff.get_fact().get_value();
            if (has_precondition_on_var[var_id]) {
                has_precond_and_effect_on_var[var_id] = true;
                eff_pairs.emplace_back(pattern_var_id, val);
            } else {
                effects_without_pre.emplace_back(pattern_var_id, val);
            }
        }
    }
    for (FactProxy pre : op.get_propositional_preconditions()) {
        int var_id = pre.get_variable().get_id();
        int pattern_var_id = variable_to_index[var_id];
        if (pattern_var_id != -1) { // variable occurs in pattern
            int val = pre.get_value();
            if (has_precond_and_effect_on_var[var_id]) {
                pre_pairs.emplace_back(pattern_var_id, val);
            } else {
                prev_pairs.emplace_back(pattern_var_id, val);
            }
        }
    }
    multiply_out(0, op.get_id(), cost, prev_pairs, pre_pairs, eff_pairs, effects_without_pre,
                 operators, regression);
}

bool PatternDatabase::is_applicable(const NumericState &state,
                                    const NumericOperatorProxy &op) const {
    for (const auto &num_pre : op.get_numeric_preconditions()){
        if (num_pre->is_constant()) {
            // TODO remove such preconditions from the op
            if (!num_pre->satisfied(0)) {
                return false;
            }
            continue;
        }
        int num_index = num_variable_to_index[num_pre->get_var_id()];
        if (num_index != -1){
            if (!num_pre->satisfied(state.num_state[num_index])){
                return false;
            }
        }
    }
    return true;
}

vector<ap_float> PatternDatabase::get_numeric_successor(vector<ap_float> state,
                                                        const NumericOperatorProxy &op) const {
    const vector<ap_float> &num_effs = task_proxy->get_action_eff_list(op.get_id());
    for (int var: pattern.numeric) {
        int num_index = num_variable_to_index[var];
        state[num_index] += num_effs[task_proxy->get_regular_var_id(var)];
    }
    for (auto &[var_id, value] : op.get_assign_effects()){
        int pattern_id = num_variable_to_index[var_id];
        if (pattern_id != -1){
            state[pattern_id] = value;
        }
    }
    return state;
}

void PatternDatabase::build_goals(const vector<int> &variable_to_index,
                                  const vector<int> &num_variable_to_index) {
    // compute abstract goal var-val pairs
    for (FactProxy goal: task_proxy->get_propositional_goals()) {
        int var_id = goal.get_variable().get_id();
        int val = goal.get_value();
        if (variable_to_index[var_id] != -1) {
            propositional_goals.emplace_back(variable_to_index[var_id], val);
        }
    }
    if (!pattern.numeric.empty()) {
        for (const auto &num_goal: task_proxy->get_numeric_goals()) {
            if (num_variable_to_index[num_goal.get_var_id()] != -1) {
                numeric_goals.push_back(num_goal);
            }
        }
    }
}

NumericState PatternDatabase::project_numeric_state(const NumericState &state,
                                                    const Pattern &superset_pattern,
                                                    const vector<size_t> &sup_hash_multipliers) const {
    assert(std::all_of(pattern.regular.begin(),
                       pattern.regular.end(),
                       [&superset_pattern] (int var) {
        return std::find(superset_pattern.regular.begin(),
                         superset_pattern.regular.end(),
                         var) != superset_pattern.regular.end();
    }));
    assert(std::all_of(pattern.numeric.begin(),
                       pattern.numeric.end(),
                       [&superset_pattern] (int var) {
                           return std::find(superset_pattern.numeric.begin(),
                                            superset_pattern.numeric.end(),
                                            var) != superset_pattern.numeric.end();
                       }));

    vector<int> prop_state(task_proxy->get_variables().size(), -1);
    for (size_t i = 0; i < superset_pattern.regular.size(); ++i){
        int var = superset_pattern.regular[i];
        int tmp = state.prop_hash / sup_hash_multipliers[i];
        int val = tmp % task_proxy->get_variables()[var].get_domain_size();
        prop_state[var] = val;
    }

    size_t prop_hash = 0;
    for (size_t i = 0; i < pattern.regular.size(); ++i) {
        int var = pattern.regular[i];
        assert(prop_state[var] >= 0 && prop_state[var] < task_proxy->get_variables()[var].get_domain_size());
        prop_hash += prop_hash_multipliers[i] * prop_state[var];
    }

    vector<ap_float> proj_num_state;
    for (int var : pattern.numeric) {
        auto it = find(superset_pattern.numeric.begin(), superset_pattern.numeric.end(), var);
        assert(it != superset_pattern.numeric.end());
        proj_num_state.push_back(state.num_state[it - superset_pattern.numeric.begin()]);
    }
    return {prop_hash, proj_num_state};
}

pair<bool, ap_float> PatternDatabase::compute_inner_h(InnerHeuristic h_type,
                                                      const NumericState &succ_state) const {
    ap_float h = 0;
    bool dead_end = false;
    switch (h_type) {
        case InnerHeuristic::LMCUT:
        {
            State proj_state = inner_h_task->get_projected_state(
                    unpack_prop_state(succ_state.prop_hash),
                    succ_state.num_state,
                    pattern);
            h = lmc->compute_heuristic(proj_state);
            cout << "HEURISTIC HERE!!!! " << h << endl;
            //cout << "LMCUT heuristic: " << h << endl;
            if (h == numeric_limits<ap_float>::min()){
                dead_end = true;
            }
            return {dead_end, h};
        }
        case InnerHeuristic::PDB:
        {
            if (pdb == nullptr) {
                return {false, 0};
            } 
            NumericState proj_state = pdb->project_numeric_state(succ_state,
                                                                 pattern,
                                                                 prop_hash_multipliers);
            h = pdb->compute_heuristic(proj_state).second;
            if (h == numeric_limits<ap_float>::max()){
                dead_end = true;
            }
            return {dead_end, h};
        }
        case InnerHeuristic::BLIND:
            return {false, 0};
        default:
            cerr << "ERROR: unknown inner heuristic type " << int(failed_lookup_h) << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
    }
}

void PatternDatabase::create_pdb(size_t max_number_states,
                                 std::optional<size_t> initial_state_opt,
                                 const std::vector<ap_float> &operator_costs,
                                 bool dump) {

    

    // TODO: implement specialized efficient variants for the nice cases, e.g.
    //  all numeric variables have an equality goal => we can do regression in this case,
    //  as there are finitely many abstract goal states.
    //     => this is not worth it with the current benchmarks; no domain falls into this special case

    // TODO: if we manage to exhaust the state space, it is probably more efficient to do perfect hashing, where we map
    //  the reached values of numeric variables to indices 0..N-1

    // TODO: we could try perfect hashing in all cases, where we sort reached numeric values such that the PDB vector
    //  is as dense as possible, and only having it just large enough to fit the abstract state with highest ID that has
    //  a finite heuristic value, with all others being deadends or mapped to min_action_cost by convention.

    NumericStateRegistry *tmp_state_registry;
    if (initial_state_opt.has_value()) {
        //cout << "Using initial state: " << initial_state_opt.value() << endl;
        tmp_state_registry = state_registry.get();
    } else {
        tmp_state_registry = new NumericStateRegistry();
    }

    VariablesProxy vars = task_proxy->get_variables();
    vector<int> variable_to_index(vars.size(), -1);
    for (size_t i = 0; i < pattern.regular.size(); ++i) {
        variable_to_index[pattern.regular[i]] = i;
    }
    ResNumericVariablesProxy num_vars = task_proxy->get_numeric_variables();
    num_variable_to_index = vector<int>(num_vars.size(), -1);
    for (size_t i = 0; i < pattern.numeric.size(); ++i) {
        num_variable_to_index[pattern.numeric[i]] = i;
    }

    construct_inner_heuristics(max_number_states, variable_to_index, operator_costs);


    AdaptiveQueue<size_t> pq;
    size_t original_distance_size = distances.size();
    assert(extend_abstract_state_space || original_distance_size == 0);
    // size 1 prevents segfault in Dijkstra loop in case no new states are reached
    vector<vector<pair<int, size_t>>> parent_pointers(1);

    {
        // compute all abstract operators
        vector<AbstractOperator> operators;
        vector<int> num_operators;
        for (NumericOperatorProxy op: task_proxy->get_operators()) {
            ap_float op_cost;
            if (operator_costs.empty()) {
                op_cost = op.get_cost();
            } else {
                op_cost = operator_costs[op.get_id()];
            }
            size_t size_before = operators.size();
            build_abstract_operators(op, op_cost, variable_to_index, operators, false);
            if (size_before == operators.size()) {
                // op does not affect a propositional variable in the pattern, check numeric variables
                const vector<ap_float> &effs = task_proxy->get_action_eff_list(op.get_id());
                for (int var: pattern.numeric) {
                    int regular_var_id = task_proxy->get_regular_var_id(var);
                    if (effs[regular_var_id] != 0) {
                        num_operators.push_back(op.get_id());
                        min_action_cost = min(min_action_cost, op_cost);
                        break;
                    }
                }
                for (const auto &[num_var, val]: op.get_assign_effects()) {
                    if (num_variable_to_index[num_var] != -1) {
                        num_operators.push_back(op.get_id());
                        min_action_cost = min(min_action_cost, op_cost);
                        break;
                    }
                }
            } else {
                min_action_cost = min(min_action_cost, op_cost);
            }
        }
        

        // build the match tree
        MatchTree match_tree(task_proxy, pattern, prop_hash_multipliers);
        for (const AbstractOperator &op: operators) {
            match_tree.insert(op);
        }

        build_goals(variable_to_index, num_variable_to_index);

        vector<bool> closed;
        vector<bool> is_open_or_closed(1, true);
        vector<size_t> goal_states;

        size_t num_reached_states = 0;

        // first implicit entry: priority, second entry: index for an abstract state
        AdaptiveQueue<pair<size_t, ap_float>> open;

        // initialize queue
        size_t init_state_id;
        if (initial_state_opt.has_value()) {
            init_state_id = initial_state_opt.value();
            assert(init_state_id != numeric_limits<size_t>::max());
            open.push(0, {init_state_id, 0});
        } else {
            size_t prop_init = 0;
            for (size_t i = 0; i < pattern.regular.size(); ++i) {
                prop_init += prop_hash_multipliers[i] * task_proxy->get_restricted_initial_state()[pattern.regular[i]];
            }
            vector<ap_float> num_init(pattern.numeric.size());
            for (int var: pattern.numeric) {
                num_init[num_variable_to_index[var]] = num_vars[var].get_initial_state_value();
            }

            init_state_id = tmp_state_registry->insert_state(NumericState(prop_init, std::move(num_init)));
            open.push(0, {init_state_id, 0});
        }

        parent_pointers.resize(tmp_state_registry->size());

        /*
         * A) forward exploration:
         *
         * 1) pop state s from open:                        (repeat until open is empty or limit on number of states is reached => number of states in open + closed)
         * 1.1) check if it's goal using method is_goal(s) => store goal states in some vector & don't expand them
         * 2) use MatchTree to obtain applicable operators as before (only propositional preconditions are checked in match tree)
         * 2.1) for mixed (propositional+numeric) operators: go over ops from 2) and check numeric precondition in s
         *      use pointer to original operator => remove ops not applicable in numeric part of s (check this using NumericHelper)
         * 2.2) add all purely numeric operators that are applicable in s (check this using NumericHelper)
         * => vector of applicable operators in s: app_ops
         * 3) apply app_ops to s
         * 3.1) propositional part: hash_effect from AbstractOperator
         * 3.2) numeric part: use the NumericHelper to apply numeric effects to numeric part of s
         * 4) add successor states s' to open list (check if they were previously closed, i.e. have some entry in closed list; possibly update parent)
         *    add s as parent node of s' in parent_pointers
         *
         *
         * B) distance computation: store result in distances
         *
         * 0.5) iterate over states in open and check if they are goal states; if yes, add to goal state vector
         * 1) start from vector of goal+fringe states (if open list not empty)
         * 1.1) put goal states into (new!) open with cost 0; move fringe states into open with cost *minimum action cost of entire task*
         * 2) pop state s from open
         * 3) follow parent pointers to compute cost for all states
         *
         */


        // we go beyond the state limit iff there are no 0-cost actions, need_goal is set, and no goal state has been reached, yet.
        ap_float goal_g = numeric_limits<ap_float>::max();
        ap_float last_cost = 0;
        while (!open.empty() && (num_reached_states < max_number_states || need_goal)) {

            if (need_goal && last_cost >= goal_g) {
                // stop when goal reached
                break;
            }
            if (need_goal && num_reached_states >= 10 * max_number_states) {
                // stop when we are way above the limit
                break;
            }

            auto [cost, state_pair] = open.pop();
            last_cost = cost;

            size_t state_id = state_pair.first;
            ap_float g_value = state_pair.second;

            assert(cost >= 0 && cost < numeric_limits<ap_float>::max());

            if (state_id >= closed.size()) {
                closed.resize(state_id + 1, false);
            } else if (closed[state_id]) {
                // we don't do duplicate checking in the open list
                continue;
            }

            closed[state_id] = true;

            const NumericState &state = tmp_state_registry->lookup_state(state_id);

            if (is_goal_state(state)) {
                goal_states.push_back(state_id);
                if (goal_g == numeric_limits<ap_float>::max() && need_goal) {
                    goal_g = cost;
                    // TODO this does not actually do what we want.
                    //  we need to be two *steps* behind the goal, not 2 * any cost
                    if (f_layer_offset_ratio > 0) {
                        goal_g += f_layer_offset_ratio * cost;
                    } else {
                        // TODO fix this hack
                        goal_g += abs(f_layer_offset_ratio) * min_action_cost;
                    }
                }
            }

            vector<const AbstractOperator *> applicable_operators;
            match_tree.get_applicable_operators(state.prop_hash, applicable_operators);

            for (auto abs_op: applicable_operators) {
                const auto &op = task_proxy->get_operators()[abs_op->get_op_id()];
                if (!is_applicable(state, op)) {
                    continue;
                }

                size_t prop_successor = state.prop_hash + abs_op->get_hash_effect();

                vector<ap_float> num_successor = get_numeric_successor(state.num_state,
                                                                       op);

                NumericState succ_state(prop_successor, std::move(num_successor));

                size_t succ_id = tmp_state_registry->insert_state(succ_state);

                if (succ_id == state_id) {
                    // no need to keep self-loops
                    continue;
                }

                if (parent_pointers.size() <= succ_id) {
                    parent_pointers.resize(succ_id + 1);
                }
                parent_pointers[succ_id].emplace_back(abs_op->get_op_id(), state_id);
                if (succ_id >= closed.size() || !closed[succ_id]) {
                    if (succ_id >= is_open_or_closed.size()){
                        is_open_or_closed.resize(succ_id + 1, false);
                    }
                    if (!is_open_or_closed[succ_id]) {
                        is_open_or_closed[succ_id] = true;
                        ++num_reached_states;
                    }

                    auto [dead_end, h] = compute_inner_h(exploration_h, succ_state);
                    if (!dead_end) {
                        open.push(g_value + abs_op->get_cost() + h, {succ_id, g_value + abs_op->get_cost()});
                    }
                }
            }
            for (auto op_id: num_operators) {
                const auto &op = task_proxy->get_operators()[op_id];
                if (!is_applicable(state, op)) {
                    continue;
                }

                vector<ap_float> num_successor = get_numeric_successor(state.num_state, op);

                NumericState succ_state(state.prop_hash, std::move(num_successor));

                size_t succ_id = tmp_state_registry->insert_state(succ_state);

                if (succ_id == state_id) {
                    // no need to keep self-loops
                    continue;
                }

                if (parent_pointers.size() <= succ_id) {
                    parent_pointers.resize(succ_id + 1);
                }
                parent_pointers[succ_id].emplace_back(op_id, state_id);
                if (succ_id >= closed.size() || !closed[succ_id]) {
                    if (succ_id >= is_open_or_closed.size()){
                        is_open_or_closed.resize(succ_id + 1, false);
                    }
                    if (!is_open_or_closed[succ_id]) {
                        is_open_or_closed[succ_id] = true;
                        ++num_reached_states;
                    }
                    ap_float op_cost;
                    if (operator_costs.empty()) {
                        op_cost = task_proxy->get_operators()[op_id].get_cost();
                    } else {
                        op_cost = operator_costs[op_id];
                    }

                    auto [dead_end, h] = compute_inner_h(exploration_h, succ_state);
                    if (!dead_end) {
                        open.push(g_value + op_cost + h, {succ_id, g_value + op_cost});
                    }
                }
            }
        }

        if (open.empty()) {
            exhausted_abstract_state_space = true;
        }

        if (!initial_state_opt.has_value()) {
            assert(distances.empty());
        }
        distances.resize(tmp_state_registry->size(), numeric_limits<ap_float>::max());

        for (const auto &goal_state_id: goal_states) {
            pq.push(0, goal_state_id);
        }

        size_t num_open_goal_states = 0;
        while (!open.empty()) {
            size_t state_id = open.pop().second.first;
            if (state_id < closed.size() && closed[state_id]) {
                // open lists may contain closed states
                continue;
            }
            const NumericState &state = tmp_state_registry->lookup_state(state_id);
            if (is_goal_state(state)) {
                // we have not checked this for states in open
                pq.push(0, state_id);
                num_open_goal_states++;
            } else {
                auto [dead_end, h] = compute_inner_h(frontier_h, state);
                if (state_id < original_distance_size) {
                    assert(extend_abstract_state_space);
                    h = max(distances[state_id], h);
                }
                h = max(h, min_action_cost);

                if (!dead_end) {
                    pq.push(h, state_id);
                }
            }
        }
        if (dump) {
            cout << "Generated abstract states: " << tmp_state_registry->size() << endl;
            cout << "Reached abstract goal states: " << goal_states.size() + num_open_goal_states << endl;
        }
    }

    size_t num_bwd_reached_states = 0;

    // Dijkstra loop
    while (!pq.empty()) {
        auto [distance, state_id] = pq.pop();
        assert(distance >= 0);
        if (distance >= distances[state_id]) {
            continue;
        }
        ++num_bwd_reached_states;
        distances[state_id] = distance;

        // regress state
        for (const auto &[op_id, parent_state_id] : parent_pointers[state_id]) {
            assert(state_id < parent_pointers.size());
            ap_float alternative_cost = distance;
            if (operator_costs.empty()) {
                alternative_cost += task_proxy->get_operators()[op_id].get_cost();
            } else {
                alternative_cost += operator_costs[op_id];
            }
            if (parent_state_id >= original_distance_size && alternative_cost < distances[parent_state_id]) {
                assert(alternative_cost >= 0);
                pq.push(alternative_cost, parent_state_id);
            }
        }
    }

    if (dump) {
        cout << "Number backwards reachable abstract states: " << num_bwd_reached_states << endl;
    }

    if (initial_state_opt.has_value()) {
        
    } else if (num_bwd_reached_states < 0.75 * tmp_state_registry->size()) {
        state_registry = make_unique<NumericStateRegistry>();
        size_t state_id = 0;
        for (size_t i = 0; i < distances.size(); ++i) {
            ap_float dist = distances[i];
            if (dist != numeric_limits<ap_float>::max()) {
                const NumericState &state = tmp_state_registry->lookup_state(i);
                state_registry->insert_state(state);
                distances[state_id++]  = dist;
            }
        }
        distances.resize(state_id);
        distances.shrink_to_fit();
        if (dump) {
            cout << "Shrink size of state registry from " << tmp_state_registry->size() << " to " << distances.size() << endl;
        }
        delete tmp_state_registry;
    } else {
        state_registry.reset(tmp_state_registry);
    }

    if (dump) {
        cout << "Initial state h: " << compute_heuristic(task_proxy->get_original_initial_state()).second << endl;
    }

    switch (failed_lookup_h) {
        case InnerHeuristic::LMCUT:
            pdb.reset();
            break;
        case InnerHeuristic::PDB:
            lmc.reset();
            break;
        case InnerHeuristic::BLIND:
            pdb.reset();
            lmc.reset();
            break;
        default:
            cerr << "ERROR: unknown inner heuristic type " << int(failed_lookup_h) << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
    }
}

void PatternDatabase::create_pdb_propositional(size_t size,
                                               const std::vector<ap_float> &operator_costs) {

    exhausted_abstract_state_space = true;

    VariablesProxy vars = task_proxy->get_variables();
    vector<int> variable_to_index(vars.size(), -1);
    for (size_t i = 0; i < pattern.regular.size(); ++i) {
        variable_to_index[pattern.regular[i]] = i;
    }

    // compute all abstract operators
    vector<AbstractOperator> operators;
    for (NumericOperatorProxy op : task_proxy->get_operators()) {
        ap_float op_cost;
        if (operator_costs.empty()) {
            op_cost = op.get_cost();
        } else {
            op_cost = operator_costs[op.get_id()];
        }
        build_abstract_operators(op, op_cost, variable_to_index, operators, true);
    }

    // build the match tree
    MatchTree match_tree(task_proxy, pattern, prop_hash_multipliers);
    for (const AbstractOperator &op : operators) {
        match_tree.insert(op);
    }

    build_goals(variable_to_index, vector<int>());

    distances.reserve(size);
    // first implicit entry: priority, second entry: index for an abstract state
    AdaptiveQueue<size_t> pq;

    // initialize queue
    for (size_t state_index = 0; state_index < size; ++state_index) {
        if (is_goal_state(NumericState(state_index, vector<ap_float>()))) {
            pq.push(0, state_index);
            distances.push_back(0);
        } else {
            distances.push_back(numeric_limits<ap_float>::max());
        }
    }

    // Dijkstra loop
    while (!pq.empty()) {
        pair<ap_float, size_t> node = pq.pop();
        ap_float distance = node.first;
        size_t state_index = node.second;
        if (distance > distances[state_index]) {
            continue;
        }

        // regress abstract_state
        vector<const AbstractOperator *> applicable_operators;
        match_tree.get_applicable_operators(state_index, applicable_operators);
        for (const AbstractOperator *op : applicable_operators) {
            size_t predecessor = state_index + op->get_hash_effect();
            ap_float alternative_cost = distances[state_index] + op->get_cost();
            if (alternative_cost < distances[predecessor]) {
                distances[predecessor] = alternative_cost;
                pq.push(alternative_cost, predecessor);
            }
        }
    }
}

bool PatternDatabase::is_goal_state(
        const NumericState &state) const {
    for (const pair<int, int> &abstract_goal : propositional_goals) {
        int pattern_var_id = abstract_goal.first;
        int var_id = pattern.regular[pattern_var_id];
        VariableProxy var = task_proxy->get_variables()[var_id];
        int temp = state.prop_hash / prop_hash_multipliers[pattern_var_id];
        int val = temp % var.get_domain_size();
        if (val != abstract_goal.second) {
            return false;
        }
    }
    for (const auto &num_goal : numeric_goals){
        int num_index = num_variable_to_index[num_goal.get_var_id()];
        assert(num_index >= 0 && static_cast<size_t>(num_index) < pattern.numeric.size());
        if (!num_goal.satisfied(state.num_state[num_index])){
            return false;
        }
    }
    return true;
}

bool PatternDatabase::is_abstract_goal_state(const State &state) const {
    for (const pair<int, int> &abstract_goal : propositional_goals) {
        int var_id = pattern.regular[abstract_goal.first];
        if (state[var_id].get_value() != abstract_goal.second) {
            return false;
        }
    }
    for (const auto &num_goal : numeric_goals){
        ap_float val = task_proxy->get_numeric_state_value(state, num_goal.get_var_id());
        if (!num_goal.satisfied(val)){
            return false;
        }
    }
    return true;
}

size_t PatternDatabase::prop_hash_index(const State &state) const {
    size_t index = 0;
    for (size_t i = 0; i < pattern.regular.size(); ++i) {
        index += prop_hash_multipliers[i] * state[pattern.regular[i]].get_value();
    }
    return index;
}

vector<int> PatternDatabase::unpack_prop_state(size_t prop_hash) const {
    vector<int> prop_state(pattern.regular.size(), -1);
    for (size_t i = 0; i < pattern.regular.size(); ++i) {
        int var_id = pattern.regular[i];
        VariableProxy var = task_proxy->get_variables()[var_id];
        int temp = prop_hash / prop_hash_multipliers[i];
        int val = temp % var.get_domain_size();
        prop_state[i] = val;
    }
    return prop_state;
}

const vector<ap_float> &PatternDatabase::get_abstract_numeric_state(const State &state) const {
    tmp_abstract_numeric_state.resize(pattern.numeric.size());
    for (size_t i = 0; i < pattern.numeric.size(); ++i){
        int var = pattern.numeric[i];
        ap_float val = task_proxy->get_numeric_state_value(state, var);
        tmp_abstract_numeric_state[i] = val;
    }
    return tmp_abstract_numeric_state;
}

pair<bool, ap_float> PatternDatabase::get_value(const State &state) {
    if (pattern.numeric.empty()){
        // purely propositional pattern
        return {true, distances[prop_hash_index(state)]};
    }

    NumericState abs_state = NumericState(prop_hash_index(state),
                                          get_abstract_numeric_state(state));
    size_t abs_state_id = state_registry->get_id(abs_state);
    if (abs_state_id == numeric_limits<size_t>::max()) {
        // we have not seen an abstract state that corresponds to state
        if (exhausted_abstract_state_space) {
            // here we can guarantee that state is indeed a deadend
            //cout << "Deadend" << endl;
            return {true, numeric_limits<ap_float>::max()};
        } else if (is_abstract_goal_state(state)) {
            // abstract goals are satisfied
            return {false, 0};
        } else {
            if (extend_abstract_state_space) {
                abs_state_id = state_registry->insert_state(abs_state);
                // TODO don't hard-code the limit here
                create_pdb(1000, abs_state_id);
                return {false, distances[abs_state_id]};
            }

            ap_float h = 0;
            switch (failed_lookup_h) {
                case InnerHeuristic::LMCUT:
                    h = compute_heuristic(abs_state).second;
                    break;
                case InnerHeuristic::PDB:
                {
                    NumericState proj_state = pdb->project_numeric_state(abs_state,
                                                                         pattern,
                                                                         prop_hash_multipliers);
                    h = pdb->get_value(proj_state).second;
                    break;
                }
                case InnerHeuristic::BLIND:
                    break;
                default:
                    cerr << "ERROR: unknown inner heuristic type " << int(failed_lookup_h) << endl;
                    utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
            }
            // we don't know any better
            return {false, max(h, min_action_cost)};
        }
    }
    return {true, distances[abs_state_id]};
}

pair<bool, ap_float> PatternDatabase::get_value(const NumericState &state) {
    if (pattern.numeric.empty()){
        // purely propositional pattern
        return {true, distances[state.prop_hash]};
    }
    size_t abs_state_id = state_registry->get_id(state);

    if (abs_state_id == numeric_limits<size_t>::max()) {
        // we have not seen an abstract state that corresponds to state
        if (exhausted_abstract_state_space) {
            // here we can guarantee that state is indeed a deadend
            return {true, numeric_limits<ap_float>::max()};
        }
        if (is_goal_state(state)) {
            // abstract goals are satisfied
            return {true, 0};
        } else {
            if (extend_abstract_state_space) {
                abs_state_id = state_registry->insert_state(state);
                // TODO don't hard-code the limit here
                create_pdb(1000, abs_state_id);
                return {false, distances[abs_state_id]};
            }
            // TODO implement failed_lookup_h here
            // we don't know any better
            return {false, min_action_cost};
        }
    }
    return {true, distances[abs_state_id]};
}

ap_float PatternDatabase::compute_mean_finite_h() const {
    cerr << "Not yet implemented: numeric PatternDatabase::compute_mean_finite_h()" << endl;
    utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
//    double sum = 0;
//    int size = 0;
//    for (size_t i = 0; i < distances.size(); ++i) {
//        if (distances[i] != numeric_limits<int>::max()) {
//            sum += distances[i];
//            ++size;
//        }
//    }
//    if (size == 0) { // All states are dead ends.
//        return numeric_limits<double>::infinity();
//    } else {
//        return sum / size;
//    }
}

bool PatternDatabase::is_operator_relevant(const OperatorProxy &op) const {
    cerr << "Not yet implemented: numeric PatternDatabase::is_operator_relevant()" << endl;
    utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
    for (EffectProxy effect : op.get_effects()) {
        int var_id = effect.get_fact().get_variable().get_id();
        if (binary_search(pattern.regular.begin(), pattern.regular.end(), var_id)) {
            return true;
        }
    }
    return false;
}
}
