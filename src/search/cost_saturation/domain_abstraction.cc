#include "domain_abstraction.h"

#include "types.h"

#include "../task_proxy.h"

#include "../algorithms/priority_queues.h"
#include "../domain_abstractions/domain_abstraction.h"
#include "../domain_abstractions/match_tree_with_pattern.h"
#include "../task_utils/task_properties.h"
#include "../utils/collections.h"
#include "../utils/logging.h"
#include "../utils/math.h"
#include "../utils/memory.h"

#include <cassert>
#include <unordered_map>

using namespace std;

namespace cost_saturation {
static vector<int> get_abstract_preconditions(
    const vector<Fact> &prev_pairs,
    const vector<Fact> &pre_pairs,
    const vector<int> &hash_multipliers) {
    vector<int> abstract_preconditions(hash_multipliers.size(), -1);
    for (const Fact &fact : prev_pairs) {
        int pattern_index = fact.var;
        abstract_preconditions[pattern_index] = fact.value;
    }
    for (const Fact &fact : pre_pairs) {
        int pattern_index = fact.var;
        abstract_preconditions[pattern_index] = fact.value;
    }
    return abstract_preconditions;
}

static int compute_hash_effect(
    const vector<Fact> &preconditions,
    const vector<Fact> &effects,
    const vector<int> &hash_multipliers) {
    int hash_effect = 0;
    assert(preconditions.size() == effects.size());
    for (size_t i = 0; i < preconditions.size(); ++i) {
        int var = preconditions[i].var;
        assert(var == effects[i].var);
        int old_val = preconditions[i].value;
        int new_val = effects[i].value;
        assert(old_val != -1);
        int effect = (new_val - old_val) * hash_multipliers[var];
        hash_effect += effect;
    }
    assert(hash_effect != 0);
    return hash_effect;
}

static bool variable_is_trivial(
    int var_id, const domain_abstractions::DomainMapping &domain_mapping) {
    return domain_mapping[var_id].empty();
}

static vector<bool> compute_looping_operators(
    const TaskProxy &task_proxy,
    const domain_abstractions::DomainMapping &domain_mapping) {
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
            const Fact pre = precondition.get_pair();
            if (!variable_is_trivial(pre.var, domain_mapping)) {
                var_to_precondition[pre.var] =
                    domain_mapping[pre.var][pre.value];
            }
        }
        for (EffectProxy effect : op.get_effects()) {
            const Fact eff = effect.get_fact().get_pair();
            if (var_to_precondition.count(eff.var) > 0
                && !variable_is_trivial(eff.var, domain_mapping)
                && var_to_precondition[eff.var]
                   != domain_mapping[eff.var][eff.value]) {
                loops[op_id] = false;
                break;
            }
        }
    }
    return loops;
}

struct OperatorGroup {
    vector<Fact> preconditions;
    vector<Fact> effects;
    vector<int> operator_ids;

    bool operator<(const OperatorGroup &other) const {
        return operator_ids < other.operator_ids;
    }
};

using OperatorIDsByPreEffMap = utils::HashMap<pair<vector<Fact>, vector<Fact>>, vector<int>>;
using OperatorGroups = vector<OperatorGroup>;

