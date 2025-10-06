#include "domain_abstraction_factory.h"

#include "domain_abstraction.h"
#include "match_tree.h"


#include "../tasks/root_task.h"

#include "../utils/math.h"
#include "../utils/logging.h"
#include "../utils/rng.h"

#include "../task_tools.h"
#include "match_tree.h"
#include "../priority_queue.h"


using namespace std;

namespace domain_abstractions {
AbstractOperator::AbstractOperator(const vector<Fact> &prev_pairs,
                                   const vector<Fact> &pre_pairs,
                                   const vector<Fact> &eff_pairs,
                                   const std::vector<NumAssProxy> &ass_effects,
                                   int cost,
                                   const vector<int> &hash_multipliers,
                                   const NumericDomainMappingType &numeric_domain_mapping,
                                   const vector<int> &numeric_domain_sizes,
                                   int concrete_op_id)
    : concrete_op_id(concrete_op_id),
      cost(cost),
      regression_preconditions(prev_pairs) {
    regression_preconditions.insert(regression_preconditions.end(),
                                    eff_pairs.begin(), eff_pairs.end());
    // Sort preconditions for MatchTree construction.
    sort(regression_preconditions.begin(), regression_preconditions.end());
    for (size_t i = 1; i < regression_preconditions.size(); ++i) {
        assert(regression_preconditions[i].var !=
               regression_preconditions[i - 1].var);
    }
    
    // Compute base hash effect for propositional variables
    int base_hash_effect = 0;
    assert(pre_pairs.size() == eff_pairs.size());
    for (size_t i = 0; i < pre_pairs.size(); ++i) {
        int var = pre_pairs[i].var;
        assert(var == eff_pairs[i].var);
        int old_val = eff_pairs[i].value;
        int new_val = pre_pairs[i].value;
        assert(new_val != -1);
        int effect = (new_val - old_val) * hash_multipliers[var];
        base_hash_effect += effect;
    }
    
    // Check if this operator has numeric effects
    bool has_numeric_effects = !ass_effects.empty();
    
    if (!has_numeric_effects || numeric_domain_mapping.empty()) {
        // Propositional-only operator: single hash effect
        hash_effects.push_back(base_hash_effect);
    } else {
        // Operator with numeric effects: compute all possible hash effects
        // corresponding to all possible partition transitions
        
        // Identify which numeric variables are affected
        vector<bool> affected_numeric_vars(numeric_domain_mapping.size(), false);
        for (const auto &ass_eff : ass_effects) {
            int num_var_id = ass_eff.get_affected_variable().get_id();
            if (num_var_id < static_cast<int>(numeric_domain_mapping.size())) {
                affected_numeric_vars[num_var_id] = true;
            }
        }
        
        // Enumerate all possible combinations of partition transitions
        // For each affected variable, we consider all possible target partitions
        function<void(size_t, int)> enumerate_effects = [&](size_t var_idx, int current_effect) {
            if (var_idx == numeric_domain_mapping.size()) {
                hash_effects.push_back(current_effect);
                return;
            }
            
            if (affected_numeric_vars[var_idx]) {
                // Try all possible target partitions for this affected variable
                int num_partitions = numeric_domain_sizes[var_idx];
                int hash_multiplier = hash_multipliers[pre_pairs.size() + var_idx];
                
                for (int target_partition = 0; target_partition < num_partitions; ++target_partition) {
                    // We don't know the source partition here, so we compute effects
                    // relative to partition 0. During regression, we'll adjust based on
                    // the actual state.
                    int effect_contribution = target_partition * hash_multiplier;
                    enumerate_effects(var_idx + 1, current_effect + effect_contribution);
                }
            } else {
                // Not affected: no contribution to hash effect from this variable
                enumerate_effects(var_idx + 1, current_effect);
            }
        };
        
        enumerate_effects(0, base_hash_effect);
    }
}

// Constructor with pre-computed hash effects (used by numeric helper)
AbstractOperator::AbstractOperator(
    const vector<Fact> &prev_pairs,
    const vector<Fact> &pre_pairs,
    const vector<Fact> &eff_pairs,
    const vector<NumAssProxy> &ass_effects,
    int cost,
    const vector<int> &pre_computed_hash_effects,
    int concrete_op_id)
    : concrete_op_id(concrete_op_id),
      cost(cost),
      hash_effects(pre_computed_hash_effects),
      regression_numeric_preconditions(ass_effects) {
    
    // Build regression preconditions from progression effects and prevail
    for (const Fact &prev : prev_pairs) {
        regression_preconditions.push_back(prev);
    }
    for (const Fact &eff : eff_pairs) {
        regression_preconditions.push_back(eff);
    }
    
    // Note: pre_pairs are preconditions in progression, not needed in regression
    // (they're handled by the effects already)
}

DomainAbstractionFactory::DomainAbstractionFactory (
    const TaskProxy &task_proxy,
    const DomainMapping &domain_mapping,
    const vector<int> &domain_sizes,
    const NumericDomainMappingType &numeric_domain_mapping,
    const vector<int> &numeric_domain_sizes,
    bool compute_plan,
    const shared_ptr<utils::RandomNumberGenerator> &rng,
    bool compute_wildcard_plan)
    : domain_mapping(domain_mapping),
      numeric_domain_mapping(numeric_domain_mapping) {
        verify_no_axioms(task_proxy);
        verify_no_conditional_effects(task_proxy);

    // Compute hash multipliers for propositional variables
    int num_variables = task_proxy.get_variables().size();
    hash_multipliers.reserve(num_variables + numeric_domain_mapping.size());
    num_states = 1;
    for (int var_id = 0; var_id < num_variables; ++var_id) {
        hash_multipliers.push_back(num_states);
        if (utils::is_product_within_limit(num_states, domain_sizes[var_id],
                                           numeric_limits<int>::max())) {
            num_states *= domain_sizes[var_id];
        } else {
            cerr << "Given domain mapping is too large! (Overflow occurred). "
                 << "Domain sizes: " << domain_sizes << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        }
    }
    
    // Add hash multipliers for numeric variables
    for (size_t i = 0; i < numeric_domain_mapping.size(); ++i) {
        hash_multipliers.push_back(num_states);
        int num_partitions = numeric_domain_sizes[i];
        if (utils::is_product_within_limit(num_states, num_partitions,
                                           numeric_limits<int>::max())) {
            num_states *= num_partitions;
        } else {
            cerr << "Domain abstraction with numeric variables is too large! (Overflow occurred)." << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        }
    }

    // Collect operators with numeric effects
    // These are handled separately during regression because they can
    // cause transitions between multiple abstract states
    if (!numeric_domain_mapping.empty()) {
        for (OperatorProxy op : task_proxy.get_operators()) {
            if (operator_has_numeric_effects(op)) {
                numeric_operators.push_back(op.get_id());
            }
        }
    }
    
    vector<AbstractOperator> operators =
        compute_abstract_operators(task_proxy, domain_sizes);
    MatchTree match_tree = build_match_tree(domain_sizes, operators);
    vector<Fact> abstract_goals = compute_abstract_goals(task_proxy);
    //TODO: add abstract numeric goals

    //TODO: next function assumes finite state space. 
    //That is crucial for our implementation of Dijkstra.
    // what we cannot do is, e.g., splitting into fixed intervals. 
    compute_distances(task_proxy, operators, match_tree, abstract_goals,
                      domain_sizes, compute_plan);
    if (compute_plan) {
        compute_abstract_plan(
            task_proxy, operators, match_tree, abstract_goals,
            domain_sizes, rng, compute_wildcard_plan);
    }
}

vector<AbstractOperator> DomainAbstractionFactory::compute_abstract_operators(
    const TaskProxy &task_proxy, const vector<int> &domain_sizes) {
    vector<AbstractOperator> operators;
    for (OperatorProxy op : task_proxy.get_operators()) {
        build_abstract_operators(op, task_proxy.get_variables().size(),
                                 domain_sizes, operators);
    }
    return operators;
}

//A match tree exists to compute applicaple operators, given a state
MatchTree DomainAbstractionFactory::build_match_tree(
    const vector<int> &domain_sizes,
    const vector<AbstractOperator> &operators) {
    MatchTree match_tree(domain_sizes, hash_multipliers);
    for (size_t op_id = 0; op_id < operators.size(); ++op_id) {
        const AbstractOperator &op = operators[op_id];
        match_tree.insert(op_id, op.get_regression_preconditions());
    }
    return match_tree;
}

//TODO: Support numeric goal states as well.
vector<Fact> DomainAbstractionFactory::compute_abstract_goals(
    const TaskProxy &task_proxy) {
    vector<Fact> abstract_goals;
    for (FactProxy goal : task_proxy.get_goals()) {
        int var_id = goal.get_variable().get_id();
        if (!variable_is_trivial(var_id)) {
            int val = goal.get_value();
            abstract_goals.emplace_back(var_id, domain_mapping[var_id][val]);
        }
    }
    return abstract_goals;
}

//Regression search to get lookup value for all abstract states, similar to PDBs.
void DomainAbstractionFactory::compute_distances(
    const TaskProxy &task_proxy,
    const vector<AbstractOperator> &operators, const MatchTree &match_tree,
    const vector<Fact> &abstract_goals, const vector<int> &domain_sizes,
    bool compute_plan) {
    distances.reserve(num_states);
    // first implicit entry: priority, second entry: index for an abstract state
    AdaptiveQueue<int> pq;

    // initialize queue
    //TODO: Add numeric vars here. Not trivial how to achieve that. 
    //Can we implement domain abstractions such that we have finite state spaces?
    for (int state_index = 0; state_index < num_states; ++state_index) {
        if (is_goal_state(state_index, abstract_goals, domain_sizes)) {
            pq.push(0, state_index);
            distances.push_back(0);
        } else {
            distances.push_back(numeric_limits<int>::max());
        }
    }

    if (compute_plan) {
        /*
          If computing a plan during Dijkstra, we store, for each state,
          an operator leading from that state to another state on a
          strongly optimal plan of the PDB. We store the first operator
          encountered during Dijkstra and only update it if the goal distance
          of the state was updated. Note that in the presence of zero-cost
          operators, this does not guarantee that we compute a strongly
          optimal plan because we do not minimize the number of used zero-cost
          operators.
         */
        generating_op_ids.resize(num_states);
    }

    //NOTE: looks like regression. Why not progression from inital state?
    // Dijkstra loop
    while (!pq.empty()) {
        pair<int, int> node = pq.pop();
        int distance = node.first;
        int state_index = node.second;
        if (distance > distances[state_index]) {
            continue;
        }

        // Regress using abstract operators (from match tree)
        // These handle both propositional-only and numeric operators
        vector<int> applicable_operator_ids;
        match_tree.get_applicable_operator_ids(state_index, applicable_operator_ids);
        for (int op_id : applicable_operator_ids) {
            const AbstractOperator &op = operators[op_id];
            int alternative_cost = distances[state_index] + op.get_cost();
            
            // Iterate over all possible hash effects (predecessors)
            // Propositional operators have 1 effect, numeric operators have multiple
            for (int hash_effect : op.get_hash_effects()) {
                int predecessor = state_index + hash_effect;
                if (alternative_cost < distances[predecessor]) {
                    distances[predecessor] = alternative_cost;
                    pq.push(alternative_cost, predecessor);
                    if (compute_plan) {
                        generating_op_ids[predecessor] = op_id;
                    }
                }
            }
        }
    }
}

void DomainAbstractionFactory::compute_abstract_plan(
    const TaskProxy &task_proxy,
    const vector<AbstractOperator> &operators,
    const MatchTree &match_tree,
    const vector<Fact> &abstract_goals,
    const vector<int> &domain_sizes,
    const shared_ptr<utils::RandomNumberGenerator> &rng,
    bool compute_wildcard_plan) {
    /*
      Using the generating operators computed during Dijkstra, we start
      from the initial state and follow the generating operator to the
      next state. Then we compute all operators of the same cost inducing
      the same abstract transition and randomly pick one of them to
      set for the next state. We iterate until reaching a goal state.
      Note that this kind of plan extraction does not uniformly at random
      consider all successor of a state but rather uses the arbitrarily
      chosen generating operator to settle on one successor state, which
      is biased by the number of operators leading to the same successor
      from the given state.
    */
    State initial_state = task_proxy.get_initial_state();

    vector<int> prop_state;
    for (int var_id = 0; var_id < initial_state.size(); ++var_id) {
        prop_state.push_back(initial_state[var_id].get_value());
    }
    //TODO: This state is propositional. Add numeric state as well.
    int current_state = hash_index(prop_state);

    if (distances[current_state] != numeric_limits<int>::max()) {
        while (!is_goal_state(current_state, abstract_goals, domain_sizes)) {
            int op_id = generating_op_ids[current_state];
            assert(op_id != -1);
            const AbstractOperator &op = operators[op_id];
            
            // TODO: With numeric operators having multiple hash effects, we need to
            // determine which specific effect was used. For now, use the first one.
            // This needs to be improved for proper plan extraction with numeric operators.
            int hash_effect = op.get_hash_effects()[0];
            int successor_state = current_state - hash_effect;

            // Compute equivalent ops
            vector<int> cheapest_operators;
            vector<int> applicable_operator_ids;
            match_tree.get_applicable_operator_ids(successor_state, applicable_operator_ids);
            for (int applicable_op_id : applicable_operator_ids) {
                const AbstractOperator &applicable_op = operators[applicable_op_id];
                // Check all hash effects of the applicable operator
                for (int applicable_hash_effect : applicable_op.get_hash_effects()) {
                    int predecessor = successor_state + applicable_hash_effect;
                    if (predecessor == current_state && op.get_cost() == applicable_op.get_cost()) {
                        cheapest_operators.emplace_back(applicable_op.get_concrete_op_id());
                        break; // Only add once per operator
                    }
                }
            }
            if (compute_wildcard_plan) {
                rng->shuffle(cheapest_operators);
                wildcard_plan.push_back(move(cheapest_operators));
            } else {
                int random_op_id = *rng->choose(cheapest_operators);
                wildcard_plan.emplace_back();
                wildcard_plan.back().push_back(random_op_id);
            }

            current_state = successor_state;
        }
    }
    utils::release_vector_memory(generating_op_ids);
}

//NOTE: required for regression. What happens here?
// Consider concrete operators with effect x = 1 and no(!) precondition. 
// Assume domain(x) = {0, 1, 2}.
// Then, we add the following abstract operators: 
// OP 1: pre = {x = 0}, eff = {x = 1}
// OP 1: pre = {x = 2}, eff = {x = 1}
// more efficient that way. 
void DomainAbstractionFactory::multiply_out(
    int pos, int cost, vector<Fact> &prev_pairs,
    vector<Fact> &pre_pairs,
    vector<Fact> &eff_pairs,
    const vector<Fact> &effects_without_pre,
    const vector<NumAssProxy> &ass_effects,
    int concrete_op_id,
    const vector<int> &domain_sizes,
    vector<AbstractOperator> &operators) {
    if (pos == static_cast<int>(effects_without_pre.size())) {
        // All effects without precondition have been checked: insert op.
        if (!eff_pairs.empty()) {
            // Compute numeric_domain_sizes from numeric_domain_mapping
            vector<int> numeric_domain_sizes;
            for (const auto &ndm : numeric_domain_mapping) {
                numeric_domain_sizes.push_back(ndm.get_num_partitions());
            }
            
            operators.push_back(AbstractOperator(
                prev_pairs, pre_pairs, eff_pairs, ass_effects, cost,
                hash_multipliers, numeric_domain_mapping, numeric_domain_sizes, concrete_op_id));
        }
    } else {
        // For each possible value for the current variable, build an
        // abstract operator.
        int var_id = effects_without_pre[pos].var;
        int eff = effects_without_pre[pos].value;
        for (int i = 0; i < domain_sizes[var_id]; ++i) {
            if (i != eff) {
                pre_pairs.emplace_back(var_id, i);
                eff_pairs.emplace_back(var_id, eff);
            } else {
                prev_pairs.emplace_back(var_id, i);
            }
            multiply_out(pos + 1, cost, prev_pairs, pre_pairs, eff_pairs,
                         effects_without_pre, ass_effects, concrete_op_id, domain_sizes,
                         operators);
            if (i != eff) {
                pre_pairs.pop_back();
                eff_pairs.pop_back();
            } else {
                prev_pairs.pop_back();
            }
        }
    }
}

//NOTE: In case you wonder what the prevail pairs are: 
// used for regression. Basically saying what variables stay equal. 
// during regression, strange things can happen if we don't have that. 
void DomainAbstractionFactory::build_abstract_operators(
    const OperatorProxy &op,
    int num_variables,
    const vector<int> &domain_sizes,
    vector<AbstractOperator> &operators) {
    // All variable value pairs that are a prevail condition
    vector<Fact> prev_pairs;
    // All variable value pairs that are a precondition (value != -1)
    vector<Fact> pre_pairs;
    // All variable value pairs that are an effect
    vector<Fact> eff_pairs;
    // All variable value pairs that are a precondition (value = -1)
    vector<Fact> effects_without_pre;

    vector<int> has_precondition_on_var(num_variables, -1);
    vector<int> has_effect_on_var(num_variables, -1);

    for (FactProxy pre : op.get_preconditions()) {
        int var_id = pre.get_variable().get_id();
        if (variable_is_trivial(var_id)) {
            has_precondition_on_var[var_id] = 0;
        } else {
            has_precondition_on_var[var_id] =
                domain_mapping[var_id][pre.get_value()];
        }
    }

    for (EffectProxy eff : op.get_effects()) {
        int var_id = eff.get_fact().get_variable().get_id();
        if (!variable_is_trivial(var_id)) {
            int val = domain_mapping[var_id][eff.get_fact().get_value()];
            //NOTE: Collect effects only they dont have themself as precon
            int pre_val = has_precondition_on_var[var_id];
            if (pre_val < 0) {
                effects_without_pre.emplace_back(var_id, val);
            } else if (pre_val != val) {
                has_effect_on_var[var_id] = val;
                eff_pairs.emplace_back(var_id, val);
            }
        }
    }
    for (FactProxy pre : op.get_preconditions()) {
        int var_id = pre.get_variable().get_id();
        if (!variable_is_trivial(var_id)) {
            int val = domain_mapping[var_id][pre.get_value()];
            if (has_effect_on_var[var_id] >= 0) {
                pre_pairs.emplace_back(var_id, val);
            } else {
                prev_pairs.emplace_back(var_id, val);
            }
        }
    }
    
    // Collect numeric effects (assignment effects)
    vector<NumAssProxy> ass_effects;
    for (auto ass_eff : op.get_ass_effects()) {
        ass_effects.push_back(ass_eff.get_assignment());
    }
    
    multiply_out(0, op.get_cost(), prev_pairs, pre_pairs, eff_pairs,
                 effects_without_pre, ass_effects, op.get_id(), domain_sizes, operators);
}

//TODO: Does not support numeric (goal) states yet. 
bool DomainAbstractionFactory::is_goal_state(
    int state_index,
    const vector<Fact> &abstract_goals,
    const vector<int> &domain_sizes) const {
    for (const Fact &abstract_goal : abstract_goals) {
        int var_id = abstract_goal.var;
        int temp = state_index / hash_multipliers[var_id];
        int val = temp % domain_sizes[var_id];
        if (val != abstract_goal.value) {
            return false;
        }
    }
    return true;
}

// state compression for propositional states. 
int DomainAbstractionFactory::hash_index(const vector<int> &state) const {
    int index = 0;
    for (size_t i = 0; i < state.size(); ++i) {
        if (!variable_is_trivial(i)) {
            index += hash_multipliers[i] * domain_mapping[i][state[i]];
        }
    }
    return index;
}

bool DomainAbstractionFactory::variable_is_trivial(int var_id) const {
    return domain_mapping[var_id].empty();
}

bool DomainAbstractionFactory::operator_has_numeric_effects(const OperatorProxy &op) const {
    // Check if operator has any numeric effects (assignment or additive)
    if (!numeric_domain_mapping.empty()) {
        for (auto eff : op.get_ass_effects()) {
            // If any numeric variable in the abstraction is affected, return true
            int num_var_id = eff.get_assignment().get_affected_variable().get_id();
            if (num_var_id < static_cast<int>(numeric_domain_mapping.size())) {
                return true;
            }
        }
    }
    return false;
}

std::vector<int> DomainAbstractionFactory::compute_abstract_numeric_predecessors(
    int state_index,
    const OperatorProxy &op,
    const vector<int> &domain_sizes) const {
    /*
      Given an abstract state (state_index) and an operator with numeric effects,
      compute all possible abstract predecessor states.
      
      For regression: predecessor = state where we'd need to apply op to reach state_index
      
      The challenge: numeric effects like x += 2 can cause transitions between partitions.
      Example: If state has x in partition [0, inf), and op does x += 2,
               predecessors could have x in [-inf, 0) or [0, inf) depending on the exact value.
      
      For now, we use a conservative approach: we consider ALL partitions as possible predecessors
      for numeric variables affected by the operator.
    */
    vector<int> predecessors;
    
    // Extract abstract state for both propositional and numeric variables
    vector<int> prop_vals(domain_mapping.size());
    for (size_t i = 0; i < domain_mapping.size(); ++i) {
        if (!variable_is_trivial(i)) {
            int temp = state_index / hash_multipliers[i];
            prop_vals[i] = temp % domain_sizes[i];
        }
    }
    
    vector<int> num_partitions(numeric_domain_mapping.size());
    for (size_t i = 0; i < numeric_domain_mapping.size(); ++i) {
        int temp = state_index / hash_multipliers[domain_mapping.size() + i];
        int domain_size_at_i = domain_sizes[domain_mapping.size() + i];
        num_partitions[i] = temp % domain_size_at_i;
    }
    
    // Identify which numeric variables are affected by this operator
    vector<bool> affected_numeric_vars(numeric_domain_mapping.size(), false);
    for (auto eff : op.get_ass_effects()) {
        int num_var_id = eff.get_assignment().get_affected_variable().get_id();
        if (num_var_id < static_cast<int>(numeric_domain_mapping.size())) {
            affected_numeric_vars[num_var_id] = true;
        }
    }
    
    // For each affected numeric variable, we need to consider all possible predecessor partitions
    // This is a conservative overapproximation
    function<void(size_t, int)> enumerate_predecessors = [&](size_t var_idx, int current_pred) {
        if (var_idx == numeric_domain_mapping.size()) {
            predecessors.push_back(current_pred);
            return;
        }
        
        if (affected_numeric_vars[var_idx]) {
            // Try all possible partitions for this affected variable
            int num_partitions_for_var = domain_sizes[domain_mapping.size() + var_idx];
            for (int partition = 0; partition < num_partitions_for_var; ++partition) {
                int pred_with_partition = current_pred + hash_multipliers[domain_mapping.size() + var_idx] * partition;
                enumerate_predecessors(var_idx + 1, pred_with_partition);
            }
        } else {
            // Not affected: keep the same partition
            int pred_with_partition = current_pred + hash_multipliers[domain_mapping.size() + var_idx] * num_partitions[var_idx];
            enumerate_predecessors(var_idx + 1, pred_with_partition);
        }
    };
    
    // Start with propositional part
    int prop_predecessor = 0;
    for (size_t i = 0; i < domain_mapping.size(); ++i) {
        if (!variable_is_trivial(i)) {
            prop_predecessor += hash_multipliers[i] * prop_vals[i];
        }
    }
    
    enumerate_predecessors(0, prop_predecessor);
    return predecessors;
}

DomainAbstraction DomainAbstractionFactory::generate() {
    return DomainAbstraction(move(domain_mapping), move(numeric_domain_mapping),
                             move(hash_multipliers), move(distances), move(wildcard_plan));
}
}
