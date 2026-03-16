#include "domain_abstraction.h"

#include "types.h"

#include "../task_proxy.h"
#include "../tasks/root_task.h"

#include "../priority_queue.h"
#include "../domain_abstractions/domain_abstraction.h"
#include "../domain_abstractions/match_tree_with_pattern.h"
#include "../domain_abstractions/domain_abstraction_factory.h"
#include "../domain_abstractions/numeric_helper.h"
#include "../task_tools.h"
#include "../utils/collections.h"
#include "../utils/logging.h"
#include "../utils/math.h"
#include "../utils/memory.h"

#include <cassert>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <tuple>

using namespace std;

namespace {

// Cached assignment axiom data to avoid repeated proxy calls per state.
struct CachedAssignmentAxiom {
    int derived_id;  // get_assignment_variable().get_id()
    int left_id;     // get_left_variable().get_id()
    int right_id;    // get_right_variable().get_id()
    numType left_type;  // get_left_variable().get_var_type()
    numType right_type; // get_right_variable().get_var_type()
    ap_float left_const_val;  // get_left_variable().get_initial_state_value() if constant
    ap_float right_const_val; // get_right_variable().get_initial_state_value() if constant
    cal_operator op_type;     // get_arithmetic_operator_type()
};

// Cached comparison axiom data to avoid repeated proxy calls per state.
struct CachedComparisonAxiom {
    int left_id;      // get_left_variable().get_id()
    int right_id;     // get_right_variable().get_id()
    numType left_type;   // get_left_variable().get_var_type()
    numType right_type;  // get_right_variable().get_var_type()
    ap_float left_const_val;
    ap_float right_const_val;
    comp_operator op_type;  // get_comparison_operator_type()
    int prop_var_id;  // get_true_fact().get_variable().get_id()
    int true_val;     // get_true_fact().get_value()
    int false_val;    // get_false_fact().get_value()
};

// Build cached axiom data on first use (thread-local for safety).
// Use cached_num_ass_axioms and cached_num_cmp_axioms as validity indicators.
thread_local vector<CachedAssignmentAxiom> g_cached_assignment_axioms;
thread_local vector<CachedComparisonAxiom> g_cached_comparison_axioms;
thread_local size_t g_cached_num_ass_axioms = static_cast<size_t>(-1);
thread_local size_t g_cached_num_cmp_axioms = static_cast<size_t>(-1);

// Thread-local reusable vectors for compute_numeric_context to avoid hash overhead.
// Using dense vector + validity flags instead of unordered_map for O(1) indexed access.
thread_local vector<domain_abstractions::NumericRange> g_ranges_vec;
thread_local vector<bool> g_ranges_valid;
thread_local vector<int> g_cur_num_partitions;

void ensure_axiom_cache(const TaskProxy &task_proxy) {
    size_t num_ass = task_proxy.get_assignment_axioms().size();
    size_t num_cmp = task_proxy.get_comparison_axioms().size();
    
    // Fast check: if counts match, assume same task (safe for single-task runs).
    if (num_ass == g_cached_num_ass_axioms && num_cmp == g_cached_num_cmp_axioms) {
        return;  // Already cached.
    }
    g_cached_num_ass_axioms = num_ass;
    g_cached_num_cmp_axioms = num_cmp;
    
    // Cache assignment axioms.
    AssignmentAxiomsProxy ass_axioms = task_proxy.get_assignment_axioms();
    g_cached_assignment_axioms.clear();
    g_cached_assignment_axioms.reserve(ass_axioms.size());
    for (AssignmentAxiomProxy ax : ass_axioms) {
        CachedAssignmentAxiom cached;
        cached.derived_id = ax.get_assignment_variable().get_id();
        cached.left_id = ax.get_left_variable().get_id();
        cached.right_id = ax.get_right_variable().get_id();
        cached.left_type = ax.get_left_variable().get_var_type();
        cached.right_type = ax.get_right_variable().get_var_type();
        cached.left_const_val = (cached.left_type == numType::constant) 
            ? ax.get_left_variable().get_initial_state_value() : 0.0;
        cached.right_const_val = (cached.right_type == numType::constant)
            ? ax.get_right_variable().get_initial_state_value() : 0.0;
        cached.op_type = ax.get_arithmetic_operator_type();
        g_cached_assignment_axioms.push_back(cached);
    }
    
    // Cache comparison axioms.
    ComparisonAxiomsProxy comp_axioms = task_proxy.get_comparison_axioms();
    g_cached_comparison_axioms.clear();
    g_cached_comparison_axioms.reserve(comp_axioms.size());
    for (ComparisonAxiomProxy ax : comp_axioms) {
        CachedComparisonAxiom cached;
        cached.left_id = ax.get_left_variable().get_id();
        cached.right_id = ax.get_right_variable().get_id();
        cached.left_type = ax.get_left_variable().get_var_type();
        cached.right_type = ax.get_right_variable().get_var_type();
        cached.left_const_val = (cached.left_type == numType::constant)
            ? ax.get_left_variable().get_initial_state_value() : 0.0;
        cached.right_const_val = (cached.right_type == numType::constant)
            ? ax.get_right_variable().get_initial_state_value() : 0.0;
        cached.op_type = ax.get_comparison_operator_type();
        cached.prop_var_id = ax.get_true_fact().get_variable().get_id();
        cached.true_val = ax.get_true_fact().get_value();
        cached.false_val = ax.get_false_fact().get_value();
        g_cached_comparison_axioms.push_back(cached);
    }
}

struct CompEvalHelper {
    int prop_var_id;   // propositional var id of the comparison axiom
    int true_val;      // concrete value index for TRUE branch
    int false_val;     // concrete value index for FALSE branch
    int eval;          // COMPARISON AXIOM EVAL (normalized): 0=true, 1=false, 2=unknown
};

void compute_numeric_context(
    int state_index,
    const domain_abstractions::DomainMapping &domain_mapping,
    const domain_abstractions::NumericDomainMappings &numeric_domain_mapping,
    const vector<int> &hash_multipliers,
    const vector<int> &pattern_domain_sizes,
    const pdbs::Pattern &pattern,
    const TaskProxy &task_proxy,
    vector<domain_abstractions::NumericRange> &ranges_out,
    vector<bool> &ranges_valid_out,
    vector<int> &cur_num_partitions_out) {
    
    // Resize vectors if needed (usually no-op after first call).
    size_t num_numeric_vars = task_proxy.get_numeric_variables().size();
    if (ranges_out.size() < num_numeric_vars) {
        ranges_out.resize(num_numeric_vars);
        ranges_valid_out.resize(num_numeric_vars);
    }
    // Reset validity flags - O(n) but cache-friendly and avoids hash overhead.
    fill(ranges_valid_out.begin(), ranges_valid_out.begin() + num_numeric_vars, false);
    cur_num_partitions_out.clear();

    // Initialize cur_num_partitions_out with -1 for all numeric variables
    // (meaning "not in pattern")
    cur_num_partitions_out.resize(numeric_domain_mapping.size(), -1);
    
    int num_prop_vars = static_cast<int>(domain_mapping.size());
    
    // Iterate over pattern positions, only processing numeric variables in the pattern
    for (size_t pattern_idx = 0; pattern_idx < pattern.size(); ++pattern_idx) {
        int var_id = pattern[pattern_idx];
        // Check if this is a numeric variable (var_id >= num_prop_vars)
        if (var_id >= num_prop_vars) {
            int num_var_id = var_id - num_prop_vars;
            int multiplier = hash_multipliers[pattern_idx];
            int num_parts = pattern_domain_sizes[pattern_idx];
            int part = (state_index / multiplier) % num_parts;
            cur_num_partitions_out[num_var_id] = part;
        }
    }

    NumericVariablesProxy num_vars = task_proxy.get_numeric_variables();
    for (size_t num_var_id = 0; num_var_id < num_vars.size(); ++num_var_id) {
        NumericVariableProxy var = num_vars[num_var_id];
        if (var.get_var_type() == numType::constant) {
            ap_float val = var.get_initial_state_value();
            ranges_out[num_var_id] = domain_abstractions::NumericRange(val, val, true, true);
            ranges_valid_out[num_var_id] = true;
        } else if (var.get_var_type() == numType::regular && num_var_id < numeric_domain_mapping.size()) {
            int part = cur_num_partitions_out[num_var_id];
            // part == -1 means this numeric variable is not in the pattern
            if (part >= 0) {
                const domain_abstractions::NumericDomainMapping &mapping = *numeric_domain_mapping[num_var_id];
                const domain_abstractions::NumericRange *rng = mapping.get_range_for_partition(part);
                if (rng) {
                    ranges_out[num_var_id] = *rng;
                    ranges_valid_out[num_var_id] = true;
                }
            }
        }
    }

    // Use cached assignment axiom data instead of proxy calls on each iteration.
    ensure_axiom_cache(task_proxy);
    bool changed = true;
    while (changed) {
        changed = false;
        for (const CachedAssignmentAxiom &axiom : g_cached_assignment_axioms) {
            bool left_known = false;
            domain_abstractions::NumericRange l_range;
            if (axiom.left_type == numType::constant) {
                l_range = domain_abstractions::NumericRange(axiom.left_const_val, axiom.left_const_val, true, true);
                left_known = true;
            } else if (axiom.left_id >= 0 && axiom.left_id < static_cast<int>(num_numeric_vars) && ranges_valid_out[axiom.left_id]) {
                l_range = ranges_out[axiom.left_id];
                left_known = true;
            }

            bool right_known = false;
            domain_abstractions::NumericRange r_range;
            if (axiom.right_type == numType::constant) {
                r_range = domain_abstractions::NumericRange(axiom.right_const_val, axiom.right_const_val, true, true);
                right_known = true;
            } else if (axiom.right_id >= 0 && axiom.right_id < static_cast<int>(num_numeric_vars) && ranges_valid_out[axiom.right_id]) {
                r_range = ranges_out[axiom.right_id];
                right_known = true;
            }
            
            if (left_known && right_known) {
                domain_abstractions::NumericRange res = domain_abstractions::NumericDomainMapping::apply_range_operation(
                    l_range, r_range, axiom.op_type);
                // Ensure derived_id is in bounds.
                if (axiom.derived_id >= 0 && axiom.derived_id < static_cast<int>(num_numeric_vars)) {
                    bool needs_update = !ranges_valid_out[axiom.derived_id] ||
                        ranges_out[axiom.derived_id].lower != res.lower || 
                        ranges_out[axiom.derived_id].upper != res.upper ||
                        ranges_out[axiom.derived_id].lower_inclusive != res.lower_inclusive || 
                        ranges_out[axiom.derived_id].upper_inclusive != res.upper_inclusive;
                    if (needs_update) {
                        ranges_out[axiom.derived_id] = res;
                        ranges_valid_out[axiom.derived_id] = true;
                        changed = true;
                    }
                }
            }
        }
    }
}

vector<CompEvalHelper> evaluate_all_comparisons(
    const vector<domain_abstractions::NumericRange> &ranges,
    const vector<bool> &ranges_valid,
    const vector<int> &cur_num_partitions,
    const domain_abstractions::NumericDomainMappings &numeric_domain_mapping,
    const TaskProxy &task_proxy) {
    
    // Use cached comparison axiom data instead of proxy calls on each iteration.
    ensure_axiom_cache(task_proxy);
    
    vector<CompEvalHelper> out;
    out.reserve(g_cached_comparison_axioms.size());
    
    size_t ranges_size = ranges.size();

    for (const CachedComparisonAxiom &axiom : g_cached_comparison_axioms) {
        domain_abstractions::NumericRange l_range;
        bool left_known = false;
        if (axiom.left_type == numType::constant) {
            l_range = domain_abstractions::NumericRange(axiom.left_const_val, axiom.left_const_val, true, true);
            left_known = true;
        } else if (axiom.left_id >= 0 && axiom.left_id < static_cast<int>(ranges_size) && ranges_valid[axiom.left_id]) {
            l_range = ranges[axiom.left_id];
            left_known = true;
        } else if (axiom.left_id >= 0 && axiom.left_id < static_cast<int>(numeric_domain_mapping.size())) {
            const domain_abstractions::NumericDomainMapping &m = *numeric_domain_mapping[axiom.left_id];
            int part = cur_num_partitions[axiom.left_id];
            const domain_abstractions::NumericRange *rng = m.get_range_for_partition(part);
            if (rng) { l_range = *rng; left_known = true; }
        }

        domain_abstractions::NumericRange r_range;
        bool right_known = false;
        if (axiom.right_type == numType::constant) {
            r_range = domain_abstractions::NumericRange(axiom.right_const_val, axiom.right_const_val, true, true);
            right_known = true;
        } else if (axiom.right_id >= 0 && axiom.right_id < static_cast<int>(ranges_size) && ranges_valid[axiom.right_id]) {
            r_range = ranges[axiom.right_id];
            right_known = true;
        } else if (axiom.right_id >= 0 && axiom.right_id < static_cast<int>(numeric_domain_mapping.size())) {
            const domain_abstractions::NumericDomainMapping &m = *numeric_domain_mapping[axiom.right_id];
            int part = cur_num_partitions[axiom.right_id];
            const domain_abstractions::NumericRange *rng = m.get_range_for_partition(part);
            if (rng) { r_range = *rng; right_known = true; }
        }

        if (left_known && right_known) {
            int eval = 2;
            int raw = domain_abstractions::NumericDomainMapping::evaluate_comparison(
                axiom.op_type, l_range, r_range);
            if (raw == 2) {
                eval = 2;
            } else if (raw == 0) {
                eval = 0; 
            } else {
                eval = 1; 
            }

            out.push_back(CompEvalHelper{axiom.prop_var_id, axiom.true_val, axiom.false_val, eval});
        } else {
            out.push_back(CompEvalHelper{axiom.prop_var_id, axiom.true_val, axiom.false_val, 2});
        }
    }
    return out;
}
}

