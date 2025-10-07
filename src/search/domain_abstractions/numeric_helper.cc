#include "numeric_helper.h"
#include "domain_abstraction_factory.h"

#include "../numeric_pdbs/arithmetic_expression.h"
#include "../numeric_pdbs/numeric_condition.h"
#include "../task_tools.h"

#include <algorithm>
#include <cassert>
#include <queue>

using namespace std;
using namespace arithmetic_expression;
using namespace numeric_condition;

namespace domain_abstractions {

DomainAbstractionNumericHelper::DomainAbstractionNumericHelper(
    const shared_ptr<AbstractTask> &task,
    const DomainMapping &domain_mapping,
    const NumericDomainMappingType &numeric_domain_mapping,
    const vector<int> &domain_sizes,
    const vector<int> &numeric_domain_sizes,
    const vector<int> &hash_multipliers)
    : task(task), 
      task_proxy(*task),
      domain_mapping(domain_mapping),
      numeric_domain_mapping(numeric_domain_mapping),
      domain_sizes(domain_sizes),
      numeric_domain_sizes(numeric_domain_sizes),
      hash_multipliers(hash_multipliers) {
    
    // Verify this is a valid numeric task
    verify_no_axioms(task_proxy);
    verify_no_conditional_effects(task_proxy);
    
    n_numeric_variables = task_proxy.get_numeric_variables().size();
    n_propositional_variables = task_proxy.get_variables().size();
    
    // Initialize ID mappings and derived variable tracking
    reg_num_var_id_to_glob_var_id.resize(n_numeric_variables);
    glob_var_id_to_reg_num_var_id.resize(n_numeric_variables);
    is_derived_num_var.resize(n_numeric_variables, false);
    is_derived_prop_var.resize(n_propositional_variables, false);
    
    // Initialize dependency structures
    axiom_dependencies.resize(n_numeric_variables);
    reverse_axiom_dependencies.resize(n_numeric_variables);
    
    // Build internal data structures
    build_numeric_variables();
    find_derived_variables();
    build_axiom_dependencies();
    build_goals();
}

void DomainAbstractionNumericHelper::build_numeric_variables() {
    // Get initial values
    initial_numeric_values = task->get_initial_state_numeric_values();
    
    // Build mappings between regular and global IDs
    // TODO: Why do I need that mapping? That is not a PDB, 
    // meaning all variables are considered.
    // Can remove later, I suppose.
    int regular_id = 0;
    for (int i = 0; i < n_numeric_variables; ++i) {
        if (!is_derived_num_var[i]) {
            reg_num_var_id_to_glob_var_id[regular_id] = i;
            glob_var_id_to_reg_num_var_id[i] = regular_id;
            ++regular_id;
        }
    }
}

void DomainAbstractionNumericHelper::find_derived_variables() {
    // Analyze task axioms to identify derived variables
    
    // 1. Find derived numeric variables from assignment axioms
    // Assignment axioms have the form: derived_var := left_var op right_var
    // where op is one of: +, -, *, /
    AssignmentAxiomsProxy assignment_axioms = task_proxy.get_assignment_axioms();
    for (AssignmentAxiomProxy axiom : assignment_axioms) {
        NumericVariableProxy derived_var = axiom.get_assignment_variable();
        int derived_id = derived_var.get_id();
        
        // Mark this as a derived numeric variable
        if (derived_id >= 0 && derived_id < static_cast<int>(is_derived_num_var.size())) {
            is_derived_num_var[derived_id] = true;
        }
        
        // Note: The left and right variables are the sources
        // We'll build dependency graph in build_axiom_dependencies()
    }
    
    // 2. Find derived propositional variables from comparison axioms
    // Comparison axioms have the form: (left_var comp_op right_var) 
    // which creates a derived propositional fact (boolean variable)
    ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
    for (ComparisonAxiomProxy axiom : comparison_axioms) {
        // The axiom produces a true_fact when the comparison holds
        // and a false_fact when it doesn't
        FactProxy true_fact = axiom.get_true_fact();
        FactProxy false_fact = axiom.get_false_fact();
        
        int var_id = true_fact.get_variable().get_id();
        
        // Mark this propositional variable as derived from a comparison
        if (var_id >= 0 && var_id < static_cast<int>(is_derived_prop_var.size())) {
            is_derived_prop_var[var_id] = true;
        }
        
        // Sanity check: true_fact and false_fact should be on same variable
        assert(var_id == false_fact.get_variable().get_id());
    }
}

void DomainAbstractionNumericHelper::build_axiom_dependencies() {
    // Build dependency graph for assignment axioms
    // This tracks which variables affect which derived variables
    
    AssignmentAxiomsProxy assignment_axioms = task_proxy.get_assignment_axioms();
    for (AssignmentAxiomProxy axiom : assignment_axioms) {
        int derived_id = axiom.get_assignment_variable().get_id();
        int left_id = axiom.get_left_variable().get_id();
        int right_id = axiom.get_right_variable().get_id();
        
        // Forward dependencies: derived variable depends on left and right
        if (derived_id >= 0 && derived_id < static_cast<int>(axiom_dependencies.size())) {
            if (left_id >= 0) {
                // Check if not already in dependencies
                if (std::find(axiom_dependencies[derived_id].begin(),
                             axiom_dependencies[derived_id].end(),
                             left_id) == axiom_dependencies[derived_id].end()) {
                    axiom_dependencies[derived_id].push_back(left_id);
                }
            }
            if (right_id >= 0 && right_id != left_id) {
                if (std::find(axiom_dependencies[derived_id].begin(),
                             axiom_dependencies[derived_id].end(),
                             right_id) == axiom_dependencies[derived_id].end()) {
                    axiom_dependencies[derived_id].push_back(right_id);
                }
            }
        }
        
        // Reverse dependencies: left and right variables affect derived variable
        if (left_id >= 0 && left_id < static_cast<int>(reverse_axiom_dependencies.size())) {
            if (std::find(reverse_axiom_dependencies[left_id].begin(),
                         reverse_axiom_dependencies[left_id].end(),
                         derived_id) == reverse_axiom_dependencies[left_id].end()) {
                reverse_axiom_dependencies[left_id].push_back(derived_id);
            }
        }
        if (right_id >= 0 && right_id < static_cast<int>(reverse_axiom_dependencies.size()) 
            && right_id != left_id) {
            if (std::find(reverse_axiom_dependencies[right_id].begin(),
                         reverse_axiom_dependencies[right_id].end(),
                         derived_id) == reverse_axiom_dependencies[right_id].end()) {
                reverse_axiom_dependencies[right_id].push_back(derived_id);
            }
        }
    }
    
    // For comparison axioms, we don't add to numeric dependencies
    // because they produce propositional variables, not numeric ones.
    // However, we'll need to track them for cascade computation.
    // This is handled in compute_affected_comparison_axioms().
}

void DomainAbstractionNumericHelper::build_goals() {
    // Collect propositional goals
    for (FactProxy goal : task_proxy.get_goals()) {
        propositional_goals.push_back(goal);
    }
    
    // TODO: Collect numeric goals
    // This requires parsing numeric goal conditions
    // Markus: Don't think such thing exists. Comparison axioms produce propositional goals.
    // Question is if they are stored as "numeric goals" or "propositional goals".
}

vector<int> DomainAbstractionNumericHelper::get_affected_variables(int var_id) const {
    // Compute transitive closure of reverse dependencies
    vector<int> affected;
    vector<bool> visited(n_numeric_variables, false);
    queue<int> to_process;
    
    to_process.push(var_id);
    visited[var_id] = true;
    affected.push_back(var_id);
    
    while (!to_process.empty()) {
        int current = to_process.front();
        to_process.pop();
        
        // Add all variables that depend on current
        for (int derived_var : reverse_axiom_dependencies[current]) {
            if (!visited[derived_var]) {
                visited[derived_var] = true;
                affected.push_back(derived_var);
                to_process.push(derived_var);
            }
        }
    }
    
    return affected;
}

vector<pair<int, ap_float>> DomainAbstractionNumericHelper::compute_derived_updates(
    int changed_var_id,
    ap_float new_value,
    const vector<ap_float> &current_state) const {
    
    vector<pair<int, ap_float>> updates;
    
    // TODO: Implement proper derived variable computation
    // This requires:
    // 1. Get all variables affected by changed_var_id
    // 2. Compute their new values in topological order
    // 3. Return the list of (var_id, new_value) pairs
    
    // For now, return empty (no derived variables)
    return updates;
}

vector<AbstractOperator> DomainAbstractionNumericHelper::build_abstract_operators(const TaskProxy &task_proxy) {
    vector<AbstractOperator> abstract_operators;
    
    // Build abstract operators for all concrete operators
    OperatorsProxy operators = task_proxy.get_operators();
    for (OperatorProxy op : operators) {
        build_abstract_operator(op, abstract_operators);
    }
    
    return abstract_operators;
}

void DomainAbstractionNumericHelper::build_abstract_operator(
    const OperatorProxy &op,
    vector<AbstractOperator> &operators) {
    
    // Build abstract operator following the same pattern as factory's
    // build_abstract_operators, but adapted for numeric effects
    
    // All variable value pairs that are a prevail condition
    vector<Fact> prev_pairs;
    // All variable value pairs that are a precondition (value != -1)
    vector<Fact> pre_pairs;
    // All variable value pairs that are an effect
    vector<Fact> eff_pairs;
    // All variable value pairs that are an effect without precondition
    vector<Fact> effects_without_pre;

    int num_variables = task_proxy.get_variables().size();
    vector<int> has_precondition_on_var(num_variables, -1);
    vector<int> has_effect_on_var(num_variables, -1);

    // Process propositional preconditions
    for (FactProxy pre : op.get_preconditions()) {
        int var_id = pre.get_variable().get_id();
        // Map concrete value to abstract value
        has_precondition_on_var[var_id] = domain_mapping[var_id][pre.get_value()];
    }

    // Process propositional effects
    for (EffectProxy eff : op.get_effects()) {
        int var_id = eff.get_fact().get_variable().get_id();
        int val = domain_mapping[var_id][eff.get_fact().get_value()];
        
        // Collect effects that don't have themselves as precondition
        int pre_val = has_precondition_on_var[var_id];
        if (pre_val < 0) {
            effects_without_pre.emplace_back(var_id, val);
        } else if (pre_val != val) {
            has_effect_on_var[var_id] = val;
            eff_pairs.emplace_back(var_id, val);
        }
    }
    
    // Classify preconditions as either pre_pairs or prev_pairs
    for (FactProxy pre : op.get_preconditions()) {
        int var_id = pre.get_variable().get_id();
        int val = domain_mapping[var_id][pre.get_value()];
        if (has_effect_on_var[var_id] >= 0) {
            pre_pairs.emplace_back(var_id, val);
        } else {
            prev_pairs.emplace_back(var_id, val);
        }
    }
    
    // Collect numeric effects (assignment effects)
    vector<NumAssProxy> ass_effects;
    for (auto ass_eff : op.get_ass_effects()) {
        ass_effects.push_back(ass_eff.get_assignment());
    }
    
    // Enumerate all possible abstract transitions
    enumerate_abstract_transitions(
        op, prev_pairs, pre_pairs, eff_pairs,
        effects_without_pre, ass_effects,
        op.get_id(), operators);
}

void DomainAbstractionNumericHelper::enumerate_abstract_transitions(
    const OperatorProxy &op,
    vector<Fact> &prev_pairs,
    vector<Fact> &pre_pairs,
    vector<Fact> &eff_pairs,
    const vector<Fact> &effects_without_pre,
    const vector<NumAssProxy> &ass_effects,
    int concrete_op_id,
    vector<AbstractOperator> &operators) {
    
    // This method is recursive and handles two types of enumeration:
    // 1. Propositional effects without preconditions (multiply_out pattern)
    // 2. Numeric effects with partition transitions
    //
    // The recursion first handles all propositional effects_without_pre,
    // then creates the AbstractOperator which internally enumerates
    // numeric partition transitions in its constructor.
    
    multiply_out_propositional(
        0, op.get_cost(), prev_pairs, pre_pairs, eff_pairs,
        effects_without_pre, ass_effects, concrete_op_id, operators);
}

void DomainAbstractionNumericHelper::multiply_out_propositional(
    int pos, int cost, vector<Fact> &prev_pairs,
    vector<Fact> &pre_pairs,
    vector<Fact> &eff_pairs,
    const vector<Fact> &effects_without_pre,
    const vector<NumAssProxy> &ass_effects,
    int concrete_op_id,
    vector<AbstractOperator> &operators) {
    
    if (pos == static_cast<int>(effects_without_pre.size())) {
        // All effects without precondition have been checked: insert operator.
        if (!eff_pairs.empty() || !ass_effects.empty()) {
            // Compute all hash effects including cascades
            vector<int> complete_hash_effects = 
                compute_hash_effects_with_cascades(eff_pairs, ass_effects);
            
            // Create abstract operator with pre-computed hash effects
            operators.emplace_back(
                prev_pairs,              // prevail conditions
                pre_pairs,               // preconditions
                eff_pairs,               // propositional effects
                ass_effects,             // numeric assignment effects
                cost,                    // operator cost
                complete_hash_effects,   // pre-computed hash effects with cascades
                concrete_op_id);         // concrete operator ID
        }
    } else {
        // For each possible value for the current variable, build an
        // abstract operator (multiply-out for effects without precondition)
        int var_id = effects_without_pre[pos].var;
        int eff = effects_without_pre[pos].value;
        for (int i = 0; i < this->domain_sizes[var_id]; ++i) {
            if (i != eff) {
                pre_pairs.emplace_back(var_id, i);
                eff_pairs.emplace_back(var_id, eff);
            } else {
                prev_pairs.emplace_back(var_id, i);
            }
            multiply_out_propositional(
                pos + 1, cost, prev_pairs, pre_pairs, eff_pairs,
                effects_without_pre, ass_effects, concrete_op_id, operators);
            if (i != eff) {
                pre_pairs.pop_back();
                eff_pairs.pop_back();
            } else {
                prev_pairs.pop_back();
            }
        }
    }
}

vector<int> DomainAbstractionNumericHelper::compute_hash_effects_with_cascades(
    const vector<Fact> &eff_pairs,
    const vector<NumAssProxy> &ass_effects) {
    
    vector<int> hash_effects;
    
    // Compute base hash effect from propositional effects
    int base_hash_effect = 0;
    for (const Fact &effect : eff_pairs) {
        int var_id = effect.var;
        int old_val = -1;  // We don't know the old value in abstraction
        int new_val = effect.value;
        
        // Contribution to hash: new_val * multiplier - old_val * multiplier
        // But we can't compute old_val here, so we just store the new value contribution
        // The actual hash computation during regression will handle this
        base_hash_effect += new_val * hash_multipliers[var_id];
    }
    
    // If no numeric effects, just return the base effect
    if (ass_effects.empty()) {
        hash_effects.push_back(base_hash_effect);
        return hash_effects;
    }
    
    // Identify which numeric variables are affected
    vector<bool> affected_numeric_vars(numeric_domain_mapping.size(), false);
    for (const NumAssProxy &ass_eff : ass_effects) {
        int num_var_id = ass_eff.get_affected_variable().get_id();
        if (num_var_id >= 0 && num_var_id < static_cast<int>(numeric_domain_mapping.size())) {
            affected_numeric_vars[num_var_id] = true;
        }
    }
    
    // Enumerate all possible combinations of partition transitions
    // For each affected variable, we consider REACHABLE (source, target) partition pairs
    // based on the effect expression and source partition ranges
    function<void(size_t, int, vector<int>&, vector<int>&, vector<int>&, const NumAssProxy*)> enumerate_effects =
        [&](size_t var_idx, int current_effect,
            vector<int> &changed_vars, vector<int> &old_parts, vector<int> &new_parts,
            const NumAssProxy *current_ass_effect) {
        
        if (var_idx == numeric_domain_mapping.size()) {
            // Base case: computed effect for one combination
            int total_effect = base_hash_effect + current_effect;
            
            // Now compute cascading effects
            if (!changed_vars.empty()) {
                // Compute affected comparison axioms (propositional cascades)
                vector<Fact> affected_facts = 
                    compute_affected_comparison_axioms(changed_vars, old_parts, new_parts);
                
                for (const Fact &fact : affected_facts) {
                    // Add propositional variable contribution
                    // This is conservative - we add both true and false possibilities
                    total_effect += fact.value * hash_multipliers[fact.var];
                }
                
                // TODO: Compute affected assignment axioms (numeric cascades)
                // This would require evaluating assignment axiom expressions
                // For now, we're conservative and only handle comparison cascades
            }
            
            hash_effects.push_back(total_effect);
            return;
        }
        
        if (affected_numeric_vars[var_idx]) {
            // This variable is affected - enumerate reachable partition transitions
            int num_partitions = numeric_domain_sizes[var_idx];
            int hash_multiplier = hash_multipliers[eff_pairs.size() + var_idx];
            
            // Find the assignment effect for this variable
            const NumAssProxy *ass_eff_for_var = nullptr;
            for (const NumAssProxy &ass_eff : ass_effects) {
                if (ass_eff.get_affected_variable().get_id() == static_cast<int>(var_idx)) {
                    ass_eff_for_var = &ass_eff;
                    break;
                }
            }
            
            // Try all possible source partitions
            for (int source_partition = 0; source_partition < num_partitions; ++source_partition) {
                // Compute reachable target partitions from this source
                vector<int> reachable_targets;
                if (ass_eff_for_var) {
                    reachable_targets = compute_reachable_partitions(
                        var_idx, source_partition, *ass_eff_for_var);
                } else {
                    // No effect found - shouldn't happen, but be conservative
                    for (int i = 0; i < num_partitions; ++i) {
                        reachable_targets.push_back(i);
                    }
                }
                
                // Only enumerate reachable target partitions
                for (int target_partition : reachable_targets) {
                    // Compute hash contribution for this transition
                    int effect_contribution = 
                        (target_partition - source_partition) * hash_multiplier;
                    
                    // Track this variable change for cascade computation
                    changed_vars.push_back(var_idx);
                    old_parts.push_back(source_partition);
                    new_parts.push_back(target_partition);
                    
                    enumerate_effects(var_idx + 1, current_effect + effect_contribution,
                                    changed_vars, old_parts, new_parts, ass_eff_for_var);
                    
                    // Backtrack
                    changed_vars.pop_back();
                    old_parts.pop_back();
                    new_parts.pop_back();
                }
            }
        } else {
            // Not affected: no contribution from this variable
            enumerate_effects(var_idx + 1, current_effect, changed_vars, old_parts, new_parts, nullptr);
        }
    };
    
    vector<int> changed_vars, old_parts, new_parts;
    enumerate_effects(0, 0, changed_vars, old_parts, new_parts, nullptr);
    
    return hash_effects;
}

vector<Fact> DomainAbstractionNumericHelper::compute_affected_comparison_axioms(
    const vector<int> &changed_numeric_vars,
    const vector<int> &old_partitions,
    const vector<int> &new_partitions) const {
    
    // Compute which comparison axioms (propositional derived variables) 
    // change truth values when numeric variables change partitions
    
    vector<Fact> affected_facts;
    
    // For each comparison axiom, check if it depends on any changed variable
    ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
    for (ComparisonAxiomProxy axiom : comparison_axioms) {
        int left_var_id = axiom.get_left_variable().get_id();
        int right_var_id = axiom.get_right_variable().get_id();
        
        // Check if this axiom depends on any changed variable
        bool depends_on_changed_var = false;
        for (size_t i = 0; i < changed_numeric_vars.size(); ++i) {
            int changed_id = changed_numeric_vars[i];
            if (left_var_id == changed_id || right_var_id == changed_id) {
                depends_on_changed_var = true;
                break;
            }
        }
        
        if (!depends_on_changed_var) {
            continue;
        }
        
        // This axiom depends on a changed variable
        // Check if the truth value might change
        
        // Get the partition ranges for left and right variables
        int left_partition_old = -1;
        int left_partition_new = -1;
        int right_partition_old = -1;
        int right_partition_new = -1;
        
        for (size_t i = 0; i < changed_numeric_vars.size(); ++i) {
            if (changed_numeric_vars[i] == left_var_id) {
                left_partition_old = old_partitions[i];
                left_partition_new = new_partitions[i];
            }
            if (changed_numeric_vars[i] == right_var_id) {
                right_partition_old = old_partitions[i];
                right_partition_new = new_partitions[i];
            }
        }
        
        // If either partition changed, the comparison result might change
        // Evaluate the comparison exactly to determine the truth value
        
        if (left_partition_old != left_partition_new || 
            right_partition_old != right_partition_new) {
            
            // Get the facts
            FactProxy true_fact = axiom.get_true_fact();
            FactProxy false_fact = axiom.get_false_fact();
            int prop_var_id = true_fact.get_variable().get_id();
            
            // If a variable didn't change, use its current (new) partition
            int eval_left_partition = (left_partition_new != -1) ? left_partition_new : left_partition_old;
            int eval_right_partition = (right_partition_new != -1) ? right_partition_new : right_partition_old;
            
            // Evaluate the comparison exactly
            int eval_result = evaluate_comparison_exactly(axiom, eval_left_partition, eval_right_partition);
            
            if (eval_result == 0) {
                // Definitely false
                affected_facts.emplace_back(prop_var_id, false_fact.get_value());
            } else if (eval_result == 1) {
                // Definitely true
                affected_facts.emplace_back(prop_var_id, true_fact.get_value());
            } else {
                // Unknown - add both possibilities (conservative)
                affected_facts.emplace_back(prop_var_id, true_fact.get_value());
                affected_facts.emplace_back(prop_var_id, false_fact.get_value());
            }
        }
    }
    
    return affected_facts;
}

vector<int> DomainAbstractionNumericHelper::compute_reachable_partitions(
    int numeric_var_id,
    int source_partition,
    const NumAssProxy &ass_effect) const {
    
    vector<int> reachable_partitions;
    
    // Get the source partition range
    const NumericDomainMapping &mapping = numeric_domain_mapping[numeric_var_id];
    const vector<NumericRange> &ranges = mapping.get_ranges();
    
    // Find the range corresponding to the source partition
    NumericRange source_range(-numeric_limits<ap_float>::infinity(),
                             numeric_limits<ap_float>::infinity(),
                             -1);
    for (const NumericRange &range : ranges) {
        if (range.partition_index == source_partition) {
            source_range = range;
            break;
        }
    }
    
    if (source_range.partition_index == -1) {
        // Source partition not found - this shouldn't happen
        // Return empty vector to indicate error
        return reachable_partitions;
    }
    
    // Apply the effect to compute the result range
    // For now, we conservatively return ALL partitions as reachable
    // TODO: Implement proper range arithmetic based on the effect expression
    // This would require:
    // 1. Getting the actual numeric value/expression from the assigned variable
    // 2. Evaluating it for the bounds of the source partition
    // 3. Determining which target partitions the result overlaps with
    //
    // For now, conservative approach: all partitions are potentially reachable
    for (const NumericRange &range : ranges) {
        reachable_partitions.push_back(range.partition_index);
    }
    
    return reachable_partitions;
}

int DomainAbstractionNumericHelper::evaluate_comparison_exactly(
    const ComparisonAxiomProxy &axiom,
    int left_partition,
    int right_partition) const {
    
    // Get the comparison operator
    comp_operator comp_op = axiom.get_comparison_operator_type();
    
    // Get the variable IDs
    int left_var_id = axiom.get_left_variable().get_id();
    int right_var_id = axiom.get_right_variable().get_id();
    
    // Get the partition ranges
    // Note: We need to check if these are in the numeric_domain_mapping
    if (left_var_id >= static_cast<int>(numeric_domain_mapping.size()) ||
        right_var_id >= static_cast<int>(numeric_domain_mapping.size())) {
        // Variables not in our domain mapping, be conservative
        return 2; // unknown
    }
    
    const NumericDomainMapping &left_mapping = numeric_domain_mapping[left_var_id];
    const NumericDomainMapping &right_mapping = numeric_domain_mapping[right_var_id];
    
    // Get the actual ranges
    const vector<NumericRange> &left_ranges = left_mapping.get_ranges();
    const vector<NumericRange> &right_ranges = right_mapping.get_ranges();
    
    // Find the ranges corresponding to the partitions
    NumericRange left_range(-numeric_limits<ap_float>::infinity(),
                           numeric_limits<ap_float>::infinity(), -1);
    NumericRange right_range(-numeric_limits<ap_float>::infinity(),
                            numeric_limits<ap_float>::infinity(), -1);
    
    for (const NumericRange &range : left_ranges) {
        if (range.partition_index == left_partition) {
            left_range = range;
            break;
        }
    }
    
    for (const NumericRange &range : right_ranges) {
        if (range.partition_index == right_partition) {
            right_range = range;
            break;
        }
    }
    
    if (left_range.partition_index == -1 || right_range.partition_index == -1) {
        // Partition not found, be conservative
        return 2; // unknown
    }
    
    // Now evaluate the comparison based on the ranges
    // left_range: [left_lower, left_upper)
    // right_range: [right_lower, right_upper)
    
    ap_float left_lower = left_range.lower;
    ap_float left_upper = left_range.upper;
    ap_float right_lower = right_range.lower;
    ap_float right_upper = right_range.upper;
    
    // Evaluate based on comparison operator
    switch (comp_op) {
        case lt: // left < right
            // Definitely true if: max(left) < min(right), i.e., left_upper <= right_lower
            // Definitely false if: min(left) >= max(right), i.e., left_lower >= right_upper
            if (left_upper <= right_lower) {
                return 1; // definitely true
            } else if (left_lower >= right_upper) {
                return 0; // definitely false
            } else {
                return 2; // unknown
            }
            
        case le: // left <= right
            // Definitely true if: max(left) <= max(right), but more precisely:
            // all values in left are <= all values in right
            if (left_upper <= right_lower) {
                return 1; // definitely true (even stricter than <=)
            } else if (left_lower > right_upper) {
                return 0; // definitely false
            } else {
                return 2; // unknown
            }
            
        case eq: // left == right
            // Definitely true only if both ranges are the same single point
            // This is very rare in practice
            if (left_lower == left_upper && right_lower == right_upper && 
                left_lower == right_lower) {
                return 1; // definitely true
            } else if (left_upper <= right_lower || right_upper <= left_lower) {
                return 0; // definitely false (ranges don't overlap)
            } else {
                return 2; // unknown
            }
            
        case ge: // left >= right
            // Definitely true if: min(left) >= max(right), i.e., left_lower >= right_upper
            // Definitely false if: max(left) < min(right), i.e., left_upper < right_lower
            if (left_lower >= right_upper) {
                return 1; // definitely true
            } else if (left_upper <= right_lower) {
                return 0; // definitely false
            } else {
                return 2; // unknown
            }
            
        case gt: // left > right
            // Definitely true if: min(left) >= max(right), i.e., left_lower >= right_upper
            // Definitely false if: max(left) <= min(right), i.e., left_upper <= right_lower
            if (left_lower >= right_upper) {
                return 1; // definitely true
            } else if (left_upper <= right_lower) {
                return 0; // definitely false
            } else {
                return 2; // unknown
            }
            
        case ue: // undefined/unknown equality
        default:
            return 2; // unknown
    }
}

shared_ptr<ArithmeticExpression> 
DomainAbstractionNumericHelper::parse_arithmetic_expression(NumericVariableProxy num_var) {
    // TODO: Implement parsing of arithmetic expressions
    // This is needed to handle assignment axioms like: derived := x + y * 2
    return nullptr;
}

} // namespace domain_abstractions
