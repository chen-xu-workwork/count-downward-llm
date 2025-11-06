#include "numeric_helper.h"
#include "domain_abstraction_factory.h"

#include "../numeric_pdbs/arithmetic_expression.h"
#include "../numeric_pdbs/numeric_condition.h"
#include "../task_tools.h"

#include <algorithm>
#include <cassert>
#include <queue>
#include <unordered_set>

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
    verify_no_non_numeric_axioms(task_proxy);
    verify_no_conditional_effects(task_proxy);
    
    n_numeric_variables = task_proxy.get_numeric_variables().size();
    n_propositional_variables = task_proxy.get_variables().size();
    
    // Initialize ID mappings and derived variable tracking
    is_derived_num_var.resize(n_numeric_variables, false);
    is_derived_prop_var.resize(n_propositional_variables, false);
    
    // Initialize dependency structures
    axiom_dependencies.resize(n_numeric_variables);
    reverse_axiom_dependencies.resize(n_numeric_variables);
    
    // Build internal data structures
    find_derived_variables();
    build_axiom_dependencies();
    print_axiom_dependency_trees();  // DEBUG: Print dependency trees
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
        
        // TODO: are axioms sorted?
        // Forward dependencies: derived variable depends on left and right
        assert(derived_id >= 0 && derived_id < static_cast<int>(axiom_dependencies.size()));
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
        assert(derived_id >= 0 && derived_id < static_cast<int>(reverse_axiom_dependencies.size()));
        if (left_id >= 0 && left_id < static_cast<int>(reverse_axiom_dependencies.size())) {
            if (std::find(reverse_axiom_dependencies[left_id].begin(),
                         reverse_axiom_dependencies[left_id].end(),
                         derived_id) == reverse_axiom_dependencies[left_id].end()) {
                reverse_axiom_dependencies[left_id].push_back(derived_id);
            }
        }
        assert(derived_id >= 0 && derived_id < static_cast<int>(reverse_axiom_dependencies.size()));
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

void DomainAbstractionNumericHelper::print_axiom_dependency_trees() {
    
    AssignmentAxiomsProxy assignment_axioms = task_proxy.get_assignment_axioms();
    
    for (AssignmentAxiomProxy axiom : assignment_axioms) {
        int derived_id = axiom.get_assignment_variable().get_id();
        int left_id = axiom.get_left_variable().get_id();
        int right_id = axiom.get_right_variable().get_id();
        cal_operator op = axiom.get_arithmetic_operator_type();
        
        string op_str;
        switch (op) {
            case cal_operator::sum: op_str = "+"; break;
            case cal_operator::diff: op_str = "-"; break;
            case cal_operator::mult: op_str = "*"; break;
            case cal_operator::divi: op_str = "/"; break;
            default: op_str = "?"; break;
        }
        
        NumericVariableProxy derived_var = axiom.get_assignment_variable();
        NumericVariableProxy left_var = axiom.get_left_variable();
        NumericVariableProxy right_var = axiom.get_right_variable();
    }
    
    // Print comparison axiom dependencies (numeric -> propositional)
    ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
    
    for (ComparisonAxiomProxy axiom : comparison_axioms) {
        NumericVariableProxy left_var = axiom.get_left_variable();
        NumericVariableProxy right_var = axiom.get_right_variable();
        comp_operator op = axiom.get_comparison_operator_type();
        FactProxy true_fact = axiom.get_true_fact();
        
        int left_id = left_var.get_id();
        int right_id = right_var.get_id();
        int prop_var_id = true_fact.get_variable().get_id();
        
        string op_str;
        switch (op) {
            case comp_operator::lt: op_str = "<"; break;
            case comp_operator::le: op_str = "<="; break;
            case comp_operator::eq: op_str = "=="; break;
            case comp_operator::ge: op_str = ">="; break;
            case comp_operator::gt: op_str = ">"; break;
            default: op_str = "?"; break;
        }
    }
    
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
    
    // Track which numeric variables are modified by operators
    unordered_set<int> modified_numeric_vars;
    
    for (OperatorProxy op : operators) {
        // Check which numeric variables this operator modifies
        for (auto ass_eff_proxy : op.get_ass_effects()) {
            NumAssProxy ass_eff = ass_eff_proxy.get_assignment();
            int num_var_id = ass_eff.get_affected_variable().get_id();
            modified_numeric_vars.insert(num_var_id);
        }
        
        build_abstract_operator(op, abstract_operators);
    }
    
    return abstract_operators;
}