std::ostream &operator<<(std::ostream &os, const Fact &fact) {
    return os << fact.var << "=" << fact.value;
}

namespace cost_saturation {

// Reset all comparison-axiom variables in the given abstract state to UNKNOWN (value index 2),
// EXCEPT for variables that appear in fixed_comparisons - those keep their specified values.
// Returns the adjusted abstract state index.
// NOTE: This function relies on the axiom cache being initialized. Call ensure_axiom_cache() first
// or use enumerate_states_with_evaluated_comparisons which does so.
int reset_comparison_vars_to_unknown_except(
    int state_index,
    const domain_abstractions::DomainMapping &domain_mapping,
    const vector<int> &hash_multipliers_by_var_id,
    const TaskProxy &task_proxy,
    const vector<Fact> &fixed_comparisons) {
    
    // Build set of fixed variable IDs for quick lookup - use linear search for small sets
    auto is_fixed = [&fixed_comparisons](int var_id) {
        for (const Fact &f : fixed_comparisons) {
            if (f.var == var_id) return true;
        }
        return false;
    };
    
    // Ensure the axiom cache is populated.
    ensure_axiom_cache(task_proxy);
    
    int delta = 0;
    for (const CachedComparisonAxiom &ax : g_cached_comparison_axioms) {
        int var_id = ax.prop_var_id;
        if (domain_mapping[var_id].empty())
            continue;

        // Skip variables that are fixed (e.g., operator preconditions during regression)
        if (is_fixed(var_id))
            continue;

        // Use hash_multipliers_by_var_id since var_id is an original variable ID, not pattern index
        int multiplier = hash_multipliers_by_var_id[var_id];
        if (multiplier == 0) continue;  // Variable not in pattern
        int abstract_size = 1;
        for (int mapped : domain_mapping[var_id]) abstract_size = max(abstract_size, mapped + 1);
        int cur_val = (state_index / multiplier) % abstract_size;
        int unknown_abs = domain_mapping[var_id][2];
        delta += (unknown_abs - cur_val) * multiplier;
    }
    return state_index + delta;
}

// Wrapper for backward compatibility - resets ALL comparison vars to unknown
int reset_all_comparison_vars_to_unknown(
    int state_index,
    const domain_abstractions::DomainMapping &domain_mapping,
    const vector<int> &hash_multipliers_by_var_id,
    const TaskProxy &task_proxy) {
    return reset_comparison_vars_to_unknown_except(
        state_index, domain_mapping, hash_multipliers_by_var_id, task_proxy, {});
}

static bool variable_is_trivial(
    int var_id, const domain_abstractions::DomainMapping &domain_mapping) {
    return domain_mapping[var_id].empty();
}

// Build set of comparison axiom variable IDs (uses cached axiom data)
static unordered_set<int> get_comparison_axiom_var_ids(const TaskProxy &task_proxy) {
    ensure_axiom_cache(task_proxy);
    unordered_set<int> comparison_var_ids;
    comparison_var_ids.reserve(g_cached_comparison_axioms.size());
    for (const CachedComparisonAxiom &ax : g_cached_comparison_axioms) {
        comparison_var_ids.insert(ax.prop_var_id);
    }
    return comparison_var_ids;
}

// Extract comparison axiom preconditions from a group's preconditions.
// pattern_index_to_var_id maps pattern indices back to original variable IDs.
static vector<Fact> get_comparison_preconditions_from_group(
    const vector<Fact> &preconditions,
    const vector<int> &pattern,
    const unordered_set<int> &comparison_var_ids) {
    vector<Fact> result;
    
    // preconditions are in pattern index space, we need to convert to var_id space
    for (const Fact &pre : preconditions) {
        int pattern_idx = pre.var;
        if (pattern_idx < 0 || pattern_idx >= static_cast<int>(pattern.size())) {
            continue;
        }
        int var_id = pattern[pattern_idx];
        if (comparison_var_ids.count(var_id) > 0) {
            // Store with original var_id, not pattern index
            result.emplace_back(var_id, pre.value);
        }
    }
    
    return result;
}

static vector<bool> compute_looping_operators(
    const TaskProxy &task_proxy,
    const domain_abstractions::DomainMapping &domain_mapping,
    const domain_abstractions::NumericDomainMappings &numeric_domain_mapping,
    const vector<int> &variable_to_pattern_index,
    const vector<int> &numeric_variable_to_pattern_index) {
    OperatorsProxy ops = task_proxy.get_operators();
    int num_ops = ops.size();
    vector<bool> loops(num_ops, true);
    vector<bool> changed_variables;
    for (int op_id = 0; op_id < num_ops; ++op_id) {
        OperatorProxy op = ops[op_id];
        /*
          An operator has the potential to induce self-loops if one of
          the following three conditions holds for every effect, because
          they allow cases where the effect does not really change the
          value of the corresponding variable:
          1. There is no precondition on the variable of the effect.
          2. The variable is trivial, i.e., it has only one value in the
             abstraction.
          3. The value of the effect is the same as the value of the
             precondition in the abstraction.

          We approximate the operators that induce self-loops by marking
          all cases where neither of these conditions holds. This might
          over-estimate the set of operators that induce self-loops, but
          this is fine because the main purpose of looping operators is
          to exclude them from having negative costs in the
          cost-partitioning, and by over-estimating them we can only
          loose potential of SCP but not make thins inadmissible.
        */
        unordered_map<int, int> var_to_precondition;
        for (FactProxy precondition : op.get_preconditions()) {
            const Fact pre(precondition.get_variable().get_id(), precondition.get_value());
            if (!variable_is_trivial(pre.var, domain_mapping)) {
                var_to_precondition[pre.var] =
                    domain_mapping[pre.var][pre.value];
            }
        }
        for (EffectProxy effect : op.get_effects()) {
            const Fact eff(effect.get_fact().get_variable().get_id(), effect.get_fact().get_value());
            // check if eff var id is comparison
            //ComparisonAxiomsProxy axioms = task_proxy.get_comparison_axioms();
            //bool is_comparison = false;
            //for (ComparisonAxiomProxy ax : axioms) {
            //    if (ax.get_true_fact().get_variable().get_id() == eff.var) {
            //        is_comparison = true;
            //        break;
            //    }
            //}
            //if (is_comparison) {
            //    continue;
            //}
            if (var_to_precondition.count(eff.var) > 0
                && !variable_is_trivial(eff.var, domain_mapping)
                && var_to_precondition[eff.var]
                   != domain_mapping[eff.var][eff.value]) {
                loops[op_id] = false;
                break;
            }
        }
        //if (!loops[op_id]) continue;
        //for (AssEffectProxy eff : op.get_ass_effects()) {
        //    int aff_var = eff.get_assignment().get_affected_variable().get_id();
        //    if (aff_var < static_cast<int>(numeric_variable_to_pattern_index.size()) &&
        //        numeric_variable_to_pattern_index[aff_var] != -1) {
        //        // If there is a numeric effect on a pattern variable, assume it changes state.
        //        loops[op_id] = false;
        //        break;
        //    }
        //}
    }
    return loops;
}


struct DomainAbstractionOperatorGroup {
    vector<Fact> preconditions;           // Original preconditions (pre_pairs)
    vector<Fact> regression_preconditions; // Effects + prevail for match tree
    vector<int> operator_ids;

