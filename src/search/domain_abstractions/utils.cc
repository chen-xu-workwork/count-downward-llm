#include "utils.h"

#include "domain_abstraction.h"

#include "../task_proxy.h"


#include "../task_tools.h"
#include "../causal_graph.h"


#include "../utils/logging.h"
#include "../utils/markup.h"
#include "../utils/math.h"
#include "../utils/rng.h"
#include "../numeric_pdbs/numeric_helper.h"

#include <limits>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>

using namespace std;
using namespace numeric_pdbs;

namespace domain_abstractions {
vector<Fact> get_goals_in_random_order(
    const TaskProxy &task_proxy, utils::RandomNumberGenerator &rng) {
    //vector<Fact> goals = task_tools::get_facts(task_proxy.get_goals());
    vector<Fact> goals;
    int goal_id = -1;
    AxiomsProxy axioms = task_proxy.get_axioms();
    for (size_t i = 0; i < axioms.size(); ++i) {
        OperatorProxy axiom = axioms[i];
        assert(axiom.get_effects().size() == 1);    
        int effect_id = axiom.get_effects()[0].get_fact().get_variable().get_id();
        goal_id = effect_id;
        PreconditionsProxy conditions = axiom.get_preconditions();
        if (conditions.size() > 0) {
            for (const FactProxy &condition : conditions) {
                goals.push_back(Fact(condition.get_variable().get_id(), condition.get_value()));
            }
            cout << endl;
        }
    }
    GoalsProxy goals_proxy = task_proxy.get_goals();
    goals.reserve(goals_proxy.size());
    for (size_t i = 0; i < goals_proxy.size(); ++i) {
        FactProxy goal = goals_proxy[i];
        if (goal_id == goal.get_variable().get_id()) {
            assert(goal_id != -1);
            continue;
        }
        goals.push_back(Fact(goal.get_variable().get_id(), goal.get_value()));
    }
    rng.shuffle(goals);
    return goals;
}

string get_rovner_et_al_reference() {
    return " (Rovner, Helmert, and Domshlak 2019, https://doi.org/10.1007/978-3-030-30244-3_22).";
}

vector<int> get_non_goal_variables(const TaskProxy &task_proxy) {
    size_t num_vars = task_proxy.get_variables().size();
    GoalsProxy goals = task_proxy.get_goals();
    vector<bool> is_goal(num_vars, false);
    for (FactProxy goal : goals) {
        is_goal[goal.get_variable().get_id()] = true;
    }

    vector<int> non_goal_variables;
    non_goal_variables.reserve(num_vars - goals.size());
    for (int var_id = 0; var_id < static_cast<int>(num_vars); ++var_id) {
        if (!is_goal[var_id]) {
            non_goal_variables.push_back(var_id);
        }
    }
    return non_goal_variables;
}

vector<vector<int>> compute_cg_neighbors(
    const TaskProxy &task_proxy,
    bool bidirectional) {
    const ::CausalGraph &cg = task_proxy.get_causal_graph();
    int num_vars = task_proxy.get_variables().size();
    int num_numeric_vars = task_proxy.get_numeric_variables().size();
    vector<vector<int>> cg_neighbors(num_vars + num_numeric_vars);
    for (int var_id = 0; var_id < num_vars; ++var_id) {
        cg_neighbors[var_id] = cg.get_predecessors(var_id);
        if (bidirectional) {
            const vector<int> &successors = cg.get_successors(var_id);
            cg_neighbors[var_id].insert(cg_neighbors[var_id].end(), successors.begin(), successors.end());
        }
        std::sort(cg_neighbors[var_id].begin(), cg_neighbors[var_id].end());
        cg_neighbors[var_id].erase(
            std::unique(cg_neighbors[var_id].begin(), cg_neighbors[var_id].end()),
            cg_neighbors[var_id].end());
    }
    return cg_neighbors;
}

size_t compute_abstract_state_hash(
    const State &state,
    const TaskProxy &task_proxy,
    const DomainMapping &domain_mapping,
    const NumericDomainMappingType &numeric_domain_mapping,
    const std::vector<int> &hash_multipliers) {
    
    /* IMPORTANT: This function computes the abstract state hash for a CONCRETE state
     * by using the actual evaluated comparison axiom values from the state.
     * 
     * This is correct because:
     * 1. The concrete state has all comparison axioms properly evaluated (TRUE/FALSE)
     * 2. We should hash based on what the state ACTUALLY IS, not what it COULD BE
     * 
     * Example to clarify partition boundaries:
     * - If we split numeric variable at value 0, we get:
     *   * Partition 0: (-∞, 0)   [lower part keeps old partition index]
     *   * Partition 1: [0, ∞)    [upper part gets new partition index]
     * 
     * - So value 0 is in partition 1, not partition 0!
     * 
     * For comparison axioms with derived numeric variables:
     * - Initial state: var17=0, var66=1000, var2=1060
     * - Comparison: var17 >= var66-var2  →  0 >= 1000-1060  →  0 >= -60  →  TRUE
     * - In abstract state:
     *   * var17=0 → partition 1 → range [0, ∞)
     *   * var66-var2=-60 → partition 0 → range (-∞, 0)
     *   * Concrete evaluation: 0 >= -60 → TRUE
     * 
     * The OLD bug was using range-based optimistic evaluation which would consider:
     *   * var17 ∈ [0, ∞), var66-var2 ∈ (-∞, 0)
     *   * Could 0 >= x for x in (-∞, 0)? YES (always true)
     *   * But this incorrectly made initial and goal states hash the same way!
     * 
     * The FIX is to use the concrete evaluation: the state KNOWS var17 >= var66-var2 is TRUE,
     * so we use TRUE directly, not "what could it be based on ranges".
     */
    
    size_t state_hash = 0;
    
    // 1. Add propositional variables to hash
    for (size_t i = 0; i < domain_mapping.size(); ++i) {
        if (!domain_mapping[i].empty()) {
            int val;
            // Regular propositional variable: use actual state value
            val = state[i].get_value();
            int abstract_val = domain_mapping[i][val];
            state_hash += hash_multipliers[i] * abstract_val;
        }
    }
    
    // 2. Add numeric variables to hash
    for (size_t i = 0; i < numeric_domain_mapping.size(); ++i) {
        ap_float value = state.nval(i);
        int partition = numeric_domain_mapping[i]->get_partition_index(value);
        state_hash += hash_multipliers[domain_mapping.size() + i] * partition;
    }

    // 3. Evaluate comparison axioms using CONCRETE state values

    return state_hash;
}

size_t compute_abstract_state_hash_backup(
    const State &state,
    const TaskProxy &task_proxy,
    const DomainMapping &domain_mapping,
    const NumericDomainMappingType &numeric_domain_mapping,
    const std::vector<int> &hash_multipliers) {
    
    /* BACKUP VERSION using OPTIMISTIC range-based evaluation
     * 
     * This version computes comparison axiom values based on the RANGES of numeric
     * variables (from partitions + derived variable cascades), then optimistically
     * chooses TRUE when both TRUE and FALSE are possible.
     * 
     * This was the ORIGINAL implementation that had a bug:
     * - It treated concrete states as if they were abstract/partial states
     * - It used ranges to determine "what could be" instead of "what is"
     * - This caused initial and goal states to hash the same way
     * 
     * This approach MIGHT be useful for:
     * - Predecessor enumeration (exploring what states COULD lead here)
     * - Abstract state space construction (when we don't have concrete values)
     * 
     * But it should NOT be used for hashing concrete states!
     * Use compute_abstract_state_hash() instead for that purpose.
     */
    
    size_t state_hash = 0;
    
    // Build a set of comparison axiom variable IDs for quick lookup
    unordered_set<int> comparison_axiom_vars;
    ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
    for (ComparisonAxiomProxy axiom : comparison_axioms) {
        comparison_axiom_vars.insert(axiom.get_true_fact().get_variable().get_id());
    }
    
    // 1. Add propositional variables to hash
    // For comparison axioms, use UNKNOWN value (2) instead of the concrete state value
    for (size_t i = 0; i < domain_mapping.size(); ++i) {
        if (!domain_mapping[i].empty()) {
            int val;
            if (comparison_axiom_vars.count(i) > 0) {
                // Comparison axiom: use UNKNOWN value (2)
                val = 2;
            } else {
                // Regular propositional variable: use actual state value
                val = state[i].get_value();
            }
            int abstract_val = domain_mapping[i][val];
            state_hash += hash_multipliers[i] * abstract_val;
        }
    }
    
    // 2. Add numeric variables to hash
    for (size_t i = 0; i < numeric_domain_mapping.size(); ++i) {
        ap_float value = state.nval(i);
        int partition = numeric_domain_mapping[i]->get_partition_index(value);
        state_hash += hash_multipliers[domain_mapping.size() + i] * partition;
    }

    // 3. Build the complete cascade: compute derived numeric variables and evaluate comparison axioms
    
    // Step 3a: Compute ranges for all numeric variables (including derived)
    unordered_set<int> affected_numeric_vars;
    unordered_map<int, pair<ap_float, ap_float>> computed_ranges;  // var_id -> (lower, upper)
    
    // Start with all base numeric variables
    for (size_t i = 0; i < numeric_domain_mapping.size(); ++i) {
        ap_float value = state.nval(i);
        int partition = numeric_domain_mapping[i]->get_partition_index(value);
        
        // Find the range for this partition
        const vector<NumericRange> &ranges = numeric_domain_mapping[i]->get_ranges();
        for (const NumericRange &range : ranges) {
            if (range.partition_index == partition) {
                computed_ranges[i] = {range.lower, range.upper};
                affected_numeric_vars.insert(i);
                break;
            }
        }
    }
    
    // Step 3b: Iteratively compute derived variable ranges from assignment axioms
    bool added_new = true;
    while (added_new) {
        added_new = false;
        
        AssignmentAxiomsProxy assignment_axioms = task_proxy.get_assignment_axioms();
        for (AssignmentAxiomProxy axiom : assignment_axioms) {
            int derived_var_id = axiom.get_assignment_variable().get_id();
            
            // Skip if we've already computed this derived variable
            if (affected_numeric_vars.count(derived_var_id) > 0) {
                continue;
            }
            
            // Check if this axiom depends on variables we know
            NumericVariableProxy left_var = axiom.get_left_variable();
            NumericVariableProxy right_var = axiom.get_right_variable();
            int left_var_id = left_var.get_id();
            int right_var_id = right_var.get_id();
            
            // A variable is "known" if it's in computed_ranges or is a constant
            bool left_known = (computed_ranges.count(left_var_id) > 0) ||
                             (left_var.get_var_type() == numType::constant);
            bool right_known = (computed_ranges.count(right_var_id) > 0) ||
                              (right_var.get_var_type() == numType::constant);
            
            // We need BOTH operands to be known
            if (!left_known || !right_known) {
                continue;
            }
            
            // Get ranges for left and right variables
            ap_float left_lower, left_upper, right_lower, right_upper;
            
            if (left_var.get_var_type() == numType::constant) {
                ap_float const_val = left_var.get_initial_state_value();
                left_lower = const_val;
                left_upper = const_val;
            } else {
                left_lower = computed_ranges[left_var_id].first;
                left_upper = computed_ranges[left_var_id].second;
            }
            
            if (right_var.get_var_type() == numType::constant) {
                ap_float const_val = right_var.get_initial_state_value();
                right_lower = const_val;
                right_upper = const_val;
            } else {
                right_lower = computed_ranges[right_var_id].first;
                right_upper = computed_ranges[right_var_id].second;
            }
            
            ap_float derived_lower, derived_upper;
            
            switch (axiom.get_arithmetic_operator_type()) {
                case cal_operator::sum:
                    derived_lower = left_lower + right_lower;
                    derived_upper = left_upper + right_upper;
                    break;
                case cal_operator::diff:
                    derived_lower = left_lower - right_upper;
                    derived_upper = left_upper - right_lower;
                    break;
                case cal_operator::mult:
                    {
                        ap_float vals[4] = {
                            left_lower * right_lower,
                            left_lower * right_upper,
                            left_upper * right_lower,
                            left_upper * right_upper
                        };
                        derived_lower = *min_element(vals, vals + 4);
                        derived_upper = *max_element(vals, vals + 4);
                    }
                    break;
                case cal_operator::divi:
                    // Division - skip for now
                    continue;
                default:
                    continue;
            }
            
            computed_ranges[derived_var_id] = {derived_lower, derived_upper};
            affected_numeric_vars.insert(derived_var_id);
            added_new = true;
        }
    }
    
    // Step 3c: Evaluate ALL comparison axioms optimistically
    for (ComparisonAxiomProxy axiom : comparison_axioms) {
        int prop_var_id = axiom.get_true_fact().get_variable().get_id();
        
        // Skip if this comparison axiom is not in the abstraction
        if (prop_var_id >= static_cast<int>(domain_mapping.size()) || 
            domain_mapping[prop_var_id].empty()) {
            continue;
        }
        
        int left_var_id = axiom.get_left_variable().get_id();
        int right_var_id = axiom.get_right_variable().get_id();
        
        // Get ranges for left and right variables
        ap_float left_lower = -numeric_limits<ap_float>::infinity();
        ap_float left_upper = numeric_limits<ap_float>::infinity();
        ap_float right_lower = -numeric_limits<ap_float>::infinity();
        ap_float right_upper = numeric_limits<ap_float>::infinity();
        
        bool found_left = false, found_right = false;
        
        // Try to get from computed_ranges first
        if (computed_ranges.count(left_var_id) > 0) {
            left_lower = computed_ranges[left_var_id].first;
            left_upper = computed_ranges[left_var_id].second;
            found_left = true;
        }
        
        if (computed_ranges.count(right_var_id) > 0) {
            right_lower = computed_ranges[right_var_id].first;
            right_upper = computed_ranges[right_var_id].second;
            found_right = true;
        }
        
        // If not in computed_ranges, try domain mapping
        if (!found_left && left_var_id < static_cast<int>(numeric_domain_mapping.size())) {
            const vector<NumericRange> &left_ranges = numeric_domain_mapping[left_var_id]->get_ranges();
            if (!left_ranges.empty()) {
                left_lower = numeric_limits<ap_float>::infinity();
                left_upper = -numeric_limits<ap_float>::infinity();
                for (const NumericRange &range : left_ranges) {
                    left_lower = min(left_lower, range.lower);
                    left_upper = max(left_upper, range.upper);
                }
                found_left = true;
            }
        }
        
        if (!found_right && right_var_id < static_cast<int>(numeric_domain_mapping.size())) {
            const vector<NumericRange> &right_ranges = numeric_domain_mapping[right_var_id]->get_ranges();
            if (!right_ranges.empty()) {
                right_lower = numeric_limits<ap_float>::infinity();
                right_upper = -numeric_limits<ap_float>::infinity();
                for (const NumericRange &range : right_ranges) {
                    right_lower = min(right_lower, range.lower);
                    right_upper = max(right_upper, range.upper);
                }
                found_right = true;
            }
        }
        
        // If we can't determine ranges, skip (will remain UNKNOWN)
        if (!found_left || !found_right) {
            continue;
        }
        
        // Evaluate: can the comparison be true? false? or both?
        bool can_be_true = false;
        bool can_be_false = false;
        
        switch (axiom.get_comparison_operator_type()) {
            case comp_operator::lt:
                can_be_true = (left_lower < right_upper);
                can_be_false = (left_upper >= right_lower);
                break;
            case comp_operator::le:
                can_be_true = (left_lower <= right_upper);
                can_be_false = (left_upper > right_lower);
                break;
            case comp_operator::eq:
                can_be_true = (left_lower <= right_upper && right_lower <= left_upper);
                can_be_false = (left_lower < right_lower || left_upper > right_upper ||
                               right_lower < left_lower || right_upper > left_upper);
                break;
            case comp_operator::ge:
                can_be_true = (left_upper >= right_lower);
                can_be_false = (left_lower < right_upper);
                break;
            case comp_operator::gt:
                can_be_true = (left_upper > right_lower);
                can_be_false = (left_lower <= right_upper);
                break;
            case comp_operator::ue:
                // Handle unknown equality if needed
                break;
        }
        
        // Compute the hash adjustment from UNKNOWN to the target value
        int unknown_value = domain_mapping[prop_var_id][2];
        int true_value = axiom.get_true_fact().get_value();
        int false_value = axiom.get_false_fact().get_value();
        
        int hash_adjustment = 0;
        if (can_be_true && !can_be_false) {
            // Definitely TRUE
            int target_abstract = domain_mapping[prop_var_id][true_value];
            hash_adjustment = (target_abstract - unknown_value) * hash_multipliers[prop_var_id];
        } else if (!can_be_true && can_be_false) {
            // Definitely FALSE
            int target_abstract = domain_mapping[prop_var_id][false_value];
            hash_adjustment = (target_abstract - unknown_value) * hash_multipliers[prop_var_id];
        } else {
            // UNKNOWN or can be both - use TRUE optimistically
            int target_abstract = domain_mapping[prop_var_id][true_value];
            hash_adjustment = (target_abstract - unknown_value) * hash_multipliers[prop_var_id];
        }
        
        state_hash += hash_adjustment;
    }
    
    return state_hash;
}
}