void DomainAbstractionNumericHelper::build_abstract_operator(
    const OperatorProxy &op,
    vector<AbstractOperator> &operators) {
    
    // Build abstract operator following the same pattern as factory's
    // build_abstract_operators, but adapted for numeric effects
    
    // DEBUG: Track specific operators
    string op_name = op.get_name();
    
    // All variable value pairs that are a prevail condition
    vector<Fact> prev_pairs;
    // All variable value pairs that are a precondition (value != -1)
    vector<Fact> pre_pairs;
    // All variable value pairs that are an effect
    vector<Fact> eff_pairs;
    // All variable value pairs that are an effect without precondition
    vector<Fact> effects_without_pre;

    ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
    vector<int> comparison_axiom_var_ids;
    for (ComparisonAxiomProxy axiom : comparison_axioms) {
        comparison_axiom_var_ids.push_back(axiom.get_true_fact().get_variable().get_id());
        assert(axiom.get_true_fact().get_variable().get_id() == axiom.get_false_fact().get_variable().get_id());
    }
    


    int num_variables = task_proxy.get_variables().size();
    vector<int> has_precondition_on_var(num_variables, -1);
    vector<int> has_effect_on_var(num_variables, -1);

    // Process propositional preconditions
    for (FactProxy pre : op.get_preconditions()) {
        int var_id = pre.get_variable().get_id();
        
        // Skip trivial variables - they're completely abstracted away
        if (variable_is_trivial(var_id)) {
            continue;
            has_precondition_on_var[var_id] = 0; // No meaning in original code?
        }
        has_precondition_on_var[var_id] = 0; // No meaning in original code?
        
        // Map concrete value to abstract value
        int abstract_val = domain_mapping[var_id][pre.get_value()];
        has_precondition_on_var[var_id] = abstract_val;
    }

    // Process propositional effects
    for (EffectProxy eff : op.get_effects()) {
        int var_id = eff.get_fact().get_variable().get_id();
        
        // Skip trivial variables - they're completely abstracted away
        if (variable_is_trivial(var_id)) {
            continue;
        }
        
        // There should never be a comparison axiom variable here
        assert(find(comparison_axiom_var_ids.begin(), comparison_axiom_var_ids.end(), var_id) 
            == comparison_axiom_var_ids.end());
        
        // Map concrete value to abstract value
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
        
        // Skip trivial variables - they're completely abstracted away
        if (variable_is_trivial(var_id)) {
            continue;
        }
        
        // Map concrete value to abstract value
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
        effects_without_pre, ass_effects, concrete_op_id, operators, op);
}