    bool operator<(const DomainAbstractionOperatorGroup &other) const {
        if (preconditions != other.preconditions)
            return preconditions < other.preconditions;
        if (regression_preconditions != other.regression_preconditions)
            return regression_preconditions < other.regression_preconditions;
        return operator_ids < other.operator_ids;
    }
};

using DomainAbstractionOperatorGroups = vector<DomainAbstractionOperatorGroup>;

static vector<Fact> get_pattern_preconditions(
    const domain_abstractions::AbstractOperator &abs_op,
    const vector<int> &flattened_var_to_pattern_index) {
    
    vector<Fact> pattern_pre;
    pattern_pre.reserve(abs_op.get_preconditions().size());
    
    for (const Fact &f : abs_op.get_preconditions()) {
        if (f.var >= 0 && f.var < static_cast<int>(flattened_var_to_pattern_index.size())) {
            int pattern_idx = flattened_var_to_pattern_index[f.var];
            if (pattern_idx != -1) {
                pattern_pre.emplace_back(pattern_idx, f.value);
            }
        }
    }
    sort(pattern_pre.begin(), pattern_pre.end());
    return pattern_pre;
}

static vector<Fact> get_pattern_regression_preconditions(
    const domain_abstractions::AbstractOperator &abs_op,
    const vector<int> &flattened_var_to_pattern_index) {
    
    vector<Fact> pattern_reg_pre;
    pattern_reg_pre.reserve(abs_op.get_regression_preconditions().size());
    
    for (const Fact &f : abs_op.get_regression_preconditions()) {
        if (f.var >= 0 && f.var < static_cast<int>(flattened_var_to_pattern_index.size())) {
            int pattern_idx = flattened_var_to_pattern_index[f.var];
            if (pattern_idx != -1) {
                pattern_reg_pre.emplace_back(pattern_idx, f.value);
            }
        }
    }
    sort(pattern_reg_pre.begin(), pattern_reg_pre.end());
    return pattern_reg_pre;
}

// Convert abstract operators to operator groups with pattern-projected preconditions.
// The abstract operators are already grouped by the numeric helper if combine_labels was enabled.
static DomainAbstractionOperatorGroups get_operator_groups_from_abstract_ops(
    const vector<domain_abstractions::AbstractOperator> &abstract_operators,
    const vector<int> &variable_to_pattern_index,
    const vector<int> &numeric_variable_to_pattern_index,
    const domain_abstractions::DomainMapping &domain_mapping,
    const domain_abstractions::NumericDomainMappings &numeric_domain_mapping) {
    
    vector<int> flattened_var_to_pattern_index(domain_mapping.size() + numeric_domain_mapping.size(), -1);
    for (size_t i = 0; i < variable_to_pattern_index.size(); ++i) {
        if (i < flattened_var_to_pattern_index.size()) {
            flattened_var_to_pattern_index[i] = variable_to_pattern_index[i];
        }
    }
    for (size_t i = 0; i < numeric_variable_to_pattern_index.size(); ++i) {
        size_t idx = i + domain_mapping.size();
        if (idx < flattened_var_to_pattern_index.size()) {
            flattened_var_to_pattern_index[idx] = numeric_variable_to_pattern_index[i];
        }
    }

    DomainAbstractionOperatorGroups groups;
    groups.reserve(abstract_operators.size());
    for (const auto &abs_op : abstract_operators) {
        DomainAbstractionOperatorGroup group;
        group.preconditions = get_pattern_preconditions(abs_op, flattened_var_to_pattern_index);
        group.regression_preconditions = get_pattern_regression_preconditions(abs_op, flattened_var_to_pattern_index);
        group.operator_ids = abs_op.get_concrete_op_ids();
        groups.push_back(move(group));
    }
    return groups;
}


DomainAbstractionFunction::DomainAbstractionFunction(
    const pdbs::Pattern &pattern,
    const vector<int> &hash_multipliers,
    const domain_abstractions::DomainMapping domain_mapping,
    const domain_abstractions::NumericDomainMappings &numeric_domain_mapping)
    : domain_mapping(move(domain_mapping)) {
    // Deep copy numeric_domain_mapping by cloning each unique_ptr
    this->numeric_domain_mapping.reserve(numeric_domain_mapping.size());
    for (const auto &mapping : numeric_domain_mapping) {
        this->numeric_domain_mapping.push_back(mapping->clone());
    }
    assert(pattern.size() == hash_multipliers.size());
    variables_and_multipliers.reserve(pattern.size());
    for (size_t i = 0; i < pattern.size(); ++i) {
        variables_and_multipliers.emplace_back(pattern[i], hash_multipliers[i]);
    }
}

int DomainAbstractionFunction::get_abstract_state_id(const State &concrete_state) const {
    int index = 0;
    for (const VariableAndMultiplier &pair : variables_and_multipliers) {
        int val;
        if (pair.pattern_var < static_cast<int>(domain_mapping.size())) {
            val = domain_mapping[pair.pattern_var][concrete_state[pair.pattern_var].get_value()];
        } else {
            int num_var_id = pair.pattern_var - domain_mapping.size();
            val = numeric_domain_mapping[num_var_id]->get_partition_index(concrete_state.nval(num_var_id));
        }
        index += pair.hash_multiplier * val;
    }
    return index;
}


DomainAbstraction::DomainAbstraction(
    const TaskProxy &task_proxy,
    const std::shared_ptr<TaskInfo> &task_info,
    domain_abstractions::DomainAbstraction &domain_abstraction,
    bool combine_labels,
    utils::Log &log)
    : Abstraction(nullptr),
      task_proxy(task_proxy),
      task_info(task_info),
      domain_mapping(domain_abstraction.extract_domain_mapping()),
      numeric_domain_mapping(domain_abstraction.extract_numeric_domain_mapping()) {
    
    // Compute domain_sizes and numeric_domain_sizes for multiplicator
    vector<int> domain_sizes(domain_mapping.size(), 1);
    for (size_t var_id = 0; var_id < domain_mapping.size(); ++var_id) {
        if (!domain_mapping[var_id].empty()) {
            int max_val = *max_element(domain_mapping[var_id].begin(),
                                       domain_mapping[var_id].end());
            domain_sizes[var_id] = max_val + 1;
        }
    }
    
    vector<int> numeric_domain_sizes(numeric_domain_mapping.size(), 1);
    for (size_t var_id = 0; var_id < numeric_domain_mapping.size(); ++var_id) {
        if (!numeric_domain_mapping[var_id]) {
             cerr << "CRITICAL ERROR: numeric_domain_mapping[" << var_id << "] is NULL!" << endl;
             utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        }
        int num_parts = numeric_domain_mapping[var_id]->get_num_partitions();
        if (num_parts != 0) {
            numeric_domain_sizes[var_id] = num_parts;
        }
    }

    for (size_t var_id = 0; var_id < domain_mapping.size(); ++var_id) {
        if (!domain_mapping[var_id].empty()) {
            int max_val = *max_element(domain_mapping[var_id].begin(),
                                       domain_mapping[var_id].end());
            assert(max_val > 0); // Variable is non-trivial.
            pattern.push_back(var_id);
            pattern_domain_sizes.push_back(max_val + 1);
        }
    }
    for (size_t var_id = 0; var_id < numeric_domain_mapping.size(); ++var_id) {
        int max_val = numeric_domain_mapping[var_id]->get_num_partitions();
        assert(max_val > 0); // Variable is non-trivial.
        int pattern_var_id = var_id + domain_mapping.size();
        pattern.push_back(pattern_var_id);
        pattern_domain_sizes.push_back(max_val);
    }
    // assert(utils::is_sorted_unique(pattern));

    VariablesProxy variables = task_proxy.get_variables();
    NumericVariablesProxy numeric_variables = task_proxy.get_numeric_variables();

    vector<int> variable_to_pattern_index(variables.size(), -1);
    vector<int> numeric_variable_to_pattern_index(numeric_variables.size(), -1);
    for (size_t i = 0; i < pattern.size(); ++i) {
        int var_id = pattern[i];
        if (var_id < static_cast<int>(variables.size())) {
            variable_to_pattern_index[var_id] = i;
        } else {
            int idx = var_id - variables.size();
            if (idx < 0 || idx >= static_cast<int>(numeric_variable_to_pattern_index.size())) {
                cerr << "CRITICAL ERROR: Index out of bounds! idx=" << idx << ", size=" << numeric_variable_to_pattern_index.size() << endl;
                utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
            }
            numeric_variable_to_pattern_index[idx] = i;
        }
    }

    looping_operators = compute_looping_operators(
        task_proxy, domain_mapping, numeric_domain_mapping,
        variable_to_pattern_index, numeric_variable_to_pattern_index);

    hash_multipliers.reserve(pattern.size());
    num_states = 1;
    for (int dom_size : pattern_domain_sizes) {
        hash_multipliers.push_back(num_states);
        if (utils::is_product_within_limit(num_states, dom_size,
                                           numeric_limits<int>::max())) {
            num_states *= dom_size;
        } else {
            cerr << "Given pattern is too large! (Overflow occured): " << endl;
            cerr << pattern << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        }
    }
    assert(num_states == domain_abstraction.size());

    abstraction_function = utils::make_unique_ptr<DomainAbstractionFunction>(
        pattern, hash_multipliers, domain_mapping, numeric_domain_mapping);

    match_tree_backward = utils::make_unique_ptr<domain_abstractions::MatchTreeWithPattern>(
        pattern_domain_sizes, hash_multipliers);

    // Create hash_multipliers indexed by original variable ID for Multiplicator and comparison evaluation
    // pattern contains the original variable IDs, hash_multipliers[i] is the multiplier for pattern[i]
    // We need hash_multipliers_by_var_id[var_id] = hash_multipliers[pattern_index] where pattern[pattern_index] = var_id
    int total_vars = variables.size() + numeric_variables.size();
    // Initialize with 0: variables not in the pattern have multiplier 0
    hash_multipliers_by_var_id.resize(total_vars, 0);
    for (size_t i = 0; i < pattern.size(); ++i) {
        int var_id = pattern[i];
        assert(var_id >= 0 && var_id < total_vars);
        hash_multipliers_by_var_id[var_id] = hash_multipliers[i];
    }

    // Instantiate DomainAbstractionNumericHelper to build abstract operators
    // Pass combine_labels to let the helper do label reduction if enabled
    domain_abstractions::DomainAbstractionNumericHelper helper(
        g_root_task(),
        domain_mapping,
        numeric_domain_mapping,
        domain_sizes,
        numeric_domain_sizes,
        hash_multipliers_by_var_id,
        combine_labels,  // Let helper do label reduction
        nullptr  // No logger for cost saturation
    );
    
    vector<domain_abstractions::AbstractOperator> abstract_operators =
        helper.build_abstract_operators(task_proxy);

    use_int_costs = helper.get_all_costs_are_ints();
    
    // Convert abstract operators to operator groups with pattern-projected preconditions
    // No additional grouping needed - the helper already grouped if combine_labels was true
    DomainAbstractionOperatorGroups operator_groups = get_operator_groups_from_abstract_ops(
        abstract_operators, variable_to_pattern_index, numeric_variable_to_pattern_index,
        domain_mapping, numeric_domain_mapping);

    int num_ops_covered_by_labels = 0;
    for (const auto &group : operator_groups) {
        num_ops_covered_by_labels += group.operator_ids.size();
    }
    label_to_operators.reserve(operator_groups.size(), num_ops_covered_by_labels);

    // Build set of comparison axiom variable IDs for extracting comparison preconditions
    unordered_set<int> comparison_var_ids = get_comparison_axiom_var_ids(task_proxy);

    for (DomainAbstractionOperatorGroup &group : operator_groups) {
        int label_id = label_to_operators.size();
        label_to_operators.push_back(move(group.operator_ids));
        
        // Compute precondition_hash from preconditions
        int precondition_hash = 0;
        for (const Fact &f : group.preconditions) {
            precondition_hash += hash_multipliers[f.var] * f.value;
        }
        
        // Extract comparison axiom preconditions for this operator group
        vector<Fact> comparison_preconds = get_comparison_preconditions_from_group(
            group.preconditions, pattern, comparison_var_ids);
        
        // Compute hash_effect from matching precondition/effect pairs
        // regression_preconditions contains effects + prevail
        // preconditions contains the original preconditions
        // We need to find matched pairs where pre.var == eff.var
        int hash_effect = 0;
        
        // Build a map of precondition values by variable
        unordered_map<int, int> pre_val_by_var;
        for (const Fact &pre : group.preconditions) {
            pre_val_by_var[pre.var] = pre.value;
        }
        
        // For each effect (in regression_preconditions), if there's a matching precondition,
        // compute the effect contribution
        for (const Fact &eff : group.regression_preconditions) {
            auto it = pre_val_by_var.find(eff.var);
            if (it != pre_val_by_var.end()) {
                int pre_val = it->second;
                int eff_val = eff.value;
                // hash_effect = (new_val - old_val) * multiplier
                // In progression: new_val = eff_val, old_val = pre_val
                hash_effect += (eff_val - pre_val) * hash_multipliers[eff.var];
            }
        }
        
        ranked_operators.emplace_back(label_id, precondition_hash, hash_effect,
                                      std::move(comparison_preconds));
        match_tree_backward->insert(ranked_operators.size() - 1, group.regression_preconditions);
    }
    
    ranked_operators.shrink_to_fit();

    goal_states = compute_goal_states(variable_to_pattern_index);
}


DomainAbstraction::~DomainAbstraction() {
}

bool DomainAbstraction::increment_to_next_state(vector<Fact> &facts) const {
    for (int i = facts.size() - 1; i >= 0; --i) {
        int var = facts[i].var;
        int max_val = pattern_domain_sizes[var] - 1;
        if (facts[i].value < max_val) {
            facts[i].value++;
            return true;
        } else {
            facts[i].value = 0;
        }
    }
    return false;
}

vector<int> DomainAbstraction::compute_goal_states(
    const vector<int> &variable_to_pattern_index) const {
    vector<Fact> abstract_goals;
    for (const Fact &goal : task_info->get_goals()) {
        int var_id = goal.var;
        int mapped_var = variable_to_pattern_index[var_id];
        if (mapped_var != -1) {
            abstract_goals.emplace_back(
                mapped_var, domain_mapping[var_id][goal.value]);
        }
    }
    sort(abstract_goals.begin(), abstract_goals.end());

    vector<int> goals;
    vector<Fact> state;
    state.reserve(pattern.size());
    for (size_t i = 0; i < pattern.size(); ++i) {
        state.emplace_back(i, 0);
    }
    
    do {
        int state_index = 0;
        for (const Fact &fact : state) {
            state_index += hash_multipliers[fact.var] * fact.value;
        }
        if (is_consistent(state_index, abstract_goals)) {
            goals.push_back(state_index);
        }
    } while (increment_to_next_state(state));
    return goals;
}

bool DomainAbstraction::is_consistent(
    int state_index,
    const vector<Fact> &abstract_facts) const {
    for (const Fact &abstract_goal : abstract_facts) {
        int pattern_var_id = abstract_goal.var;
        int temp = state_index / hash_multipliers[pattern_var_id];
        int val = temp % pattern_domain_sizes[pattern_var_id];
        if (val != abstract_goal.value) {
            return false;
        }
    }
    return true;
}

vector<ap_float> DomainAbstraction::compute_saturated_costs(
    const vector<ap_float> &h_values) const {
    int num_operators = get_num_operators();

    int num_labels = label_to_operators.size();
    vector<ap_float> saturated_label_costs(num_labels, -INF);

    //print all non 0 non inf h values
    for (int i = 0; i < (int)h_values.size(); ++i) {
        if (h_values[i] != 0 && h_values[i] != INF) {
            //cout << "h_values[" << i << "] = " << h_values[i] << endl;
        }
    }

    for_each_label_transition(
        [&saturated_label_costs, &h_values](const Transition &t) {
            assert(utils::in_bounds(t.src, h_values));
            assert(utils::in_bounds(t.target, h_values));
            ap_float src_h = h_values[t.src];
            ap_float target_h = h_values[t.target];
            if (src_h == INF || target_h == INF) {
                return;
            }
            ap_float &needed_costs = saturated_label_costs[t.op];
            needed_costs = max(needed_costs, src_h - target_h);
            //cout << "Transition: " << t.src << " --" << t.op << "--> " << t.target
            //     << ", src_h: " << src_h << ", target_h: " << target_h
            //     << ", needed_costs: " << needed_costs << endl;
        });

    vector<ap_float> saturated_costs(num_operators, -INF);
    /* To prevent negative cost cycles, we ensure that all operators inducing
       self-loops (among possibly other transitions) have non-negative costs. */
    for (int op_id = 0; op_id < num_operators; ++op_id) {
        if (operator_induces_self_loop(op_id) || true) { //TODO: Change back
            saturated_costs[op_id] = 0;
        }
    }

    for (int label_id = 0; label_id < num_labels; ++label_id) {
        ap_float saturated_label_cost = saturated_label_costs[label_id];
        for (int op_id : label_to_operators.get_slice(label_id)) {
            saturated_costs[op_id] = max(saturated_costs[op_id], saturated_label_cost);
        }
    }

    return saturated_costs;
}

int DomainAbstraction::get_num_operators() const {
    return task_info->get_num_operators();
}

vector<ap_float> DomainAbstraction::compute_goal_distances(const vector<ap_float> &operator_costs) const {

    // TODO: use log
//    if (log.is_at_least_debug()) {
//        log << "computing goal distances for: " << endl;
//        log << "domain mapping: " << domain_mapping << endl;
//        log << "pattern: " << pattern << endl;
//        log << "pattern domain sizes: " << pattern_domain_sizes << endl;
//    }

    // Assign each label the cost of cheapest operator that the label covers.
    int num_labels = label_to_operators.size();
    vector<ap_float> label_costs;
    label_costs.reserve(num_labels);
    for (int label_id = 0; label_id < num_labels; ++label_id) {
        ap_float min_cost = INF;
        for (int op_id : label_to_operators.get_slice(label_id)) {
            min_cost = min(min_cost, operator_costs[op_id]);
        }
        label_costs.push_back(min_cost);
    }

    vector<ap_float> distances(num_states, INF);

    // Compute initial state hash for optimization (always process initial state)
    int init_hash = abstraction_function->get_abstract_state_id(task_proxy.get_initial_state());

    // Initialize queue with goal states that are numerically/comparison-feasible.
    AdaptiveQueue<int> pq;

    if (!use_int_costs) {
        pq.convert_now();
    }


    for (int goal : goal_states) {
        // Enumerate all states compatible with comparison evaluations starting from this goal.
        vector<int> alt_states = enumerate_states_with_evaluated_comparisons(goal);
        if (find(alt_states.begin(), alt_states.end(), goal) != alt_states.end()) {
            pq.push(0, goal);
            distances[goal] = 0;
        }
    }

    // Reuse vectors to save allocations.
    vector<int> applicable_operators;
    vector<int> predecessors;

    // Run Dijkstra loop.
    while (!pq.empty()) {

        pair<ap_float, int> node = pq.pop();
        ap_float distance = node.first;
        int state_index = node.second;

        //cout << "Processing state " << state_index << " with distance " << distance << endl;
        assert(utils::in_bounds(state_index, distances));
        if (distance > distances[state_index]) {
            continue;
        }

        // Regress abstract state, taking into account ambiguity in comparison axioms.
        applicable_operators.clear();
        int base_state = reset_all_comparison_vars_to_unknown(
            state_index, domain_mapping, hash_multipliers_by_var_id, task_proxy);
        match_tree_backward->get_applicable_operator_ids(
            base_state, applicable_operators);
        for (int ranked_op_id : applicable_operators) {
            const RankedOperator &op = ranked_operators[ranked_op_id];
            assert(utils::in_bounds(op.label, label_costs));
            ap_float alternative_cost = (label_costs[op.label] == INF) ?
                INF : distances[state_index] + label_costs[op.label];
            //alternative_cost =  distances[state_index] + 1;
            if (alternative_cost == INF) {
                continue;
            }

            // Compute the canonical predecessor and then enumerate all
            // comparison-consistent variants.
            // Use comparison preconditions to preserve operator requirements during regression:
            // - Reset all comparison vars to unknown EXCEPT those in preconditions
            // - Enumerate states with those comparison values fixed
            int predecessor_base = reset_comparison_vars_to_unknown_except(
                base_state, domain_mapping, hash_multipliers_by_var_id, task_proxy,
                op.comparison_preconditions);
            predecessor_base = predecessor_base - op.hash_effect;
            
            predecessors.clear();
            enumerate_states_with_evaluated_comparisons(
                predecessor_base, op.comparison_preconditions, predecessors);
            for (int predecessor : predecessors) {
                assert(utils::in_bounds(predecessor, distances));
                if (alternative_cost < distances[predecessor]) {
                    distances[predecessor] = alternative_cost;
                    // Optimization: Only insert into pq if this is the canonical
                    // (smallest index) representative among all alternatives,
                    // OR if it equals the initial state (always insert initial).
                    bool insert_into_pq = true;
                    if (init_hash != predecessor) {
                        for (int alt_state : predecessors) {
                            if (alt_state > predecessor) {
                                insert_into_pq = false;
                                break;
                            }
                        }
                    }
                    if (insert_into_pq) {
                        //cout << "Current distance: " << distances[predecessor]
                        //     << ", pushing predecessor state " << predecessor
                        //     << " with alternative cost " << alternative_cost << endl;
                        pq.push(alternative_cost, predecessor);
                    }
                }
            }
        }
    }

    //exit(0);

    // print all distances
    // for (size_t i = 0; i < distances.size(); ++i) {
    //     cout << "Distance to goal for state " << i << ": " << distances[i] << endl;
    // }

    return distances;
}

int DomainAbstraction::get_num_states() const {
    return num_states;
}

bool DomainAbstraction::operator_is_active(int op_id) const {
    return task_info->operator_is_active(pattern, op_id);
}

bool DomainAbstraction::operator_induces_self_loop(int op_id) const {
    return looping_operators[op_id];
}

void DomainAbstraction::for_each_transition(const TransitionCallback &callback) const {
    return for_each_label_transition(
        [this, &callback](const Transition &t) {
            for (int op_id : label_to_operators.get_slice(t.op)) {
                callback(Transition(t.src, op_id, t.target));
            }
        });
}

const vector<int> &DomainAbstraction::get_goal_states() const {
    return goal_states;
}

const pdbs::Pattern &DomainAbstraction::get_pattern() const {
    return pattern;
}

void DomainAbstraction::dump() const {
    // TODO: use log
    cout << "Ranked operators: " << ranked_operators.size()
        << ", goal states: " << goal_states.size() << "/" << num_states
        << endl;
}

string DomainAbstraction::decode_state(int state_index) const {
    stringstream ss;
    ss << "State " << state_index << ": [";
    
    VariablesProxy variables = task_proxy.get_variables();
    NumericVariablesProxy num_vars = task_proxy.get_numeric_variables();
    int num_prop_vars = static_cast<int>(domain_mapping.size());
    
    bool first = true;
    for (size_t pattern_idx = 0; pattern_idx < pattern.size(); ++pattern_idx) {
        int var_id = pattern[pattern_idx];
        int multiplier = hash_multipliers[pattern_idx];
        int domain_size = pattern_domain_sizes[pattern_idx];
        if (domain_size == 1) {
            continue;
        }
        int value = (state_index / multiplier) % domain_size;
        
        if (!first) ss << ", ";
        first = false;
        
        if (var_id < num_prop_vars) {
            // Propositional variable
            ss << variables[var_id].get_name() << "=" << value;
        } else {
            // Numeric variable
            int num_var_id = var_id - num_prop_vars;
            const domain_abstractions::NumericRange *rng = 
                numeric_domain_mapping[num_var_id]->get_range_for_partition(value);
            ss << num_vars[num_var_id].get_name() << "=";
            if (rng) {
                ss << (rng->lower_inclusive ? "[" : "(") 
                   << rng->lower << "," << rng->upper 
                   << (rng->upper_inclusive ? "]" : ")");
            } else {
                ss << "INVALID_PARTITION(" << value << ")";
            }
        }
    }
    ss << "]";
    return ss.str();
}

string DomainAbstraction::decode_ranked_operator(const RankedOperator &ranked_op) const {
    stringstream ss;
    
    // Get operator name from label
    int concrete_op_id = *label_to_operators.get_slice(ranked_op.label).begin();
    string op_name = task_proxy.get_operators()[concrete_op_id].get_name();
    
    ss << "Operator: " << op_name << " (label=" << ranked_op.label << ")\n";
    
    // Decode precondition_hash as a state
    ss << "  Precondition: " << decode_state(ranked_op.precondition_hash) << "\n";
    
    // Show hash_effect and target state
    ss << "  Effect hash: " << ranked_op.hash_effect << "\n";
    
    int target_hash = ranked_op.precondition_hash + ranked_op.hash_effect;
    ss << "  Target: " << decode_state(target_hash);
    
    return ss.str();
}

string DomainAbstraction::decode_mentioned_variables(int concrete_op_id) const {
    stringstream ss;
    ss << "Mentioned variables: [";
    
    VariablesProxy variables = task_proxy.get_variables();
    NumericVariablesProxy num_vars = task_proxy.get_numeric_variables();
    int num_prop_vars = static_cast<int>(domain_mapping.size());
    int num_vars_in_task = static_cast<int>(variables.size());
    
    // Debug: Show pattern contents and TaskInfo lookup results
    ss << "\n  Pattern size: " << pattern.size() << ", num_prop_vars (domain_mapping.size): " << num_prop_vars;
    ss << ", num_vars in task: " << num_vars_in_task << "\n";
    ss << "  Pattern contents: [";
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << pattern[i];
    }
    ss << "]\n";
    
