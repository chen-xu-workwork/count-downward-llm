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
static const bool VERBOSE_DEBUG = true;      // gate noisy, step-by-step diagnostics

// Small helpers shared across functions in this translation unit
struct CompEvalHelper {
    int prop_var_id;   // propositional var id of the comparison axiom
    int true_val;      // concrete value index for TRUE branch
    int false_val;     // concrete value index for FALSE branch
    int eval;          // COMPARISON AXIOM EVAL (normalized): 0=true, 1=false, 2=unknown
};

// Compute numeric operand context (ranges for all numeric variables and the
// current numeric partitions decoded from an abstract state index).
static void compute_numeric_context(
    int state_index,
    const DomainMapping &domain_mapping,
    const NumericDomainMappingType &numeric_domain_mapping,
    const vector<int> &hash_multipliers,
    const TaskProxy &task_proxy,
    unordered_map<int, pair<ap_float, ap_float>> &ranges_out,
    vector<int> &cur_num_partitions_out) {
    ranges_out.clear();
    cur_num_partitions_out.clear();

    // Decode numeric partitions from the abstract state index
    cur_num_partitions_out.reserve(numeric_domain_mapping.size());
    for (size_t num_var_id = 0; num_var_id < numeric_domain_mapping.size(); ++num_var_id) {
        int abstract_var_id = static_cast<int>(domain_mapping.size()) + static_cast<int>(num_var_id);
        int multiplier = hash_multipliers[abstract_var_id];
        int num_parts = numeric_domain_mapping[num_var_id]->get_num_partitions();
        int part = (state_index / multiplier) % num_parts;
        cur_num_partitions_out.push_back(part);
    }

    // Seed ranges for constants and current-partition regulars
    NumericVariablesProxy num_vars = task_proxy.get_numeric_variables();
    for (size_t num_var_id = 0; num_var_id < num_vars.size(); ++num_var_id) {
        NumericVariableProxy var = num_vars[num_var_id];
        if (var.get_var_type() == numType::constant) {
            ap_float val = var.get_initial_state_value();
            ranges_out[num_var_id] = make_pair(val, val);
        } else if (var.get_var_type() == numType::regular && num_var_id < numeric_domain_mapping.size()) {
            const NumericDomainMapping &mapping = *numeric_domain_mapping[num_var_id];
            int part = cur_num_partitions_out[num_var_id];
            const NumericRange *rng = mapping.get_range_for_partition(part);
            if (rng) {
                ranges_out[num_var_id] = make_pair(rng->lower, rng->upper);
            }
        }
        // Derived variables will be computed below
    }

    // Propagate through assignment axioms (fixpoint with safety cap)
    AssignmentAxiomsProxy assignment_axioms = task_proxy.get_assignment_axioms();
    bool changed = true;
    while (changed) {
        changed = false;
        for (AssignmentAxiomProxy axiom : assignment_axioms) {
            int derived_id = axiom.get_assignment_variable().get_id();
            int left_id = axiom.get_left_variable().get_id();
            int right_id = axiom.get_right_variable().get_id();

            // Left operand range
            bool left_known = false;
            ap_float l_lo = -numeric_limits<ap_float>::infinity();
            ap_float l_hi = numeric_limits<ap_float>::infinity();
            if (axiom.get_left_variable().get_var_type() == numType::constant) {
                ap_float val = axiom.get_left_variable().get_initial_state_value();
                l_lo = l_hi = val;
                left_known = true;
            } else if (ranges_out.count(left_id)) {
                l_lo = ranges_out[left_id].first;
                l_hi = ranges_out[left_id].second;
                left_known = true;
            }


            // Right operand range
            bool right_known = false;
            ap_float r_lo = -numeric_limits<ap_float>::infinity();
            ap_float r_hi = numeric_limits<ap_float>::infinity();
            if (axiom.get_right_variable().get_var_type() == numType::constant) {
                ap_float val = axiom.get_right_variable().get_initial_state_value();
                r_lo = r_hi = val;
                right_known = true;
            } else if (ranges_out.count(right_id)) {
                r_lo = ranges_out[right_id].first;
                r_hi = ranges_out[right_id].second;
                right_known = true;
            }
            assert(left_known && right_known);


            if (left_known && right_known) {
                pair<ap_float, ap_float> res = NumericDomainMapping::apply_range_operation(
                    l_lo, l_hi, r_lo, r_hi, axiom.get_arithmetic_operator_type());
                auto it = ranges_out.find(derived_id);
                if (it == ranges_out.end() || it->second.first != res.first || it->second.second != res.second) {
                    ranges_out[derived_id] = res;
                    changed = true;
                }
            }
        }
    }
}

