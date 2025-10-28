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
                    // Try all possible source and target partitions for this affected variable
                    // and compute the hash contribution as (target - source) * multiplier.
                    // This yields correct regression offsets: predecessor = state + (target - source)*multiplier
                    int num_partitions = numeric_domain_sizes[var_idx];
                    // Numeric hash multipliers are stored after all propositional
                    // variable multipliers in the hash_multipliers vector. Using
                    // pre_pairs.size() here is incorrect (that is the number of
                    // precondition facts) and can lead to picking the wrong
                    // multiplier index. Compute the correct offset into
                    // hash_multipliers for numeric variables.
                    int numeric_offset = static_cast<int>(hash_multipliers.size()) - static_cast<int>(numeric_domain_mapping.size());
                    int hash_multiplier = hash_multipliers[numeric_offset + var_idx];

                    for (int source_partition = 0; source_partition < num_partitions; ++source_partition) {
                        for (int target_partition = 0; target_partition < num_partitions; ++target_partition) {
                            int effect_contribution = (target_partition - source_partition) * hash_multiplier;
                            enumerate_effects(var_idx + 1, current_effect + effect_contribution);
                        }
                    }
                } else {
                // Not affected: no contribution to hash effect from this variable
                enumerate_effects(var_idx + 1, current_effect);
            }
        };
        
        enumerate_effects(0, base_hash_effect);
    }

    // Debug: if a particular concrete operator shows suspicious hash effects,
    // print details to help trace the source of negative/large effects.
    // (Concrete operator IDs are from the input task; adjust as needed.)
    if (concrete_op_id == 145) {
        cout << "DEBUG FACTORY: Operator concrete_id=145 constructed" << endl;
        cout << "  prev_pairs:";
        for (const Fact &f : prev_pairs) cout << " (v" << f.var << "=" << f.value << ")";
        cout << endl;
        cout << "  pre_pairs:";
        for (const Fact &f : pre_pairs) cout << " (v" << f.var << "=" << f.value << ")";
        cout << endl;
        cout << "  eff_pairs:";
        for (const Fact &f : eff_pairs) cout << " (v" << f.var << "=" << f.value << ")";
        cout << endl;
        cout << "  ass_effects size=" << ass_effects.size() << endl;
        cout << "  base_hash_effect=" << base_hash_effect << endl;
        cout << "  hash_effects:";
        for (int he : hash_effects) cout << " " << he;
        cout << endl;
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
    int concrete_op_id,
    const vector<int> &changed_numeric_vars,
    const vector<int> &source_partitions,
    const vector<int> &target_partitions)
    : concrete_op_id(concrete_op_id),
      cost(cost),
      hash_effects(pre_computed_hash_effects),
      regression_numeric_preconditions(ass_effects),
      changed_numeric_vars(changed_numeric_vars),
      source_partitions(source_partitions),
      target_partitions(target_partitions) {
    
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

    // Debug print for operators constructed via pre-computed hash effects
    if (concrete_op_id == 145) {
        cout << "DEBUG FACTORY: (precomputed) Operator concrete_id=145 constructed" << endl;
        cout << "  prev_pairs:";
        for (const Fact &f : prev_pairs) cout << " (v" << f.var << "=" << f.value << ")";
        cout << endl;
        cout << "  eff_pairs:";
        for (const Fact &f : eff_pairs) cout << " (v" << f.var << "=" << f.value << ")";
        cout << endl;
        cout << "  ass_effects size=" << ass_effects.size() << endl;
        cout << "  hash_effects:";
        for (int he : hash_effects) cout << " " << he;
        cout << endl;
        cout << "  changed_numeric_vars: " << changed_numeric_vars.size() << endl;
    }
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
    
    // Build combined domain sizes for MatchTree (propositional + numeric)
    // MatchTree needs to know domain sizes for ALL variables, including numeric partitions
    vector<int> combined_domain_sizes = domain_sizes;
    combined_domain_sizes.insert(combined_domain_sizes.end(),
                                 numeric_domain_sizes.begin(),
                                 numeric_domain_sizes.end());
    
    MatchTree match_tree = build_match_tree(combined_domain_sizes, operators);
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

vector<int> DomainAbstractionFactory::enumerate_cascade_predecessors(
    int base_predecessor_index,
    const vector<int> &changed_numeric_vars,
    const vector<int> &source_partitions,
    const vector<int> &target_partitions,
    const TaskProxy &task_proxy) const {
    
    vector<int> result;
    
    // DEBUG: Check comparison axiom refinement status (every time to track iterations)
    static int call_count = 0;
    call_count++;
    if (call_count == 1 || call_count % 100 == 0) {
        cout << "DEBUG FACTORY [call " << call_count << "]: Checking ALL comparison axiom variables for refinement status:" << endl;
        ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
        int total_comp_axioms = 0;
        int trivial_comp_axioms = 0;
        int refined_comp_axioms = 0;
        for (ComparisonAxiomProxy axiom : comparison_axioms) {
            int prop_var_id = axiom.get_true_fact().get_variable().get_id();
            total_comp_axioms++;
            bool is_trivial = variable_is_trivial(prop_var_id);
            if (is_trivial) {
                trivial_comp_axioms++;
            } else {
                refined_comp_axioms++;
                cout << "  Comparison axiom var" << prop_var_id << " IS REFINED, domain_mapping size=" 
                     << domain_mapping[prop_var_id].size() << endl;
            }
        }
        cout << "DEBUG FACTORY: Comparison axiom summary: total=" << total_comp_axioms 
             << ", trivial=" << trivial_comp_axioms 
             << ", refined=" << refined_comp_axioms << endl;
    }
    
    // If no numeric variables changed, just return the base predecessor
    if (changed_numeric_vars.empty()) {
        result.push_back(base_predecessor_index);
        return result;
    }
    
    // Step 0: Compute transitive closure of affected numeric variables through assignment axioms
    // Start with the directly changed variables, then add derived variables that depend on them
    unordered_set<int> affected_numeric_vars(changed_numeric_vars.begin(), changed_numeric_vars.end());
    unordered_map<int, pair<ap_float, ap_float>> computed_ranges;  // var_id -> (lower, upper)
    //cout << "DEBUG FACTORY: Directly changed numeric vars:";
    //for (int var_id : changed_numeric_vars) {
    //    cout << " " << var_id;
    //}
    //cout << endl;
    // Store the ranges of directly changed variables
    for (size_t i = 0; i < changed_numeric_vars.size(); ++i) {
        int var_id = changed_numeric_vars[i];
        int partition = source_partitions[i];
        
        //cout << "DEBUG FACTORY:   Looking up range for var" << var_id 
        //     << " partition=" << partition << endl;
        
        if (var_id < static_cast<int>(numeric_domain_mapping.size())) {
            const NumericDomainMapping &mapping = numeric_domain_mapping[var_id];
            const vector<NumericRange> &ranges = mapping.get_ranges();
            
            //cout << "DEBUG FACTORY:     Found " << ranges.size() << " ranges" << endl;
            
            bool found = false;
            for (const NumericRange &range : ranges) {
                if (range.partition_index == partition) {
                    computed_ranges[var_id] = {range.lower, range.upper};
                    //cout << "DEBUG FACTORY:     -> Stored range [" << range.lower 
                    //     << ", " << range.upper << "]" << endl;
                    found = true;
                    break;
                }
            }
            if (!found) {
                //cout << "DEBUG FACTORY:     -> WARNING: Partition " << partition 
                //     << " not found in ranges!" << endl;
            }
        }
    }
    
    // Iteratively compute derived variable ranges from assignment axioms
    // Keep going until no new derived variables are added
    bool added_new = true;
    while (added_new) {
        added_new = false;
        
        AssignmentAxiomsProxy assignment_axioms = task_proxy.get_assignment_axioms();
        for (AssignmentAxiomProxy axiom : assignment_axioms) {
            //cout << "DEBUG FACTORY: Considering assignment axiom for derived var "
            //     << axiom.get_assignment_variable().get_id() << endl;
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

            //cout << "DEBUG FACTORY:   left_var=" << left_var_id 
            //     << " (type=" << (int)left_var.get_var_type() << ")"
            //     << ", right_var=" << right_var_id 
            //     << " (type=" << (int)right_var.get_var_type() << ")" << endl;

            // A variable is "known" if:
            // 1. It's in affected_numeric_vars AND computed_ranges (changed by operator or derived)
            // 2. It's a constant (always known)
            bool left_known = (affected_numeric_vars.count(left_var_id) > 0 && 
                              computed_ranges.count(left_var_id) > 0) ||
                             (left_var.get_var_type() == numType::constant);
            bool right_known = (affected_numeric_vars.count(right_var_id) > 0 && 
                               computed_ranges.count(right_var_id) > 0) ||
                              (right_var.get_var_type() == numType::constant);
            
            //cout << "DEBUG FACTORY:   left_known=" << left_known 
            //     << ", right_known=" << right_known << endl;
            //
            // We need BOTH operands to be known to compute the derived variable
            if (!left_known || !right_known) {
                continue;
            }
            
            // Get ranges for left and right variables
            ap_float left_lower, left_upper, right_lower, right_upper;
            
            if (left_var.get_var_type() == numType::constant) {
                // Constant: range is a single value
                ap_float const_val = left_var.get_initial_state_value();
                left_lower = const_val;
                left_upper = const_val;
            } else {
                // Regular or derived variable: use computed range
                left_lower = computed_ranges[left_var_id].first;
                left_upper = computed_ranges[left_var_id].second;
            }
            
            if (right_var.get_var_type() == numType::constant) {
                // Constant: range is a single value
                ap_float const_val = right_var.get_initial_state_value();
                right_lower = const_val;
                right_upper = const_val;
            } else {
                // Regular or derived variable: use computed range
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
                    // Multiplication is more complex - need to consider all combinations
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
                    // Division is tricky - skip for now (would need to handle division by zero)
                    continue;
                default:
                    // Unknown operator - skip
                    continue;
            }
            
            computed_ranges[derived_var_id] = {derived_lower, derived_upper};
            affected_numeric_vars.insert(derived_var_id);
            added_new = true;
        }
    }
    
    // Step 1: Identify affected comparison axioms
    // A comparison axiom is affected if it depends on any affected numeric variable (including derived)
    struct AffectedComparison {
        int prop_var_id;           // Propositional variable ID for the comparison result
        int true_value;            // Value when comparison is true
        int false_value;           // Value when comparison is false
        int left_var_id;           // Left operand numeric variable ID
        int right_var_id;          // Right operand numeric variable ID
        comp_operator op;          // Comparison operator
        
        // Evaluation result for this specific transition
        enum EvalResult { DEFINITELY_TRUE, DEFINITELY_FALSE, UNKNOWN };
        EvalResult eval_result;
    };

    
    vector<AffectedComparison> affected_comparisons;
    
    // Scan all comparison axioms
    ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
    //cout << "DEBUG FACTORY: Scanning " << comparison_axioms.size() << " comparison axioms" << endl;
    for (ComparisonAxiomProxy axiom : comparison_axioms) {
        int left_var_id = axiom.get_left_variable().get_id();
        int right_var_id = axiom.get_right_variable().get_id();
        int prop_var_id = axiom.get_true_fact().get_variable().get_id();
        
        //cout << "DEBUG FACTORY:   Comparison axiom prop_var=" << prop_var_id 
        //     << " checks left=" << left_var_id << " vs right=" << right_var_id << endl;
        
        // Check if this comparison depends on any affected variable (including derived)
        bool depends_on_affected = (affected_numeric_vars.count(left_var_id) > 0 || 
                                    affected_numeric_vars.count(right_var_id) > 0);
        
        //cout << "DEBUG FACTORY:     depends_on_affected=" << depends_on_affected << endl;
        
        if (!depends_on_affected) {
            continue;  // This comparison is not affected by the operator
        }
        
        //cout << "DEBUG FACTORY:     -> This comparison IS affected!" << endl;
        
        // This comparison is affected - evaluate it
        AffectedComparison affected;
        affected.prop_var_id = axiom.get_true_fact().get_variable().get_id();
        affected.true_value = axiom.get_true_fact().get_value();
        affected.false_value = axiom.get_false_fact().get_value();
        affected.left_var_id = left_var_id;
        affected.right_var_id = right_var_id;
        affected.op = axiom.get_comparison_operator_type();
        
        // Get ranges for left and right variables
        // If a variable is in computed_ranges (regular or derived), use that
        // Otherwise, look it up in the domain mapping
        ap_float left_lower = -numeric_limits<ap_float>::infinity();
        ap_float left_upper = numeric_limits<ap_float>::infinity();
        ap_float right_lower = -numeric_limits<ap_float>::infinity();
        ap_float right_upper = numeric_limits<ap_float>::infinity();
        
        bool found_left = false, found_right = false;
        
        // Try to get left variable range from computed_ranges or domain mapping
        if (computed_ranges.count(left_var_id) > 0) {
            left_lower = computed_ranges[left_var_id].first;
            left_upper = computed_ranges[left_var_id].second;
            found_left = true;
        }
        
        // Try to get right variable range from computed_ranges or domain mapping
        if (computed_ranges.count(right_var_id) > 0) {
            right_lower = computed_ranges[right_var_id].first;
            right_upper = computed_ranges[right_var_id].second;
            found_right = true;
        }
        
        // If not found yet and variables are not derived, they might be constants or in the base state
        // For constants (numeric variables with fixed values), we need to get them from the domain mapping
        if (!found_left && left_var_id < static_cast<int>(numeric_domain_mapping.size())) {
            const NumericDomainMapping &left_mapping = numeric_domain_mapping[left_var_id];
            const vector<NumericRange> &left_ranges = left_mapping.get_ranges();
            // For variables not in changed_numeric_vars, we don't know the partition - assume the whole range
            if (!left_ranges.empty()) {
                // Take the union of all ranges (min of all lowers, max of all uppers)
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
            const NumericDomainMapping &right_mapping = numeric_domain_mapping[right_var_id];
            const vector<NumericRange> &right_ranges = right_mapping.get_ranges();
            // For variables not in changed_numeric_vars, we don't know the partition - assume the whole range
            if (!right_ranges.empty()) {
                // Take the union of all ranges (min of all lowers, max of all uppers)
                right_lower = numeric_limits<ap_float>::infinity();
                right_upper = -numeric_limits<ap_float>::infinity();
                for (const NumericRange &range : right_ranges) {
                    right_lower = min(right_lower, range.lower);
                    right_upper = max(right_upper, range.upper);
                }
                found_right = true;
            }
        }
        
        if (!found_left || !found_right) {
            // If we can't determine ranges, mark as UNKNOWN (optimistically assume both true/false possible)
            affected.eval_result = AffectedComparison::UNKNOWN;
            affected_comparisons.push_back(affected);
            continue;
        }
        
        // Now evaluate: can the comparison be true? false? or both?
        // For each operator, check if there exist values in the ranges that satisfy/don't satisfy it
        bool can_be_true = false;
        bool can_be_false = false;
        
        switch (affected.op) {
            case comp_operator::lt:  // left < right
                // Can be true if: exists left_val < right_val
                // This is possible if left_lower < right_upper
                can_be_true = (left_lower < right_upper);
                // Can be false if: exists left_val >= right_val
                // This is possible if left_upper >= right_lower
                can_be_false = (left_upper >= right_lower);
                break;
            case comp_operator::le:  // left <= right
                can_be_true = (left_lower <= right_upper);
                can_be_false = (left_upper > right_lower);
                break;
            case comp_operator::eq:  // left == right
                // Can be true if ranges overlap
                can_be_true = (left_lower < right_upper && right_lower < left_upper);
                // Can be false if values can differ
                can_be_false = (left_lower < right_lower || left_upper > right_upper ||
                               right_lower < left_lower || right_upper > left_upper);
                break;
            case comp_operator::ge:  // left >= right
                can_be_true = (left_upper >= right_lower);
                can_be_false = (left_lower < right_upper);
                break;
            case comp_operator::gt:  // left > right
                can_be_true = (left_upper > right_lower);
                can_be_false = (left_lower <= right_upper);
                break;
            default:
                can_be_true = true;
                can_be_false = true;
        }
        
        if (can_be_true && !can_be_false) {
            affected.eval_result = AffectedComparison::DEFINITELY_TRUE;
        } else if (!can_be_true && can_be_false) {
            affected.eval_result = AffectedComparison::DEFINITELY_FALSE;
        } else {
            affected.eval_result = AffectedComparison::UNKNOWN;
        }
        
        affected_comparisons.push_back(affected);
    }

    cout << "DEBUG FACTORY: Total affected comparisons: " 
         << affected_comparisons.size() << endl;
    
    // Step 2: Reset all affected comparison axioms to UNKNOWN in the base state
    // This ensures we compute deltas from a consistent baseline
    int reset_to_unknown_adjustment = 0;
    int trivial_count = 0;
    int non_trivial_count = 0;
    for (const AffectedComparison &comp : affected_comparisons) {
        int prop_var_id = comp.prop_var_id;
        if (prop_var_id >= static_cast<int>(hash_multipliers.size())) {
            continue;  // Variable not in hash function - skip
        }
        
        // Skip trivial variables (those with empty domain_mapping)
        if (variable_is_trivial(prop_var_id)) {
            trivial_count++;
            continue;
        }
        non_trivial_count++;
        
        int multiplier = hash_multipliers[prop_var_id];
        
        // Extract current value of this comparison axiom from base_predecessor_index
        // Comparison axioms have domain size 3: true (0), false (1), unknown (2)
        int current_value = (base_predecessor_index / multiplier) % 3;
        
        // UNKNOWN is value 2 (from output file: index 0 = true, 1 = false, 2 = <none of those>)
        // We need to reset from current_value to UNKNOWN (2)
        // Delta = (2 - current_value) * multiplier
        int unknown_value = domain_mapping[prop_var_id][2];
        int delta_to_unknown = (unknown_value - current_value) * multiplier;
        reset_to_unknown_adjustment += delta_to_unknown;
    }
    
    cout << "DEBUG FACTORY: Comparison axiom refinement status: " 
         << "trivial=" << trivial_count 
         << ", non_trivial=" << non_trivial_count 
         << ", total=" << affected_comparisons.size() << endl;
    
    // Apply the reset: now all affected comparisons are UNKNOWN
    int state_with_unknowns = base_predecessor_index + reset_to_unknown_adjustment;
    
    // Step 3: Enumerate all combinations of comparison axiom truth values
    // For DEFINITELY_TRUE/FALSE, we use the fixed value
    // For UNKNOWN, we enumerate both true and false (optimistic branching)
    // All deltas are now computed from UNKNOWN (value 0) to the target value
    
    function<void(size_t, int)> enumerate_combinations =
        [&](size_t comparison_idx, int current_hash_adjustment) {
        
        if (comparison_idx == affected_comparisons.size()) {
            // Base case: we've fixed all comparison axiom values
            result.push_back(state_with_unknowns + current_hash_adjustment);
            return;
        }
        
        const AffectedComparison &comp = affected_comparisons[comparison_idx];
        int prop_var_id = comp.prop_var_id;
        
        // Get the hash multiplier for this propositional variable
        if (prop_var_id >= static_cast<int>(hash_multipliers.size())) {
            // Variable not in hash function - skip
            enumerate_combinations(comparison_idx + 1, current_hash_adjustment);
            return;
        }
        
        // Skip trivial variables (those with empty domain_mapping)
        if (variable_is_trivial(prop_var_id)) {
            enumerate_combinations(comparison_idx + 1, current_hash_adjustment);
            return;
        }
        
        int multiplier = hash_multipliers[prop_var_id];
        
        // All comparisons are now at UNKNOWN (value 2) in state_with_unknowns
        // Compute delta from UNKNOWN (2) to target value
        int unknown_value = domain_mapping[prop_var_id][2];
        
        if (comp.eval_result == AffectedComparison::DEFINITELY_TRUE) {
            // Comparison must be true in the predecessor
            // Delta from UNKNOWN to true_value
            int delta_from_unknown = (domain_mapping[comp.prop_var_id][comp.true_value] - unknown_value) * multiplier;
            enumerate_combinations(comparison_idx + 1, 
                                 current_hash_adjustment + delta_from_unknown);
        } else if (comp.eval_result == AffectedComparison::DEFINITELY_FALSE) {
            // Comparison must be false in the predecessor
            // Delta from UNKNOWN to false_value
            
            int delta_from_unknown = (domain_mapping[comp.prop_var_id][comp.false_value] - unknown_value) * multiplier;
            enumerate_combinations(comparison_idx + 1,
                                 current_hash_adjustment + delta_from_unknown);
            exit(1);
        } else {
            // UNKNOWN - enumerate true possibilitie (optimistic branching)
            // Try TRUE: delta from UNKNOWN to true_value
            int true_delta = (domain_mapping[comp.prop_var_id][comp.true_value] - unknown_value) * multiplier;
            enumerate_combinations(comparison_idx + 1,
                                 current_hash_adjustment + true_delta);
            exit(1);
        }
    };
    
    enumerate_combinations(0, 0);
    
    // TODO: Handle assignment axiom cascades (derived numeric variables)
    // This would require computing derived variable ranges and recursively
    // checking comparison axioms that depend on them
    
    return result;
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
    
    // First, identify goal axiom effect variables (these should be filtered out)
    // Goal axioms have preconditions and exactly one effect
    // The effect is NOT a real goal, only the preconditions are
    unordered_set<int> goal_axiom_effect_vars;
    for (OperatorProxy axiom : task_proxy.get_axioms()) {
        if (!axiom.get_preconditions().empty() && axiom.get_effects().size() == 1) {
            // This is a goal axiom - its effect variable should be filtered
            int effect_var_id = axiom.get_effects()[0].get_fact().get_variable().get_id();
            goal_axiom_effect_vars.insert(effect_var_id);
            cout << "DEBUG GOALS: Identified goal axiom effect variable: var" << effect_var_id << endl;
        }
    }
    
    // Now collect non-derived goals directly, but SKIP goal axiom effects
    for (FactProxy goal : task_proxy.get_goals()) {
        int var_id = goal.get_variable().get_id();
        cout << "DEBUG GOALS:   Goal var" << var_id << "=" << goal.get_value() 
             << " (derived=" << (is_derived_variable(task_proxy, var_id) ? "yes" : "no")
             << ", trivial=" << (variable_is_trivial(var_id) ? "yes" : "no")
             << ", is_goal_axiom_effect=" << (goal_axiom_effect_vars.count(var_id) > 0 ? "yes" : "no") << ")" << endl;
        
        // Skip goal axiom effect variables - they are not real goals
        if (goal_axiom_effect_vars.count(var_id) > 0) {
            cout << "DEBUG GOALS:     Skipping goal axiom effect variable" << endl;
            continue;
        }
        
        // Add goal if it has a domain mapping
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
            for (int base_hash_effect : hash_effects_vec) {
                // Enumerate all possible predecessors considering comparison axiom cascades
                vector<int> possible_predecessors = enumerate_cascade_predecessors(
                    state_index + base_hash_effect,
                    op.get_changed_numeric_vars(),
                    op.get_source_partitions(),
                    op.get_target_partitions(),
                    task_proxy);
                
                if (is_first_goal_expansion && operators_checked <= 5) {
                    cout << "    Base hash_effect=" << base_hash_effect 
                         << ", cascade predecessors: " << possible_predecessors.size() << endl;
                }
                
                for (int predecessor : possible_predecessors) {
                    // Skip predecessors that are out of bounds. This can legitimately
                    // happen because we enumerate many numeric partition transitions
                    // conservatively; some of those transitions do not correspond to
                    // valid abstract predecessors for the current propositional part.
                    if (predecessor < 0 || predecessor >= num_states) {
                        if (dijkstra_iterations == 1) {
                            out_of_bounds_predecessors++;
                        }
                        if (is_first_goal_expansion && operators_checked <= 5) {
                            out_of_bounds_this_op++;
                            cout << "DEBUG DIJKSTRA:   Skipping out-of-bounds predecessor: "
                                 << "state_index=" << state_index
                                 << " base_hash_effect=" << base_hash_effect
                                 << " predecessor=" << predecessor
                                 << " op_id=" << op_id
                                 << " concrete_id=" << op.get_concrete_op_id()
                                 << endl;
                            // Print preconditions of this operator
                            cout << "DEBUG DIJKSTRA:   Operator preconditions: ";
                            const vector<Fact> &preconds = op.get_regression_preconditions();
                            for (const Fact &pc : preconds) {
                                cout << "var" << pc.var << "=" << pc.value << " ";
                            }
                            cout << endl;
                        }
                        // Continue without asserting to allow the search to proceed
                        // while we gather diagnostics. Invalid predecessors are
                        // expected in conservative enumerations and should be skipped.
                        continue;
                    }
                    
                    valid_predecessors_this_state++;
                    predecessors_this_op++;
                    
                    if (alternative_cost < distances[predecessor]) {
                        total_expansions++;
                        
                        if (is_first_goal_expansion && operators_checked <= 5 && predecessors_this_op <= 3) {
                            string pred_decoded = decode_abstract_state(predecessor, domain_sizes,
                                                                       numeric_domain_mapping, hash_multipliers);
                            cout << "    Predecessor " << predecessors_this_op << ": " << pred_decoded 
                                 << " (base_hash=" << base_hash_effect << ", cascaded_index=" << predecessor << ")" << endl;
                        }
                        
                        if (total_expansions <= 20) {
                            cout << "DEBUG DIJKSTRA:   Updated state " << predecessor 
                                 << " from distance " << distances[predecessor] 
                                 << " to " << alternative_cost 
                                 << " (base_hash_effect=" << base_hash_effect << ")" << endl;
                        }
                        distances[predecessor] = alternative_cost;
                        pq.push(alternative_cost, predecessor);
                        if (compute_plan) {
                            generating_op_ids[predecessor] = op_id;
                        }
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

    //print distances
    for (int i = 0; i < distances.size(); ++i) {
        bool is_goal = is_goal_state(i, abstract_goals, domain_sizes);
        cout << "DEBUG PLAN: Distance[" << i << "] = " << distances[i] 
                << (is_goal ? " (GOAL)" : "")
                << (i == current_state ? " (INITIAL)" : "") << endl;
    }
    
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
                
                // Check if the ENTIRE partition satisfies the goal (restrictive check)
                // This requires all values in the partition to satisfy the goal condition
                
                switch (numeric_goal.op) {
                    case comp_operator::lt:
                        // All values in partition < constant? YES if upper <= constant
                        partition_satisfies_goal = (upper <= constant);
                        break;
                    case comp_operator::le:
                        // All values in partition <= constant? YES if upper <= constant
                        partition_satisfies_goal = (upper <= constant);
                        break;
                    case comp_operator::eq:
                        // All values equal constant? YES if partition is single point [c, c)
                        partition_satisfies_goal = (lower == constant && upper == constant);
                        break;
                    case comp_operator::ge:
                        // All values in partition >= constant? YES if lower >= constant
                        partition_satisfies_goal = (lower >= constant);
                        break;
                    case comp_operator::gt:
                        // All values in partition > constant? YES if lower > constant
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
    // Check if we have any non-trivial numeric variables (with more than 1 partition)
    bool has_numeric_vars = false;
    for (const auto &num_mapping : numeric_domain_mapping) {
        if (num_mapping.get_ranges().size() > 1) {
            has_numeric_vars = true;
            break;
        }
    }
    
    // Create state registry if we have numeric variables
    unique_ptr<DomainAbstractionStateRegistry> state_registry = nullptr;
    if (has_numeric_vars) {
        state_registry = make_unique<DomainAbstractionStateRegistry>();
    }
    
    return DomainAbstraction(move(domain_mapping), move(numeric_domain_mapping),
                             move(hash_multipliers), move(distances), move(wildcard_plan),
                             move(state_registry));
}
}