    ss << "  Per-variable analysis:\n";
    for (size_t i = 0; i < pattern.size(); ++i) {
        int var = pattern[i];
        bool mentioned = task_info->operator_mentions_variable(concrete_op_id, var);
        
        ss << "    pattern[" << i << "] = var " << var << ": ";
        if (var < num_prop_vars) {
            ss << variables[var].get_name();
        } else {
            int num_var_id = var - num_prop_vars;
            if (num_var_id < static_cast<int>(num_vars.size())) {
                ss << num_vars[num_var_id].get_name();
            } else {
                ss << "INVALID_NUM_VAR";
            }
        }
        ss << " -> mentioned=" << (mentioned ? "YES" : "NO") << "\n";
    }
    
    ss << "  Mentioned: [";
    bool first = true;
    for (size_t i = 0; i < pattern.size(); ++i) {
        int var = pattern[i];
        if (task_info->operator_mentions_variable(concrete_op_id, var)) {
            if (!first) ss << ", ";
            first = false;
            
            if (var < num_prop_vars) {
                ss << variables[var].get_name() << " (id=" << var << ", pattern_idx=" << i << ")";
            } else {
                int num_var_id = var - num_prop_vars;
                ss << num_vars[num_var_id].get_name() << " (num_id=" << num_var_id << ", var=" << var << ", pattern_idx=" << i << ")";
            }
        }
    }
    ss << "]";
    return ss.str();
}

