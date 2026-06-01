#include "numeric_helper.h"
#include "domain_abstraction_factory.h"

#include "../numeric_pdbs/arithmetic_expression.h"
#include "../numeric_pdbs/numeric_condition.h"
#include "../task_tools.h"

#include <algorithm>
#include <cassert>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <optional>

using namespace std;
using namespace arithmetic_expression;
using namespace numeric_condition;

namespace domain_abstractions {

// Hash function for vector of Facts
struct VectorFactHash {
    size_t operator()(const vector<Fact> &facts) const {
        size_t hash = 0;
        for (const Fact &f : facts) {
            // Combine var and value into hash using FNV-1a style mixing
            hash ^= (static_cast<size_t>(f.var) * 31 + f.value) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        }
        return hash;
    }
};

// Key for operator grouping: (prev_pairs, pre_pairs, eff_pairs, cost)
struct OperatorSignature {
    vector<Fact> prev_pairs;
    vector<Fact> pre_pairs;
    vector<Fact> eff_pairs;
    ap_float cost;
    
    bool operator==(const OperatorSignature &other) const {
        return cost == other.cost && 
               prev_pairs == other.prev_pairs && 
               pre_pairs == other.pre_pairs && 
               eff_pairs == other.eff_pairs;
    }
};

struct OperatorSignatureHash {
    VectorFactHash fact_hash;
    