static OperatorGroups group_equivalent_operators(
    const TaskProxy &task_proxy,
    const vector<int> &variable_to_pattern_index,
    const domain_abstractions::DomainMapping &domain_mapping) {
    OperatorIDsByPreEffMap grouped_operator_ids;
    // Reuse vectors to save allocations.
    vector<Fact> preconditions;
    vector<Fact> effects;
    for (OperatorProxy op : task_proxy.get_operators()) {
        /* Skip operators that only induce self-loops. They can be queried
           with operator_induces_self_loop(). */
        effects.clear();
        for (EffectProxy eff : op.get_effects()) {
            Fact e = eff.get_fact().get_pair();
            int mapped_var = variable_to_pattern_index[e.var];
            if (mapped_var != -1) {
                effects.emplace_back(mapped_var, domain_mapping[e.var][e.value]);
            }
        }
        if (effects.empty()) {
            continue;
        }
        sort(effects.begin(), effects.end());

        preconditions.clear();
        for (FactProxy fact : op.get_preconditions()) {
            Fact p = fact.get_pair();
            int mapped_var = variable_to_pattern_index[p.var];
            if (mapped_var != -1) {
                preconditions.emplace_back(mapped_var, domain_mapping[p.var][p.value]);
            }
        }
        sort(preconditions.begin(), preconditions.end());

        // Silvan: Search for equal pre/eff variables and remove the eff so
        // that it is a prevail condition, i.e., pre without eff.
        auto pre_it = preconditions.begin();
        auto eff_it = effects.begin();
        while (pre_it != preconditions.end() && eff_it != effects.end()) {
            if (pre_it->var < eff_it->var) {
                ++pre_it;
            } else if (eff_it->var < pre_it->var) {
                ++eff_it;
            } else {
                if (pre_it->value == eff_it->value) {
                    eff_it = effects.erase(eff_it);
                } else {
                    ++eff_it;
                }
                ++pre_it;
            }
        }

        grouped_operator_ids[make_pair(move(preconditions), move(effects))].push_back(op.get_id());
    }
    OperatorGroups groups;
    for (auto &entry : grouped_operator_ids) {
        auto &pre_eff = entry.first;
        OperatorGroup group;
        group.preconditions = move(pre_eff.first);
        group.effects = move(pre_eff.second);
        group.operator_ids = move(entry.second);
        assert(utils::is_sorted_unique(group.operator_ids));
        groups.push_back(move(group));
    }
    // Sort by first operator ID for better cache locality.
    sort(groups.begin(), groups.end());
    return groups;
}

static OperatorGroups get_singleton_operator_groups(
    const TaskProxy &task_proxy,
    const vector<int> &variable_to_pattern_index,
    const domain_abstractions::DomainMapping &domain_mapping) {
    OperatorGroups groups;
    for (OperatorProxy op : task_proxy.get_operators()) {
        OperatorGroup group;
        vector<int> pre_vals(variable_to_pattern_index.size(), -1);
        group.preconditions.reserve(op.get_preconditions().size());
        for (FactProxy pre : op.get_preconditions()) {
            Fact p = pre.get_pair();
            int mapped_var = variable_to_pattern_index[p.var];
            if (mapped_var != -1) {
                int mapped_val = domain_mapping[p.var][p.value];
                pre_vals[p.var] = mapped_val;
                group.preconditions.emplace_back(mapped_var, mapped_val);
            }
        }
        sort(group.preconditions.begin(), group.preconditions.end());

        group.effects.reserve(op.get_effects().size());
        for (EffectProxy eff : op.get_effects()) {
            Fact e = eff.get_fact().get_pair();
            int mapped_var = variable_to_pattern_index[e.var];
            if (mapped_var != -1) {
                int mapped_val = domain_mapping[e.var][e.value];
                if (mapped_val != pre_vals[e.var]) {
                    group.effects.emplace_back(mapped_var, mapped_val);
                }
            }
        }
        if (group.effects.empty()) {
            continue;
        }

        sort(group.effects.begin(), group.effects.end());
        group.operator_ids = {op.get_id()};
        groups.push_back(move(group));
    }
    return groups;
}


DomainAbstractionFunction::DomainAbstractionFunction(
    const pdbs::Pattern &pattern,
    const vector<int> &hash_multipliers,
    const domain_abstractions::DomainMapping domain_mapping)
    : domain_mapping(move(domain_mapping)) {
    assert(pattern.size() == hash_multipliers.size());
    variables_and_multipliers.reserve(pattern.size());
    for (size_t i = 0; i < pattern.size(); ++i) {
        variables_and_multipliers.emplace_back(pattern[i], hash_multipliers[i]);
    }
}

int DomainAbstractionFunction::get_abstract_state_id(const State &concrete_state) const {
    int index = 0;
    for (const VariableAndMultiplier &pair : variables_and_multipliers) {
        index += pair.hash_multiplier * domain_mapping[pair.pattern_var][concrete_state[pair.pattern_var].get_value()];
    }
    return index;
}