void DomainAbstraction::enumerate_states_with_evaluated_comparisons(
    int base_state_index,
    const vector<Fact> &fixed_comparisons,
    vector<int> &result) const {
    
    // Build set of fixed variable IDs and their values for quick lookup.
    // Linear search is faster than unordered_map for small vectors (typically 0-3 elements).
    auto is_fixed = [&fixed_comparisons](int var_id) -> bool {
        for (const Fact &f : fixed_comparisons) {
            if (f.var == var_id) {
                return true;
            }
        }
        return false;
    };
    
    // Use thread-local reusable vectors to avoid repeated allocations and hash overhead.
    compute_numeric_context(base_state_index, domain_mapping, numeric_domain_mapping,
                            hash_multipliers, pattern_domain_sizes, pattern,
                            task_proxy, g_ranges_vec, g_ranges_valid, g_cur_num_partitions);
    vector<CompEvalHelper> comparisons = evaluate_all_comparisons(
        g_ranges_vec, g_ranges_valid, g_cur_num_partitions, numeric_domain_mapping, task_proxy);

    // Reset comparison axiom variables to UNKNOWN, EXCEPT fixed ones
    int state_with_unknowns = reset_comparison_vars_to_unknown_except(
        base_state_index, domain_mapping, hash_multipliers_by_var_id, task_proxy, fixed_comparisons);

    // Use explicit stack instead of std::function to avoid type-erasure overhead.
    // Stack stores (comparison_index, delta_from_unknown).
    vector<pair<size_t, int>> stack;
    stack.reserve(comparisons.size() + 1);
    stack.emplace_back(0, 0);
    
    while (!stack.empty()) {
        auto [idx, delta_from_unknown] = stack.back();
        stack.pop_back();
        
        // Skip comparisons that don't contribute to branching
        while (idx < comparisons.size()) {
            const CompEvalHelper &comp = comparisons[idx];
            int var_id = comp.prop_var_id;
            
            if (variable_is_trivial(var_id, domain_mapping)) {
                ++idx;
                continue;
            }
            
            int multiplier = hash_multipliers_by_var_id[var_id];
            if (multiplier == 0) {
                ++idx;
                continue;
            }
            
            if (is_fixed(var_id)) {
                ++idx;
                continue;
            }
            
            int unknown_value = domain_mapping[var_id][2];
            
            if (comp.eval == 0) {
                // Only TRUE branch
                int delta = (domain_mapping[var_id][comp.true_val] - unknown_value) * multiplier;
                delta_from_unknown += delta;
                ++idx;
            } else if (comp.eval == 1) {
                // Only FALSE branch
                int delta = (domain_mapping[var_id][comp.false_val] - unknown_value) * multiplier;
                delta_from_unknown += delta;
                ++idx;
            } else {
                // Both branches - push FALSE onto stack, continue with TRUE
                int delta_true = (domain_mapping[var_id][comp.true_val] - unknown_value) * multiplier;
                int delta_false = (domain_mapping[var_id][comp.false_val] - unknown_value) * multiplier;
                stack.emplace_back(idx + 1, delta_from_unknown + delta_false);
                delta_from_unknown += delta_true;
                ++idx;
            }
        }
        
        // Reached end of comparisons - emit result
        result.push_back(state_with_unknowns + delta_from_unknown);
    }
    
    if (result.empty()) {
        result.push_back(state_with_unknowns);
    }
}

