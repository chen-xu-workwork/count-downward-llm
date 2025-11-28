#include "domain_abstraction.h"

#include "types.h"

#include "../task_proxy.h"
#include "../tasks/root_task.h"

#include "../priority_queue.h"
#include "../domain_abstractions/domain_abstraction.h"
#include "../domain_abstractions/match_tree_with_pattern.h"
#include "../domain_abstractions/numeric_helper.h"
#include "../domain_abstractions/domain_abstraction_factory.h"
#include "../task_tools.h"
#include "../utils/collections.h"
#include "../utils/logging.h"
#include "../utils/math.h"
#include "../utils/memory.h"

#include <cassert>
#include <unordered_map>
#include <map>
#include <tuple>

using namespace std;

std::ostream &operator<<(std::ostream &os, const Fact &fact) {
    return os << fact.var << "=" << fact.value;
}

namespace cost_saturation {
static bool variable_is_trivial(
    int var_id, const domain_abstractions::DomainMapping &domain_mapping) {
    return domain_mapping[var_id].empty();
}

static vector<bool> compute_looping_operators(
    const TaskProxy &task_proxy,
    const domain_abstractions::DomainMapping &domain_mapping,
    const domain_abstractions::NumericDomainMappingType &numeric_domain_mapping,
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
            if (var_to_precondition.count(eff.var) > 0
                && !variable_is_trivial(eff.var, domain_mapping)
                && var_to_precondition[eff.var]
                   != domain_mapping[eff.var][eff.value]) {
                loops[op_id] = false;
                break;
            }
        }
        if (!loops[op_id]) continue;

        for (AssEffectProxy eff : op.get_ass_effects()) {
            int aff_var = eff.get_assignment().get_affected_variable().get_id();
            if (aff_var < static_cast<int>(numeric_variable_to_pattern_index.size()) &&
                numeric_variable_to_pattern_index[aff_var] != -1) {
                // If there is a numeric effect on a pattern variable, assume it changes state.
                loops[op_id] = false;
                break;
            }
        }
    }
    return loops;
}


struct OperatorGroup {
    vector<Fact> regression_preconditions;
    int hash_effect;
    vector<int> operator_ids;

    bool operator<(const OperatorGroup &other) const {
        if (hash_effect != other.hash_effect)
            return hash_effect < other.hash_effect;
        if (regression_preconditions != other.regression_preconditions)
            return regression_preconditions < other.regression_preconditions;
        return operator_ids < other.operator_ids;
    }
};

using OperatorGroups = vector<OperatorGroup>;

static vector<Fact> get_pattern_regression_preconditions(
    const domain_abstractions::AbstractOperator &abs_op,
    const vector<int> &flattened_var_to_pattern_index) {
    
    vector<Fact> pattern_reg_pre;
    pattern_reg_pre.reserve(abs_op.get_regression_preconditions().size());
    
    for (const Fact &f : abs_op.get_regression_preconditions()) {
        if (f.var < static_cast<int>(flattened_var_to_pattern_index.size())) {
            int pattern_idx = flattened_var_to_pattern_index[f.var];
            if (pattern_idx != -1) {
                pattern_reg_pre.emplace_back(pattern_idx, f.value);
            }
        }
    }
    sort(pattern_reg_pre.begin(), pattern_reg_pre.end());
    return pattern_reg_pre;
}