void DomainAbstractionNumericHelper::multiply_out_propositional(
    int pos, int cost, vector<Fact> &prev_pairs,
    vector<Fact> &pre_pairs,
    vector<Fact> &eff_pairs,
    const vector<Fact> &effects_without_pre,
    const vector<NumAssProxy> &ass_effects,
    int concrete_op_id,
    vector<AbstractOperator> &operators,
    const OperatorProxy &op) {
    
    if (pos == static_cast<int>(effects_without_pre.size())) {
        // All effects without precondition have been checked.
        // Now enumerate numeric partition transitions.
        if (!eff_pairs.empty() || !ass_effects.empty()) {
            size_t ops_before = operators.size();
            
            vector<TransitionInfo> transitions = 
                compute_hash_effects_with_preconditions(pre_pairs, eff_pairs, ass_effects);
            
            // Create one abstract operator per transition
            for (const TransitionInfo &trans : transitions) {
                // Add partition facts to pre_pairs and eff_pairs (for regression)
                // In regression the match tree enforces CURRENT state conditions via regression_preconditions.
                // We therefore put TARGET partition facts into eff_pairs so they become regression preconditions,
                // while SOURCE partition facts remain in pre_pairs for predecessor-side checks during enumeration.
                vector<Fact> extended_pre_pairs = pre_pairs;
                vector<Fact> extended_eff_pairs = eff_pairs;
                vector<Fact> extended_prev_pairs = prev_pairs;
                
                // Sanity check: source and target facts must have same size
                if (trans.source_partition_facts.size() != trans.target_partition_facts.size()) {
                    cout << "ERROR: Mismatched partition facts! source=" << trans.source_partition_facts.size()
                         << " target=" << trans.target_partition_facts.size() << endl;
                    exit(1);
                }
                
                extended_pre_pairs.insert(extended_pre_pairs.end(),
                                         trans.source_partition_facts.begin(),
                                         trans.source_partition_facts.end());
                extended_eff_pairs.insert(extended_eff_pairs.end(),
                                         trans.target_partition_facts.begin(),
                                         trans.target_partition_facts.end());
                extended_prev_pairs.insert(extended_prev_pairs.end(),
                                         trans.prevail_facts.begin(),
                                         trans.prevail_facts.end());

                // Extract numeric transition information for cascade enumeration
                vector<int> changed_numeric_vars;
                vector<int> source_partitions_list;
                vector<int> target_partitions_list;
                
                for (size_t i = 0; i < trans.source_partition_facts.size(); ++i) {
                    // Extract the numeric variable ID and partitions
                    // source_partition_facts[i].var is the abstract state variable ID
                    // We need to convert back to numeric variable ID
                    int abstract_var_id = trans.source_partition_facts[i].var;
                    int num_var_id = abstract_var_id - domain_sizes.size();  // Numeric vars come after propositional
                    
                    
                    changed_numeric_vars.push_back(num_var_id);
                    source_partitions_list.push_back(trans.source_partition_facts[i].value);
                    target_partitions_list.push_back(trans.target_partition_facts[i].value);
                }
                
                // Build predecessor-only preconditions from comparison axiom preconditions
                // of the concrete operator. These must hold in the predecessor, but should
                // not affect hash effects or MatchTree applicability on the current state.
                vector<Fact> predecessor_only_pres;
                // We don't need that
                //{
                //    ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
                //    vector<int> comparison_axiom_var_ids;
                //    comparison_axiom_var_ids.reserve(comparison_axioms.size());
                //    for (ComparisonAxiomProxy ax : comparison_axioms) {
                //        comparison_axiom_var_ids.push_back(ax.get_true_fact().get_variable().get_id());
                //    }
                //    for (FactProxy pre : op.get_preconditions()) {
                //        int var_id = pre.get_variable().get_id();
                //        // Only collect comparison axiom preconditions (skip trivial variables)
                //        if (variable_is_trivial(var_id))
                //            continue;
                //        if (find(comparison_axiom_var_ids.begin(), comparison_axiom_var_ids.end(), var_id)
                //            != comparison_axiom_var_ids.end()) {
                //            int abstract_val = domain_mapping[var_id][pre.get_value()];
                //            predecessor_only_pres.emplace_back(var_id, abstract_val);
                //        }
                //    }
                //}

                std::vector<int> single_hash_effect = {};
                operators.emplace_back(
                    extended_prev_pairs,        // prevail conditions (propositional only)
                    extended_pre_pairs,         // preconditions (propositional + source partitions)
                    extended_eff_pairs,         // effects (propositional + target partitions)
                    cost,                       // operator cost
                    hash_multipliers,           // hash multipliers
                    concrete_op_id              // concrete operator ID
                );                            
            }
        }
    } else {
        // For each possible value for the current variable, build an
        // abstract operator (multiply-out for effects without precondition)
        int var_id = effects_without_pre[pos].var;
        int eff = effects_without_pre[pos].value;
        
        // Enumerate all abstract values (trivial variables already filtered out)
        for (int i = 0; i < this->domain_sizes[var_id]; ++i) {
            if (i != eff) {
                pre_pairs.emplace_back(var_id, i);
                eff_pairs.emplace_back(var_id, eff);
            } else {
                prev_pairs.emplace_back(var_id, i);
            }
            multiply_out_propositional(
                pos + 1, cost, prev_pairs, pre_pairs, eff_pairs,
                effects_without_pre, ass_effects, concrete_op_id, operators, op);
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
    const vector<Fact> &pre_pairs,
    const vector<Fact> &eff_pairs,
    const vector<NumAssProxy> &ass_effects) {
    
    vector<int> hash_effects;
    
    // Compute base hash effect from propositional effects
    // Following the same pattern as domain_abstraction_factory.cc:
    // In regression, eff_pairs contains the old values (where we came from)
    // and pre_pairs contains the new values (where we're going to)
    int base_hash_effect = 0;
    assert(pre_pairs.size() == eff_pairs.size());



    
    for (size_t i = 0; i < pre_pairs.size(); ++i) {
        int var_id = pre_pairs[i].var;
        assert(var_id == eff_pairs[i].var);
        int old_val = eff_pairs[i].value;
        int new_val = pre_pairs[i].value;
        int effect = (new_val - old_val) * hash_multipliers[var_id];
        base_hash_effect += effect;
        
  
    }
    
    
    // If no numeric effects, just return the base effect
    if (ass_effects.empty()) {
        hash_effects.push_back(base_hash_effect);
        return hash_effects;
    }
    
    // Identify which numeric variables are affected
    vector<bool> affected_numeric_vars(numeric_domain_mapping.size(), false);
    vector<int> affected_var_list;
    for (const NumAssProxy &ass_eff : ass_effects) {
        int num_var_id = ass_eff.get_affected_variable().get_id();
        if (num_var_id >= 0 && num_var_id < static_cast<int>(numeric_domain_mapping.size())) {
            affected_numeric_vars[num_var_id] = true;
            affected_var_list.push_back(num_var_id);

        } else {
            exit(1);
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
                // 1. Direct cascades: Compute affected comparison axioms (propositional cascades)
                vector<Fact> affected_facts = 
                    compute_affected_comparison_axioms(changed_vars, old_parts, new_parts);
                
                for (const Fact &fact : affected_facts) {
                    // Add propositional variable contribution
                    // This is conservative - we add both true and false possibilities
                    total_effect += fact.value * hash_multipliers[fact.var];
                }
                
                // 2. Indirect cascades: Compute affected assignment axioms (numeric cascades)
                // These are derived numeric variables that change due to source variable changes,
                // which then may affect comparison axioms
                vector<Fact> assignment_cascade_facts = 
                    compute_assignment_axiom_cascades(changed_vars, old_parts, new_parts);
                
                for (const Fact &fact : assignment_cascade_facts) {
                    // Add propositional variable contribution from indirect cascades
                    total_effect += fact.value * hash_multipliers[fact.var];
                }
            }
            
            hash_effects.push_back(total_effect);
            return;
        }
        
        if (affected_numeric_vars[var_idx]) {
            // This variable is affected - enumerate reachable partition transitions
            int num_partitions = numeric_domain_sizes[var_idx];
            // Hash multipliers for numeric vars come after ALL propositional vars
            if (domain_sizes.size() + var_idx >= static_cast<int>(hash_multipliers.size())) {
                exit(1);
            }
            int hash_multiplier = hash_multipliers[domain_sizes.size() + var_idx];
            
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

vector<TransitionInfo> DomainAbstractionNumericHelper::compute_hash_effects_with_preconditions(
    const vector<Fact> &pre_pairs,
    const vector<Fact> &eff_pairs,
    const vector<NumAssProxy> &ass_effects) {
    
    vector<TransitionInfo> transitions;
    
    assert(pre_pairs.size() == eff_pairs.size());
    
    // Identify which numeric variables are affected
    vector<bool> affected_numeric_vars(numeric_domain_mapping.size(), false);
    for (const NumAssProxy &ass_eff : ass_effects) {
        int num_var_id = ass_eff.get_affected_variable().get_id();
        assert(num_var_id >= 0 && num_var_id < static_cast<int>(numeric_domain_mapping.size()));
       
        // Only enumerate partitions for REFINED numeric variables (> 1 partition)
        if (numeric_domain_sizes[num_var_id] > 1) {
            affected_numeric_vars[num_var_id] = true;

        } 
    }
    
    // PARTITION TRANSITION ENUMERATION:
    // We enumerate partition transitions in PROGRESSION semantics (easier to understand):
    // - source_partition = forward PRE (where operator starts)
    // - target_partition = forward POST (where operator ends)
    // 
    // Then we convert to REGRESSION for the hash effect:
    // In regression, we go backwards, so:
    // - Current state (where we are) = forward POST = target_partition
    // - Predecessor state (where we go) = forward PRE = source_partition
    // - Hash effect moves from current to predecessor: (pre - post) = (source - target)
    // 
    // Example: forward effect v += 1 with partitions {(-inf,9), [9,inf)}
    //   Transition 0->1: source=0 (PRE: v<9), target=1 (POST: v>=9)
    //   In regression: current has v in partition 1, predecessor has v in partition 0
    //   Hash effect = (0 - 1) * multiplier = -multiplier
    function<void(size_t, vector<Fact>&, vector<Fact>&, vector<int>&, vector<int>&, vector<int>&)> enumerate_targets =
        [&](size_t var_idx, 
            vector<Fact> &source_facts, vector<Fact> &target_facts,
            vector<int> &changed_vars, vector<int> &old_parts, vector<int> &new_parts) {
        
        if (var_idx == numeric_domain_mapping.size()) {
            // Base case: we've fixed a combination of target partitions
            // Now add the transition with hash effect and partition facts
            
            // NOTE: We do NOT compute cascading effects here anymore!
            // Comparison axiom cascades (propositional variables derived from comparisons)
            // and assignment axiom cascades (derived numeric variables) are now handled
            // ON-THE-FLY during Dijkstra search, not pre-computed in operators.
            // This prevents operator explosion and allows more accurate evaluation.
            
            TransitionInfo trans;
            trans.source_partition_facts = source_facts;
            trans.target_partition_facts = target_facts;
            transitions.push_back(trans);
            
            return;
        }
        
        if (affected_numeric_vars[var_idx]) {
            // This variable is affected - enumerate target partitions
            int num_partitions = numeric_domain_sizes[var_idx];
            
            // Find the assignment effect for this variable
            const NumAssProxy *ass_eff_for_var = nullptr;
            for (const NumAssProxy &ass_eff : ass_effects) {
                if (ass_eff.get_affected_variable().get_id() == static_cast<int>(var_idx)) {
                    ass_eff_for_var = &ass_eff;
                    break;
                }
            }
            
            // For each source partition (progression PRE), determine which target partitions 
            // can be reached (progression POST).
            // This uses PROGRESSION semantics: source = where we start, target = where we end
            for (int source_partition = 0; source_partition < num_partitions; ++source_partition) {
                vector<int> reachable_targets;
                
                if (ass_eff_for_var) {
                    // Compute which target partitions can be reached from this source
                    // in the FORWARD/PROGRESSION direction
                    reachable_targets = compute_reachable_partitions(
                        var_idx, source_partition, *ass_eff_for_var);
                    
                } else {
                    cout << "CRITICAL ERROR: Ass eff var not present" << endl;
                    utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
                }
                
                // For each reachable target partition, create a transition
                for (int target_partition : reachable_targets) {

                    // Add both source and target partition facts
                    // The variable ID in the abstract state is: domain_sizes.size() + var_idx
                    int abstract_num_var_id = domain_sizes.size() + var_idx;
                    source_facts.emplace_back(abstract_num_var_id, source_partition);
                    target_facts.emplace_back(abstract_num_var_id, target_partition);
                    
                    
                    // Track this change for cascades
                    changed_vars.push_back(var_idx);
                    old_parts.push_back(source_partition);
                    new_parts.push_back(target_partition);
                    
                    enumerate_targets(var_idx + 1,
                                    source_facts, target_facts, changed_vars, old_parts, new_parts);
                    
                    // Backtrack
                    source_facts.pop_back();
                    target_facts.pop_back();
                    changed_vars.pop_back();
                    old_parts.pop_back();
                    new_parts.pop_back();
                }
            }
        } else {
            // Unaffected variable: if refined (>1 partition), enforce frame-like identity
            int num_partitions = numeric_domain_sizes[var_idx];
            if (num_partitions > 1) {
                int abstract_num_var_id = domain_sizes.size() + var_idx;
                for (int p = 0; p < num_partitions; ++p) {
                    // Add identity partition facts (P -> P) with zero hash contribution
                    source_facts.emplace_back(abstract_num_var_id, p);
                    target_facts.emplace_back(abstract_num_var_id, p);
                    enumerate_targets(var_idx + 1, source_facts, target_facts,
                                      changed_vars, old_parts, new_parts);
                    source_facts.pop_back();
                    target_facts.pop_back();
                }
            } else {
                // Trivial numeric variable: nothing to add
                enumerate_targets(var_idx + 1, source_facts, target_facts,
                                  changed_vars, old_parts, new_parts);
            }
        }
    };
    
    vector<Fact> source_facts, target_facts;
    vector<int> changed_vars, old_parts, new_parts;
    enumerate_targets(0, source_facts, target_facts, changed_vars, old_parts, new_parts);
    

    
    return transitions;
}

vector<Fact> DomainAbstractionNumericHelper::compute_assignment_axiom_cascades(
    const vector<int> &changed_numeric_vars,
    const vector<int> &old_partitions,
    const vector<int> &new_partitions) const {
    
    // Compute which derived numeric variables (from assignment axioms) change,
    // and then check if those changes affect comparison axioms
    
    vector<Fact> affected_facts;
    
    // Track which derived numeric variables change and their new partitions
    vector<int> derived_changed_vars;
    vector<int> derived_old_partitions;
    vector<int> derived_new_partitions;
    
    // For each assignment axiom, check if it depends on a changed variable
    AssignmentAxiomsProxy assignment_axioms = task_proxy.get_assignment_axioms();
    for (AssignmentAxiomProxy axiom : assignment_axioms) {
        int derived_var_id = axiom.get_assignment_variable().get_id();
        int left_var_id = axiom.get_left_variable().get_id();
        int right_var_id = axiom.get_right_variable().get_id();
        cal_operator op = axiom.get_arithmetic_operator_type();
        
        // Check if this axiom depends on any changed variable
        bool depends_on_changed = false;
        for (int changed_id : changed_numeric_vars) {
            if (left_var_id == changed_id || right_var_id == changed_id) {
                depends_on_changed = true;
                break;
            }
        }
        
        if (!depends_on_changed) {
            continue;
        }
        
        // This assignment axiom is affected!
        // Compute the old and new ranges for the derived variable
        
        // Get partition ranges for left and right variables (old state)
        ap_float left_lower_old, left_upper_old;
        ap_float right_lower_old, right_upper_old;
        
        // Find old partitions for left and right
        int left_partition_old = -1;
        int right_partition_old = -1;
        
        for (size_t i = 0; i < changed_numeric_vars.size(); ++i) {
            if (changed_numeric_vars[i] == left_var_id) {
                left_partition_old = old_partitions[i];
            }
            if (changed_numeric_vars[i] == right_var_id) {
                right_partition_old = old_partitions[i];
            }
        }
        
        // If a variable wasn't changed, we don't know its partition in this context
        // This is a limitation - we're computing cascades based only on the
        // variables that changed. For a complete solution, we'd need the full state.
        // For now, skip this axiom if we don't have both partitions
        if (left_partition_old == -1 || right_partition_old == -1) {
            continue;
        }
        
        // Get the ranges
        if (left_var_id >= static_cast<int>(numeric_domain_mapping.size()) ||
            right_var_id >= static_cast<int>(numeric_domain_mapping.size())) {
            continue; // Variable not in mapping
        }
        
        const NumericDomainMapping &left_mapping = *numeric_domain_mapping[left_var_id];
        const NumericDomainMapping &right_mapping = *numeric_domain_mapping[right_var_id];
        
        const vector<NumericRange> &left_ranges = left_mapping.get_ranges();
        const vector<NumericRange> &right_ranges = right_mapping.get_ranges();
        
        // Find old ranges
        bool found_left_old = false, found_right_old = false;
        for (const NumericRange &range : left_ranges) {
            if (range.partition_index == left_partition_old) {
                left_lower_old = range.lower;
                left_upper_old = range.upper;
                found_left_old = true;
                break;
            }
        }
        for (const NumericRange &range : right_ranges) {
            if (range.partition_index == right_partition_old) {
                right_lower_old = range.lower;
                right_upper_old = range.upper;
                found_right_old = true;
                break;
            }
        }
        
        if (!found_left_old || !found_right_old) {
            continue; // Couldn't find ranges
        }
        
        // Compute old derived range
        pair<ap_float, ap_float> old_derived_range = 
            apply_range_operation(left_lower_old, left_upper_old,
                                right_lower_old, right_upper_old, op);
        
        // Now compute new derived range (with new partitions)
        ap_float left_lower_new, left_upper_new;
        ap_float right_lower_new, right_upper_new;
        
        int left_partition_new = -1;
        int right_partition_new = -1;
        
        for (size_t i = 0; i < changed_numeric_vars.size(); ++i) {
            if (changed_numeric_vars[i] == left_var_id) {
                left_partition_new = new_partitions[i];
            }
            if (changed_numeric_vars[i] == right_var_id) {
                right_partition_new = new_partitions[i];
            }
        }
        
        if (left_partition_new == -1 || right_partition_new == -1) {
            continue;
        }
        
        // Find new ranges
        bool found_left_new = false, found_right_new = false;
        for (const NumericRange &range : left_ranges) {
            if (range.partition_index == left_partition_new) {
                left_lower_new = range.lower;
                left_upper_new = range.upper;
                found_left_new = true;
                break;
            }
        }
        for (const NumericRange &range : right_ranges) {
            if (range.partition_index == right_partition_new) {
                right_lower_new = range.lower;
                right_upper_new = range.upper;
                found_right_new = true;
                break;
            }
        }
        
        if (!found_left_new || !found_right_new) {
            continue;
        }
        
        // Compute new derived range
        pair<ap_float, ap_float> new_derived_range = 
            apply_range_operation(left_lower_new, left_upper_new,
                                right_lower_new, right_upper_new, op);
        
        // Now determine which partitions the old and new ranges correspond to
        if (derived_var_id >= static_cast<int>(numeric_domain_mapping.size())) {
            continue; // Derived variable not in mapping
        }
        
        const NumericDomainMapping &derived_mapping = *numeric_domain_mapping[derived_var_id];
        const vector<NumericRange> &derived_ranges = derived_mapping.get_ranges();
        
        // Find which partition(s) the old and new ranges overlap with
        // For simplicity, take the first overlapping partition
        // Derived ranges use [lower, upper) convention (left-inclusive, right-exclusive)
        int old_derived_partition = -1;
        int new_derived_partition = -1;
        
        for (const NumericRange &range : derived_ranges) {
            if (old_derived_partition == -1 &&
                range.overlaps_with(old_derived_range.first, old_derived_range.second, 
                                   true, false)) {
                old_derived_partition = range.partition_index;
            }
            if (new_derived_partition == -1 &&
                range.overlaps_with(new_derived_range.first, new_derived_range.second,
                                   true, false)) {
                new_derived_partition = range.partition_index;
            }
            if (old_derived_partition != -1 && new_derived_partition != -1) {
                break;
            }
        }
        
        // If the derived variable changed partition, track it
        if (old_derived_partition != -1 && new_derived_partition != -1 &&
            old_derived_partition != new_derived_partition) {
            derived_changed_vars.push_back(derived_var_id);
            derived_old_partitions.push_back(old_derived_partition);
            derived_new_partitions.push_back(new_derived_partition);
        }
    }
    
    // Now check which comparison axioms are affected by the derived variable changes
    if (!derived_changed_vars.empty()) {
        vector<Fact> comparison_facts = 
            compute_affected_comparison_axioms(derived_changed_vars,
                                             derived_old_partitions,
                                             derived_new_partitions);
        affected_facts.insert(affected_facts.end(),
                            comparison_facts.begin(),
                            comparison_facts.end());
    }
    
    return affected_facts;
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
        
        // Get the facts
        FactProxy true_fact = axiom.get_true_fact();
        FactProxy false_fact = axiom.get_false_fact();
        int prop_var_id = true_fact.get_variable().get_id();
        
        // CRITICAL FIX: Even if partitions don't change, the comparison result MIGHT change
        // because the concrete numeric values change within the partition!
        // This is especially important when variables have only 1 partition (fully abstract).
        // 
        // We must be CONSERVATIVE: if a variable in the comparison is modified,
        // assume BOTH possible truth values for the comparison axiom.
        
        // Check if partitions actually changed
        bool partition_changed = (left_partition_old != left_partition_new || 
                                  right_partition_old != right_partition_new);
        
        if (partition_changed) {
            // Partitions changed - evaluate comparison based on new partitions
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
        } else {
            // Partitions didn't change, but numeric values DID change
            // We can't determine the exact result, so be conservative
            // and add BOTH possible truth values
            affected_facts.emplace_back(prop_var_id, true_fact.get_value());
            affected_facts.emplace_back(prop_var_id, false_fact.get_value());
        }
    }
    
    return affected_facts;
}

vector<int> DomainAbstractionNumericHelper::compute_reachable_partitions(
    int numeric_var_id,
    int source_partition,
    const NumAssProxy &ass_effect) const {
    
    // Get the numeric domain mapping for this variable
    const NumericDomainMapping &mapping = *numeric_domain_mapping[numeric_var_id];
    
    // Get the effect operator and operand value
    f_operator op_type = ass_effect.get_assigment_operator_type();
    NumericVariableProxy assigned_var = ass_effect.get_assigned_variable();
    ap_float operand_value = assigned_var.get_initial_state_value();
    //assert that assigned var is always constant!

    assert(assigned_var.get_var_type() == numType::constant);

    // IMPORTANT: This function computes partition transitions using PROGRESSION semantics.
    // Even though we're building operators for REGRESSION search, we enumerate transitions
    // in the forward/progression direction because it's easier to reason about:
    //   - source_partition = where the operator starts (forward PRE)
    //   - target_partition = where the operator ends (forward POST)
    //
    // Example: For effect "v += 7" with partitions {0: [-inf,1000), 1: [1000,inf)}:
    //   - Source 0, target 0: v starts in [0, 1000), stays in [0, 1000) after adding 7
    //   - Source 0, target 1: v starts in [993, 1000), ends in [1000, 1007) after adding 7
    //   - Source 1, target 1: v starts in [1000, inf), stays in [1000, inf) after adding 7
    //   - No transition 1→0: can't decrease from 1 to 0 with += operation
    //
    // Later, the hash effect will be computed using REGRESSION formula:
    //   hash_effect = (source - target) * multiplier
    // This correctly moves from current state (target) to predecessor (source) in regression.
    
    // Use NumericDomainMapping method to compute reachable partitions
    return mapping.compute_reachable_partitions(source_partition, op_type, operand_value);
}

pair<ap_float, ap_float> DomainAbstractionNumericHelper::apply_range_operation(
    ap_float left_lower, ap_float left_upper,
    ap_float right_lower, ap_float right_upper,
    cal_operator op) const {
    
    // Use NumericDomainMapping static method
    return NumericDomainMapping::apply_range_operation(
        left_lower, left_upper, right_lower, right_upper, op);
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
    
    const NumericDomainMapping &left_mapping = *numeric_domain_mapping[left_var_id];
    const NumericDomainMapping &right_mapping = *numeric_domain_mapping[right_var_id];
    
    // Use evaluate_comparison_with method from NumericDomainMapping
    return left_mapping.evaluate_comparison_with(right_mapping, left_partition, right_partition, comp_op);
}

shared_ptr<ArithmeticExpression> 
DomainAbstractionNumericHelper::parse_arithmetic_expression(NumericVariableProxy num_var) {
    // TODO: Implement parsing of arithmetic expressions
    // This is needed to handle assignment axioms like: derived := x + y * 2
    return nullptr;
}

} // namespace domain_abstractions