DomainAbstraction::DomainAbstraction(
    const TaskProxy &task_proxy,
    const std::shared_ptr<TaskInfo> &task_info,
    domain_abstractions::DomainAbstraction &domain_abstraction,
    bool combine_labels,
    utils::LogProxy &log)
    : Abstraction(nullptr),
      task_info(task_info),
      domain_mapping(domain_abstraction.extract_domain_mapping()) {
    if (log.is_at_least_debug()) {
        task_properties::dump_task(task_proxy);
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
    assert(utils::is_sorted_unique(pattern));

    looping_operators = compute_looping_operators(task_proxy, domain_mapping);

    if (log.is_at_least_debug()) {
        log << "domain mapping: " << domain_mapping << endl;
        log << "pattern: " << pattern << endl;
        log << "pattern domain sizes: " << pattern_domain_sizes << endl;
        log << "looping operators: " << looping_operators << endl;
    }

    VariablesProxy variables = task_proxy.get_variables();
    vector<int> variable_to_pattern_index(variables.size(), -1);
    for (size_t i = 0; i < pattern.size(); ++i) {
        variable_to_pattern_index[pattern[i]] = i;
    }

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
            utils::exit_with(utils::ExitCode::SEARCH_CRITICAL_ERROR);
        }
    }
    if (log.is_at_least_debug()) {
        log << "hash multipliers: " << hash_multipliers << endl;
        log << "num states: " << num_states << endl;
    }
    assert(num_states == domain_abstraction.size());

    abstraction_function = utils::make_unique_ptr<DomainAbstractionFunction>(
        pattern, hash_multipliers, domain_mapping);

    match_tree_backward = utils::make_unique_ptr<domain_abstractions::MatchTreeWithPattern>(
        pattern_domain_sizes, hash_multipliers);

    OperatorGroups operator_groups;
    if (combine_labels) {
        operator_groups = group_equivalent_operators(
            task_proxy, variable_to_pattern_index, domain_mapping);
    } else {
        operator_groups = get_singleton_operator_groups(
            task_proxy, variable_to_pattern_index, domain_mapping);
    }
    int num_ops_covered_by_labels = 0;
    for (const auto &group : operator_groups) {
        num_ops_covered_by_labels += group.operator_ids.size();
    }
    label_to_operators.reserve(operator_groups.size(), num_ops_covered_by_labels);
    for (OperatorGroup &group : operator_groups) {
        const vector<Fact> &preconditions = group.preconditions;
        const vector<Fact> &effects = group.effects;

        int label_id = label_to_operators.size();
        if (log.is_at_least_debug()) {
            log << "label: " << label_id << ", preconditions: " << preconditions
            << ", effects: " << effects << ", op ids: " << group.operator_ids
            << endl;
        }
        label_to_operators.push_back(move(group.operator_ids));

        build_ranked_operators(
            preconditions, effects, pattern.size(),
            [this, label_id](
                const vector<Fact> &prevail,
                const vector<Fact> &preconditions_,
                const vector<Fact> &effects_,
                const vector<int> &hash_multipliers_) {
                vector<Fact> regression_preconditions = prevail;
                regression_preconditions.insert(
                    regression_preconditions.end(), effects_.begin(), effects_.end());
                sort(regression_preconditions.begin(), regression_preconditions.end());
                int ranked_op_id = ranked_operators.size();
                match_tree_backward->insert(ranked_op_id, regression_preconditions);

                vector<int> abstract_preconditions = get_abstract_preconditions(
                    prevail, preconditions_, hash_multipliers_);
                int precondition_hash = 0;
                for (size_t pos = 0; pos < hash_multipliers_.size(); ++pos) {
                    int pre_val = abstract_preconditions[pos];
                    if (pre_val != -1) {
                        precondition_hash += hash_multipliers_[pos] * pre_val;
                    }
                }

                ranked_operators.emplace_back(
                    label_id,
                    precondition_hash,
                    compute_hash_effect(preconditions_, effects_, hash_multipliers_));
            },
            log);
    }
    ranked_operators.shrink_to_fit();
    if (log.is_at_least_debug()) {
        for (const auto &op : ranked_operators) {
            log << "label: " << op.label << ", pre: " << op.precondition_hash << ", effect: " << op.hash_effect << endl;
        }
    }

    goal_states = compute_goal_states(variable_to_pattern_index);
    if (log.is_at_least_debug()) {
        log << "goal states: " << goal_states << endl;
    }
}