vector<int> DomainAbstraction::enumerate_states_with_evaluated_comparisons(
    int base_state_index,
    const vector<Fact> &fixed_comparisons) const {
    
    vector<int> result;
    enumerate_states_with_evaluated_comparisons(base_state_index, fixed_comparisons, result);
    return result;
}

string DomainAbstraction::decode_domain_abstraction() const {
    stringstream ss;
    VariablesProxy variables = task_proxy.get_variables();
    NumericVariablesProxy num_vars = task_proxy.get_numeric_variables();
    
    ss << "=== DOMAIN ABSTRACTION DEBUG ===" << endl;
    ss << "Pattern size: " << pattern.size() << endl;
    ss << "Num states: " << num_states << endl;
    ss << endl;
    
    // Print propositional domain mapping
    ss << "--- Propositional Domain Mapping ---" << endl;
    for (size_t var_id = 0; var_id < domain_mapping.size(); ++var_id) {
        if (domain_mapping[var_id].empty()) {
            continue;  // Skip trivial variables not in abstraction
        }
        ss << "  " << variables[var_id].get_name() << " (id=" << var_id << "):" << endl;
        ss << "    Original domain size: " << variables[var_id].get_domain_size() << endl;
        ss << "    Abstract domain size: ";
        int abstract_size = 0;
        for (int mapped : domain_mapping[var_id]) {
            abstract_size = max(abstract_size, mapped + 1);
        }
        ss << abstract_size << endl;
        ss << "    Mapping: [";
        for (size_t val = 0; val < domain_mapping[var_id].size(); ++val) {
            if (val > 0) ss << ", ";
            ss << val << "->" << domain_mapping[var_id][val];
        }
        ss << "]" << endl;
    }
    ss << endl;
    
    // Print numeric domain mapping
    ss << "--- Numeric Domain Mapping ---" << endl;
    for (size_t num_var_id = 0; num_var_id < numeric_domain_mapping.size(); ++num_var_id) {
        if (!numeric_domain_mapping[num_var_id]) {
            ss << "  Numeric var " << num_var_id << ": NULL mapping" << endl;
            continue;
        }
        
        int num_partitions = numeric_domain_mapping[num_var_id]->get_num_partitions();
        if (num_partitions <= 1) {
            continue;  // Skip trivial variables (0 or 1 partition)
        }
        
        ss << "  " << num_vars[num_var_id].get_name() << " (num_id=" << num_var_id 
           << ", pattern_var=" << (num_var_id + domain_mapping.size()) << "):" << endl;
        ss << "    Num partitions: " << num_partitions << endl;
        ss << "    Partitions:" << endl;
        
        for (int part = 0; part < num_partitions; ++part) {
            const domain_abstractions::NumericRange *rng = 
                numeric_domain_mapping[num_var_id]->get_range_for_partition(part);
            ss << "      [" << part << "]: ";
            if (rng) {
                ss << (rng->lower_inclusive ? "[" : "(") 
                   << rng->lower << ", " << rng->upper 
                   << (rng->upper_inclusive ? "]" : ")");
            } else {
                ss << "INVALID";
            }
            ss << endl;
        }
    }
    ss << endl;
    
    // Print pattern with hash multipliers
    ss << "--- Pattern & Hash Multipliers ---" << endl;
    for (size_t i = 0; i < pattern.size(); ++i) {
        int var_id = pattern[i];
        ss << "  pattern[" << i << "]: var " << var_id << " (";
        if (var_id < static_cast<int>(domain_mapping.size())) {
            ss << variables[var_id].get_name();
        } else {
            int num_var_id = var_id - domain_mapping.size();
            if (num_var_id < static_cast<int>(num_vars.size())) {
                ss << num_vars[num_var_id].get_name();
            } else {
                ss << "INVALID";
            }
        }
        ss << "), multiplier=" << hash_multipliers[i] 
           << ", domain_size=" << pattern_domain_sizes[i] << endl;
    }
    ss << "=================================" << endl;
    
    return ss.str();
}
}