    size_t operator()(const OperatorSignature &sig) const {
        size_t h1 = fact_hash(sig.prev_pairs);
        size_t h2 = fact_hash(sig.pre_pairs);
        size_t h3 = fact_hash(sig.eff_pairs);
        // Hash the cost as well
        size_t h4 = hash<ap_float>{}(sig.cost);
        // Combine all hashes
        size_t result = h1;
        result ^= h2 + 0x9e3779b9 + (result << 6) + (result >> 2);
        result ^= h3 + 0x9e3779b9 + (result << 6) + (result >> 2);
        result ^= h4 + 0x9e3779b9 + (result << 6) + (result >> 2);
        return result;
    }
};

static int debug_counter = 0;


DomainAbstractionNumericHelper::DomainAbstractionNumericHelper(
    const shared_ptr<AbstractTask> &task,
    const DomainMapping &domain_mapping,
    const NumericDomainMappings &numeric_domain_mapping,
    const vector<int> &domain_sizes,
    const vector<int> &numeric_domain_sizes,
    const vector<int> &hash_multipliers,
    bool combine_labels,
    shared_ptr<CEGARLogger> logger)
    : task(task), 
      task_proxy(*task),
      domain_mapping(domain_mapping),
      numeric_domain_mapping(numeric_domain_mapping),
      domain_sizes(domain_sizes),
      numeric_domain_sizes(numeric_domain_sizes),
      hash_multipliers(hash_multipliers),
      combine_labels(combine_labels),
      logger(logger) {
    
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
}


void DomainAbstractionNumericHelper::find_derived_variables() {
    // Analyze task axioms to identify derived variables
    
    // 1. Find derived numeric variables from assignment axioms
    // Assignment axioms have the form: derived_var := left_var op right_var
    // where op is one of: +, -, *, /
    AssignmentAxiomsProxy assignment_axioms = task_proxy.get_assignment_axioms();
    cached_assignment_axioms.clear();
    cached_assignment_axioms.reserve(assignment_axioms.size());
    
    for (AssignmentAxiomProxy axiom : assignment_axioms) {
        NumericVariableProxy derived_var = axiom.get_assignment_variable();
        int derived_id = derived_var.get_id();
        
        // Mark this as a derived numeric variable
        if (derived_id >= 0 && derived_id < static_cast<int>(is_derived_num_var.size())) {
            is_derived_num_var[derived_id] = true;
        }
        
        // Cache the axiom data for hot-path performance
        CachedAssignmentAxiom cached;
        cached.derived_id = derived_id;
        cached.left_id = axiom.get_left_variable().get_id();
        cached.right_id = axiom.get_right_variable().get_id();
        cached.left_type = axiom.get_left_variable().get_var_type();
        cached.right_type = axiom.get_right_variable().get_var_type();
        cached.left_const_val = (cached.left_type == numType::constant)
            ? axiom.get_left_variable().get_initial_state_value() : 0.0;
        cached.right_const_val = (cached.right_type == numType::constant)
            ? axiom.get_right_variable().get_initial_state_value() : 0.0;
        cached.op_type = axiom.get_arithmetic_operator_type();
        cached_assignment_axioms.push_back(cached);
    }
    
    // 2. Find derived propositional variables from comparison axioms
    // Comparison axioms have the form: (left_var comp_op right_var) 
    // which creates a derived propositional fact (boolean variable)
    ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
    comparison_axiom_by_var_id.reserve(comparison_axioms.size());
    cached_comparison_axioms.clear();
    cached_comparison_axioms.reserve(comparison_axioms.size());
    
    for (size_t axiom_idx = 0; axiom_idx < comparison_axioms.size(); ++axiom_idx) {
        ComparisonAxiomProxy axiom = comparison_axioms[axiom_idx];
        // The axiom produces a true_fact when the comparison holds
        // and a false_fact when it doesn't
        FactProxy true_fact = axiom.get_true_fact();
        FactProxy false_fact = axiom.get_false_fact();
        
        int var_id = true_fact.get_variable().get_id();
        
        // Mark this propositional variable as derived from a comparison
        if (var_id >= 0 && var_id < static_cast<int>(is_derived_prop_var.size())) {
            is_derived_prop_var[var_id] = true;
            comparison_axiom_by_var_id[var_id] = axiom_idx;
        }
        
        // Sanity check: true_fact and false_fact should be on same variable
        assert(var_id == false_fact.get_variable().get_id());
        
        // Cache the axiom data for hot-path performance
        CachedComparisonAxiom cached;
        cached.left_id = axiom.get_left_variable().get_id();
        cached.right_id = axiom.get_right_variable().get_id();
        cached.left_type = axiom.get_left_variable().get_var_type();
        cached.right_type = axiom.get_right_variable().get_var_type();
        cached.left_const_val = (cached.left_type == numType::constant)
            ? axiom.get_left_variable().get_initial_state_value() : 0.0;
        cached.right_const_val = (cached.right_type == numType::constant)
            ? axiom.get_right_variable().get_initial_state_value() : 0.0;
        cached.op_type = axiom.get_comparison_operator_type();
        cached.prop_var_id = var_id;
        cached.true_val = true_fact.get_value();
        cached.false_val = false_fact.get_value();
        cached_comparison_axioms.push_back(cached);
    }
    
    // Build reverse dependency map: numeric_var_id -> comparison axiom indices.
    // First determine the max numeric variable ID referenced by comparison axioms.
    int max_num_var_id = 0;
    for (const CachedComparisonAxiom &ax : cached_comparison_axioms) {
        if (ax.left_type != numType::constant && ax.left_id > max_num_var_id) {
            max_num_var_id = ax.left_id;
        }
        if (ax.right_type != numType::constant && ax.right_id > max_num_var_id) {
            max_num_var_id = ax.right_id;
        }
    }
    var_to_comparison_axiom_indices.clear();
    var_to_comparison_axiom_indices.resize(max_num_var_id + 1);
    for (size_t axiom_idx = 0; axiom_idx < cached_comparison_axioms.size(); ++axiom_idx) {
        const CachedComparisonAxiom &ax = cached_comparison_axioms[axiom_idx];
        if (ax.left_type != numType::constant && ax.left_id >= 0) {
            var_to_comparison_axiom_indices[ax.left_id].push_back(axiom_idx);
        }
        if (ax.right_type != numType::constant && ax.right_id >= 0 && ax.right_id != ax.left_id) {
            var_to_comparison_axiom_indices[ax.right_id].push_back(axiom_idx);
        }
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
        assert(derived_id >= 0 && derived_id < static_cast<int>(axiom_dependencies.size()));
        if (derived_id >= 0 && derived_id < static_cast<int>(axiom_dependencies.size())) {
            if (left_id >= 0) {
                axiom_dependencies[derived_id].push_back(left_id);
            }
            if (right_id >= 0 && right_id != left_id) {
                axiom_dependencies[derived_id].push_back(right_id);
            }
        }
        
        // Reverse dependencies: left and right variables affect derived variable
        assert(derived_id >= 0 && derived_id < static_cast<int>(reverse_axiom_dependencies.size()));
        if (left_id >= 0 && left_id < static_cast<int>(reverse_axiom_dependencies.size())) {
            reverse_axiom_dependencies[left_id].push_back(derived_id);
        }
        assert(derived_id >= 0 && derived_id < static_cast<int>(reverse_axiom_dependencies.size()));
        if (right_id >= 0 && right_id < static_cast<int>(reverse_axiom_dependencies.size()) 
            && right_id != left_id) {
            reverse_axiom_dependencies[right_id].push_back(derived_id);
        }
    }

    for (auto &deps : axiom_dependencies) {
        std::sort(deps.begin(), deps.end());
        deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
    }
    for (auto &deps : reverse_axiom_dependencies) {
        std::sort(deps.begin(), deps.end());
        deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
    }
    
    numeric_var_can_affect_comparison.assign(n_numeric_variables, false);
    std::vector<int> queue;
    queue.reserve(n_numeric_variables);
    for (const CachedComparisonAxiom &ax : cached_comparison_axioms) {
        if (ax.left_type != numType::constant && ax.left_id >= 0 && ax.left_id < n_numeric_variables && !numeric_var_can_affect_comparison[ax.left_id]) {
            numeric_var_can_affect_comparison[ax.left_id] = true;
            queue.push_back(ax.left_id);
        }
        if (ax.right_type != numType::constant && ax.right_id >= 0 && ax.right_id < n_numeric_variables && !numeric_var_can_affect_comparison[ax.right_id]) {
            numeric_var_can_affect_comparison[ax.right_id] = true;
            queue.push_back(ax.right_id);
        }
    }
    for (size_t i = 0; i < queue.size(); ++i) {
        int v = queue[i];
        for (int dep : axiom_dependencies[v]) {
            if (dep >= 0 && dep < n_numeric_variables && !numeric_var_can_affect_comparison[dep]) {
                numeric_var_can_affect_comparison[dep] = true;
                queue.push_back(dep);
            }
        }
    }
}

vector<AbstractOperator> DomainAbstractionNumericHelper::build_abstract_operators(const TaskProxy &task_proxy) {
    vector<AbstractOperator> abstract_operators;
    
    // Build abstract operators for all concrete operators
    OperatorsProxy operators = task_proxy.get_operators();
    
    // Track which numeric variables are modified by operators
    unordered_set<int> modified_numeric_vars;

    
    // Create grouping map if label reduction is enabled
    OperatorGroupingMap grouping_map;
    OperatorGroupingMap *grouping_map_ptr = combine_labels ? &grouping_map : nullptr;
    
    // Check if all costs are integers
    all_costs_are_ints = true;
    
    for (OperatorProxy op : operators) {
        ap_float cost = op.get_cost();
        assert(cost >= 0);
        
        // Check if cost is an integer
        if (all_costs_are_ints && cost != static_cast<ap_float>(static_cast<int>(cost))) {
            all_costs_are_ints = false;
        }
        
        // Check which numeric variables this operator modifies
        for (auto ass_eff_proxy : op.get_ass_effects()) {
            NumAssProxy ass_eff = ass_eff_proxy.get_assignment();
            int num_var_id = ass_eff.get_affected_variable().get_id();
            modified_numeric_vars.insert(num_var_id);
        }

        

        build_abstract_operator(op, abstract_operators, grouping_map_ptr);
    }
    
    return abstract_operators;
}

void DomainAbstractionNumericHelper::build_abstract_operator(
    const OperatorProxy &op,
    vector<AbstractOperator> &operators,
    OperatorGroupingMap *grouping_map) {
    
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

    // Use pre-built is_derived_prop_var instead of computing locally
    ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();

    int num_variables = task_proxy.get_variables().size();
    vector<int> has_precondition_on_var(num_variables, -1);
    vector<int> has_effect_on_var(num_variables, -1);

    // Process propositional preconditions
    for (FactProxy pre : op.get_preconditions()) {
        int var_id = pre.get_variable().get_id();

        if (variable_is_trivial(var_id)) {
            has_precondition_on_var[var_id] = 0; // No meaning in original code?
        } else if (!variable_is_trivial(var_id)) {
            // Map concrete value to abstract value
            int abstract_val = domain_mapping[var_id][pre.get_value()];
            has_precondition_on_var[var_id] = abstract_val;
        }
       
    }

    // Process propositional effects
    for (EffectProxy eff : op.get_effects()) {
        int var_id = eff.get_fact().get_variable().get_id();

        bool is_var_id_in_comparison_axioms = is_derived_prop_var[var_id];

        // Comparison axiom variables cannot be modified by effects directly
        assert(!is_var_id_in_comparison_axioms);
        
        // Skip trivial variables - they're completely abstracted away
        if (variable_is_trivial(var_id)) {
            continue;
        }
        
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
        bool is_var_id_in_comparison_axioms = is_derived_prop_var[var_id];
        
        // Skip trivial variables - they're completely abstracted away
        if (variable_is_trivial(var_id)) {
            continue;
        }
        
        // Map concrete value to abstract value
        int val = domain_mapping[var_id][pre.get_value()];
        if (has_effect_on_var[var_id] >= 0) {
            pre_pairs.emplace_back(var_id, val);
        } else {
            if (!is_var_id_in_comparison_axioms) {
                prev_pairs.emplace_back(var_id, val);
            } 
        }
    }

    for (FactProxy pre : op.get_preconditions()) {
        int var_id = pre.get_variable().get_id();
        bool is_var_id_in_comparison_axioms = is_derived_prop_var[var_id];
        
        // Skip trivial variables - they're completely abstracted away
        if (variable_is_trivial(var_id)) {
            continue;
        }
        
        // Map concrete value to abstract value
        int val = domain_mapping[var_id][pre.get_value()];
        if (is_var_id_in_comparison_axioms) {
            pre_pairs.emplace_back(var_id, val);
            eff_pairs.emplace_back(var_id, domain_mapping[var_id][2]); // unknown value
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
        op.get_id(), operators, grouping_map);
}

void DomainAbstractionNumericHelper::enumerate_abstract_transitions(
    const OperatorProxy &op,
    vector<Fact> &prev_pairs,
    vector<Fact> &pre_pairs,
    vector<Fact> &eff_pairs,
    const vector<Fact> &effects_without_pre,
    const vector<NumAssProxy> &ass_effects,
    int concrete_op_id,
    vector<AbstractOperator> &operators,
    OperatorGroupingMap *grouping_map) {
    
    // This method is recursive and handles two types of enumeration:
    // 1. Propositional effects without preconditions (multiply_out pattern)
    // 2. Numeric effects with partition transitions
    //
    // The recursion first handles all propositional effects_without_pre,
    // then creates the AbstractOperator which internally enumerates
    // numeric partition transitions in its constructor.
    
    multiply_out_propositional(
        0, op.get_cost(), prev_pairs, pre_pairs, eff_pairs,
        effects_without_pre, ass_effects, concrete_op_id, operators, op, grouping_map);
}

void DomainAbstractionNumericHelper::multiply_out_propositional(
    int pos, ap_float cost, vector<Fact> &prev_pairs,
    vector<Fact> &pre_pairs,
    vector<Fact> &eff_pairs,
    const vector<Fact> &effects_without_pre,
    const vector<NumAssProxy> &ass_effects,
    int concrete_op_id,
    vector<AbstractOperator> &operators,
    const OperatorProxy &op,
    OperatorGroupingMap *grouping_map) {
    
    if (pos == static_cast<int>(effects_without_pre.size())) {
        // All effects without precondition have been checked.
        // Now enumerate numeric partition transitions.
        if (!eff_pairs.empty() || !ass_effects.empty()) {
            size_t ops_before = operators.size();
            
            vector<TransitionInfo> transitions = 
                compute_hash_effects_with_preconditions(pre_pairs, eff_pairs, ass_effects, op);
            
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
                    if (logger) {
                        logger->log(Verbosity::INFO, "ERROR: Mismatched partition facts! source=", trans.source_partition_facts.size(),
                                   " target=", trans.target_partition_facts.size());
                    }
                    utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
                }

                // Build a set of variables already in pre_pairs to avoid duplicates
                // (Cascade facts may overlap with explicit operator preconditions on comparison axioms)
                unordered_set<int> vars_in_pre_pairs;
                for (const Fact &f : pre_pairs) {
                    vars_in_pre_pairs.insert(f.var);
                }

                // Add cascade facts, but skip any that would duplicate existing pre_pairs variables
                for (size_t i = 0; i < trans.source_partition_facts.size(); ++i) {
                    int var_id = trans.source_partition_facts[i].var;
                    if (vars_in_pre_pairs.count(var_id) == 0) {
                        extended_pre_pairs.push_back(trans.source_partition_facts[i]);
                        extended_eff_pairs.push_back(trans.target_partition_facts[i]);
                    }
                    // If already in pre_pairs, skip - the original precondition takes precedence
                }
                
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

                // Check if grouping is enabled and operator signature exists
                if (grouping_map) {
                    OperatorSignature sig{extended_prev_pairs, extended_pre_pairs, extended_eff_pairs, cost};
                    auto it = grouping_map->find(sig);
                    if (it != grouping_map->end()) {
                        // Operator with same signature exists - add concrete op id to it
                        operators[it->second].add_concrete_op_id(concrete_op_id);
                    } else {
                        // New signature - create operator and add to map
                        size_t new_idx = operators.size();
                        operators.emplace_back(
                            extended_prev_pairs,        // prevail conditions (propositional only)
                            extended_pre_pairs,         // preconditions (propositional + source partitions)
                            extended_eff_pairs,         // effects (propositional + target partitions)
                            cost,                       // operator cost
                            hash_multipliers,           // hash multipliers
                            vector<int>{concrete_op_id} // concrete operator ID
                        );
                        (*grouping_map)[sig] = new_idx;
                    }
                } else {
                    // No grouping - create operator as before
                    operators.emplace_back(
                        extended_prev_pairs,        // prevail conditions (propositional only)
                        extended_pre_pairs,         // preconditions (propositional + source partitions)
                        extended_eff_pairs,         // effects (propositional + target partitions)
                        cost,                       // operator cost
                        hash_multipliers,           // hash multipliers
                        vector<int>{concrete_op_id} // concrete operator ID
                    );
                }
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
                effects_without_pre, ass_effects, concrete_op_id, operators, op, grouping_map);
            if (i != eff) {
                pre_pairs.pop_back();
                eff_pairs.pop_back();
            } else {
                prev_pairs.pop_back();
            }
        }
    }
}