// Evaluate all comparison axioms against the current numeric ranges/partitions.
static vector<CompEvalHelper> evaluate_all_comparisons(
    const unordered_map<int, pair<ap_float, ap_float>> &ranges,
    const vector<int> &cur_num_partitions,
    const NumericDomainMappingType &numeric_domain_mapping,
    const TaskProxy &task_proxy) {
    vector<CompEvalHelper> out;
    ComparisonAxiomsProxy comp_axioms = task_proxy.get_comparison_axioms();
    out.reserve(comp_axioms.size());

    for (ComparisonAxiomProxy axiom : comp_axioms) {
        int left_id = axiom.get_left_variable().get_id();
        int right_id = axiom.get_right_variable().get_id();

        // Left range
        ap_float l_lo = -numeric_limits<ap_float>::infinity();
        ap_float l_hi = numeric_limits<ap_float>::infinity();
        bool left_known = false;
        if (axiom.get_left_variable().get_var_type() == numType::constant) {
            ap_float val = axiom.get_left_variable().get_initial_state_value();
            l_lo = l_hi = val;
            left_known = true;
        } else if (ranges.count(left_id)) {
            l_lo = ranges.at(left_id).first;
            l_hi = ranges.at(left_id).second;
            left_known = true;
        } else if (left_id >= 0 && left_id < static_cast<int>(numeric_domain_mapping.size())) {
            const NumericDomainMapping &m = *numeric_domain_mapping[left_id];
            int part = cur_num_partitions[left_id];
            const NumericRange *rng = m.get_range_for_partition(part);
            if (rng) { l_lo = rng->lower; l_hi = rng->upper; left_known = true; }
        }

        // Right range
        ap_float r_lo = -numeric_limits<ap_float>::infinity();
        ap_float r_hi = numeric_limits<ap_float>::infinity();
        bool right_known = false;
        if (axiom.get_right_variable().get_var_type() == numType::constant) {
            ap_float val = axiom.get_right_variable().get_initial_state_value();
            r_lo = r_hi = val;
            right_known = true;
        } else if (ranges.count(right_id)) {
            r_lo = ranges.at(right_id).first;
            r_hi = ranges.at(right_id).second;
            right_known = true;
        } else if (right_id >= 0 && right_id < static_cast<int>(numeric_domain_mapping.size())) {
            const NumericDomainMapping &m = *numeric_domain_mapping[right_id];
            int part = cur_num_partitions[right_id];
            const NumericRange *rng = m.get_range_for_partition(part);
            if (rng) { r_lo = rng->lower; r_hi = rng->upper; right_known = true; }
        }

        int eval = 2;
        if (left_known && right_known) {
            // Raw evaluation from NumericDomainMapping uses: 0=false, 1=true, 2=unknown.
            // Normalize here to the comparison-axiom convention: 0=true, 1=false, 2=unknown.
            int raw = NumericDomainMapping::evaluate_comparison(
                axiom.get_comparison_operator_type(), l_lo, l_hi, r_lo, r_hi);
            if (raw == 2) {
                eval = 2;
            } else if (raw == 0) {
                eval = 0; // true -> 0
            } else {
                eval = 1; // false -> 1
            }
        }

        out.push_back(CompEvalHelper{
            axiom.get_true_fact().get_variable().get_id(),
            axiom.get_true_fact().get_value(),
            axiom.get_false_fact().get_value(),
            eval
        });
    }
    return out;
}

// Reset all comparison-axiom variables in the given abstract state to UNKNOWN (value index 2).
// Returns the adjusted abstract state index.
static int reset_all_comparison_vars_to_unknown(
    int state_index,
    const DomainMapping &domain_mapping,
    const vector<int> &hash_multipliers,
    const TaskProxy &task_proxy) {
    int delta = 0;
    unordered_set<int> seen;
    ComparisonAxiomsProxy comp_axioms = task_proxy.get_comparison_axioms();
    for (ComparisonAxiomProxy ax : comp_axioms) {
        int var_id = ax.get_true_fact().get_variable().get_id();
        if (!seen.insert(var_id).second)
            continue;
        if (var_id < 0 || var_id >= static_cast<int>(hash_multipliers.size()))
            continue;
        // Treat empty mapping as trivial; skip.
        if (var_id >= static_cast<int>(domain_mapping.size()) || domain_mapping[var_id].empty())
            continue;

        int multiplier = hash_multipliers[var_id];
        // Determine abstract domain size (1 + max mapped value) guarding empties
        int abstract_size = 1;
        for (int mapped : domain_mapping[var_id]) abstract_size = max(abstract_size, mapped + 1);
        int cur_val = (state_index / multiplier) % abstract_size;
        int unknown_abs = domain_mapping[var_id][2];
        delta += (unknown_abs - cur_val) * multiplier;
    }
    return state_index + delta;
}