static OperatorGroups group_equivalent_operators(
    const vector<domain_abstractions::AbstractOperator> &abstract_operators,
    const vector<int> &variable_to_pattern_index,
    const vector<int> &numeric_variable_to_pattern_index,
    const domain_abstractions::DomainMapping &domain_mapping,
    const domain_abstractions::NumericDomainMappingType &numeric_domain_mapping) {
    
    map<pair<vector<Fact>, int>, vector<int>> grouped_ops;
    
    vector<int> flattened_var_to_pattern_index(domain_mapping.size() + numeric_domain_mapping.size(), -1);
    for (size_t i = 0; i < variable_to_pattern_index.size(); ++i) {
        flattened_var_to_pattern_index[i] = variable_to_pattern_index[i];
    }
    for (size_t i = 0; i < numeric_variable_to_pattern_index.size(); ++i) {
        flattened_var_to_pattern_index[i + domain_mapping.size()] = numeric_variable_to_pattern_index[i];
    }

    for (const auto &abs_op : abstract_operators) {
        vector<Fact> pattern_reg_pre = get_pattern_regression_preconditions(abs_op, flattened_var_to_pattern_index);
        grouped_ops[{pattern_reg_pre, abs_op.get_hash_effect()}].push_back(abs_op.get_concrete_op_id());
    }

    OperatorGroups groups;
    groups.reserve(grouped_ops.size());
    for (auto &entry : grouped_ops) {
        OperatorGroup group;
        group.regression_preconditions = move(entry.first.first);
        group.hash_effect = entry.first.second;
        group.operator_ids = move(entry.second);
        groups.push_back(move(group));
    }
    sort(groups.begin(), groups.end());
    return groups;
}

static OperatorGroups get_singleton_operator_groups(
    const vector<domain_abstractions::AbstractOperator> &abstract_operators,
    const vector<int> &variable_to_pattern_index,
    const vector<int> &numeric_variable_to_pattern_index,
    const domain_abstractions::DomainMapping &domain_mapping,
    const domain_abstractions::NumericDomainMappingType &numeric_domain_mapping) {
    
    vector<int> flattened_var_to_pattern_index(domain_mapping.size() + numeric_domain_mapping.size(), -1);
    for (size_t i = 0; i < variable_to_pattern_index.size(); ++i) {
        flattened_var_to_pattern_index[i] = variable_to_pattern_index[i];
    }
    for (size_t i = 0; i < numeric_variable_to_pattern_index.size(); ++i) {
        flattened_var_to_pattern_index[i + domain_mapping.size()] = numeric_variable_to_pattern_index[i];
    }

    OperatorGroups groups;
    groups.reserve(abstract_operators.size());
    for (const auto &abs_op : abstract_operators) {
        OperatorGroup group;
        group.regression_preconditions = get_pattern_regression_preconditions(abs_op, flattened_var_to_pattern_index);
        group.hash_effect = abs_op.get_hash_effect();
        group.operator_ids = {abs_op.get_concrete_op_id()};
        groups.push_back(move(group));
    }
    return groups;
}