vector<TransitionInfo> DomainAbstractionNumericHelper::compute_hash_effects_with_preconditions(
    const vector<Fact> &pre_pairs,
    const vector<Fact> &eff_pairs,
    const vector<NumAssProxy> &ass_effects,
    const OperatorProxy &op) {
    
    vector<TransitionInfo> transitions;

    bool op_has_comparison_preconditions = false;
    for (FactProxy concrete_pre : op.get_preconditions()) {
        if (comparison_axiom_by_var_id.find(concrete_pre.get_variable().get_id()) != comparison_axiom_by_var_id.end()) {
            op_has_comparison_preconditions = true;
            break;
        }
    }
    
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
    // - target_partition = forward EFF (where operator ends)
    // 
    // Then we convert to REGRESSION for the hash effect:
    // In regression, we go backwards, so:
    // - Current state (where we are) = forward EFF = target_partition
    // - Predecessor state (where we go) = forward PRE = source_partition
    // - Hash effect moves from current to predecessor: (pre - eff) = (source - target)
    // 
    // Example: forward effect v += 1 with partitions {(-inf,9), [9,inf)}
    //   Transition 0->1: source=0 (PRE: v<9), target=1 (EFF: v>=9)
    //   In regression: current has v in partition 1, predecessor has v in partition 0
    //   Hash effect = (0 - 1) * multiplier = -multiplier
    function<void(size_t, vector<Fact>&, vector<Fact>&, vector<int>&, vector<int>&, vector<int>&)> enumerate_targets =
        [&](size_t var_idx, 
            vector<Fact> &source_facts, vector<Fact> &target_facts,
            vector<int> &changed_vars, vector<int> &old_parts, vector<int> &new_parts) {
        
        if (var_idx == numeric_domain_mapping.size()) {
            // Base case: we've fixed a combination of target partitions
            // FILTERING: Check if source partitions violate comparison preconditions
            
            unordered_map<int, int> partition_assignment;
            for (const Fact &fact : source_facts) {
                // fact.var is abstract var ID, convert to numeric var ID
                int num_var_id = fact.var - domain_sizes.size();
                partition_assignment[num_var_id] = fact.value;
            }

            bool changed_can_affect_comparison = false;
            for (int changed_id : changed_vars) {
                if (changed_id >= 0 && changed_id < static_cast<int>(numeric_var_can_affect_comparison.size()) &&
                    numeric_var_can_affect_comparison[changed_id]) {
                    changed_can_affect_comparison = true;
                    break;
                }
            }

            if (op_has_comparison_preconditions && changed_can_affect_comparison) {
                for (FactProxy concrete_pre : op.get_preconditions()) {
                    int var_id = concrete_pre.get_variable().get_id();
                    auto axiom_it = comparison_axiom_by_var_id.find(var_id);
                    if (axiom_it == comparison_axiom_by_var_id.end()) {
                        continue;
                    }

                    const CachedComparisonAxiom &ax = cached_comparison_axioms[axiom_it->second];
                    if (ax.left_type == numType::constant || ax.right_type == numType::constant) {
                        continue;
                    }

                    auto left_part_it = partition_assignment.find(ax.left_id);
                    auto right_part_it = partition_assignment.find(ax.right_id);
                    if (left_part_it == partition_assignment.end() || right_part_it == partition_assignment.end()) {
                        continue;
                    }

                    int eval = numeric_domain_mapping[ax.left_id]->evaluate_comparison_with(
                        *numeric_domain_mapping[ax.right_id],
                        left_part_it->second,
                        right_part_it->second,
                        ax.op_type);
                    int required_eval = (concrete_pre.get_value() == ax.true_val) ? 0 : 1;
                    if (eval != 2 && eval != required_eval) {
                        return;
                    }
                }
            }

            if (!op_has_comparison_preconditions && !changed_can_affect_comparison) {
                TransitionInfo trans;
                trans.source_partition_facts = source_facts;
                trans.target_partition_facts = target_facts;
                transitions.push_back(std::move(trans));
                return;
            }
            
            unordered_map<int, NumericRange> ranges;
            NumericVariablesProxy num_vars = task_proxy.get_numeric_variables();
            
            // Seed ranges from partition assignments and constants
            for (size_t nvar_id = 0; nvar_id < num_vars.size(); ++nvar_id) {
                NumericVariableProxy var = num_vars[nvar_id];
                
                if (var.get_var_type() == numType::constant) {
                    ap_float val = var.get_initial_state_value();
                    ranges[nvar_id] = NumericRange(val, val, true, true);
                } else if (var.get_var_type() == numType::regular) {
                    auto it = partition_assignment.find(nvar_id);
                    if (it != partition_assignment.end()) {
                        // Variable has explicit partition assignment
                        const NumericRange *rng = numeric_domain_mapping[nvar_id]->get_range_for_partition(it->second);
                        if (rng) {
                            ranges[nvar_id] = *rng;
                        }
                    } else if (numeric_domain_sizes[nvar_id] == 1) {
                        // Trivial variable (not in source_facts) - use full range (-inf, inf)
                        ranges[nvar_id] = NumericRange(
                            -numeric_limits<ap_float>::infinity(),
                            numeric_limits<ap_float>::infinity(),
                            false, false
                        );
                    }
                }
            }
            
            // Use cached assignment axiom data for performance.
            bool changed = true;
            int iterations = 0;
            const int MAX_ITERATIONS = 100;
            
            while (changed && iterations++ < MAX_ITERATIONS) {
                changed = false;
                for (const CachedAssignmentAxiom &axiom : cached_assignment_axioms) {
                    // Get left operand range
                    bool left_known = false;
                    NumericRange left_range;
                    
                    if (axiom.left_type == numType::constant) {
                        left_range = NumericRange(axiom.left_const_val, axiom.left_const_val, true, true);
                        left_known = true;
                    } else {
                        auto left_it = ranges.find(axiom.left_id);
                        if (left_it != ranges.end()) {
                            left_range = left_it->second;
                            left_known = true;
                        }
                    }
                    
                    // Get right operand range
                    bool right_known = false;
                    NumericRange right_range;
                    
                    if (axiom.right_type == numType::constant) {
                        right_range = NumericRange(axiom.right_const_val, axiom.right_const_val, true, true);
                        right_known = true;
                    } else {
                        auto right_it = ranges.find(axiom.right_id);
                        if (right_it != ranges.end()) {
                            right_range = right_it->second;
                            right_known = true;
                        }
                    }
                    
                    // Compute derived range if both operands known
                    if (left_known && right_known) {
                        NumericRange res = NumericDomainMapping::apply_range_operation(
                            left_range, right_range, axiom.op_type);
                        
                        auto it = ranges.find(axiom.derived_id);
                        if (it == ranges.end() || 
                            it->second.lower != res.lower || it->second.upper != res.upper ||
                            it->second.lower_inclusive != res.lower_inclusive || it->second.upper_inclusive != res.upper_inclusive) {
                            ranges[axiom.derived_id] = res;
                            changed = true;
                        }
                    }
                }
            }
            
            // Step 4: Evaluate comparison preconditions optimistically
            // Check concrete operator preconditions directly
            bool satisfies_preconditions = true;
            
            for (FactProxy concrete_pre : op.get_preconditions()) {
                int var_id = concrete_pre.get_variable().get_id();
                int concrete_val = concrete_pre.get_value();
                
                // Check if this is a comparison axiom variable using O(1) map lookup
                auto axiom_it = comparison_axiom_by_var_id.find(var_id);
                if (axiom_it == comparison_axiom_by_var_id.end()) {
                    continue; // Not a comparison axiom precondition
                }

                // Use cached axiom data for performance.
                const CachedComparisonAxiom &matching_axiom = cached_comparison_axioms[axiom_it->second];
                
                // Get left operand range
                NumericRange left_range;
                if (matching_axiom.left_type == numType::constant) {
                    left_range = NumericRange(matching_axiom.left_const_val, matching_axiom.left_const_val, true, true);
                } else {
                    auto left_it = ranges.find(matching_axiom.left_id);
                    if (left_it != ranges.end()) {
                        left_range = left_it->second;
                    }
                }
                
                // Get right operand range
                NumericRange right_range;
                if (matching_axiom.right_type == numType::constant) {
                    right_range = NumericRange(matching_axiom.right_const_val, matching_axiom.right_const_val, true, true);
                } else {
                    auto right_it = ranges.find(matching_axiom.right_id);
                    if (right_it != ranges.end()) {
                        right_range = right_it->second;
                    }
                }
                
                // Evaluate comparison: 0=true, 1=false, 2=unknown
                int eval = NumericDomainMapping::evaluate_comparison(
                    matching_axiom.op_type, left_range, right_range);
                
                // Determine required evaluation result from concrete precondition
                int required_eval = (concrete_val == matching_axiom.true_val) ? 0 : 1;
                
                // Optimistic filtering: only reject if definitive contradiction
                if (eval != 2 && eval != required_eval) {
                    satisfies_preconditions = false;
                    break;
                }
            }
            
            if (satisfies_preconditions) {
                TransitionInfo trans;
                trans.source_partition_facts = source_facts;
                trans.target_partition_facts = target_facts;

                // Compute cascades
                if (!changed_vars.empty()) {
                    // 1. Direct cascades (Comparison Axioms)
                    // Target state (NEW)
                    vector<Fact> target_cascades = 
                        compute_affected_comparison_axioms(changed_vars, old_parts, new_parts);
                    
                    // Source state (OLD) - use old_parts as new_parts to evaluate in old state
                    vector<Fact> source_cascades = 
                        compute_affected_comparison_axioms(changed_vars, old_parts, old_parts);
                    
                    // Add to transition facts
                    trans.target_partition_facts.insert(trans.target_partition_facts.end(),
                                                      target_cascades.begin(), target_cascades.end());
                    trans.source_partition_facts.insert(trans.source_partition_facts.end(),
                                                      source_cascades.begin(), source_cascades.end());

                    // 2. Indirect cascades (Assignment Axioms -> Comparison Axioms)
                    // Target state
                    vector<Fact> target_assignment_cascades = 
                        compute_assignment_axiom_cascades(changed_vars, old_parts, new_parts);
                    
                    // Source state
                    vector<Fact> source_assignment_cascades = 
                        compute_assignment_axiom_cascades(changed_vars, old_parts, old_parts);

                    trans.target_partition_facts.insert(trans.target_partition_facts.end(),
                                                      target_assignment_cascades.begin(), target_assignment_cascades.end());
                    trans.source_partition_facts.insert(trans.source_partition_facts.end(),
                                                      source_assignment_cascades.begin(), source_assignment_cascades.end());
                }

                transitions.push_back(trans);
            }
            
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
                        var_idx, source_partition, *ass_eff_for_var, source_facts);
                    
                } else {
                    if (logger) {
                        logger->log(Verbosity::INFO, "CRITICAL ERROR: Ass eff var not present");
                    }
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
    // Use cached axiom data for performance.
    for (const CachedAssignmentAxiom &axiom : cached_assignment_axioms) {
        int derived_var_id = axiom.derived_id;
        int left_var_id = axiom.left_id;
        int right_var_id = axiom.right_id;
        cal_operator op = axiom.op_type;
        
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
    
    // Use reverse dependency map to find only affected axioms.
    // Collect unique axiom indices that might be affected.
    vector<bool> axiom_seen(cached_comparison_axioms.size(), false);
    vector<size_t> affected_axiom_indices;
    
    for (int changed_var_id : changed_numeric_vars) {
        if (changed_var_id >= 0 && changed_var_id < static_cast<int>(var_to_comparison_axiom_indices.size())) {
            for (size_t axiom_idx : var_to_comparison_axiom_indices[changed_var_id]) {
                if (!axiom_seen[axiom_idx]) {
                    axiom_seen[axiom_idx] = true;
                    affected_axiom_indices.push_back(axiom_idx);
                }
            }
        }
    }
    
    // Process only the affected axioms.
    for (size_t axiom_idx : affected_axiom_indices) {
        const CachedComparisonAxiom &axiom = cached_comparison_axioms[axiom_idx];
        int left_var_id = axiom.left_id;
        int right_var_id = axiom.right_id;
        
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
        
        int prop_var_id = axiom.prop_var_id;
        
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
            
            // Evaluate the comparison exactly - need to use proxy for this function
            ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
            ComparisonAxiomProxy ax_proxy = comparison_axioms[comparison_axiom_by_var_id.at(prop_var_id)];
            int eval_result = evaluate_comparison_exactly(ax_proxy, eval_left_partition, eval_right_partition);
            
            if (eval_result == 0) {
                // Definitely false
                affected_facts.emplace_back(prop_var_id, axiom.false_val);
            } else if (eval_result == 1) {
                // Definitely true
                affected_facts.emplace_back(prop_var_id, axiom.true_val);
            }
            // If unknown (eval_result == 2): don't add any fact - the comparison
            // could go either way and we can't constrain the operator preconditions
        }
        // If partitions didn't change: don't add any fact - the comparison axiom
        // value doesn't change due to this numeric variable change
    }
    
    return affected_facts;
}

vector<int> DomainAbstractionNumericHelper::compute_reachable_partitions(
    int numeric_var_id,
    int source_partition,
    const NumAssProxy &ass_effect,
    const vector<Fact> &source_facts) const {
    
    // Get the numeric domain mapping for this variable
    const NumericDomainMapping &mapping = *numeric_domain_mapping[numeric_var_id];
    
    // Get the effect operator and operand value
    f_operator op_type = ass_effect.get_assigment_operator_type();
    NumericVariableProxy assigned_var = ass_effect.get_assigned_variable();
    ap_float operand_value = assigned_var.get_initial_state_value();

    if (assigned_var.get_var_type() == numType::constant) {
        // Existing behavior for constant RHS.
        return mapping.compute_reachable_partitions(source_partition, op_type, operand_value);
    }

    // New behavior: RHS is a numeric variable (e.g., x += y).
    // We keep all interval pieces produced by partition operations and only then
    // map them to target partitions via overlap.
    int rhs_var_id = assigned_var.get_id();
    if (rhs_var_id < 0 || rhs_var_id >= static_cast<int>(numeric_domain_mapping.size())) {
        // Conservative fallback.
        vector<int> all_targets;
        for (const auto &range : mapping.get_ranges()) {
            all_targets.push_back(range.partition_index);
        }
        return all_targets;
    }

    vector<int> rhs_partitions;
    rhs_partitions.reserve(numeric_domain_mapping[rhs_var_id]->get_num_partitions());

    // If RHS is the same variable, we know its source partition exactly.
    if (rhs_var_id == numeric_var_id) {
        rhs_partitions.push_back(source_partition);
    } else {
        // Try to read RHS partition from already-fixed source facts.
        int rhs_abstract_var = static_cast<int>(domain_sizes.size()) + rhs_var_id;
        int fixed_rhs_partition = -1;
        for (const Fact &f : source_facts) {
            if (f.var == rhs_abstract_var) {
                fixed_rhs_partition = f.value;
                break;
            }
        }

        if (fixed_rhs_partition != -1) {
            rhs_partitions.push_back(fixed_rhs_partition);
        } else {
            // Unknown at this recursion point: use all RHS partitions conservatively.
            int num_rhs_parts = numeric_domain_mapping[rhs_var_id]->get_num_partitions();
            for (int p = 0; p < num_rhs_parts; ++p) {
                rhs_partitions.push_back(p);
            }
        }
    }

    const NumericDomainMapping &rhs_mapping = *numeric_domain_mapping[rhs_var_id];
    Partition source_x = mapping.get_partition(source_partition);

    unordered_set<int> reachable_partitions_set;

    for (int rhs_partition : rhs_partitions) {
        Partition rhs_part = rhs_mapping.get_partition(rhs_partition);
        if (rhs_part.is_empty()) {
            continue;
        }

        Partition result_part;
        switch (op_type) {
            case assign:
                result_part = rhs_part;
                break;
            case increase:
                result_part = Partition::apply_binary_operation(source_x, rhs_part, cal_operator::sum);
                break;
            case decrease:
                result_part = Partition::apply_binary_operation(source_x, rhs_part, cal_operator::diff);
                break;
            case scale_up:
                result_part = Partition::apply_binary_operation(source_x, rhs_part, cal_operator::mult);
                break;
            case scale_down:
                result_part = Partition::apply_binary_operation(source_x, rhs_part, cal_operator::divi);
                break;
            default:
                break;
        }

        // Map all resulting intervals to target x partitions by overlap.
        const vector<NumericRange> &target_ranges = mapping.get_ranges();
        for (const NumericRange &res_range : result_part.get_ranges()) {
            for (const NumericRange &tgt_range : target_ranges) {
                if (tgt_range.overlaps_with(
                        res_range.lower,
                        res_range.upper,
                        res_range.lower_inclusive,
                        res_range.upper_inclusive)) {
                    reachable_partitions_set.insert(tgt_range.partition_index);
                }
            }
        }
    }

    vector<int> reachable_targets(
        reachable_partitions_set.begin(), reachable_partitions_set.end());
    sort(reachable_targets.begin(), reachable_targets.end());

    if (reachable_targets.empty()) {
        // Conservative fallback.
        for (const auto &range : mapping.get_ranges()) {
            reachable_targets.push_back(range.partition_index);
        }
    }

    return reachable_targets;
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
    assert(left_var_id < static_cast<int>(numeric_domain_mapping.size()) &&
        right_var_id < static_cast<int>(numeric_domain_mapping.size()));
    
    const NumericDomainMapping &left_mapping = *numeric_domain_mapping[left_var_id];
    const NumericDomainMapping &right_mapping = *numeric_domain_mapping[right_var_id];
    
    // Use evaluate_comparison_with method from NumericDomainMapping
    return left_mapping.evaluate_comparison_with(right_mapping, left_partition, right_partition, comp_op);
}

shared_ptr<ArithmeticExpression> 
DomainAbstractionNumericHelper::parse_arithmetic_expression(NumericVariableProxy /*num_var*/) {
    // Arithmetic expression parsing is handled through axiom evaluation
    return nullptr;
}

} // namespace domain_abstractions