DomainAbstraction::~DomainAbstraction() {
}

bool DomainAbstraction::increment_to_next_state(vector<Fact> &facts) const {
    for (Fact &fact : facts) {
        ++fact.value;
        if (fact.value > pattern_domain_sizes[fact.var] - 1) {
            fact.value = 0;
        } else {
            return true;
        }
    }
    return false;
}

vector<int> DomainAbstraction::compute_goal_states(
    const vector<int> &variable_to_pattern_index) const {
    vector<Fact> abstract_goals;
    for (Fact goal : task_info->get_goals()) {
        int mapped_var = variable_to_pattern_index[goal.var];
        if (mapped_var != -1) {
            abstract_goals.emplace_back(
                mapped_var, domain_mapping[goal.var][goal.value]);
        }
    }

    vector<int> goals;
    for (int state_index = 0; state_index < num_states; ++state_index) {
        if (is_consistent(state_index, abstract_goals)) {
            goals.push_back(state_index);
        }
    }
    return goals;
}

void DomainAbstraction::multiply_out(int pos,
                              vector<Fact> &prev_pairs,
                              vector<Fact> &pre_pairs,
                              vector<Fact> &eff_pairs,
                              const vector<Fact> &effects_without_pre,
                              const OperatorCallback &callback,
                              utils::LogProxy &log) const {
    if (log.is_at_least_debug()) {
        log << "recursive call" << endl;
        log << prev_pairs << endl;
        log << pre_pairs << endl;
        log << eff_pairs << endl;
        log << effects_without_pre << endl;
    }
    if (pos == static_cast<int>(effects_without_pre.size())) {
        // All effects without precondition have been checked.
        if (!eff_pairs.empty()) {
            callback(prev_pairs, pre_pairs, eff_pairs, hash_multipliers);
        }
    } else {
        // For each possible value for the current variable, build an
        // abstract operator.
        int var_id = effects_without_pre[pos].var;
        assert(utils::in_bounds(var_id, pattern));
        int eff = effects_without_pre[pos].value;
        assert(0 <= eff && eff < pattern_domain_sizes[var_id]);
        for (int i = 0; i < pattern_domain_sizes[var_id]; ++i) {
            if (i != eff) {
                pre_pairs.emplace_back(var_id, i);
                eff_pairs.emplace_back(var_id, eff);
            } else {
                prev_pairs.emplace_back(var_id, i);
            }
            multiply_out(pos + 1, prev_pairs, pre_pairs, eff_pairs,
                         effects_without_pre, callback, log);
            if (i != eff) {
                pre_pairs.pop_back();
                eff_pairs.pop_back();
            } else {
                prev_pairs.pop_back();
            }
        }
    }
    if (log.is_at_least_debug()) {
        log << "backtracking" << endl;
    }
}

