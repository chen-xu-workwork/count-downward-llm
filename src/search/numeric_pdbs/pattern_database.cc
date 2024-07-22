#include "pattern_database.h"

#include "match_tree.h"
#include "numeric_condition.h"
#include "numeric_helper.h"

#include "../priority_queue.h"

#include "../utils/logging.h"
#include "../utils/math.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

using namespace std;
using namespace numeric_condition;
using namespace numeric_pdb_helper;

namespace numeric_pdbs {
AbstractOperator::AbstractOperator(const vector<pair<int, int>> &prev_pairs,
                                   const vector<pair<int, int>> &pre_pairs,
                                   const vector<pair<int, int>> &eff_pairs,
                                   int op_id,
                                   int cost,
                                   const vector<size_t> &hash_multipliers)
    : op_id(op_id),
      cost(cost),
      preconditions(prev_pairs) {
    preconditions.insert(preconditions.end(),
                         pre_pairs.begin(),
                         pre_pairs.end());
    // Sort preconditions for MatchTree construction.
    sort(preconditions.begin(), preconditions.end());
    for (size_t i = 1; i < preconditions.size(); ++i) {
        assert(preconditions[i].first !=
               preconditions[i - 1].first);
    }
    hash_effect = 0;
    assert(pre_pairs.size() == eff_pairs.size());
    for (size_t i = 0; i < eff_pairs.size(); ++i) {
        int var = eff_pairs[i].first;
        assert(var == eff_pairs[i].first);
        int old_val = pre_pairs[i].second;
        int new_val = eff_pairs[i].second;
        assert(new_val != -1);
        size_t effect = (new_val - old_val) * hash_multipliers[var];
        hash_effect += effect;
    }
}

void AbstractOperator::dump(const Pattern &pattern,
                            const TaskProxy &task_proxy) const {
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
    const TaskProxy &task_proxy,
    const Pattern &pattern,
    size_t max_number_states,
    bool dump,
    const vector<int> &operator_costs)
    : task_proxy(task_proxy),
      pattern(pattern),
      state_registry(make_unique<NumericStateRegistry>()),
      min_action_cost(numeric_limits<double>::max()),
      exhausted_abstract_state_space(false) {
    // verify_no_axioms(task_proxy); // TODO adapt the function to ignore the numeric axioms + dummy axioms
    verify_no_conditional_effects(task_proxy);
    assert(operator_costs.empty() ||
           operator_costs.size() == task_proxy.get_operators().size());
    assert(utils::is_sorted_unique(pattern.regular));
    assert(utils::is_sorted_unique(pattern.numeric));

    NumericTaskProxy num_task_proxy(task_proxy);

    utils::Timer timer;
    prop_hash_multipliers.reserve(pattern.regular.size());
    num_prop_states = 1;
    for (int pattern_var_id : pattern.regular) {
        prop_hash_multipliers.push_back(num_prop_states);
        VariableProxy var = task_proxy.get_variables()[pattern_var_id];
        if (utils::is_product_within_limit(num_prop_states, var.get_domain_size(),
                                           numeric_limits<int>::max())) {
            num_prop_states *= var.get_domain_size();
        } else {
            cerr << "Given pattern is too large only on propositional variables! (Overflow occured): " << endl;
            cerr << pattern.regular << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        }
    }
    create_pdb(num_task_proxy, max_number_states, operator_costs);
    if (dump)
        cout << "PDB construction time: " << timer << endl;
}

void PatternDatabase::multiply_out(
    int pos, int op_id, ap_float cost, vector<pair<int, int>> &prev_pairs,
    vector<pair<int, int>> &pre_pairs,
    vector<pair<int, int>> &eff_pairs,
    const vector<pair<int, int>> &effects_without_pre,
    vector<AbstractOperator> &operators) {

    if (pos == static_cast<int>(effects_without_pre.size())) {
        // All effects without precondition have been checked: insert op.
        if (!eff_pairs.empty()) {
            operators.emplace_back(prev_pairs, pre_pairs, eff_pairs, op_id, cost,
                                   prop_hash_multipliers);
        }
    } else {
        // For each possible value for the current variable, build an
        // abstract operator.
        int var_id = effects_without_pre[pos].first;
        int eff = effects_without_pre[pos].second;
        VariableProxy var = task_proxy.get_variables()[pattern.regular[var_id]];
        for (int i = 0; i < var.get_domain_size(); ++i) {
            if (i != eff) {
                pre_pairs.emplace_back(var_id, i);
                eff_pairs.emplace_back(var_id, eff);
            } else {
                prev_pairs.emplace_back(var_id, i);
            }
            multiply_out(pos + 1, op_id, cost, prev_pairs, pre_pairs, eff_pairs,
                         effects_without_pre, operators);
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
    const OperatorProxy &op, ap_float cost,
    const std::vector<int> &variable_to_index,
    vector<AbstractOperator> &operators) {

    // All variable value pairs that are a prevail condition
    vector<pair<int, int>> prev_pairs;
    // All variable value pairs that are a precondition (value != -1)
    vector<pair<int, int>> pre_pairs;
    // All variable value pairs that are an effect
    vector<pair<int, int>> eff_pairs;
    // All variable value pairs that are a precondition (value = -1)
    vector<pair<int, int>> effects_without_pre;

    size_t num_vars = task_proxy.get_variables().size();
    vector<bool> has_precond_and_effect_on_var(num_vars, false);
    vector<bool> has_precondition_on_var(num_vars, false);

    for (FactProxy pre : op.get_preconditions())
        has_precondition_on_var[pre.get_variable().get_id()] = true;

    for (EffectProxy eff : op.get_effects()) {
        int var_id = eff.get_fact().get_variable().get_id();
        int pattern_var_id = variable_to_index[var_id];
        int val = eff.get_fact().get_value();
        if (pattern_var_id != -1) { // variable occurs in pattern
            if (has_precondition_on_var[var_id]) {
                has_precond_and_effect_on_var[var_id] = true;
                eff_pairs.emplace_back(pattern_var_id, val);
            } else {
                effects_without_pre.emplace_back(pattern_var_id, val);
            }
        }
    }
    for (FactProxy pre : op.get_preconditions()) {
        int var_id = pre.get_variable().get_id();
        int pattern_var_id = variable_to_index[var_id];
        int val = pre.get_value();
        if (pattern_var_id != -1) { // variable occurs in pattern
            if (has_precond_and_effect_on_var[var_id]) {
                pre_pairs.emplace_back(pattern_var_id, val);
            } else {
                prev_pairs.emplace_back(pattern_var_id, val);
            }
        }
    }
    multiply_out(0, op.get_id(), cost, prev_pairs, pre_pairs, eff_pairs, effects_without_pre,
                 operators);
}

bool PatternDatabase::is_applicable(const NumericState &state,
                                    OperatorProxy op,
                                    NumericTaskProxy &num_task_proxy,
                                    const vector<int> &num_variable_to_index) const {
    for (auto pre : op.get_preconditions()){
        if (!task_proxy.is_derived_variable(pre.get_variable()) &&
                num_task_proxy.is_derived_numeric_variable(pre.get_variable())) {
            shared_ptr<RegularNumericCondition> num_pre = num_task_proxy.get_regular_numeric_condition(pre);
            int num_index = num_variable_to_index[num_pre->get_var_id()];
            if (num_index != -1){
                if (!num_pre->satisfied(state.num_state[num_index])){
//                    cout << "not applicable " << num_pre->get_name() << endl;
                    return false;
                }
            }
        }
    }
    return true;
}

vector<ap_float> PatternDatabase::get_numeric_successor(vector<ap_float> state,
                                                        int op_id,
                                                        NumericTaskProxy &num_task_proxy,
                                                        const vector<int> &num_variable_to_index) const {
    const vector<ap_float> &num_effs = num_task_proxy.get_action_eff_list(op_id);
    for (int var: pattern.numeric) {
        int num_index = num_variable_to_index[var];
        state[num_index] += num_effs[num_task_proxy.get_regular_var_id(var)];
    }
    return state;
}

void PatternDatabase::create_pdb(NumericTaskProxy &num_task_proxy,
                                 size_t max_number_states,
                                 const std::vector<int> &operator_costs) {

    // TODO: implement specialized efficient variants for the nice cases, e.g.
    //  1) no numeric variables => default to regular PatternDatabase
    //  2) no non-goal numeric variables => we can do regression in this case, as there are finitely many abstract goal states

    // TODO: if we manage to exhaust the state space, it is probably more efficient to do perfect hashing, where we map
    //  the reached values of numeric variables to indices 0..N-1

    // TODO: we could try perfect hashing in all cases, where we sort reached numeric values such that the PDB vector
    //  is as dense as possible, and only having it just large enough to fit the abstract state with highest ID that has
    //  a finite heuristic value, with all others being deadends or mapped to min_action_cost by convention.

    VariablesProxy vars = task_proxy.get_variables();
    vector<int> variable_to_index(vars.size(), -1);
    for (size_t i = 0; i < pattern.regular.size(); ++i) {
        variable_to_index[pattern.regular[i]] = i;
    }
    NumericVariablesProxy num_vars = task_proxy.get_numeric_variables();
    vector<int> num_variable_to_index(num_vars.size(), -1);
    for (size_t i = 0; i < pattern.numeric.size(); ++i) {
        num_variable_to_index[pattern.numeric[i]] = i;
    }

    // compute all abstract operators
    vector<AbstractOperator> operators;
    vector<int> num_operators;
    for (OperatorProxy op : task_proxy.get_operators()) {
        ap_float op_cost;
        if (operator_costs.empty()) {
            op_cost = op.get_cost();
        } else {
            op_cost = operator_costs[op.get_id()];
        }
        size_t size_before = operators.size();
        build_abstract_operators(op, op_cost, variable_to_index, operators);
        if (size_before == operators.size()){
            // op does not affect a propositional variable in the pattern, check numeric variables
            const vector<ap_float> &effs = num_task_proxy.get_action_eff_list(op.get_id());
            for (int var : pattern.numeric){
                int regular_var_id = num_task_proxy.get_regular_var_id(var);
                if (effs[regular_var_id] != 0){
//                    cout << "numeric operator: " << op.get_name() << endl;
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
    for (const AbstractOperator &op : operators) {
        match_tree.insert(op);
    }

    // compute abstract goal var-val pairs
    for (FactProxy goal : task_proxy.get_goals()) {
        int var_id = goal.get_variable().get_id();
        int val = goal.get_value();
        if (variable_to_index[var_id] != -1) {
            propositional_goals.emplace_back(variable_to_index[var_id], val);
        }
    }

    for (const shared_ptr<RegularNumericCondition> &num_goal : num_task_proxy.get_numeric_goals()){
        if (num_variable_to_index[num_goal->get_var_id()] != -1){
            numeric_goals.push_back(num_goal);
        }
    }

    vector<bool> closed;
    vector<vector<pair<int, size_t>>> parent_pointers;
    vector<size_t> goal_states;

    // first implicit entry: priority, second entry: index for an abstract state
    AdaptiveQueue<size_t> open;

    // initialize queue
    size_t prop_init = 0;
    for (size_t i = 0; i < pattern.regular.size(); ++i){
        prop_init += prop_hash_multipliers[i] * task_proxy.get_initial_state()[pattern.regular[i]].get_value();
    }
    vector<ap_float> num_init(pattern.numeric.size());
    for (int var : pattern.numeric){
        num_init[num_variable_to_index[var]] = num_vars[var].get_initial_state_value();
    }
    size_t init_state_id = state_registry->insert_state(NumericState(prop_init, std::move(num_init)));
    open.push(0, init_state_id);

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

    num_reached_states = 0;
    while (!open.empty() && num_reached_states < max_number_states){
        auto [cost, state_id] = open.pop();
        assert(cost >= 0 && cost < numeric_limits<ap_float>::max());

        if (state_id >= closed.size()) {
            closed.resize(state_id + 1, false);
        } else if (closed[state_id]) {
            // we don't do duplicate checking in the open list
            continue;
        }
        closed[state_id] = true;

        const NumericState &state = state_registry->lookup_state(state_id);

        if (is_goal_state(state, num_variable_to_index)){
            goal_states.push_back(state_id);
        }

        vector<const AbstractOperator *> applicable_operators;
        match_tree.get_applicable_operators(state.prop_hash, applicable_operators);

        for (auto op : applicable_operators){
            if (!is_applicable(state,
                               task_proxy.get_operators()[op->get_op_id()],
                               num_task_proxy,
                               num_variable_to_index)){
                continue;
            }

            size_t prop_successor = state.prop_hash + op->get_hash_effect();

            vector<ap_float> num_successor = get_numeric_successor(state.num_state,
                                                                   op->get_op_id(),
                                                                   num_task_proxy,
                                                                   num_variable_to_index);

            size_t succ_id = state_registry->insert_state(NumericState(prop_successor, std::move(num_successor)));
            if (parent_pointers.size() <= succ_id){
                parent_pointers.resize(succ_id + 1);
            }
            parent_pointers[succ_id].emplace_back(op->get_op_id(), state_id);
            if (succ_id >= closed.size() || !closed[succ_id]){
                ++num_reached_states;
                open.push(cost + op->get_cost(), succ_id);
            }
        }

        for (auto op_id : num_operators){
            if (!is_applicable(state,
                               task_proxy.get_operators()[op_id],
                               num_task_proxy,
                               num_variable_to_index)){
                continue;
            }

            vector<ap_float> num_successor = get_numeric_successor(state.num_state,
                                                                   op_id,
                                                                   num_task_proxy,
                                                                   num_variable_to_index);

            size_t succ_id = state_registry->insert_state(NumericState(state.prop_hash, std::move(num_successor)));
            if (parent_pointers.size() <= succ_id){
                parent_pointers.resize(succ_id + 1);
            }
            parent_pointers[succ_id].emplace_back(op_id, state_id);
            if (succ_id >= closed.size() || !closed[succ_id]){
                ++num_reached_states;
                ap_float op_cost = task_proxy.get_operators()[op_id].get_cost();
                open.push(cost + op_cost, succ_id);
            }
        }
    }

    vector<bool>().swap(closed); // save memory

    cout << "Reached abstract goal states: " << goal_states.size() << endl;
    cout << "Generated abstract states: " << num_reached_states << endl;

    assert(distances.empty());
    distances.resize(state_registry->size(), numeric_limits<ap_float>::max());
    AdaptiveQueue<size_t> pq;

    for (const auto &goal_state_id : goal_states) {
        pq.push(0, goal_state_id);
    }
    vector<size_t>().swap(goal_states); // save memory

    while (!open.empty()){
        size_t state_id = open.pop().second;
        const NumericState &state = state_registry->lookup_state(state_id);
        if (is_goal_state(state, num_variable_to_index)){
            // we have not checked this for states in open
            pq.push(0, state_id);
        } else {
            pq.push(min_action_cost, state_id);
        }
    }

    // Dijkstra loop
    while (!pq.empty()) {
        auto [distance, state_id] = pq.pop();
        assert(distance >= 0);
        if (distance >= distances[state_id]) {
            continue;
        }
        distances[state_id] = distance;

        // regress state
        for (const auto &[op_id, parent_state_id] : parent_pointers[state_id]) {
            ap_float alternative_cost = distance + task_proxy.get_operators()[op_id].get_cost();
            if (alternative_cost < distances[parent_state_id]) {
                pq.push(alternative_cost, parent_state_id);
            }
        }
    }

    if (num_reached_states < max_number_states) {
        exhausted_abstract_state_space = true;
    } else {
        for (size_t i = 0; i < distances.size(); ++i){
            // we cannot assume that unreached states are deadends
            if (distances[i] == numeric_limits<ap_float>::max()) {
                if (is_goal_state(state_registry->lookup_state(i), num_variable_to_index)) {
                    // abstract goals are satisfied
                    distances[i] = 0;
                } else {
                    // we don't know any better
                    distances[i] = min_action_cost;
                }
            }
        }
    }
}

bool PatternDatabase::is_goal_state(
        const NumericState &state,
        const vector<int> &num_variable_to_index) const {
    for (const pair<int, int> &abstract_goal : propositional_goals) {
        int pattern_var_id = abstract_goal.first;
        int var_id = pattern.regular[pattern_var_id];
        VariableProxy var = task_proxy.get_variables()[var_id];
        int temp = state.prop_hash / prop_hash_multipliers[pattern_var_id];
        int val = temp % var.get_domain_size();
        if (val != abstract_goal.second) {
            return false;
        }
    }
    for (const shared_ptr<RegularNumericCondition> &num_goal : numeric_goals){
        int num_index = num_variable_to_index[num_goal->get_var_id()];
        assert(num_index >= 0 && static_cast<size_t>(num_index) < pattern.numeric.size());
        if (!num_goal->satisfied(state.num_state[num_index])){
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
    for (const shared_ptr<RegularNumericCondition> &num_goal : numeric_goals){
        if (!num_goal->satisfied(state.nval(num_goal->get_var_id()))){
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

static vector<ap_float> abstract_numeric_state; // avoid re-allocation

const vector<ap_float> &PatternDatabase::get_abstract_numeric_state(const State &state) const {
    abstract_numeric_state.resize(pattern.numeric.size());
    for (size_t i = 0; i < pattern.numeric.size(); ++i){
        abstract_numeric_state[i] = state.nval(pattern.numeric[i]);
    }
    return abstract_numeric_state;
}

ap_float PatternDatabase::get_value(const State &state) const {
    size_t abs_state_id = state_registry->get_id(NumericState(prop_hash_index(state), get_abstract_numeric_state(state)));
    if (abs_state_id == numeric_limits<size_t>::max()) {
        // we have not seen an abstract state that corresponds to state
        if (exhausted_abstract_state_space) {
            // here we can guarantee that state is indeed a deadend
            return numeric_limits<ap_float>::max();
        } else if (is_abstract_goal_state(state)) {
            // abstract goals are satisfied
            return 0;
        } else {
            // we don't know any better
            return min_action_cost;
        }
    }
    return distances[abs_state_id];
}

double PatternDatabase::compute_mean_finite_h() const {
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