// Check if a goal abstract state is numerically feasible w.r.t. comparison axioms.
static bool is_state_goal_feasible(
    int state_index,
    const vector<Fact> &abstract_goals,
    const DomainMapping &domain_mapping,
    const NumericDomainMappingType &numeric_domain_mapping,
    const vector<int> &hash_multipliers,
    const TaskProxy &task_proxy) {
    // Build numeric context
    unordered_map<int, pair<ap_float, ap_float>> ranges;
    vector<int> cur_parts;
    compute_numeric_context(state_index, domain_mapping, numeric_domain_mapping,
                            hash_multipliers, task_proxy, ranges, cur_parts);

    // Evaluate all comparisons
    vector<CompEvalHelper> comps = evaluate_all_comparisons(ranges, cur_parts, numeric_domain_mapping, task_proxy);

    // Build a quick lookup: comp_var_id -> (abs_true, abs_false, eval)
    struct Eval { int abs_true; int abs_false; int eval; };
    unordered_map<int, Eval> comp_map;
    comp_map.reserve(comps.size());
    for (const auto &c : comps) {
        int var_id = c.prop_var_id;
        int abs_true = (var_id < static_cast<int>(domain_mapping.size()) && !domain_mapping[var_id].empty())
                           ? domain_mapping[var_id][c.true_val]
                           : c.true_val;
        int abs_false = (var_id < static_cast<int>(domain_mapping.size()) && !domain_mapping[var_id].empty())
                            ? domain_mapping[var_id][c.false_val]
                            : c.false_val;
        comp_map[var_id] = Eval{abs_true, abs_false, c.eval};
    }

    // For each abstract goal fact that references a comparison variable, ensure non-contradiction
    for (const Fact &g : abstract_goals) {
        auto it = comp_map.find(g.var);
        if (it == comp_map.end())
            continue; // not a comparison goal
        const Eval &e = it->second;
        // Only treat as contradiction if the abstract mapping distinguishes TRUE and FALSE.
        // If abs_true == abs_false (merged), the goal doesn't select a specific branch → don't reject.
        bool distinguishes = (e.abs_true != e.abs_false);
        if (distinguishes) {
            // Optimism for coarse partitions: if operands aren't singletons (point ranges),
            // treat even a definitive eval (0/1) as potentially relaxable at this abstraction
            // level and do not reject. Only reject if both operands are point ranges that
            // make the comparison deterministically contradict the chosen branch.

            // Find the comparison axiom for this goal variable to extract operand ranges.
            ap_float l_lo = -numeric_limits<ap_float>::infinity();
            ap_float l_hi = numeric_limits<ap_float>::infinity();
            ap_float r_lo = -numeric_limits<ap_float>::infinity();
            ap_float r_hi = numeric_limits<ap_float>::infinity();
            bool left_known = false;
            bool right_known = false;

            for (ComparisonAxiomProxy axiom : task_proxy.get_comparison_axioms()) {
                if (axiom.get_true_fact().get_variable().get_id() != g.var)
                    continue;
                int left_id = axiom.get_left_variable().get_id();
                int right_id = axiom.get_right_variable().get_id();

                // Left range
                if (axiom.get_left_variable().get_var_type() == numType::constant) {
                    ap_float val = axiom.get_left_variable().get_initial_state_value();
                    l_lo = l_hi = val;
                    left_known = true;
                } else if (ranges.count(left_id)) {
                    l_lo = ranges[left_id].first;
                    l_hi = ranges[left_id].second;
                    left_known = true;
                } else if (left_id >= 0 && left_id < static_cast<int>(numeric_domain_mapping.size())) {
                    const NumericDomainMapping &m = *numeric_domain_mapping[left_id];
                    int part = cur_parts[left_id];
                    const NumericRange *rng = m.get_range_for_partition(part);
                    if (rng) { l_lo = rng->lower; l_hi = rng->upper; left_known = true; }
                }

                // Right range
                if (axiom.get_right_variable().get_var_type() == numType::constant) {
                    ap_float val = axiom.get_right_variable().get_initial_state_value();
                    r_lo = r_hi = val;
                    right_known = true;
                } else if (ranges.count(right_id)) {
                    r_lo = ranges[right_id].first;
                    r_hi = ranges[right_id].second;
                    right_known = true;
                } else if (right_id >= 0 && right_id < static_cast<int>(numeric_domain_mapping.size())) {
                    const NumericDomainMapping &m = *numeric_domain_mapping[right_id];
                    int part = cur_parts[right_id];
                    const NumericRange *rng = m.get_range_for_partition(part);
                    if (rng) { r_lo = rng->lower; r_hi = rng->upper; right_known = true; }
                }
                break; // Found the matching axiom
            }

            bool operands_singletons = false;
            if (left_known && right_known) {
                bool left_point = (l_lo == l_hi);
                bool right_point = (r_lo == r_hi);
                operands_singletons = left_point && right_point;
            }

            // If definitive contradiction but operands are not both singletons, be optimistic and accept.
            // Note: normalized mapping (0=true, 1=false):
            //  - Goal wants TRUE (g == abs_true) but eval says FALSE (1)  => contradiction
            //  - Goal wants FALSE (g == abs_false) but eval says TRUE (0) => contradiction
            if (g.value == e.abs_true && e.eval == 1) {
                if (!operands_singletons) {
                    continue; // accept optimistically at this abstraction level
                } else {
                    return false; // hard contradiction on point ranges
                }
            }
            if (g.value == e.abs_false && e.eval == 0) {
                if (!operands_singletons) {
                    continue; // accept optimistically
                } else {
                    return false; // hard contradiction on point ranges
                }
            }
        }
        // If the goal maps to a merged/other abstract value, we can't conclude contradiction → accept
        // If eval is UNKNOWN, still potentially feasible → accept
    }
    return true;
}

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
    const TaskProxy &task_proxy) const {
    
    vector<int> result;
    // Build numeric context and evaluate all comparisons
    unordered_map<int, pair<ap_float, ap_float>> ranges;
    vector<int> cur_num_partitions;
    compute_numeric_context(base_state_index, domain_mapping, numeric_domain_mapping,
                            hash_multipliers, task_proxy, ranges, cur_num_partitions);
    vector<CompEvalHelper> comparisons = evaluate_all_comparisons(
        ranges, cur_num_partitions, numeric_domain_mapping, task_proxy);

    //for (const auto& r : ranges) {
    //    cout << "DEBUG ENUM: var" << r.first 
    //         << " range=[" << r.second.first << "," << r.second.second << "]" << endl;
    //}   

    // Reset ALL comparison axiom variables to UNKNOWN using shared helper
    int state_with_unknowns = reset_all_comparison_vars_to_unknown(
        base_state_index, domain_mapping, hash_multipliers, task_proxy);

    // Enumerate all possible combinations of comparison results.
    // For deterministic evaluations (TRUE/FALSE), fix the value.
    // For UNKNOWN evaluations, branch on both TRUE and FALSE.
    function<void(size_t, int)> enumerate_combinations = 
        [&](size_t idx, int delta_from_unknown) {
        if (idx == comparisons.size()) {
            result.push_back(state_with_unknowns + delta_from_unknown);
            return;
        }
        
        const CompEvalHelper &comp = comparisons[idx];
        int var_id = comp.prop_var_id;
        
        if (var_id >= static_cast<int>(hash_multipliers.size()) || variable_is_trivial(var_id)) {
            enumerate_combinations(idx + 1, delta_from_unknown);
            return;
        }
        
        int multiplier = hash_multipliers[var_id];
        int unknown_value = domain_mapping[var_id][2];
        
        if (comp.eval == 0) {
            // Definitely TRUE (normalized mapping)
            int delta = (domain_mapping[var_id][comp.true_val] - unknown_value) * multiplier;
            enumerate_combinations(idx + 1, delta_from_unknown + delta);
        } else if (comp.eval == 1) {
            // Definitely FALSE (normalized mapping)
            int delta = (domain_mapping[var_id][comp.false_val] - unknown_value) * multiplier;
            enumerate_combinations(idx + 1, delta_from_unknown + delta);
        } else {
            // UNKNOWN: branch on both possibilities
            int delta_true = (domain_mapping[var_id][comp.true_val] - unknown_value) * multiplier;
            int delta_false = (domain_mapping[var_id][comp.false_val] - unknown_value) * multiplier;
            enumerate_combinations(idx + 1, delta_from_unknown + delta_true);
            enumerate_combinations(idx + 1, delta_from_unknown + delta_false);
        }
    };

    enumerate_combinations(0, 0);
    
    // Ensure we return at least one state
    if (result.empty()) {
        result.push_back(state_with_unknowns);
    }
    
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
        if (domain_sizes[var_id] == 1) {
            continue;
        }
        int multiplier = hash_multipliers[var_id];
        int value = (remaining / multiplier) % domain_sizes[var_id];
        ss << "v" << var_id << "=" << value;
        if (var_id < domain_sizes.size() - 1 || !numeric_domain_mapping.empty()) {
            ss << ", ";
        }
    }
    
    // Decode numeric variables (partitions)
    for (size_t num_var_id = 0; num_var_id < numeric_domain_mapping.size(); ++num_var_id) {
        if (numeric_domain_mapping[num_var_id]->get_num_partitions() <= 1) {
            continue;
        }
        int multiplier_idx = domain_sizes.size() + num_var_id;
        int multiplier = hash_multipliers[multiplier_idx];
        int num_partitions = numeric_domain_mapping[num_var_id]->get_num_partitions();
        int partition = (remaining / multiplier) % num_partitions;
        const NumericRange *rng = numeric_domain_mapping[num_var_id]->get_range_for_partition(partition);
        if (rng) {
            ap_float lower = rng->lower;
            ap_float upper = rng->upper;
            bool lower_incl = rng->lower_inclusive;
            bool upper_incl = rng->upper_inclusive;
            string lower_str = lower_incl ? "[" : "(";
            string upper_str = upper_incl ? "]" : ")";
            ss << "num" << num_var_id << "=" << lower_str << lower << "," << upper << upper_str;
        } else {
            ss << "num" << num_var_id << "=INVALID";
        }
        
        //ss << "num" << num_var_id << "=p" << partition;
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
    int first_goal_state = -1;
    for (int state_index = 0; state_index < num_states; ++state_index) {
        bool is_goal = is_goal_state(state_index, abstract_goals, domain_sizes);
        if (is_goal) {
            // Filter out impossible goal states whose comparison-axiom goals
            // contradict the numeric partitions of this state.
            // THAT ONE IS APPARENTLY BROKEN!
            bool feasible = is_state_goal_feasible(state_index,
                                                   abstract_goals,
                                                   domain_mapping,
                                                   numeric_domain_mapping,
                                                   hash_multipliers,
                                                   task_proxy);
            vector<int> possible_states = 
                enumerate_states_with_evaluated_comparisons(
                    state_index,
                    task_proxy);
            if (find(possible_states.begin(),
                    possible_states.end(),
                    state_index) != possible_states.end()) {
                feasible = true;
                        } else {    
                feasible = false;
            }
            if (feasible) {
                cout << "DEBUG: Found feasible goal state: " << decode_abstract_state(state_index, domain_sizes,
                                          numeric_domain_mapping, hash_multipliers);
                pq.push(0, state_index);
                distances.push_back(0);
            } else {
                distances.push_back(numeric_limits<int>::max());
            }
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

        vector<int> comparison_alternative_states = 
            enumerate_states_with_evaluated_comparisons(
                state_index,
                task_proxy);

        if (find(comparison_alternative_states.begin(),
                    comparison_alternative_states.end(),
                    state_index) == comparison_alternative_states.end()) {
            cout << "DEBUG ERROR: State index " << state_index
                    << " not found in its own enumerated comparison alternatives!" << endl;
            cout << "  Enumerated alternatives: ";
            for (int s : comparison_alternative_states) {
                cout << s << " ";
            }
            cout << endl;
            cout << "  Decoded state: " 
                 << decode_abstract_state(state_index, domain_sizes, 
                                          numeric_domain_mapping, hash_multipliers)
                 << endl;
            enumerate_states_with_evaluated_comparisons(
                state_index,
                task_proxy);
        }

        assert(find(comparison_alternative_states.begin(),
                    comparison_alternative_states.end(),
                    state_index) != comparison_alternative_states.end());
 
        // Regress using abstract operators (from match tree)
        // These handle both propositional-only and numeric operators
        vector<int> applicable_operator_ids;
        match_tree.get_applicable_operator_ids(state_index, applicable_operator_ids);
        
        int valid_predecessors_this_state = 0;
        int out_of_bounds_predecessors = 0;
        int operators_checked = 0;
        for (int op_id : applicable_operator_ids) {
            const AbstractOperator &op = operators[op_id];
            int alternative_cost = distances[state_index] + op.get_cost();
            
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
            
            // Iterate over all possible hash effects (predecessors)
            // Propositional operators have 1 effect, numeric operators have multiple
            const int base_hash_effect = op.get_hash_effect();
            int predecessor_base = state_index + base_hash_effect;
            assert(predecessor_base < num_states && 0 <= predecessor_base);

            int predecessors_this_op = 0;
            
            // Enumerate all possible predecessors considering comparison axiom cascades
            vector<int> possible_predecessors = enumerate_states_with_evaluated_comparisons(
                predecessor_base,
                task_proxy);

            if (state_index == 1 and is_pour) {
                cout << "DEBUG: Dijkstra iteration " << dijkstra_iterations 
                     << ", state_index " << state_index 
                     << ", op_id " << op_id 
                     << " (pour agent1 plant1)" << endl;
                cout << "  base_hash_effect: " << base_hash_effect << endl;
                cout << "  predecessor_base: " << predecessor_base << endl;
                cout << "  possible_predecessors: ";
                for (int pred : possible_predecessors) {
                    cout << pred << " ";
                }
                cout << endl;

            }
            

            // DEBUG CHECK 2: Validate a few enumerated predecessors
            
            for (int predecessor : possible_predecessors) {
                
                assert(0 <= predecessor && predecessor < num_states);
                if (predecessor < 0 || predecessor >= num_states) {
                    if (dijkstra_iterations == 1) {
                        out_of_bounds_predecessors++;
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
                    
                    distances[predecessor] = alternative_cost;
                    pq.push(alternative_cost, predecessor);
                    if (compute_plan) {
                        generating_op_ids[predecessor] = op_id;
                        if (predecessor == 2) {
                            cout << "DEBUG: Setting generating op for state 2 to op_id " << op_id << endl;
                            cout << "DEBUG:   which has hash effect: " << op.get_hash_effect() << endl;
                            cout << "DEBUG:    and successor state is: " << state_index << endl;
                        }
                    }
                }
            }
            operators_checked++;
            
        }

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
                task_proxy);
            
            // Find a valid successor with lower distance
            for (int candidate_successor : possible_successors) {
                // Check if this successor is valid (was reached during Dijkstra)
                assert(candidate_successor >= 0 && candidate_successor < static_cast<int>(distances.size()));
                if (distances[candidate_successor] != numeric_limits<int>::max() &&
                    distances[candidate_successor] < distances[current_state]) {
                    // Valid successor with lower distance - use it!
                    hash_effect = candidate_hash_effect;
                    successor_state = candidate_successor;
                    break;
                }
            }

            if (successor_state == -1) {
                cout << "PLAN: No valid successor from state " << current_state
                     << " with lower distance; aborting plan extraction." << endl;
                utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
                break;
            }

            // Report this plan step: operator and abstract effects
            {
                OperatorsProxy concrete_ops = task_proxy.get_operators();
                int concrete_id = op.get_concrete_op_id();
                string op_name = (concrete_id >= 0 && concrete_id < (int)concrete_ops.size()) ?
                                  concrete_ops[concrete_id].get_name() : ("<unknown>(" + to_string(concrete_id) + ")");

                string decoded_state = decode_abstract_state(current_state, domain_sizes,
                                                      numeric_domain_mapping, hash_multipliers);
                cout << "[ABSTRACT PLAN] " << decoded_state << ", " << op_name << endl;
              
                /*
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
                */
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
                cout << "PLAN: No equivalent operators found from state " << current_state
                     << " to " << successor_state << "; aborting plan extraction." << endl;
                utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
            }
            
            if (compute_wildcard_plan) {
                rng->shuffle(cheapest_operators);
                wildcard_plan.push_back(move(cheapest_operators));
            } else {
                int random_op_id = *rng->choose(cheapest_operators);
                wildcard_plan.emplace_back();
                wildcard_plan.back().push_back(random_op_id);
            }

            string decoded = decode_abstract_state(successor_state, domain_sizes,
                                                      numeric_domain_mapping, hash_multipliers);

            //cout << "[ABSTRACT PLAN] " << decoded << endl;                              

      

            current_state = successor_state;
            plan_step++;

        }
        string decoded = decode_abstract_state(current_state, domain_sizes,
                                              numeric_domain_mapping, hash_multipliers);
        cout << "[ABSTRACT PLAN] " << decoded << endl;  
        
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