void DomainAbstraction::build_ranked_operators(
    const vector<Fact> &preconditions,
    const vector<Fact> &effects,
    int num_vars,
    const OperatorCallback &callback,
    utils::LogProxy &log) const {
    /*
      The preconditions and effects are already mapped to the abstract
      variable IDs (determined by the pattern) and the abstract
      domains/values (determined by the domain mapping). This happens in
      the get_*_operator_groups functions.
    */

    // All variable value pairs that are a prevail condition
    vector<Fact> prev_pairs;
    // All variable value pairs that are a precondition (value != -1)
    vector<Fact> pre_pairs;
    // All variable value pairs that are an effect
    vector<Fact> eff_pairs;
    // All variable value pairs that are a precondition (value = -1)
    vector<Fact> effects_without_pre;

    vector<bool> has_precond_and_effect_on_var(num_vars, false);
    vector<bool> has_precondition_on_var(num_vars, false);

    for (Fact pre : preconditions)
        has_precondition_on_var[pre.var] = true;

    for (Fact eff : effects) {
        int var_id = eff.var;
        assert(utils::in_bounds(var_id, pattern));
        int val = eff.value;
        assert(val >= 0 && val < pattern_domain_sizes[var_id]);
        if (has_precondition_on_var[var_id]) {
            has_precond_and_effect_on_var[var_id] = true;
            eff_pairs.emplace_back(var_id, val);
        } else {
            effects_without_pre.emplace_back(var_id, val);
        }
    }
    for (Fact pre : preconditions) {
        if (has_precond_and_effect_on_var[pre.var]) {
            pre_pairs.emplace_back(pre.var, pre.value);
        } else {
            prev_pairs.emplace_back(pre.var, pre.value);
        }
    }
    multiply_out(0, prev_pairs, pre_pairs, eff_pairs,
                 effects_without_pre, callback, log);
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

vector<int> DomainAbstraction::compute_saturated_costs(
    const vector<int> &h_values) const {
    int num_operators = get_num_operators();

    int num_labels = label_to_operators.size();
    vector<int> saturated_label_costs(num_labels, -INF);

    for_each_label_transition(
        [&saturated_label_costs, &h_values](const Transition &t) {
            assert(utils::in_bounds(t.src, h_values));
            assert(utils::in_bounds(t.target, h_values));
            int src_h = h_values[t.src];
            int target_h = h_values[t.target];
            if (src_h == INF || target_h == INF) {
                return;
            }
            int &needed_costs = saturated_label_costs[t.op];
            needed_costs = max(needed_costs, src_h - target_h);
        });

    vector<int> saturated_costs(num_operators, -INF);
    /* To prevent negative cost cycles, we ensure that all operators inducing
       self-loops (among possibly other transitions) have non-negative costs. */
    for (int op_id = 0; op_id < num_operators; ++op_id) {
        if (operator_induces_self_loop(op_id)) {
            saturated_costs[op_id] = 0;
        }
    }

    for (int label_id = 0; label_id < num_labels; ++label_id) {
        int saturated_label_cost = saturated_label_costs[label_id];
        for (int op_id : label_to_operators.get_slice(label_id)) {
            saturated_costs[op_id] = max(saturated_costs[op_id], saturated_label_cost);
        }
    }

    return saturated_costs;
}

int DomainAbstraction::get_num_operators() const {
    return task_info->get_num_operators();
}

vector<int> DomainAbstraction::compute_goal_distances(const vector<int> &operator_costs) const {
    assert(all_of(operator_costs.begin(), operator_costs.end(), [](int c) {return c >= 0;}));

    // TODO: use log
//    if (log.is_at_least_debug()) {
//        log << "computing goal distances for: " << endl;
//        log << "domain mapping: " << domain_mapping << endl;
//        log << "pattern: " << pattern << endl;
//        log << "pattern domain sizes: " << pattern_domain_sizes << endl;
//    }

    // Assign each label the cost of cheapest operator that the label covers.
    int num_labels = label_to_operators.size();
    vector<int> label_costs;
    label_costs.reserve(num_labels);
    for (int label_id = 0; label_id < num_labels; ++label_id) {
        int min_cost = INF;
        for (int op_id : label_to_operators.get_slice(label_id)) {
            min_cost = min(min_cost, operator_costs[op_id]);
        }
        label_costs.push_back(min_cost);
    }

    vector<int> distances(num_states, INF);

    // Initialize queue.
    priority_queues::AdaptiveQueue<int> pq;
    for (int goal : goal_states) {
        pq.push(0, goal);
        distances[goal] = 0;
    }

    // Reuse vector to save allocations.
    vector<int> applicable_operators;

    // Run Dijkstra loop.
    while (!pq.empty()) {
        pair<int, size_t> node = pq.pop();
        int distance = node.first;
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
            int alternative_cost = (label_costs[op.label] == INF) ?
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
