#include "domain_abstraction_factory.h"

#include "domain_abstraction.h"
#include "match_tree.h"
#include "numeric_helper.h"


#include "../tasks/root_task.h"
#include "../globals.h"

#include "../utils/math.h"
#include "../utils/logging.h"
#include "../utils/rng.h"

#include "../task_tools.h"
#include "match_tree.h"
#include "../priority_queue.h"

#include <sstream>


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
    // In regression: we need the post-state (effects) as preconditions
    regression_preconditions.reserve(prev_pairs.size() + eff_pairs.size());
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
      numeric_domain_mapping(numeric_domain_mapping),
      numeric_domain_sizes(numeric_domain_sizes) {
        verify_no_non_numeric_axioms(task_proxy);
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
    cout << "DEBUG FACTORY: Building abstract operators with " 
         << numeric_domain_sizes.size() << " numeric variables" << endl;
    int total_numeric_partitions = 0;
    for (int ns : numeric_domain_sizes) {
        total_numeric_partitions += ns;
    }
    cout << "DEBUG FACTORY: Total numeric partitions across all variables: " 
         << total_numeric_partitions << endl;
    
    // Create numeric helper to handle all operator construction
    // The helper handles both propositional and numeric effects, including cascades
    DomainAbstractionNumericHelper helper(
        g_root_task(),
        domain_mapping,
        numeric_domain_mapping,
        domain_sizes,
        numeric_domain_sizes,
        hash_multipliers);
    
    // Let the helper build all abstract operators
    return helper.build_abstract_operators(task_proxy);
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

// Helper function to check if a variable is derived (appears in axiom effects)
static bool is_derived_variable(const TaskProxy &task_proxy, int var_id) {
    for (OperatorProxy ax : task_proxy.get_axioms()) {
        for (EffectProxy eff : ax.get_effects()) {
            if (eff.get_fact().get_variable().get_id() == var_id) {
                return true;
            }
        }
    }
    return false;
}

// Compute abstract goals, including goals compiled into goal axioms.
// Numeric goals in numeric FD are sometimes compiled into a goal axiom,
// and we need to extract them from the axiom preconditions.
// Helper: Find which comparison axiom achieves a given fact
static int get_achieving_comp_axiom(const TaskProxy &proxy, const FactProxy &condition) {
    for (auto op : proxy.get_comparison_axioms()) {
        if (condition.get_variable() == op.get_true_fact().get_variable()) {
            return op.get_id();
        }
    }
    return -1;
}

vector<Fact> DomainAbstractionFactory::compute_abstract_goals(
    const TaskProxy &task_proxy) {
    vector<Fact> abstract_goals;
    
    cout << "DEBUG GOALS: Computing abstract goals..." << endl;
    cout << "DEBUG GOALS: Direct task goals: " << task_proxy.get_goals().size() << endl;
    cout << "DEBUG GOALS: Comparison axioms: " << task_proxy.get_comparison_axioms().size() << endl;
    cout << "DEBUG GOALS: Numeric domain mapping size: " << numeric_domain_mapping.size() << endl;
    
    // First, collect non-derived goals directly
    for (FactProxy goal : task_proxy.get_goals()) {
        int var_id = goal.get_variable().get_id();
        cout << "DEBUG GOALS:   Goal var" << var_id << "=" << goal.get_value() 
             << " (derived=" << (is_derived_variable(task_proxy, var_id) ? "yes" : "no")
             << ", trivial=" << (variable_is_trivial(var_id) ? "yes" : "no") << ")" << endl;
        
        // Add goal if it has a domain mapping (even if derived!)
        // Derived variables like var25 (goal axiom) can have domain mappings from initial splits
        if (!variable_is_trivial(var_id)) {
            int val = goal.get_value();
            abstract_goals.emplace_back(var_id, domain_mapping[var_id][val]);
            cout << "DEBUG GOALS:     Added to abstract_goals: var" << var_id 
                 << "=" << domain_mapping[var_id][val] << " (abstract value)" << endl;
        }
    }
    
    // Reconstruct goals from goal axioms (numeric goals are compiled into axioms)
    // There should be at most two axioms: one dummy axiom (no preconditions),
    // and one optional goal axiom that encodes numeric/propositional goals
    assert(task_proxy.get_axioms().size() <= 2);
    
    cout << "DEBUG GOALS: Checking axioms for goal conditions..." << endl;
    for (OperatorProxy axiom : task_proxy.get_axioms()) {
        cout << "DEBUG GOALS:   Axiom has " << axiom.get_preconditions().size() 
             << " preconditions, " << axiom.get_effects().size() << " effects" << endl;
        // Goal axioms have preconditions and exactly one effect
        if (!axiom.get_preconditions().empty() && axiom.get_effects().size() == 1) {
            cout << "DEBUG GOALS:   This is a goal axiom - extracting preconditions as goals" << endl;
            // The preconditions of this goal axiom are the actual goals
            for (FactProxy pre : axiom.get_preconditions()) {
                int var_id = pre.get_variable().get_id();
                cout << "DEBUG GOALS:     Goal axiom precondition: var" << var_id << "=" << pre.get_value()
                     << " (trivial=" << (variable_is_trivial(var_id) ? "yes" : "no") << ")" << endl;
                
                // Check if this is a comparison axiom variable (derived numeric condition)
                cout << "DEBUG GOALS:       Looking for comparison axiom..." << endl;
                int c_axiom_id = get_achieving_comp_axiom(task_proxy, pre);
                cout << "DEBUG GOALS:       c_axiom_id=" << c_axiom_id << endl;
                
                if (c_axiom_id != -1) {
                    // This is a comparison axiom - extract the numeric condition
                    cout << "DEBUG GOALS:       Found comparison axiom " << c_axiom_id << endl;
                    ComparisonAxiomProxy c_axiom = task_proxy.get_comparison_axioms()[c_axiom_id];
                    
                    NumericVariableProxy left_var = c_axiom.get_left_variable();
                    NumericVariableProxy right_var = c_axiom.get_right_variable();
                    comp_operator op = c_axiom.get_comparison_operator_type();
                    
                    cout << "DEBUG GOALS:       This is a comparison axiom goal!" << endl;
                    cout << "DEBUG GOALS:       Left: " << left_var.get_name() 
                         << " (type=" << (int)left_var.get_var_type() << ")" << endl;
                    cout << "DEBUG GOALS:       Op: " << (int)op << endl;
                    cout << "DEBUG GOALS:       Right: " << right_var.get_name() 
                         << " (type=" << (int)right_var.get_var_type() << ")" << endl;
                    
                    // Handle simple case: one numeric variable compared to a constant
                    // Format: var OP constant
                    if (left_var.get_var_type() == numType::regular && 
                        right_var.get_var_type() == numType::constant) {
                        int num_var_id = left_var.get_id();
                        ap_float constant = right_var.get_initial_state_value();
                        numeric_goal_conditions.emplace_back(num_var_id, op, constant);
                        cout << "DEBUG GOALS:       Added numeric goal: var" << num_var_id 
                             << " " << (int)op << " " << constant << endl;
                    }
                    // Format: constant OP var => need to flip operator
                    else if (left_var.get_var_type() == numType::constant && 
                             right_var.get_var_type() == numType::regular) {
                        int num_var_id = right_var.get_id();
                        ap_float constant = left_var.get_initial_state_value();
                        // Flip operator: c < v becomes v > c, etc.
                        comp_operator flipped_op;
                        if (op == comp_operator::lt) flipped_op = comp_operator::gt;
                        else if (op == comp_operator::le) flipped_op = comp_operator::ge;
                        else if (op == comp_operator::eq) flipped_op = comp_operator::eq;
                        else if (op == comp_operator::ge) flipped_op = comp_operator::le;
                        else if (op == comp_operator::gt) flipped_op = comp_operator::lt;
                        else flipped_op = op;
                        numeric_goal_conditions.emplace_back(num_var_id, flipped_op, constant);
                        cout << "DEBUG GOALS:       Added numeric goal (flipped): var" << num_var_id 
                             << " " << (int)flipped_op << " " << constant << endl;
                    }
                    else {
                        cout << "DEBUG GOALS:       WARNING: Complex numeric goal (var-var comparison), skipping" << endl;
                    }
                }
                else if (!variable_is_trivial(var_id)) {
                    // Regular propositional goal
                    int val = pre.get_value();
                    abstract_goals.emplace_back(var_id, domain_mapping[var_id][val]);
                    cout << "DEBUG GOALS:       Added to abstract_goals: var" << var_id 
                         << "=" << domain_mapping[var_id][val] << " (abstract value)" << endl;
                }
            }
        }
    }
    
    cout << "DEBUG GOALS: Total propositional abstract goals: " << abstract_goals.size() << endl;
    for (const Fact &goal : abstract_goals) {
        cout << "DEBUG GOALS:   var" << goal.var << "=" << goal.value << endl;
    }
    
    cout << "DEBUG GOALS: Total numeric goal conditions: " << numeric_goal_conditions.size() << endl;
    for (const auto &cond : numeric_goal_conditions) {
        cout << "DEBUG GOALS:   num_var" << cond.numeric_var_id 
             << " " << (int)cond.op << " " << cond.constant << endl;
    }
    
    return abstract_goals;
}

// Helper function to decode an abstract state index into its variable values
string decode_abstract_state(int state_index, const vector<int> &domain_sizes,
                              const NumericDomainMappingType &numeric_domain_mapping,
                              const vector<int> &hash_multipliers) {
    stringstream ss;
    ss << "State " << state_index << ": [";
    
    // Decode propositional variables
    int remaining = state_index;
    for (size_t var_id = 0; var_id < domain_sizes.size(); ++var_id) {
        int multiplier = hash_multipliers[var_id];
        int value = (remaining / multiplier) % domain_sizes[var_id];
        ss << "v" << var_id << "=" << value;
        if (var_id < domain_sizes.size() - 1 || !numeric_domain_mapping.empty()) {
            ss << ", ";
        }
    }
    
    // Decode numeric variables (partitions)
    for (size_t num_var_id = 0; num_var_id < numeric_domain_mapping.size(); ++num_var_id) {
        int multiplier_idx = domain_sizes.size() + num_var_id;
        int multiplier = hash_multipliers[multiplier_idx];
        int num_partitions = numeric_domain_mapping[num_var_id].get_num_partitions();
        int partition = (remaining / multiplier) % num_partitions;
        
        ss << "num" << num_var_id << "=p" << partition;
        if (num_var_id < numeric_domain_mapping.size() - 1) {
            ss << ", ";
        }
    }
    
    ss << "]";
    return ss.str();
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
    cout << "DEBUG DIJKSTRA: Checking which abstract states are goals (total states: " << num_states << ")" << endl;
    int first_goal_state = -1;
    for (int state_index = 0; state_index < num_states; ++state_index) {
        bool is_goal = is_goal_state(state_index, abstract_goals, domain_sizes);
        if (state_index < 20 || is_goal) {  // Print first 20 states or any goal states
            string decoded = decode_abstract_state(state_index, domain_sizes, 
                                                  numeric_domain_mapping, hash_multipliers);
            cout << "DEBUG DIJKSTRA: " << decoded << " is " 
                 << (is_goal ? "GOAL" : "not goal") << endl;
            if (is_goal && first_goal_state == -1) {
                first_goal_state = state_index;
            }
        }
        if (is_goal) {
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
    int dijkstra_iterations = 0;
    int total_expansions = 0;
    bool first_goal_expanded = false;
    while (!pq.empty()) {
        pair<int, int> node = pq.pop();
        int distance = node.first;
        int state_index = node.second;
        dijkstra_iterations++;
        if (distance > distances[state_index]) {
            continue;
        }
        
        // Special detailed debugging for first goal state expansion
        bool is_first_goal_expansion = (state_index == first_goal_state && !first_goal_expanded);
        if (is_first_goal_expansion) {
            first_goal_expanded = true;
            cout << "\n========== DETAILED DEBUG: FIRST GOAL STATE EXPANSION ==========" << endl;
            string decoded = decode_abstract_state(state_index, domain_sizes, 
                                                  numeric_domain_mapping, hash_multipliers);
            cout << "Expanding: " << decoded << endl;
            cout << "Distance: " << distance << endl;
        }

        // Regress using abstract operators (from match tree)
        // These handle both propositional-only and numeric operators
        vector<int> applicable_operator_ids;
        match_tree.get_applicable_operator_ids(state_index, applicable_operator_ids);
        
        if (is_first_goal_expansion) {
            cout << "Applicable operators: " << applicable_operator_ids.size() << endl;
        }
        
        if (dijkstra_iterations <= 10 && applicable_operator_ids.size() > 0) {
            cout << "DEBUG DIJKSTRA: Iteration " << dijkstra_iterations 
                 << ", expanding state " << state_index 
                 << " with distance " << distance
                 << ", applicable ops: " << applicable_operator_ids.size() << endl;
        }
        
        int valid_predecessors_this_state = 0;
        int out_of_bounds_predecessors = 0;
        int operators_checked = 0;
        for (int op_id : applicable_operator_ids) {
            const AbstractOperator &op = operators[op_id];
            int alternative_cost = distances[state_index] + op.get_cost();
            
            if (is_first_goal_expansion && operators_checked < 5) {
                cout << "  Operator " << op_id << " (concrete_id=" << op.get_concrete_op_id() 
                     << ", cost=" << op.get_cost() << ")" << endl;
                operators_checked++;
            }
            
            // Iterate over all possible hash effects (predecessors)
            // Propositional operators have 1 effect, numeric operators have multiple
            const vector<int> &hash_effects_vec = op.get_hash_effects();
            if (dijkstra_iterations == 1 && op_id == 0) {
                cout << "DEBUG DIJKSTRA:   Op 0 has " << hash_effects_vec.size() << " hash effects: ";
                for (int he : hash_effects_vec) {
                    cout << he << " ";
                }
                cout << endl;
            }
            
            if (is_first_goal_expansion && operators_checked <= 5) {
                cout << "    Hash effects (" << hash_effects_vec.size() << "): ";
                int shown = 0;
                for (int he : hash_effects_vec) {
                    if (shown < 10) {
                        cout << he << " ";
                        shown++;
                    }
                }
                if (hash_effects_vec.size() > 10) cout << "...";
                cout << endl;
            }
            
            int predecessors_this_op = 0;
            int out_of_bounds_this_op = 0;
            for (int hash_effect : hash_effects_vec) {
                int predecessor = state_index + hash_effect;
                
                // Skip predecessors that are out of bounds
                // This can happen when hash effects are computed for all possible
                // partition transitions, but the actual reachable state space is
                // constrained by propositional variables or problem structure
                if (predecessor < 0 || predecessor >= num_states) {
                    if (dijkstra_iterations == 1) {
                        out_of_bounds_predecessors++;
                    }
                    if (is_first_goal_expansion && operators_checked <= 5) {
                        out_of_bounds_this_op++;
                    }
                    continue;  // Skip this invalid predecessor
                }
                
                valid_predecessors_this_state++;
                predecessors_this_op++;
                
                if (alternative_cost < distances[predecessor]) {
                    total_expansions++;
                    
                    if (is_first_goal_expansion && operators_checked <= 5 && predecessors_this_op <= 3) {
                        string pred_decoded = decode_abstract_state(predecessor, domain_sizes,
                                                                   numeric_domain_mapping, hash_multipliers);
                        cout << "    Predecessor " << predecessors_this_op << ": " << pred_decoded 
                             << " (hash_effect=" << hash_effect << ")" << endl;
                    }
                    
                    if (total_expansions <= 20) {
                        cout << "DEBUG DIJKSTRA:   Updated state " << predecessor 
                             << " from distance " << distances[predecessor] 
                             << " to " << alternative_cost 
                             << " (hash_effect=" << hash_effect << ")" << endl;
                    }
                    distances[predecessor] = alternative_cost;
                    pq.push(alternative_cost, predecessor);
                    if (compute_plan) {
                        generating_op_ids[predecessor] = op_id;
                    }
                }
            }
            
            if (is_first_goal_expansion && operators_checked <= 5) {
                cout << "    Valid predecessors: " << predecessors_this_op 
                     << ", Out of bounds: " << out_of_bounds_this_op << endl;
            }
        }
        
        if (is_first_goal_expansion) {
            cout << "Total valid predecessors generated: " << valid_predecessors_this_state << endl;
            cout << "Total out of bounds: " << out_of_bounds_predecessors << endl;
            cout << "===============================================================\n" << endl;
        }
        
        if (dijkstra_iterations == 1) {
            cout << "DEBUG DIJKSTRA:   State " << state_index 
                 << ": valid_predecessors=" << valid_predecessors_this_state
                 << ", out_of_bounds=" << out_of_bounds_predecessors << endl;
        }
    }
    
    cout << "DEBUG DIJKSTRA: Completed " << dijkstra_iterations << " iterations, " 
         << total_expansions << " distance updates" << endl;
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

    // DEBUG: Check if initial state has evaluated axioms
    cout << "DEBUG PLAN: Initial state values:" << endl;
    for (int var_id = 0; var_id < initial_state.size(); ++var_id) {
        cout << "  var" << var_id << " = " << initial_state[var_id].get_value() << endl;
    }

    // Build propositional part of abstract state
    vector<int> prop_state;
    for (int var_id = 0; var_id < initial_state.size(); ++var_id) {
        prop_state.push_back(initial_state[var_id].get_value());
    }
    
    // Compute hash for propositional variables
    int current_state = hash_index(prop_state);
    cout << "DEBUG PLAN: Initial propositional hash = " << current_state << endl;
    
    // Add numeric variables to hash
    if (!numeric_domain_mapping.empty()) {
        vector<ap_float> numeric_values = g_root_task()->get_initial_state_numeric_values();
        
        // For each numeric variable in the abstraction, find its partition
        for (size_t num_var_id = 0; num_var_id < numeric_domain_mapping.size(); ++num_var_id) {
            ap_float value = numeric_values[num_var_id];
            
            // Find which partition this value falls into
            int partition = numeric_domain_mapping[num_var_id].get_partition_index(value);
            
            // Add partition contribution to hash
            // hash_multipliers for numeric vars start after propositional vars
            int hash_multiplier_idx = initial_state.size() + num_var_id;
            current_state += partition * hash_multipliers[hash_multiplier_idx];
            
            if (partition > 0) {
                cout << "DEBUG PLAN:   Numeric var " << num_var_id << " = " << value 
                     << " -> partition " << partition 
                     << " (multiplier=" << hash_multipliers[hash_multiplier_idx] << ")" << endl;
            }
        }
    }
    
    cout << "DEBUG PLAN: Final initial state index = " << current_state << endl;
    cout << "DEBUG PLAN: Total abstract states = " << num_states << endl;
    cout << "DEBUG PLAN: Distance to goal = " << distances[current_state] << endl;
    
    // Count how many states are reachable (have finite distance)
    int reachable_count = 0;
    for (int d : distances) {
        if (d != numeric_limits<int>::max()) {
            reachable_count++;
        }
    }
    cout << "DEBUG PLAN: Reachable states = " << reachable_count << " / " << num_states << endl;
    
    // List which states are reachable and their distances
    cout << "DEBUG PLAN: Reachable state details:" << endl;
    for (int i = 0; i < min(static_cast<int>(distances.size()), 20); ++i) {
        if (distances[i] != numeric_limits<int>::max() || i == current_state) {
            bool is_goal = is_goal_state(i, abstract_goals, domain_sizes);
            cout << "  State " << i << ": distance=" << distances[i] 
                 << (is_goal ? " (GOAL)" : "")
                 << (i == current_state ? " (INITIAL)" : "") << endl;
        }
    }


    if (distances[current_state] != numeric_limits<int>::max()) {
        while (!is_goal_state(current_state, abstract_goals, domain_sizes)) {
            int op_id = generating_op_ids[current_state];
            assert(op_id != -1);
            const AbstractOperator &op = operators[op_id];
            
            // For operators with multiple hash effects (numeric operators), find the
            // correct hash effect that leads to a valid successor
            int hash_effect = -1;
            int successor_state = -1;
            
            for (int candidate_hash_effect : op.get_hash_effects()) {
                int candidate_successor = current_state - candidate_hash_effect;
                // Check if this successor is valid (was reached during Dijkstra)
                assert(candidate_successor >= 0 && candidate_successor < static_cast<int>(distances.size()));
                if (candidate_successor >= 0 && candidate_successor < static_cast<int>(distances.size()) &&
                    distances[candidate_successor] != numeric_limits<int>::max() &&
                    distances[candidate_successor] < distances[current_state]) {
                    // Valid successor with lower distance
                    hash_effect = candidate_hash_effect;
                    successor_state = candidate_successor;
                    break;
                }
            }
            
            // If no valid successor found, use the first hash effect as fallback
            if (hash_effect == -1) {
                hash_effect = op.get_hash_effects()[0];
                successor_state = current_state - hash_effect;
            }

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
// NOTE: multiply_out() and build_abstract_operators() methods have been moved
// to DomainAbstractionNumericHelper, which now handles all operator construction.
// The factory delegates to the helper via compute_abstract_operators().

//TODO: Does not support numeric (goal) states yet. 
bool DomainAbstractionFactory::is_goal_state(
    int state_index,
    const vector<Fact> &abstract_goals,
    const vector<int> &domain_sizes) const {
    // Check propositional goals
    for (const Fact &abstract_goal : abstract_goals) {
        int var_id = abstract_goal.var;
        int temp = state_index / hash_multipliers[var_id];
        int val = temp % domain_sizes[var_id];
        if (val != abstract_goal.value) {
            return false;
        }
    }
    
    // Check numeric goal conditions
    // For each numeric goal, extract the partition index and check if it satisfies the condition
    for (const auto &numeric_goal : numeric_goal_conditions) {
        int num_var_id = numeric_goal.numeric_var_id;
        
        // Sanity check
        if (num_var_id >= static_cast<int>(numeric_domain_mapping.size())) {
            cerr << "ERROR: Numeric goal refers to var" << num_var_id 
                 << " but only " << numeric_domain_mapping.size() 
                 << " numeric variables in mapping!" << endl;
            return false;
        }
        
        // Get the partition index for this numeric variable from the state_index
        // Numeric variables come after propositional variables in the hash
        int var_id_in_hash = domain_mapping.size() + num_var_id;
        
        if (var_id_in_hash >= static_cast<int>(hash_multipliers.size())) {
            cerr << "ERROR: var_id_in_hash=" << var_id_in_hash 
                 << " exceeds hash_multipliers size " << hash_multipliers.size() << endl;
            return false;
        }
        
        int temp = state_index / hash_multipliers[var_id_in_hash];
        int partition_index = temp % numeric_domain_sizes[num_var_id];
        
        // Check if this partition satisfies the goal condition
        const NumericDomainMapping &domain_map = numeric_domain_mapping[num_var_id];
        const auto &ranges = domain_map.get_ranges();
        
        // Find the range with this partition index
        bool partition_satisfies_goal = false;
        for (const auto &range : ranges) {
            if (range.partition_index == partition_index) {
                ap_float lower = range.lower;
                ap_float upper = range.upper;
                ap_float constant = numeric_goal.constant;
                
                switch (numeric_goal.op) {
                    case comp_operator::lt:
                        // Entire partition must be < constant: upper <= constant
                        partition_satisfies_goal = (upper <= constant);
                        break;
                    case comp_operator::le:
                        // Entire partition must be <= constant: upper <= constant
                        // (since ranges are [lower, upper) exclusive on upper, upper <= c means all values are <= c)
                        partition_satisfies_goal = (upper <= constant);
                        break;
                    case comp_operator::eq:
                        // Entire partition must equal constant: lower == constant && upper == constant (single point)
                        partition_satisfies_goal = (lower == constant && upper == constant);
                        break;
                    case comp_operator::ge:
                        // Entire partition must be >= constant: lower >= constant
                        partition_satisfies_goal = (lower >= constant);
                        break;
                    case comp_operator::gt:
                        // Entire partition must be > constant: lower > constant
                        // (since ranges are [lower, upper) inclusive on lower, lower > c means all values are > c)
                        partition_satisfies_goal = (lower > constant);
                        break;
                    case comp_operator::ue:
                        // Undefined/error operator - should not appear in goals
                        partition_satisfies_goal = false;
                        break;
                }
                break;  // Found the right range, exit search
            }
        }
        
        if (!partition_satisfies_goal) {
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
