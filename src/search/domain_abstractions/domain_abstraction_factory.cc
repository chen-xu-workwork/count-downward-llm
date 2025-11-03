#include "domain_abstraction_factory.h"

#include "domain_abstraction.h"
#include "match_tree.h"
#include "numeric_helper.h"
#include "utils.h"


#include "../tasks/root_task.h"
#include "../globals.h"

#include "../utils/math.h"
#include "../utils/logging.h"
#include "../utils/rng.h"

#include "../task_tools.h"
#include "match_tree.h"
#include "../priority_queue.h"

#include <sstream>
#include <fstream>
#include <set>
#include <iomanip>


using namespace std;

namespace domain_abstractions {
// Logging controls: keep concise iteration/plan summaries by default
static const bool VERBOSE_DEBUG = false;      // gate noisy, step-by-step diagnostics

AbstractOperator::AbstractOperator(const vector<Fact> &prev_pairs,
                                   const vector<Fact> &pre_pairs,
                                   const vector<Fact> &eff_pairs,
                                   int cost,
                                   const vector<int> &hash_multipliers,
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
    hash_effect = 0;
    assert(pre_pairs.size() == eff_pairs.size());
    for (size_t i = 0; i < pre_pairs.size(); ++i) {
        int var = pre_pairs[i].var;
        assert(var == eff_pairs[i].var);
        int old_val = eff_pairs[i].value;
        int new_val = pre_pairs[i].value;
        assert(new_val != -1);
        int effect = (new_val - old_val) * hash_multipliers[var];
        if (effect != 0) {
            assert(var < hash_multipliers.size());
            cout << "DEBUG AbstractOperator: size hash mult: " << hash_multipliers[var] << endl;
            cout << "DEBUG AbstractOperator: var" << var 
                 << " eff=" << old_val << " pre=" << new_val 
                 << " delta=" << effect << endl;

        }
        hash_effect += effect;
    }
    cout << "hash_effect: " << hash_effect << endl;
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
    : task_proxy(task_proxy),
      domain_mapping(domain_mapping),
      numeric_domain_sizes(numeric_domain_sizes) {
        // Deep copy numeric_domain_mapping using clone() (can't copy unique_ptr directly)
        for (const auto &mapping : numeric_domain_mapping) {
            this->numeric_domain_mapping.push_back(mapping->clone());
        }
        
        // DEBUG: Check var24 status in domain_mapping
        static int factory_construction_count = 0;
        factory_construction_count++;
        if (factory_construction_count <= 2) {
            cout << "\n=== FACTORY CONSTRUCTION #" << factory_construction_count << " ===\n";
            if (domain_mapping.size() > 24) {
                cout << "var24 in domain_mapping:\n";
                cout << "  domain_mapping[24].size() = " << domain_mapping[24].size() << "\n";
                cout << "  domain_mapping[24].empty() = " << domain_mapping[24].empty() << "\n";
                if (!domain_mapping[24].empty()) {
                    cout << "  domain_mapping[24]: [";
                    for (size_t i = 0; i < domain_mapping[24].size(); ++i) {
                        if (i > 0) cout << ", ";
                        cout << i << "->" << domain_mapping[24][i];
                    }
                    cout << "]\n";
                }
                cout << "  domain_sizes[24] = " << domain_sizes[24] << "\n";
            }
            cout << "================================\n\n";
        }
        
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

vector<int> DomainAbstractionFactory::enumerate_states_with_evaluated_comparisons(
    int base_state_index,
    const vector<int> &changed_numeric_vars,
    const vector<int> &source_partitions,
    const vector<int> &target_partitions,
    const TaskProxy &task_proxy) const {
    
    vector<int> result;

    // Build the set of numeric variables that ACTUALLY change partitions.
    // Identity transitions (source == target) should NOT be treated as
    // affecting comparisons. Comparisons should only branch when their
    // numeric inputs truly change across the transition.
    vector<int> actually_changed_vars;
    actually_changed_vars.reserve(changed_numeric_vars.size());
    for (size_t i = 0; i < changed_numeric_vars.size(); ++i) {
        if (i < source_partitions.size() && i < target_partitions.size()) {
            if (source_partitions[i] != target_partitions[i]) {
                actually_changed_vars.push_back(changed_numeric_vars[i]);
            }
        }
    }

    // If no numeric variables change partitions, don't branch on comparison
    // axioms: their values are functions of numeric inputs. Without numeric
    // changes, the predecessor set is just the base state.
    if (actually_changed_vars.empty()) {
        // Identity-only case: no numeric partition changes, so comparison
        // axioms cannot change. Do not branch; predecessor set is the base state.
        static int no_change_log_count = 0;
        if (false && VERBOSE_DEBUG && no_change_log_count++ < 3) {
            cout << "DEBUG ENUM_COMP: no numeric partition change; returning base_state "
                 << base_state_index << endl;
        }
        result.push_back(base_state_index);
        return result;
    }
    
    // Decode the CURRENT state's numeric partitions from base_state_index. We use these
    // as precise ranges for non-changing regular variables (instead of conservative unions).
    vector<int> cur_num_partitions;
    cur_num_partitions.reserve(numeric_domain_mapping.size());
    for (size_t num_var_id = 0; num_var_id < numeric_domain_mapping.size(); ++num_var_id) {
        int numeric_offset = static_cast<int>(hash_multipliers.size()) - static_cast<int>(numeric_domain_mapping.size());
        int multiplier_idx = numeric_offset + static_cast<int>(num_var_id);
        int multiplier = hash_multipliers[multiplier_idx];
        int num_parts = numeric_domain_mapping[num_var_id]->get_num_partitions();
        int part = (base_state_index / multiplier) % num_parts;
        cur_num_partitions.push_back(part);
    }

    // Step 0: Compute transitive closure of affected numeric variables through assignment axioms
    // Start with the directly changed variables, then add derived variables that depend on them
    //
    // SPECIAL CASE: If changed_numeric_vars is empty but we have refined comparison axioms,
    // we still need to enumerate those comparison axiom values. In this case, affected_numeric_vars
    // will be empty, and we'll mark all refined comparison axioms as UNKNOWN (ambiguous).
    unordered_set<int> affected_numeric_vars(actually_changed_vars.begin(), actually_changed_vars.end());
    unordered_map<int, pair<ap_float, ap_float>> computed_ranges;  // var_id -> (lower, upper)
    //cout << "DEBUG FACTORY: Directly changed numeric vars:";
    //for (int var_id : changed_numeric_vars) {
    //    cout << " " << var_id;
    //}
    //cout << endl;
    // Store the ranges of directly changed variables (using SOURCE partitions for regression)
    for (size_t i = 0, j = 0; i < changed_numeric_vars.size(); ++i) {
        // Only seed ranges for variables that actually change.
        if (j >= actually_changed_vars.size())
            break;
        if (changed_numeric_vars[i] != actually_changed_vars[j])
            continue;
        int var_id = changed_numeric_vars[i];
        int partition = source_partitions[i];
        
        //cout << "DEBUG FACTORY:   Looking up range for var" << var_id 
        //     << " partition=" << partition << endl;
        
        if (var_id < static_cast<int>(numeric_domain_mapping.size())) {
            const NumericDomainMapping &mapping = *numeric_domain_mapping[var_id];
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
        // advance j only when we consumed a matching actually-changed var
        ++j;
    }

    // Seed ranges for all other regular numeric variables from the CURRENT state's partitions.
    // This provides precise input ranges for derived computations even if those variables didn't change.
    for (size_t var_id = 0; var_id < numeric_domain_mapping.size(); ++var_id) {
        // Don't overwrite already-seeded entries (changed vars use source partitions above).
        if (computed_ranges.count(static_cast<int>(var_id)) > 0)
            continue;
        const NumericDomainMapping &mapping = *numeric_domain_mapping[var_id];
        int part = cur_num_partitions[var_id];
        const NumericRange *rng = mapping.get_range_for_partition(part);
        if (rng) {
            computed_ranges[static_cast<int>(var_id)] = {rng->lower, rng->upper};
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

            // A variable has a known range if either we seeded it (from source/current partition)
            // or it is a constant (single-point range). We compute a derived variable only if
            // BOTH operands have known ranges AND at least one operand is affected (changed directly
            // or derived from a change). This preserves the notion of "affected" comparisons.
            bool left_has_range = (computed_ranges.count(left_var_id) > 0) ||
                                  (left_var.get_var_type() == numType::constant);
            bool right_has_range = (computed_ranges.count(right_var_id) > 0) ||
                                   (right_var.get_var_type() == numType::constant);

            bool left_dep_affected = (affected_numeric_vars.count(left_var_id) > 0);
            bool right_dep_affected = (affected_numeric_vars.count(right_var_id) > 0);
            
            //cout << "DEBUG FACTORY:   left_known=" << left_known 
            //     << ", right_known=" << right_known << endl;
            //
            // We need BOTH operands to be known to compute the derived variable
            if (!(left_has_range && right_has_range && (left_dep_affected || right_dep_affected))) {
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
    
    // No special-case enumeration for refined comparisons. We only branch when
    // numeric inputs (directly or via assignment cascades) are provided.
    
    //
    // Below is the normal path for operators WITH numeric effects
    //
    
    // cur_num_partitions already computed above; reuse it below when needed.

    for (ComparisonAxiomProxy axiom : comparison_axioms) {
        int left_var_id = axiom.get_left_variable().get_id();
        int right_var_id = axiom.get_right_variable().get_id();
        int prop_var_id = axiom.get_true_fact().get_variable().get_id();
        
        // Check if this comparison depends on any affected variable (including derived)
        bool depends_on_affected = (affected_numeric_vars.count(left_var_id) > 0 || 
                                    affected_numeric_vars.count(right_var_id) > 0);
        
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
        
        // If not found yet and variables are in the numeric mapping, use the CURRENT state's partition
        // decoded from base_state_index instead of the full union, for sharper evaluation.
        if (!found_left && left_var_id >= 0 && left_var_id < static_cast<int>(numeric_domain_mapping.size())) {
            const NumericDomainMapping &left_mapping = *numeric_domain_mapping[left_var_id];
            int part = cur_num_partitions[left_var_id];
            const NumericRange *rng = left_mapping.get_range_for_partition(part);
            if (rng) {
                left_lower = rng->lower;
                left_upper = rng->upper;
                found_left = true;
            }
        }

        if (!found_right && right_var_id >= 0 && right_var_id < static_cast<int>(numeric_domain_mapping.size())) {
            const NumericDomainMapping &right_mapping = *numeric_domain_mapping[right_var_id];
            int part = cur_num_partitions[right_var_id];
            const NumericRange *rng = right_mapping.get_range_for_partition(part);
            if (rng) {
                right_lower = rng->lower;
                right_upper = rng->upper;
                found_right = true;
            }
        }
        
        if (!found_left || !found_right) {
            // If we can't determine ranges, mark as UNKNOWN (optimistically assume both true/false possible)
            affected.eval_result = AffectedComparison::UNKNOWN;
            affected_comparisons.push_back(affected);
            
            // DEBUG: Log when adding comparison to affected list without ranges
            // No special-case debug for ENUM_ALL anymore.
            continue;
        }
        
        // Now evaluate the comparison using NumericDomainMapping's static method
        int eval_result = NumericDomainMapping::evaluate_comparison(
            affected.op, left_lower, left_upper, right_lower, right_upper);
        
        if (eval_result == 1) {
            affected.eval_result = AffectedComparison::DEFINITELY_TRUE;
        } else if (eval_result == 0) {
            affected.eval_result = AffectedComparison::DEFINITELY_FALSE;
        } else {
            affected.eval_result = AffectedComparison::UNKNOWN;
        }
        
        affected_comparisons.push_back(affected);
        
        // DEBUG: Log when adding comparison to affected list with ranges
        // No special-case debug for ENUM_ALL anymore.
    }

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

        // Determine the abstract domain size for this variable. The domain_mapping
        // stores a mapping from original values -> abstract values. The abstract
        // domain size is therefore 1 + max(mapped_value).
        int abstract_domain_size = 0;
        if (!domain_mapping[prop_var_id].empty()) {
            for (int mapped : domain_mapping[prop_var_id]) {
                if (mapped > abstract_domain_size) {
                    abstract_domain_size = mapped;
                }
            }
            abstract_domain_size += 1;
        } else {
            // Should not happen for non-trivial variables, but guard anyway.
            abstract_domain_size = 1;
        }

        // Extract the current abstract value of this comparison axiom from base_state_index
        int current_value = (base_state_index / multiplier) % abstract_domain_size;

        // UNKNOWN corresponds to original value index 2; look up its abstract value.
        // domain_mapping[prop_var_id] uses original-domain indexing, so index 2 must exist.
        int unknown_value = domain_mapping[prop_var_id][2];

        // Reset delta: move the current abstract value to the UNKNOWN abstract value
        int delta_to_unknown = (unknown_value - current_value) * multiplier;
        reset_to_unknown_adjustment += delta_to_unknown;
    }
    
    // Apply the reset: now all affected comparisons are UNKNOWN
    int state_with_unknowns = base_state_index + reset_to_unknown_adjustment;
    
    // Step 3: Enumerate all combinations of comparison axiom truth values
    // For DEFINITELY_TRUE/FALSE, we use the fixed value
    // For UNKNOWN, we enumerate both true and false (optimistic branching)
    // All deltas are now computed from UNKNOWN (value 0) to the target value
    
    static int enum_call_count = 0;
    enum_call_count++;
    bool debug_enum = false; // hard-disable noisy ENUM_COMP logs by default
    if (debug_enum && affected_comparisons.size() > 0) {
        cout << "DEBUG ENUM_COMP [call " << enum_call_count << "]: " << affected_comparisons.size() << " affected comparisons" << endl;
        for (size_t i = 0; i < affected_comparisons.size(); ++i) {
            const AffectedComparison &comp = affected_comparisons[i];
            cout << "  Comp " << i << ": fdr_" << comp.prop_var_id << ", eval_result=" << (int)comp.eval_result
                 << ", trivial=" << variable_is_trivial(comp.prop_var_id) << endl;
        }
    }
    
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
        } else {
            // UNKNOWN - enumerate true and false possibilities (optimistic branching)
            
            if (debug_enum && comparison_idx < 5) {
                cout << "  BRANCHING on var" << prop_var_id << " (UNKNOWN): generating 2 states\n";
                cout << "    TRUE path: adjustment += " << ((domain_mapping[comp.prop_var_id][comp.true_value] - unknown_value) * multiplier) << "\n";
                cout << "    FALSE path: adjustment += " << ((domain_mapping[comp.prop_var_id][comp.false_value] - unknown_value) * multiplier) << "\n";
            }
            
            // Try TRUE: delta from UNKNOWN to true_value
            int true_delta = (domain_mapping[comp.prop_var_id][comp.true_value] - unknown_value) * multiplier;
            enumerate_combinations(comparison_idx + 1,
                                 current_hash_adjustment + true_delta);

            // Try FALSE: delta from UNKNOWN to false_value
            int false_delta = (domain_mapping[comp.prop_var_id][comp.false_value] - unknown_value) * multiplier;
            enumerate_combinations(comparison_idx + 1,
                                 current_hash_adjustment + false_delta);
        }
    };
    
    enumerate_combinations(0, 0);
    
    if (debug_enum && affected_comparisons.size() > 0) {
        cout << "  RESULT: Generated " << result.size() << " states from base_state=" << base_state_index
             << " (after reset=" << state_with_unknowns << ")" << endl;
        if (result.size() <= 10) {
            cout << "  States: ";
            for (size_t i = 0; i < result.size(); ++i) {
                if (i > 0) cout << ", ";
                cout << result[i];
            }
            cout << endl;
        }
    }
    
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
    
    // Build a map from goal axiom effect variables to their axiom indices
    // Goal axioms have preconditions and exactly one effect
    unordered_map<int, int> goal_axiom_map;  // effect_var_id -> axiom_index
    int axiom_idx = 0;
    for (OperatorProxy axiom : task_proxy.get_axioms()) {
        if (!axiom.get_preconditions().empty() && axiom.get_effects().size() == 1) {
            int effect_var_id = axiom.get_effects()[0].get_fact().get_variable().get_id();
            goal_axiom_map[effect_var_id] = axiom_idx;
        }
        axiom_idx++;
    }
    
    // Process goals from task_proxy.get_goals()
    for (FactProxy goal : task_proxy.get_goals()) {
        int var_id = goal.get_variable().get_id();
        
        // Check if this is a goal axiom effect variable
        auto it = goal_axiom_map.find(var_id);
        if (it != goal_axiom_map.end()) {
            // This is a goal axiom effect - extract its preconditions as the actual goals
            int goal_axiom_idx = it->second;
            OperatorProxy goal_axiom = task_proxy.get_axioms()[goal_axiom_idx];
            for (FactProxy pre : goal_axiom.get_preconditions()) {
                int pre_var_id = pre.get_variable().get_id();
                
                // Add precondition as goal if it has a domain mapping
                // This includes comparison axiom variables (for numeric goals)
                if (!variable_is_trivial(pre_var_id)) {
                    int val = pre.get_value();
                    abstract_goals.emplace_back(pre_var_id, domain_mapping[pre_var_id][val]);
                }
            }
        } else {
            // Regular propositional goal - add directly
            if (!variable_is_trivial(var_id)) {
                int val = goal.get_value();
                abstract_goals.emplace_back(var_id, domain_mapping[var_id][val]);
            }
        }
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
        int num_partitions = numeric_domain_mapping[num_var_id]->get_num_partitions();
        int partition = (remaining / multiplier) % num_partitions;
        
        ss << "num" << num_var_id << "=p" << partition;
        if (num_var_id < numeric_domain_mapping.size() - 1) {
            ss << ", ";
        }
    }
    
    ss << "]";
    return ss.str();
}

// Helper to decode into vectors for programmatic checks
static void decode_state_to_vectors(int state_index,
                                    const vector<int> &domain_sizes,
                                    const NumericDomainMappingType &numeric_domain_mapping,
                                    const vector<int> &hash_multipliers,
                                    vector<int> &prop_values_out,
                                    vector<int> &num_partitions_out) {
    prop_values_out.clear();
    num_partitions_out.clear();

    int remaining = state_index;
    // Propositional variables
    for (size_t var_id = 0; var_id < domain_sizes.size(); ++var_id) {
        int multiplier = hash_multipliers[var_id];
        int value = (remaining / multiplier) % domain_sizes[var_id];
        prop_values_out.push_back(value);
    }
    // Numeric partitions
    for (size_t num_var_id = 0; num_var_id < numeric_domain_mapping.size(); ++num_var_id) {
        int multiplier_idx = domain_sizes.size() + num_var_id;
        int multiplier = hash_multipliers[multiplier_idx];
        int num_partitions = numeric_domain_mapping[num_var_id]->get_num_partitions();
        int partition = (remaining / multiplier) % num_partitions;
        num_partitions_out.push_back(partition);
    }
}

//Regression search to get lookup value for all abstract states, similar to PDBs.
void DomainAbstractionFactory::compute_distances(
    const TaskProxy &task_proxy,
    const vector<AbstractOperator> &operators, const MatchTree &match_tree,
    const vector<Fact> &abstract_goals, const vector<int> &domain_sizes,
    bool compute_plan) {
    
    // DETAILED DEBUG OUTPUT FOR ITERATION 2
    static int compute_distances_call_count = 0;
    compute_distances_call_count++;
    
    if (compute_distances_call_count == 2) {
        ofstream debug_file("iteration2_debug.txt");
        debug_file << "========================================\n";
        debug_file << "ITERATION 2 COMPLETE DEBUG OUTPUT\n";
        debug_file << "========================================\n\n";
        
        // 1) INITIAL ABSTRACT STATE
        debug_file << "===== 1) INITIAL ABSTRACT STATE =====\n";
        State initial_state = task_proxy.get_initial_state();
        int init_hash = compute_abstract_state_hash(
            initial_state, task_proxy, domain_mapping, 
            numeric_domain_mapping, hash_multipliers);
        debug_file << "Hash: " << init_hash << "\n";
        debug_file << "Decoded: " << decode_abstract_state(init_hash, domain_sizes, 
                                                            numeric_domain_mapping, hash_multipliers) << "\n\n";
        
        debug_file << "Concrete propositional values:\n";
        VariablesProxy vars = task_proxy.get_variables();
        for (size_t var_id = 0; var_id < vars.size(); ++var_id) {
            if (!variable_is_trivial(var_id)) {
                VariableProxy var = vars[var_id];
                debug_file << "  var" << var_id << " = " << initial_state[var_id].get_value() 
                          << " (abstract: " << domain_mapping[var_id][initial_state[var_id].get_value()] 
                          << ")\n";
            }
        }
        
        debug_file << "\nConcrete numeric values:\n";
        NumericVariablesProxy num_vars = task_proxy.get_numeric_variables();
        for (size_t num_var_id = 0; num_var_id < num_vars.size(); ++num_var_id) {
            NumericVariableProxy num_var = num_vars[num_var_id];
            ap_float init_val = num_var.get_initial_state_value();
            debug_file << "  num_var" << num_var_id << " = " << init_val;
            
            // Find partition
            if (num_var_id < numeric_domain_mapping.size()) {
                const NumericDomainMapping &mapping = *numeric_domain_mapping[num_var_id];
                if (mapping.get_num_partitions() > 1) {
                    const vector<NumericRange> &ranges = mapping.get_ranges();
                    for (size_t part = 0; part < ranges.size(); ++part) {
                        const NumericRange &range = ranges[part];
                        if (init_val >= range.lower && init_val < range.upper) {
                            debug_file << " -> partition " << part << " [" << range.lower << ", " << range.upper << ")";
                            break;
                        }
                    }
                }
            }
            debug_file << "\n";
        }
        debug_file << "\n";
        
        // 2) VARIABLE DOMAINS
        debug_file << "===== 2) VARIABLE DOMAINS =====\n\n";
        debug_file << "Propositional variables:\n";
        for (size_t var_id = 0; var_id < vars.size(); ++var_id) {
            VariableProxy var = vars[var_id];
            debug_file << "var" << var_id << " (" << var.get_name() << "):\n";
            debug_file << "  Concrete domain size: " << var.get_domain_size() << "\n";
            if (variable_is_trivial(var_id)) {
                debug_file << "  [TRIVIAL - abstracted away]\n";
            } else {
                debug_file << "  Abstract domain size: " << domain_mapping[var_id].size() << "\n";
                debug_file << "  Mapping: [";
                for (size_t val = 0; val < domain_mapping[var_id].size(); ++val) {
                    if (val > 0) debug_file << ", ";
                    debug_file << val << "->" << domain_mapping[var_id][val];
                }
                debug_file << "]\n";
            }
        }
        
        debug_file << "\nNumeric variables:\n";
        for (size_t num_var_id = 0; num_var_id < num_vars.size(); ++num_var_id) {
            NumericVariableProxy num_var = num_vars[num_var_id];
            debug_file << "num_var" << num_var_id << " (" << num_var.get_name() << "):\n";
            if (num_var_id >= numeric_domain_mapping.size()) {
                debug_file << "  [TRIVIAL - 1 partition covering all values]\n";
            } else {
                const NumericDomainMapping &mapping = *numeric_domain_mapping[num_var_id];
                int num_partitions = mapping.get_num_partitions();
                if (num_partitions <= 1) {
                    debug_file << "  [TRIVIAL - 1 partition covering all values]\n";
                } else {
                    const vector<NumericRange> &ranges = mapping.get_ranges();
                    debug_file << "  Partitions: " << num_partitions << "\n";
                    for (size_t part = 0; part < ranges.size(); ++part) {
                        const NumericRange &range = ranges[part];
                        debug_file << "    partition " << part << ": [" << range.lower << ", " << range.upper << ")\n";
                    }
                }
            }
        }
        
        debug_file << "\nAbstract numeric partition variables:\n";
        debug_file << "(These represent which partition a refined numeric variable is in)\n";
        int abstract_var_offset = domain_sizes.size();
        for (size_t num_var_id = 0; num_var_id < numeric_domain_mapping.size(); ++num_var_id) {
            const NumericDomainMapping &mapping = *numeric_domain_mapping[num_var_id];
            int num_partitions = mapping.get_num_partitions();
            if (num_partitions > 1) {
                int abstract_var_id = abstract_var_offset + num_var_id;
                debug_file << "var" << abstract_var_id << " (partition of num_var" << num_var_id << "):\n";
                debug_file << "  Domain size: " << num_partitions << " (values 0.." << (num_partitions-1) << ")\n";
                debug_file << "  Meaning:\n";
                const vector<NumericRange> &ranges = mapping.get_ranges();
                for (size_t part = 0; part < ranges.size(); ++part) {
                    const NumericRange &range = ranges[part];
                    debug_file << "    var" << abstract_var_id << " = " << part 
                              << "  means  num_var" << num_var_id << " in [" 
                              << range.lower << ", " << range.upper << ")\n";
                }
            }
        }
        debug_file << "\n";
        
        // 2.5) ABSTRACT GOAL CONDITION
        debug_file << "===== 2.5) ABSTRACT GOAL CONDITION =====\n\n";
        
        // First show the concrete goals from the task
        debug_file << "Concrete goals from task:\n";
        for (FactProxy goal : task_proxy.get_goals()) {
            int var_id = goal.get_variable().get_id();
            debug_file << "  var" << var_id << " = " << goal.get_value();
            
            // Check if this is a goal axiom variable
            bool found_axiom = false;
            for (OperatorProxy axiom : task_proxy.get_axioms()) {
                if (!axiom.get_preconditions().empty() && axiom.get_effects().size() == 1) {
                    int effect_var_id = axiom.get_effects()[0].get_fact().get_variable().get_id();
                    if (effect_var_id == var_id) {
                        debug_file << " [GOAL AXIOM - derived from:]\n";
                        for (FactProxy pre : axiom.get_preconditions()) {
                            int pre_var_id = pre.get_variable().get_id();
                            debug_file << "      var" << pre_var_id << " = " << pre.get_value();
                            
                            // Check if this is a comparison axiom variable
                            ComparisonAxiomsProxy comp_axioms = task_proxy.get_comparison_axioms();
                            for (ComparisonAxiomProxy comp_axiom : comp_axioms) {
                                FactProxy true_fact = comp_axiom.get_true_fact();
                                if (true_fact.get_variable().get_id() == pre_var_id && 
                                    true_fact.get_value() == pre.get_value()) {
                                    comp_operator op = comp_axiom.get_comparison_operator_type();
                                    NumericVariableProxy left = comp_axiom.get_left_variable();
                                    NumericVariableProxy right = comp_axiom.get_right_variable();
                                    
                                    debug_file << " [comparison: num_var" << left.get_id();
                                    if (op == lt) debug_file << " < ";
                                    else if (op == le) debug_file << " <= ";
                                    else if (op == eq) debug_file << " == ";
                                    else if (op == ge) debug_file << " >= ";
                                    else if (op == gt) debug_file << " > ";
                                    debug_file << "num_var" << right.get_id() << "]";
                                    break;
                                }
                            }
                            debug_file << "\n";
                        }
                        found_axiom = true;
                        break;
                    }
                }
            }
            if (!found_axiom) {
                debug_file << "\n";
            }
        }
        
        debug_file << "\nAbstract goals (after processing):\n";
        debug_file << "Number of abstract goal facts: " << abstract_goals.size() << "\n";
        for (const Fact &goal : abstract_goals) {
            debug_file << "  var" << goal.var << " = " << goal.value;
            
            // Try to decode what this means
            if (goal.var < domain_sizes.size()) {
                // Propositional variable
                debug_file << " (propositional)";
            } else {
                // Numeric variable partition
                int num_var_idx = goal.var - domain_sizes.size();
                debug_file << " (num_var" << num_var_idx << " in partition " << goal.value << ")";
                
                // Show the range
                if (num_var_idx < (int)numeric_domain_mapping.size()) {
                    const NumericDomainMapping &mapping = *numeric_domain_mapping[num_var_idx];
                    if (goal.value < (int)mapping.get_ranges().size()) {
                        const NumericRange &range = mapping.get_ranges()[goal.value];
                        debug_file << " [" << range.lower << ", " << range.upper << ")";
                    }
                }
            }
            debug_file << "\n";
        }
        debug_file << "\n";
        
        // 3) ALL CONCRETE OPERATORS
        debug_file << "===== 3) CONCRETE OPERATORS =====\n\n";
        OperatorsProxy ops = task_proxy.get_operators();
        for (size_t op_id = 0; op_id < ops.size(); ++op_id) {
            OperatorProxy op = ops[op_id];
            debug_file << "Operator " << op_id << ": " << op.get_name() << "\n";
            debug_file << "  Cost: " << op.get_cost() << "\n";
            
            debug_file << "  Preconditions:\n";
            for (FactProxy pre : op.get_preconditions()) {
                debug_file << "    var" << pre.get_variable().get_id() << " = " 
                          << pre.get_value() << "\n";
            }
            
            debug_file << "  Effects:\n";
            for (EffectProxy eff : op.get_effects()) {
                debug_file << "    var" << eff.get_fact().get_variable().get_id() << " := " 
                          << eff.get_fact().get_value() << "\n";
            }
            
            debug_file << "  Numeric effects:\n";
            bool has_numeric_effects = false;
            for (auto ass_eff : op.get_ass_effects()) {
                NumAssProxy ass = ass_eff.get_assignment();
                int affected_var_id = ass.get_affected_variable().get_id();
                f_operator fop = ass.get_assigment_operator_type();
                
                debug_file << "    num_var" << affected_var_id;
                
                // Show the operator type
                if (fop == assign) {
                    debug_file << " := ";
                } else if (fop == scale_up) {
                    debug_file << " *= ";
                } else if (fop == scale_down) {
                    debug_file << " /= ";
                } else if (fop == increase) {
                    debug_file << " += ";
                } else if (fop == decrease) {
                    debug_file << " -= ";
                } else {
                    debug_file << " ?= ";
                }
                
                // Try to show the expression details
                NumericVariableProxy assigned_var = ass.get_assigned_variable();
                int assigned_var_id = assigned_var.get_id();
                
                if (assigned_var_id >= 0) {
                    debug_file << "num_var" << assigned_var_id;
                } else {
                    debug_file << "<constant>";
                }
                
                debug_file << "\n";
                has_numeric_effects = true;
            }
            if (!has_numeric_effects) {
                debug_file << "    [none]\n";
            }
            debug_file << "\n";
        }
        
        // 4) ALL ABSTRACT OPERATORS
        debug_file << "===== 4) ABSTRACT OPERATORS =====\n\n";
        debug_file << "Total abstract operators: " << operators.size() << "\n\n";
        
        // Get applicable operators for initial state
        vector<int> applicable_ops;
        match_tree.get_applicable_operator_ids(init_hash, applicable_ops);
        set<int> applicable_set(applicable_ops.begin(), applicable_ops.end());
        
        debug_file << "Operators applicable to initial state: " << applicable_ops.size() << "\n";
        debug_file << "Applicable operator IDs: [";
        for (size_t i = 0; i < min(applicable_ops.size(), size_t(20)); ++i) {
            if (i > 0) debug_file << ", ";
            debug_file << applicable_ops[i];
        }
        if (applicable_ops.size() > 20) debug_file << ", ...";
        debug_file << "]\n\n";
        
        for (size_t op_idx = 0; op_idx < operators.size(); ++op_idx) {
            const AbstractOperator &op = operators[op_idx];
            bool is_applicable = applicable_set.find(op_idx) != applicable_set.end();
            
            debug_file << "Abstract Operator " << op_idx;
            if (is_applicable) debug_file << " [APPLICABLE TO INITIAL STATE]";
            debug_file << "\n";
            debug_file << "  Concrete operator: " << op.get_concrete_op_id() << "\n";
            debug_file << "  Cost: " << op.get_cost() << "\n";
            
            debug_file << "  Regression preconditions:\n";
            const vector<Fact> &regr_pre = op.get_regression_preconditions();
            if (regr_pre.empty()) {
                debug_file << "    [none]\n";
            }
            for (const Fact &pre : regr_pre) {
                debug_file << "    var" << pre.var << " = " << pre.value << "\n";
            }
            
            
            debug_file << "  Numeric variable transitions:\n";
            const vector<int> &changed_vars = op.get_changed_numeric_vars();
            const vector<int> &src_parts = op.get_source_partitions();
            const vector<int> &tgt_parts = op.get_target_partitions();
            if (changed_vars.empty()) {
                debug_file << "    [none]\n";
            }
            for (size_t i = 0; i < changed_vars.size(); ++i) {
                debug_file << "    num_var" << changed_vars[i] << ": partition " 
                          << src_parts[i] << " -> " << tgt_parts[i] << "\n";
            }
            debug_file << "\n";
        }
        
        // 5) AXIOMS TREE STRUCTURE
        debug_file << "===== 5) AXIOM TREE STRUCTURE =====\n\n";
        
        debug_file << "Assignment Axioms:\n";
        AssignmentAxiomsProxy ass_axioms = task_proxy.get_assignment_axioms();
        for (size_t ax_id = 0; ax_id < ass_axioms.size(); ++ax_id) {
            AssignmentAxiomProxy axiom = ass_axioms[ax_id];
            debug_file << "AssignmentAxiom[" << ax_id << "]: num_var" 
                      << axiom.get_assignment_variable().get_id() << " := f(...)\n";
            
            // Show what this axiom depends on
            NumericVariableProxy left = axiom.get_left_variable();
            NumericVariableProxy right = axiom.get_right_variable();
            debug_file << "  Depends on:\n";
            debug_file << "    ├─ left: num_var" << left.get_id() << "\n";
            debug_file << "    └─ right: num_var" << right.get_id() << "\n";
        }
        debug_file << "\n";
        
        debug_file << "Comparison Axioms:\n";
        ComparisonAxiomsProxy comp_axioms = task_proxy.get_comparison_axioms();
        for (size_t ax_id = 0; ax_id < comp_axioms.size(); ++ax_id) {
            ComparisonAxiomProxy axiom = comp_axioms[ax_id];
            FactProxy true_fact = axiom.get_true_fact();
            FactProxy false_fact = axiom.get_false_fact();
            NumericVariableProxy left = axiom.get_left_variable();
            NumericVariableProxy right = axiom.get_right_variable();
            
            debug_file << "ComparisonAxiom[" << ax_id << "]: var" 
                      << true_fact.get_variable().get_id() << " := (num_var" 
                      << left.get_id() << " " << axiom.get_comparison_operator_type() 
                      << " num_var" << right.get_id() << ")\n";
            debug_file << "  Sets:\n";
            debug_file << "    ├─ var" << true_fact.get_variable().get_id() 
                      << " = " << true_fact.get_value() << " (if condition true)\n";
            debug_file << "    └─ var" << false_fact.get_variable().get_id() 
                      << " = " << false_fact.get_value() << " (if condition false)\n";
            debug_file << "  Depends on:\n";
            debug_file << "    ├─ num_var" << left.get_id() << "\n";
            debug_file << "    └─ num_var" << right.get_id() << "\n";
        }
        
        debug_file << "\n========================================\n";
        debug_file << "END OF DEBUG OUTPUT\n";
        debug_file << "========================================\n";
        debug_file.close();
        
        cout << "DEBUG: Wrote detailed debug output to iteration2_debug.txt" << endl;
    }
    
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
        
        // DEBUG: Log ALL state expansions in iteration 2
        static int factory_call = 0;
        if (dijkstra_iterations == 1) factory_call++;
        if (false && VERBOSE_DEBUG && factory_call == 2) {
            cout << "DEBUG DIJKSTRA_EXPAND: Expanding state " << state_index 
                 << " at distance " << distance << endl;
        }
        
        // Special detailed debugging for first goal state expansion
        bool is_first_goal_expansion = (state_index == first_goal_state && !first_goal_expanded);
        
        // DEBUG: Log when expanding the first goal state
        if (false && VERBOSE_DEBUG && is_first_goal_expansion) {
            cout << "DEBUG EXPAND: Expanding first goal state " << state_index << endl;
            string decoded = decode_abstract_state(state_index, domain_sizes, numeric_domain_mapping, hash_multipliers);
            cout << "  " << decoded << endl;
        }
 
        // Regress using abstract operators (from match tree)
        // These handle both propositional-only and numeric operators
        vector<int> applicable_operator_ids;
        match_tree.get_applicable_operator_ids(state_index, applicable_operator_ids);
        static int first_call = true; 
        if (first_call && distance == 0) {
            first_call = false;
            cout << "DEBUG: Initial state has " << applicable_operator_ids.size() << " applicable operators." << endl;
        }
        
        // DEBUG: Show applicable operators for first goal
        if (false && VERBOSE_DEBUG && is_first_goal_expansion) {
            cout << "DEBUG EXPAND: " << applicable_operator_ids.size() << " operators applicable" << endl;
            // List all applicable operator names for positive confirmation (kept small: typically ~10-15)
            OperatorsProxy concrete_ops = task_proxy.get_operators();
            for (int op_id : applicable_operator_ids) {
                const AbstractOperator &aop = operators[op_id];
                int concrete_id = aop.get_concrete_op_id();
                string name = (concrete_id >= 0 && concrete_id < (int)concrete_ops.size())
                                  ? concrete_ops[concrete_id].get_name()
                                  : string("<unknown> (id=") + to_string(concrete_id) + ")";
                cout << "DEBUG EXPAND:   applicable=\"" << name << "\" (op_id=" << op_id << ")" << endl;
            }
        }
        
        int valid_predecessors_this_state = 0;
        int out_of_bounds_predecessors = 0;
        int operators_checked = 0;
        for (int op_id : applicable_operator_ids) {
            const AbstractOperator &op = operators[op_id];
            int alternative_cost = distances[state_index] + op.get_cost();
        
            // DEBUG: Show details for first goal expansion
            // DEBUG: Look for operators that affect the refined numeric variables (num_17, num_66, num_2)
            bool affects_refined_vars = false;
            for (int var : op.get_changed_numeric_vars()) {
                if (var == 17 || var == 66 || var == 2) {
                    affects_refined_vars = true;
                    break;
                }
            }
            
            // Always print detailed info for the first few ops OR when they affect key refined vars
            // Additionally, ALWAYS print for the concrete op named "pour agent1 plant1" to aid debugging.
            bool is_pour = false;
            string op_name_for_debug;
            {
                OperatorsProxy concrete_ops = task_proxy.get_operators();
                int concrete_id = op.get_concrete_op_id();
                if (concrete_id >= 0 && concrete_id < (int)concrete_ops.size()) {
                    op_name_for_debug = concrete_ops[concrete_id].get_name();
                    if (op_name_for_debug == "pour agent1 plant1") {
                        is_pour = true;
                    }
                }
            }

            if (VERBOSE_DEBUG && is_first_goal_expansion && (operators_checked < 3 || affects_refined_vars || is_pour)) {
                if (affects_refined_vars) {
                    cout << "DEBUG EXPAND: *** FOUND OPERATOR AFFECTING REFINED VARS ***" << endl;
                }
                cout << "DEBUG EXPAND: Operator " << op_id << ", cost=" << op.get_cost()  << endl;
                // Also print the concrete operator name for clarity (e.g., "pour agent1 plant1")
                {
                    OperatorsProxy concrete_ops = task_proxy.get_operators();
                    int concrete_id = op.get_concrete_op_id();
                    if (concrete_id >= 0 && concrete_id < (int)concrete_ops.size()) {
                        cout << "DEBUG EXPAND:   name=\"" << concrete_ops[concrete_id].get_name() << "\"" << endl;
                    } else {
                        cout << "DEBUG EXPAND:   name=\"<unknown> (id=" << concrete_id << ")\"" << endl;
                    }
                }
                cout << "DEBUG EXPAND:   changed_numeric_vars=" << op.get_changed_numeric_vars().size() << ": ";
                for (size_t i = 0; i < min(op.get_changed_numeric_vars().size(), size_t(10)); ++i) {
                    cout << op.get_changed_numeric_vars()[i];
                    if (i < op.get_changed_numeric_vars().size() - 1 && i < 9) cout << ",";
                }
                if (op.get_changed_numeric_vars().size() > 10) cout << "...";
                cout << endl;
                cout << "DEBUG EXPAND:   source_partitions=" << op.get_source_partitions().size() << ": ";
                for (size_t i = 0; i < min(op.get_source_partitions().size(), size_t(10)); ++i) {
                    cout << op.get_source_partitions()[i];
                    if (i < op.get_source_partitions().size() - 1 && i < 9) cout << ",";
                }
                if (op.get_source_partitions().size() > 10) cout << "...";
                cout << endl;
                cout << "DEBUG EXPAND:   target_partitions=" << op.get_target_partitions().size() << ": ";
                for (size_t i = 0; i < min(op.get_target_partitions().size(), size_t(10)); ++i) {
                    cout << op.get_target_partitions()[i];
                    if (i < op.get_target_partitions().size() - 1 && i < 9) cout << ",";
                }
                if (op.get_target_partitions().size() > 10) cout << "...";
                cout << endl;

                // If this is pour, print per-variable partition transitions for clarity
                if (is_pour) {
                    cout << "DEBUG POUR: partition transitions (var: src->tgt)" << endl;
                    size_t n = min(op.get_changed_numeric_vars().size(),
                                   min(op.get_source_partitions().size(), op.get_target_partitions().size()));
                    for (size_t i = 0; i < n; ++i) {
                        cout << "  num" << op.get_changed_numeric_vars()[i]
                             << ": " << op.get_source_partitions()[i]
                             << " -> " << op.get_target_partitions()[i] << endl;
                    }
                }
            }
            
            // Iterate over all possible hash effects (predecessors)
            // Propositional operators have 1 effect, numeric operators have multiple
            const int base_hash_effect = op.get_hash_effect();
            
            int predecessors_this_op = 0;
            
            // Enumerate all possible predecessors considering comparison axiom cascades
            vector<int> possible_predecessors = enumerate_states_with_evaluated_comparisons(
                state_index + base_hash_effect,
                op.get_changed_numeric_vars(),
                op.get_source_partitions(),
                op.get_target_partitions(),
                task_proxy);
            
            // DEBUG CHECK 1: Verify base hash effect decoding matches expected numeric partition transitions
            if (VERBOSE_DEBUG && is_first_goal_expansion) {
                int base_state_index = state_index + base_hash_effect;
                if (base_state_index >= 0 && base_state_index < num_states) {
                    vector<int> cur_props, cur_nums;
                    vector<int> base_props, base_nums;
                    decode_state_to_vectors(state_index, domain_sizes, numeric_domain_mapping, hash_multipliers, cur_props, cur_nums);
                    decode_state_to_vectors(base_state_index, domain_sizes, numeric_domain_mapping, hash_multipliers, base_props, base_nums);

                    bool all_ok = true;
                    // Check numeric partitions for changed vars: current should be target, base should be source
                    for (size_t i = 0; i < op.get_changed_numeric_vars().size(); ++i) {
                        int var = op.get_changed_numeric_vars()[i];
                        int src = op.get_source_partitions()[i];
                        int tgt = op.get_target_partitions()[i];
                        int cur_part = (var >= 0 && var < (int)cur_nums.size()) ? cur_nums[var] : -999;
                        int base_part = (var >= 0 && var < (int)base_nums.size()) ? base_nums[var] : -999;
                        bool ok_var = (cur_part == tgt) && (base_part == src);
                        all_ok = all_ok && ok_var;
                        cout << "DEBUG HASH-EFFECT CHECK: var=num" << var
                                << " cur(tgt?)=" << cur_part << "~=" << tgt
                                << " base(src?)=" << base_part << "~=" << src
                                << " -> " << (ok_var ? "OK" : "MISMATCH") << endl;
                    }
                    // Check regression preconditions:
                    // - Propositional facts (prev/eff on props) must hold in the BASE (predecessor) state.
                    // - Numeric partition facts inside regression_preconditions represent TARGET partitions
                    //   (they were added to eff_pairs). Those must hold in the CURRENT state, not the base state.
                    const vector<Fact> &reg_pre = op.get_regression_preconditions();
                    for (const Fact &f : reg_pre) {
                        int v = f.var;
                        int expected = f.value;
                        int actual;
                        if (v >= 0 && v < (int)base_props.size()) {
                            // Propositional preconditions: check against predecessor (base) state
                            actual = base_props[v];
                        } else {
                            // Numeric partition preconditions (stored after propositional vars):
                            // these are TARGET partition facts and must hold in the CURRENT state.
                            int num_idx = v - (int)base_props.size();
                            actual = (num_idx >= 0 && num_idx < (int)cur_nums.size()) ? cur_nums[num_idx] : -999;
                        }
                        bool ok = (actual == expected);
                        all_ok = all_ok && ok;
                        cout << "DEBUG HASH-EFFECT CHECK: precond v" << v
                                << " base_val=" << actual << " expected=" << expected
                                << " -> " << (ok ? "OK" : "MISMATCH") << endl;
                    }
                    cout << "DEBUG HASH-EFFECT CHECK: base_state=" << base_state_index
                            << " effect=" << base_hash_effect << " RESULT=" << (all_ok ? "OK" : "MISMATCH") << endl;
                } else {
                    cout << "DEBUG HASH-EFFECT CHECK: base_state out of bounds (" << base_state_index << ")" << endl;
                }
            }

            // DEBUG: Show predecessors for first goal or for pour operators specifically
            if (VERBOSE_DEBUG && is_first_goal_expansion && (operators_checked < 3 || is_pour) && possible_predecessors.size() > 0) {
                cout << "DEBUG EXPAND:   base_hash_effect=" << base_hash_effect 
                        << " → " << possible_predecessors.size() << " predecessors: ";
                for (size_t i = 0; i < min(possible_predecessors.size(), size_t(5)); ++i) {
                    cout << possible_predecessors[i];
                    if (i < possible_predecessors.size() - 1 && i < 4) cout << ",";
                }
                if (possible_predecessors.size() > 5) cout << "...";
                cout << endl;
                if (is_pour) {
                    // For pour, also list all predecessors explicitly (no truncation)
                    cout << "DEBUG POUR:   ALL predecessors (state indices): ";
                    for (size_t i = 0; i < possible_predecessors.size(); ++i) {
                        if (i) cout << ", ";
                        cout << possible_predecessors[i];
                    }
                    cout << endl;
                }
            }
            // DEBUG CHECK 2: Validate a few enumerated predecessors
            if (VERBOSE_DEBUG && is_first_goal_expansion && !possible_predecessors.empty()) {
                size_t check_limit = min<size_t>(possible_predecessors.size(), 5);
                for (size_t pi = 0; pi < check_limit; ++pi) {
                    int pred = possible_predecessors[pi];
                    if (pred < 0 || pred >= num_states) {
                        cout << "DEBUG ENUM CHECK: predecessor " << pred << " out of bounds" << endl;
                        continue;
                    }
                    vector<int> cur_props, cur_nums;
                    vector<int> pred_props, pred_nums;
                    decode_state_to_vectors(state_index, domain_sizes, numeric_domain_mapping, hash_multipliers, cur_props, cur_nums);
                    decode_state_to_vectors(pred, domain_sizes, numeric_domain_mapping, hash_multipliers, pred_props, pred_nums);

                    bool ok_pred = true;
                    // Numeric source partitions must hold in predecessor
                    for (size_t i = 0; i < op.get_changed_numeric_vars().size(); ++i) {
                        int var = op.get_changed_numeric_vars()[i];
                        int src = op.get_source_partitions()[i];
                        int pred_part = (var >= 0 && var < (int)pred_nums.size()) ? pred_nums[var] : -999;
                        bool ok_var = (pred_part == src);
                        ok_pred = ok_pred && ok_var;
                        cout << "DEBUG ENUM CHECK: pred=" << pred << " var=num" << var
                                << " src?=" << pred_part << "~=" << src
                                << " -> " << (ok_var ? "OK" : "MISMATCH") << endl;
                    }
                    // Regression preconditions for enumerated predecessors:
                    // - Propositional facts must hold in the PREDECESSOR (pred_* vectors).
                    // - Numeric partition preconditions (target partitions) must hold in the CURRENT state.
                    const vector<Fact> &reg_pre2 = op.get_regression_preconditions();
                    for (const Fact &f : reg_pre2) {
                        int v = f.var;
                        int expected = f.value;
                        int actual;
                        if (v >= 0 && v < (int)pred_props.size()) {
                            // Propositional preconditions on predecessor
                            actual = pred_props[v];
                        } else {
                            // Numeric partition preconditions (target partitions) on CURRENT state
                            int num_idx = v - (int)pred_props.size();
                            actual = (num_idx >= 0 && num_idx < (int)cur_nums.size()) ? cur_nums[num_idx] : -999;
                        }
                        bool ok = (actual == expected);
                        ok_pred = ok_pred && ok;
                        cout << "DEBUG ENUM CHECK: precond v" << v
                                << " pred_val=" << actual << " expected=" << expected
                                << " -> " << (ok ? "OK" : "MISMATCH") << endl;
                    }
                    cout << "DEBUG ENUM CHECK: predecessor " << pred << " RESULT=" << (ok_pred ? "OK" : "MISMATCH") << endl;
                }
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
                    
                    // Continue without asserting to allow the search to proceed
                    // while we gather diagnostics. Invalid predecessors are
                    // expected in conservative enumerations and should be skipped.
                    continue;
                }
                
                // Enforce predecessor-side preconditions (forward preconditions and
                // numeric source partitions) for this operator.
                bool predecessor_ok = true;
                {
                    const vector<Fact> &pred_pre = op.get_predecessor_preconditions();
                    if (!pred_pre.empty()) {
                        vector<int> pred_props, pred_nums;
                        vector<int> cur_props_dummy, cur_nums_dummy;
                        decode_state_to_vectors(predecessor, domain_sizes, numeric_domain_mapping, hash_multipliers,
                                                pred_props, pred_nums);
                        for (const Fact &f : pred_pre) {
                            int v = f.var;
                            int expected = f.value;
                            if (v < static_cast<int>(domain_sizes.size())) {
                                // Propositional var must hold in predecessor
                                int actual = (v >= 0 && v < static_cast<int>(pred_props.size())) ? pred_props[v] : -999;
                                if (actual != expected) {
                                    predecessor_ok = false;
                                    break;
                                }
                            } else {
                                // Numeric partition var (abstract var after propositional vars)
                                int num_idx = v - static_cast<int>(domain_sizes.size());
                                int actual = (num_idx >= 0 && num_idx < static_cast<int>(pred_nums.size())) ? pred_nums[num_idx] : -999;
                                if (actual != expected) {
                                    predecessor_ok = false;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (!predecessor_ok) {
                    continue; // Skip predecessors that don't satisfy predecessor preconditions
                }

                valid_predecessors_this_state++;
                predecessors_this_op++;
                
                if (alternative_cost < distances[predecessor]) {
                    total_expansions++;
                    
                    distances[predecessor] = alternative_cost;
                    pq.push(alternative_cost, predecessor);
                    if (compute_plan) {
                        generating_op_ids[predecessor] = op_id;
                    }
                }
            }
            operators_checked++;
            
        }
        
        if (VERBOSE_DEBUG && is_first_goal_expansion) {
            first_goal_expanded = true;
            cout << "DEBUG EXPAND: Total valid predecessors from goal: " << valid_predecessors_this_state << endl;
        }
    }
    
    if (VERBOSE_DEBUG) {
        cout << "DEBUG DIJKSTRA: Completed " << dijkstra_iterations << " iterations, " 
             << total_expansions << " distance updates" << endl;
    }
    
    // DEBUG: Print initial state distance
    State initial_state = task_proxy.get_initial_state();
    int init_hash = compute_abstract_state_hash(initial_state, task_proxy, domain_mapping, 
                                                  numeric_domain_mapping, hash_multipliers);
    if (VERBOSE_DEBUG) {
        cout << "DEBUG DIJKSTRA: Initial state hash = " << init_hash 
             << ", distance = " << distances[init_hash] << endl;
    }
    
    // Track which iteration this is for debug output
    static int iteration_count = 0;
    iteration_count++;
    
    // DEBUG: Print table of core variables for all states
    if (VERBOSE_DEBUG || true) {
        cout << "\n=== TABLE OF CORE VARIABLES FOR ALL " << num_states << " STATES ===\n";
        
        // First, identify which propositional variables are derived from axioms
        vector<bool> is_axiom_var(task_proxy.get_variables().size(), false);
        for (OperatorProxy axiom : task_proxy.get_axioms()) {
            if (axiom.get_effects().size() == 1) {
                int effect_var = axiom.get_effects()[0].get_fact().get_variable().get_id();
                is_axiom_var[effect_var] = true;
            }
        }
        
        // Identify refined numeric variables and non-axiom propositional variables
        vector<int> refined_numeric_vars;
        int num_prop_vars = domain_sizes.size();
        for (size_t num_var_id = 0; num_var_id < numeric_domain_mapping.size(); ++num_var_id) {
            const NumericDomainMapping &mapping = *numeric_domain_mapping[num_var_id];
            if (mapping.get_num_partitions() > 1) {
                refined_numeric_vars.push_back(num_var_id);
            }
        }
        
        vector<int> non_axiom_vars;
        for (size_t var_id = 0; var_id < domain_sizes.size(); ++var_id) {
            if (!variable_is_trivial(var_id) && !is_axiom_var[var_id]) {
                non_axiom_vars.push_back(var_id);
            }
        }
        
        // Print table header
        cout << "\nState | Distance | ";
        for (int num_var_id : refined_numeric_vars) {
            cout << "num" << num_var_id << "_p | ";
        }
        for (int var_id : non_axiom_vars) {
            cout << "var" << var_id << " | ";
        }
        cout << "\n";
        
        // Print separator
        cout << "------|----------|";
        for (size_t i = 0; i < refined_numeric_vars.size(); ++i) {
            cout << "--------|";
        }
        for (size_t i = 0; i < non_axiom_vars.size(); ++i) {
            cout << "--------|";
        }
        cout << "\n";
        
        // Print each state
        for (int state_hash = 0; state_hash < num_states; ++state_hash) {
            // State index
            cout << setw(5) << state_hash << " | ";
            
            // Distance
            int dist = distances[state_hash];
            if (dist == numeric_limits<int>::max()) {
                cout << setw(8) << "INF";
            } else {
                cout << setw(8) << dist;
            }
            cout << " | ";
            
            // Numeric partitions
            for (int num_var_id : refined_numeric_vars) {
                const NumericDomainMapping &mapping = *numeric_domain_mapping[num_var_id];
                int abstract_var_id = num_prop_vars + num_var_id;
                int partition = (state_hash / hash_multipliers[abstract_var_id]) % mapping.get_num_partitions();
                cout << setw(6) << partition << " | ";
            }
            
            // Non-axiom propositional variables
            for (int var_id : non_axiom_vars) {
                int value = (state_hash / hash_multipliers[var_id]) % domain_sizes[var_id];
                cout << setw(6) << value << " | ";
            }
            
            cout << "\n";
        }
        cout << "\n";
    }
    
    // DEBUG: Find states with same numeric partitions and non-axiom propositional variables
    if (VERBOSE_DEBUG && distances[init_hash] == numeric_limits<int>::max() && iteration_count == 2) {
        cout << "=== FINDING STATES WITH SAME CORE (numeric partitions + non-axiom vars) ===\n";
        
        // First, identify which propositional variables are derived from axioms
        vector<bool> is_axiom_var(task_proxy.get_variables().size(), false);
        for (OperatorProxy axiom : task_proxy.get_axioms()) {
            if (axiom.get_effects().size() == 1) {
                int effect_var = axiom.get_effects()[0].get_fact().get_variable().get_id();
                is_axiom_var[effect_var] = true;
            }
        }
        
        // Extract initial state's numeric partitions (the "core" we're matching)
        vector<int> init_numeric_partitions;
        int num_prop_vars = domain_sizes.size();
        for (size_t num_var_id = 0; num_var_id < numeric_domain_mapping.size(); ++num_var_id) {
            const NumericDomainMapping &mapping = *numeric_domain_mapping[num_var_id];
            if (mapping.get_num_partitions() > 1) {
                // This numeric variable is refined - extract its partition from initial state
                int abstract_var_id = num_prop_vars + num_var_id;
                int partition = (init_hash / hash_multipliers[abstract_var_id]) % mapping.get_num_partitions();
                init_numeric_partitions.push_back(partition);
                cout << "  num_var" << num_var_id << ": partition " << partition << " (abstract var" << abstract_var_id << ")\n";
            }
        }
        
        // Extract initial state's non-axiom propositional values
        vector<pair<int, int>> init_non_axiom_facts;
        for (size_t var_id = 0; var_id < domain_sizes.size(); ++var_id) {
            if (!variable_is_trivial(var_id) && !is_axiom_var[var_id]) {
                int value = (init_hash / hash_multipliers[var_id]) % domain_sizes[var_id];
                init_non_axiom_facts.push_back(make_pair(var_id, value));
                cout << "  var" << var_id << " = " << value << " (non-axiom)\n";
            }
        }
        
        // Now scan all states to find matches
        cout << "\nScanning " << num_states << " states for matches...\n";
        vector<int> matching_states;
        for (int state_hash = 0; state_hash < num_states; ++state_hash) {
            bool matches = true;
            
            // Check numeric partitions
            size_t partition_idx = 0;
            for (size_t num_var_id = 0; num_var_id < numeric_domain_mapping.size(); ++num_var_id) {
                const NumericDomainMapping &mapping = *numeric_domain_mapping[num_var_id];
                if (mapping.get_num_partitions() > 1) {
                    int abstract_var_id = num_prop_vars + num_var_id;
                    int partition = (state_hash / hash_multipliers[abstract_var_id]) % mapping.get_num_partitions();
                    if (partition != init_numeric_partitions[partition_idx]) {
                        matches = false;
                        break;
                    }
                    partition_idx++;
                }
            }
            
            if (!matches) continue;
            
            // Check non-axiom propositional variables
            for (const auto &fact : init_non_axiom_facts) {
                int var_id = fact.first;
                int expected_value = fact.second;
                int actual_value = (state_hash / hash_multipliers[var_id]) % domain_sizes[var_id];
                if (actual_value != expected_value) {
                    matches = false;
                    break;
                }
            }
            
            if (matches) {
                matching_states.push_back(state_hash);
            }
        }
        
        cout << "\nFound " << matching_states.size() << " states with same core:\n";
        for (size_t i = 0; i < min(matching_states.size(), static_cast<size_t>(20)); ++i) {
            int state_hash = matching_states[i];
            int dist = distances[state_hash];
            cout << "  State " << state_hash << ": distance = ";
            if (dist == numeric_limits<int>::max()) {
                cout << "INFINITE";
            } else {
                cout << dist;
            }
            
            // Show the axiom variable values that differ
            cout << " [";
            bool first = true;
            for (size_t var_id = 0; var_id < domain_sizes.size(); ++var_id) {
                if (!variable_is_trivial(var_id) && is_axiom_var[var_id]) {
                    int value = (state_hash / hash_multipliers[var_id]) % domain_sizes[var_id];
                    if (!first) cout << ", ";
                    cout << "var" << var_id << "=" << value;
                    first = false;
                }
            }
            cout << "]\n";
        }
        
        if (matching_states.size() > 20) {
            cout << "  ... and " << (matching_states.size() - 20) << " more\n";
        }
        cout << "\n";
    }
    
    // DEBUG: Test enumerate_states_with_evaluated_comparisons on initial state
    if (VERBOSE_DEBUG && distances[init_hash] == numeric_limits<int>::max() && iteration_count == 2) {
        cout << "=== TESTING enumerate_states_with_evaluated_comparisons ON INITIAL STATE ===\n";
        cout << "Initial state (state 14):\n";
        cout << "  Hash: " << init_hash << "\n";
        cout << "  Core: num11_p=1, num37_p=1, num66_p=1, var24=0\n";
        
        // To trigger comparison axiom evaluation, we need to pass refined numeric variables
        // even if they don't change partitions (source == target)
        vector<int> test_changed_vars;
        vector<int> test_source_partitions;
        vector<int> test_target_partitions;
        
        int num_prop_vars = domain_sizes.size();
        
        // Add the refined numeric variables with same source and target partition
        for (size_t num_var_id = 0; num_var_id < numeric_domain_mapping.size(); ++num_var_id) {
            const NumericDomainMapping &mapping = *numeric_domain_mapping[num_var_id];
            if (mapping.get_num_partitions() > 1) {
                int abstract_var_id = num_prop_vars + num_var_id;
                int partition = (init_hash / hash_multipliers[abstract_var_id]) % mapping.get_num_partitions();
                test_changed_vars.push_back(num_var_id);
                test_source_partitions.push_back(partition);
                test_target_partitions.push_back(partition);  // Same partition (no change)
            }
        }
        
        cout << "  Calling enumerate with " << test_changed_vars.size() << " refined numeric vars (no partition changes)\n";
        
        vector<int> enumerated_states = enumerate_states_with_evaluated_comparisons(
            init_hash,
            test_changed_vars,
            test_source_partitions,
            test_target_partitions,
            task_proxy);
        
        cout << "\nGenerated " << enumerated_states.size() << " states:\n";
        
        // For each enumerated state, decode it and count TRUE comparison axioms
        int most_optimistic_state = -1;
        int max_true_comparisons = -1;
        
        for (int state_hash : enumerated_states) {
            cout << "\n  State " << state_hash << ":\n";
            
            // Decode ALL propositional variables (including comparison axioms)
            cout << "    Propositional vars: [";
            vector<bool> is_axiom_var(task_proxy.get_variables().size(), false);
            for (OperatorProxy axiom : task_proxy.get_axioms()) {
                if (axiom.get_effects().size() == 1) {
                    int effect_var = axiom.get_effects()[0].get_fact().get_variable().get_id();
                    is_axiom_var[effect_var] = true;
                }
            }
            
            int true_comparison_count = 0;
            bool first = true;
            for (size_t var_id = 0; var_id < domain_sizes.size(); ++var_id) {
                if (!variable_is_trivial(var_id)) {
                    int value = (state_hash / hash_multipliers[var_id]) % domain_sizes[var_id];
                    if (!first) cout << ", ";
                    cout << "var" << var_id << "=" << value;
                    if (is_axiom_var[var_id] && value == 1) {
                        true_comparison_count++;
                        cout << "(T)";
                    } else if (is_axiom_var[var_id]) {
                        cout << "(F)";
                    }
                    first = false;
                }
            }
            cout << "]\n";
            
            // Decode numeric partitions
            cout << "    Numeric partitions: [";
            first = true;
            for (size_t num_var_id = 0; num_var_id < numeric_domain_mapping.size(); ++num_var_id) {
                const NumericDomainMapping &mapping = *numeric_domain_mapping[num_var_id];
                if (mapping.get_num_partitions() > 1) {
                    int abstract_var_id = num_prop_vars + num_var_id;
                    int partition = (state_hash / hash_multipliers[abstract_var_id]) % mapping.get_num_partitions();
                    if (!first) cout << ", ";
                    cout << "num" << num_var_id << "_p=" << partition;
                    first = false;
                }
            }
            cout << "]\n";
            
            cout << "    TRUE comparisons: " << true_comparison_count << "\n";
            cout << "    Distance: " << (state_hash < num_states ? distances[state_hash] : -1) << "\n";
            
            if (true_comparison_count > max_true_comparisons) {
                max_true_comparisons = true_comparison_count;
                most_optimistic_state = state_hash;
            }
        }
        
        cout << "\n*** MOST OPTIMISTIC STATE: " << most_optimistic_state 
             << " with " << max_true_comparisons << " TRUE comparison axioms ***\n";
        
        if (most_optimistic_state >= 0 && most_optimistic_state < num_states) {
            cout << "Distance to goal: " << distances[most_optimistic_state] << "\n";
        }
        cout << "\n";
    }
    
    // DEBUG: If initial state is unreachable, check which operators could apply to it
    if (VERBOSE_DEBUG && distances[init_hash] == numeric_limits<int>::max() && iteration_count == 2) {
        cout << "DEBUG INITIAL STATE: Checking applicable operators for unreachable initial state (iteration 2)" << endl;
        cout << "  Initial state hash: " << init_hash << endl;
        
        // Check which operators match this state
        vector<int> applicable_ops;
        match_tree.get_applicable_operator_ids(init_hash, applicable_ops);
        cout << "  Number of applicable operators: " << applicable_ops.size() << endl;
        
        // Print first 20 applicable operators with their details
        for (size_t i = 0; i < min(applicable_ops.size(), static_cast<size_t>(20)); ++i) {
            int op_idx = applicable_ops[i];
            const AbstractOperator &op = operators[op_idx];
            cout << "  Operator " << op_idx << ": cost=" << op.get_cost() 
                 << ", concrete_op=" << op.get_concrete_op_id() << endl;

        }
        
        // Also print info about operators affecting var66
        cout << "  Checking operators that affect var66:" << endl;
        int var66_operators_count = 0;
        for (size_t i = 0; i < applicable_ops.size(); ++i) {
            int op_idx = applicable_ops[i];
            const AbstractOperator &op = operators[op_idx];
            
            // Check if this operator affects var66 by examining its source/target partitions
            const vector<int> &source_parts = op.get_source_partitions();
            const vector<int> &target_parts = op.get_target_partitions();
            
            // var66 is the first numeric variable (index 0 in numeric arrays)
            if (!source_parts.empty() && !target_parts.empty()) {
                if (source_parts[0] != target_parts[0]) {
                    var66_operators_count++;
                   
                }
            }
        }
        cout << "  Total operators affecting var66: " << var66_operators_count << endl;
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

    // DEBUG: Check if initial state has evaluated axioms
    // Initial state values (suppress full dump unless verbose)
    if (VERBOSE_DEBUG) {
        cout << "DEBUG PLAN: Initial state values:" << endl;
        for (int var_id = 0; var_id < initial_state.size(); ++var_id) {
            cout << "  var" << var_id << " = " << initial_state[var_id].get_value() << endl;
        }
    }

    // Compute the abstract state hash using the utility function that includes
    // full cascade evaluation of derived numeric variables and comparison axioms
    size_t current_state_hash = compute_abstract_state_hash(
        initial_state, task_proxy, domain_mapping, 
        numeric_domain_mapping, hash_multipliers);
    
    int current_state = static_cast<int>(current_state_hash);
    
    cout << "PLAN: Initial abstract state = " << current_state << endl;
    cout << "PLAN: Abstract state count = " << num_states << endl;
    cout << "PLAN: Distance to goal = " << distances[current_state] << endl;

    //print distances
    cout << "PLAN: Distance table (state -> distance):" << endl;
    for (int i = 0; i < distances.size(); ++i) {
        bool is_goal = is_goal_state(i, abstract_goals, domain_sizes);
        cout << "  [" << i << "] = " << distances[i] 
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
    cout << "PLAN: Reachable states = " << reachable_count << " / " << num_states << endl;
    
    // List which states are reachable and their distances
    if (VERBOSE_DEBUG) cout << "DEBUG PLAN: Reachable state details:" << endl;
    for (int i = 0; i < min(static_cast<int>(distances.size()), 20); ++i) {
        if (distances[i] != numeric_limits<int>::max() || i == current_state) {
            bool is_goal = is_goal_state(i, abstract_goals, domain_sizes);
            if (VERBOSE_DEBUG) cout << "  State " << i << ": distance=" << distances[i] 
                 << (is_goal ? " (GOAL)" : "")
                 << (i == current_state ? " (INITIAL)" : "") << endl;
        }
    }
    
    // Decode the initial state to understand what it represents
    if (current_state < num_states) {
        cout << "PLAN: Initial state details:" << endl;
        string decoded = decode_abstract_state(current_state, domain_sizes, 
                                              numeric_domain_mapping, hash_multipliers);
        cout << decoded << endl;
    }
    
    // Also decode some reachable states for comparison
    // Suppress random sample of reachable states unless verbose
    if (VERBOSE_DEBUG) {
        cout << "DEBUG PLAN: Decoding some reachable states:" << endl;
        for (int i : {0, 1, 4, 6}) {
            if (i < num_states) {
                cout << "State " << i << ":" << endl;
                string decoded = decode_abstract_state(i, domain_sizes, 
                                                      numeric_domain_mapping, hash_multipliers);
                cout << decoded << endl;
            }
        }
    }


    if (distances[current_state] != numeric_limits<int>::max()) {
        int plan_step = 0;
        cout << "PLAN: Executing abstract plan..." << endl;
        while (!is_goal_state(current_state, abstract_goals, domain_sizes)) {
            int op_id = generating_op_ids[current_state];
            assert(op_id != -1);
            const AbstractOperator &op = operators[op_id];
            
            // For operators with multiple hash effects (numeric operators), find the
            // correct hash effect that leads to a valid successor.
            // IMPORTANT: We need to evaluate comparison axioms for successors just like
            // we do for predecessors in Dijkstra!
            int hash_effect = -1;
            int successor_state = -1;

            int candidate_hash_effect = op.get_hash_effect();
            
            // Compute base successor (without comparison axiom evaluation)
            int base_successor = current_state - candidate_hash_effect;
            
            // Enumerate all possible successors with evaluated comparison axioms
            // For progression: we swap source/target partitions (opposite of regression)
            // In progression: source=current partitions, target=successor partitions
            vector<int> possible_successors = enumerate_states_with_evaluated_comparisons(
                base_successor,
                op.get_changed_numeric_vars(),
                op.get_target_partitions(),  // In progression: target becomes source
                op.get_source_partitions(),  // In progression: source becomes target
                task_proxy);
            
            // Find a valid successor with lower distance
            for (int candidate_successor : possible_successors) {
                // Check if this successor is valid (was reached during Dijkstra)
                if (candidate_successor >= 0 && candidate_successor < static_cast<int>(distances.size()) &&
                    distances[candidate_successor] != numeric_limits<int>::max() &&
                    distances[candidate_successor] < distances[current_state]) {
                    // Valid successor with lower distance - use it!
                    hash_effect = candidate_hash_effect;
                    successor_state = candidate_successor;
                    break;
                }
            }
            
            if (successor_state != -1) {
                break;  // Found a valid successor
            }

            // Report this plan step: operator and abstract effects
            {
                OperatorsProxy concrete_ops = task_proxy.get_operators();
                int concrete_id = op.get_concrete_op_id();
                string op_name = (concrete_id >= 0 && concrete_id < (int)concrete_ops.size()) ?
                                  concrete_ops[concrete_id].get_name() : ("<unknown>(" + to_string(concrete_id) + ")");
                cout << "PLAN: Step " << plan_step << " | state=" << current_state
                     << " -> successor=" << successor_state
                     << " via op=\"" << op_name << "\" (concrete_id=" << concrete_id << ")" << endl;
                // List all abstract effects for this operator
                cout << "  Abstract effects (hash_effect -> numeric transitions):" << endl;
                int he = op.get_hash_effect();
                cout << "    effect=" << he << ":";
                size_t n = min(op.get_changed_numeric_vars().size(),
                                min(op.get_source_partitions().size(), op.get_target_partitions().size()));
                if (n == 0) {
                    cout << " (no numeric changes)";
                } else {
                    cout << " ";
                    for (size_t i = 0; i < n; ++i) {
                        if (i) cout << ", ";
                        cout << "num" << op.get_changed_numeric_vars()[i]
                                << ": " << op.get_source_partitions()[i]
                                << "->" << op.get_target_partitions()[i];
                    }
                }
                cout << endl;
            }

            // Compute equivalent ops
            // We need to find all operators that can take us from current_state to successor_state
            // with the same cost as the generating operator
            vector<int> cheapest_operators;
            vector<int> applicable_operator_ids;
            match_tree.get_applicable_operator_ids(successor_state, applicable_operator_ids);
            for (int applicable_op_id : applicable_operator_ids) {
                const AbstractOperator &applicable_op = operators[applicable_op_id];
                
                // Check if this operator has the same cost
                if (applicable_op.get_cost() != op.get_cost()) {
                    continue;
                }
                
                // Check all hash effects of the applicable operator
                int applicable_hash_effect = applicable_op.get_hash_effect();
                // Compute base predecessor (without comparison axiom evaluation)
                int base_predecessor = successor_state + applicable_hash_effect;
                
                // Enumerate all possible predecessors with evaluated comparison axioms
                // This is the REVERSE of progression: we're checking if applying this operator
                // to current_state leads to successor_state
                vector<int> possible_predecessors = enumerate_states_with_evaluated_comparisons(
                    base_predecessor,
                    applicable_op.get_changed_numeric_vars(),
                    applicable_op.get_source_partitions(),  // In regression: source = predecessor
                    applicable_op.get_target_partitions(),  // In regression: target = current (successor)
                    task_proxy);
                
                // Check if current_state is among the possible predecessors
                if (find(possible_predecessors.begin(), possible_predecessors.end(), current_state) 
                    != possible_predecessors.end()) {
                    // This operator can take us from current_state to successor_state!
                    cheapest_operators.emplace_back(applicable_op.get_concrete_op_id());
                    break; // Only add once per operator
                }
            }
            
           
            if (cheapest_operators.empty()) {
                cout << "DEBUG PLAN: WARNING - No operators found! This will cause an empty plan." << endl;
            }
            
            if (compute_wildcard_plan) {
                rng->shuffle(cheapest_operators);
                wildcard_plan.push_back(move(cheapest_operators));
            } else {
                int random_op_id = *rng->choose(cheapest_operators);
                wildcard_plan.emplace_back();
                wildcard_plan.back().push_back(random_op_id);
            }

            // Print decoded next state
            if (successor_state >= 0 && successor_state < num_states) {
                string decoded = decode_abstract_state(successor_state, domain_sizes,
                                                      numeric_domain_mapping, hash_multipliers);
                cout << "  State after step " << plan_step << ":\n" << decoded << endl;
            }

            current_state = successor_state;
            plan_step++;
        }
        
        cout << "PLAN: Wildcard plan construction complete with " 
             << wildcard_plan.size() << " steps" << endl;
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
    
    // DEBUG: Print goals being checked (only once)
    static bool debug_printed = false;
    if (!debug_printed) {
        debug_printed = true;
        cout << "\n=== is_goal_state DEBUG ===" << endl;
        cout << "Abstract (propositional) goals:" << endl;
        for (const Fact &goal : abstract_goals) {
            cout << "  var" << goal.var << " = " << goal.value << endl;
        }
        cout << "Numeric goal conditions:" << endl;
        for (const auto &ng : numeric_goal_conditions) {
            cout << "  var" << ng.numeric_var_id << " " << ng.op << " " << ng.constant << endl;
        }
        cout << "===================================\n" << endl;
    }
    
    // Check propositional goals
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
    // Check if we have any non-trivial numeric variables (with more than 1 partition)
    bool has_numeric_vars = false;
    for (const auto &num_mapping : numeric_domain_mapping) {
        if (num_mapping->get_ranges().size() > 1) {
            has_numeric_vars = true;
            break;
        }
    }
    
    // Create and populate state registry if we have numeric variables
    unique_ptr<DomainAbstractionStateRegistry> state_registry = nullptr;
    if (has_numeric_vars) {
        state_registry = make_unique<DomainAbstractionStateRegistry>();
        
        // Populate the state registry with all states from the distances vector
        // The state index IS the hash value in this abstraction
        if (VERBOSE_DEBUG) cout << "DEBUG: Populating state registry with " << distances.size() << " states" << endl;
        for (size_t state_idx = 0; state_idx < distances.size(); ++state_idx) {
            // The state_idx is the hash value for this abstract state
            DomainAbstractionState abs_state(state_idx);
            size_t registry_id = state_registry->insert_state(abs_state);
            
            // The registry_id should match state_idx for direct lookup
            if (registry_id != state_idx) {
                cout << "ERROR: Registry ID mismatch! state_idx=" << state_idx 
                     << ", registry_id=" << registry_id << endl;
            }
        }
        if (VERBOSE_DEBUG) cout << "DEBUG: State registry size after population: " << state_registry->size() << endl;
    }
    
    return DomainAbstraction(move(domain_mapping), move(numeric_domain_mapping),
                             move(hash_multipliers), move(distances), move(wildcard_plan),
                             move(state_registry), task_proxy);
}
}