DomainAbstractionFunction::DomainAbstractionFunction(
    const pdbs::Pattern &pattern,
    const vector<int> &hash_multipliers,
    const domain_abstractions::DomainMapping domain_mapping,
    const domain_abstractions::NumericDomainMappingType &numeric_domain_mapping)
    : domain_mapping(move(domain_mapping)),
      numeric_domain_mapping(numeric_domain_mapping) {
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
      task_info(task_info),
      domain_mapping(domain_abstraction.extract_domain_mapping()),
      numeric_domain_mapping(domain_abstraction.extract_numeric_domain_mapping()) {
    
    // Compute domain_sizes and numeric_domain_sizes for helper
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
        if (numeric_domain_mapping[var_id]->get_num_partitions() != 0) {
            numeric_domain_sizes[var_id] = numeric_domain_mapping[var_id]->get_num_partitions();
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
        if (numeric_domain_mapping[var_id]->get_num_partitions() != 0) {
            int max_val = numeric_domain_mapping[var_id]->get_num_partitions();
            assert(max_val > 0); // Variable is non-trivial.
            int pattern_var_id = var_id + domain_mapping.size();
            pattern.push_back(pattern_var_id);
            pattern_domain_sizes.push_back(max_val);
        }
    }

    assert(utils::is_sorted_unique(pattern));

    VariablesProxy variables = task_proxy.get_variables();
    NumericVariablesProxy numeric_variables = task_proxy.get_numeric_variables();

    vector<int> variable_to_pattern_index(variables.size(), -1);
    vector<int> numeric_variable_to_pattern_index(numeric_variables.size(), -1);
    for (size_t i = 0; i < pattern.size(); ++i) {
        int var_id = pattern[i];
        if (var_id < static_cast<int>(variables.size())) {
            variable_to_pattern_index[var_id] = i;
        } else {
            numeric_variable_to_pattern_index[var_id - variables.size()] = i;
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

    // Instantiate DomainAbstractionNumericHelper
    domain_abstractions::DomainAbstractionNumericHelper helper(
        g_root_task(),
        domain_mapping,
        numeric_domain_mapping,
        domain_sizes,
        numeric_domain_sizes,
        hash_multipliers
    );
    
    vector<domain_abstractions::AbstractOperator> abstract_operators = helper.build_abstract_operators(task_proxy);
    
    OperatorGroups operator_groups;
    if (combine_labels) {
        operator_groups = group_equivalent_operators(
            abstract_operators, variable_to_pattern_index, numeric_variable_to_pattern_index,
            domain_mapping, numeric_domain_mapping);
    } else {
        operator_groups = get_singleton_operator_groups(
            abstract_operators, variable_to_pattern_index, numeric_variable_to_pattern_index,
            domain_mapping, numeric_domain_mapping);
    }

    int num_ops_covered_by_labels = 0;
    for (const auto &group : operator_groups) {
        num_ops_covered_by_labels += group.operator_ids.size();
    }
    label_to_operators.reserve(operator_groups.size(), num_ops_covered_by_labels);

    for (OperatorGroup &group : operator_groups) {
        int label_id = label_to_operators.size();
        label_to_operators.push_back(move(group.operator_ids));
        
        int precondition_hash = 0;
        for (const Fact &f : group.regression_preconditions) {
            precondition_hash += hash_multipliers[f.var] * f.value;
        }
        
        // source_hash = target_hash - hash_effect
        int source_hash = precondition_hash - group.hash_effect;
        
        ranked_operators.emplace_back(label_id, source_hash, group.hash_effect);
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
        });

    vector<ap_float> saturated_costs(num_operators, -INF);
    /* To prevent negative cost cycles, we ensure that all operators inducing
       self-loops (among possibly other transitions) have non-negative costs. */
    for (int op_id = 0; op_id < num_operators; ++op_id) {
        if (operator_induces_self_loop(op_id)) {
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
    assert(all_of(operator_costs.begin(), operator_costs.end(), [](ap_float c) {return c >= 0;}));

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

    // Initialize queue.
    AdaptiveQueue<int> pq;
    for (int goal : goal_states) {
        pq.push(0, goal);
        distances[goal] = 0;
    }

    // Reuse vector to save allocations.
    vector<int> applicable_operators;

    // Run Dijkstra loop.
    while (!pq.empty()) {
        pair<ap_float, int> node = pq.pop();
        ap_float distance = node.first;
        int state_index = node.second;
        assert(utils::in_bounds(state_index, distances));
        if (distance > distances[state_index]) {
            continue;
        }

        // Regress abstract state.
        applicable_operators.clear();
        match_tree_backward->get_applicable_operator_ids(
            state_index, applicable_operators);
        for (int ranked_op_id : applicable_operators) {
            const RankedOperator &op = ranked_operators[ranked_op_id];
            int predecessor = state_index - op.hash_effect;
            assert(utils::in_bounds(op.label, label_costs));
            ap_float alternative_cost = (label_costs[op.label] == INF) ?
                INF : distances[state_index] + label_costs[op.label];
            assert(utils::in_bounds(predecessor, distances));
            if (alternative_cost < distances[predecessor]) {
                distances[predecessor] = alternative_cost;
                pq.push(alternative_cost, predecessor);
            }
        }
    }
//    if (log.is_at_least_debug()) {
//        log << "distances: " << distances << endl;
//    }
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
}
