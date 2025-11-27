#include "domain_abstraction.h"

#include "types.h"

#include "../task_proxy.h"

#include "../priority_queue.h"
#include "../domain_abstractions/domain_abstraction.h"
#include "../domain_abstractions/match_tree.h"
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
    vector<Fact> preconditions;
    vector<Fact> effects;
    vector<NumericEffect> numeric_effects;
    vector<int> operator_ids;

    OperatorGroup() = default;
    OperatorGroup(const OperatorGroup&) = default;
    OperatorGroup(OperatorGroup&&) = default;
    OperatorGroup& operator=(const OperatorGroup&) = default;
    OperatorGroup& operator=(OperatorGroup&&) = default;
    ~OperatorGroup() = default;

    bool operator<(const OperatorGroup &other) const {
        if (preconditions < other.preconditions) return true;
        if (other.preconditions < preconditions) return false;
        if (effects < other.effects) return true;
        if (other.effects < effects) return false;
        if (numeric_effects < other.numeric_effects) return true;
        if (other.numeric_effects < numeric_effects) return false;
        return operator_ids < other.operator_ids;
    }
};

using OperatorKey = tuple<vector<Fact>, vector<Fact>, vector<NumericEffect>>;
using OperatorIDsByPreEffMap = std::map<OperatorKey, vector<int>>;
using OperatorGroups = vector<OperatorGroup>;

// Custom hash function for OperatorKey to use unordered_map
struct OperatorKeyHash {
    size_t operator()(const OperatorKey& key) const {
        size_t h = 0;
        for (const auto& fact : get<0>(key)) {
            h ^= std::hash<int>()(fact.var) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>()(fact.value) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        for (const auto& fact : get<1>(key)) {
            h ^= std::hash<int>()(fact.var) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>()(fact.value) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        for (const auto& ne : get<2>(key)) {
            h ^= std::hash<int>()(ne.var) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>()(static_cast<int>(ne.op)) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>()(ne.operand_var) + 0x9e3779b9 + (h << 6) + (h >> 2);
            // Hash the double value as bits
            uint64_t bits;
            std::memcpy(&bits, &ne.value, sizeof(bits));
            h ^= std::hash<uint64_t>()(bits) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
    }
};

using OperatorIDsByPreEffHashMap = std::unordered_map<OperatorKey, vector<int>, OperatorKeyHash>;

static OperatorGroups group_equivalent_operators(
    const TaskProxy &task_proxy,
    const vector<int> &variable_to_pattern_index,
    const vector<int> &numeric_variable_to_pattern_index,
    const domain_abstractions::DomainMapping &domain_mapping,
    const domain_abstractions::NumericDomainMappingType &numeric_domain_mapping) {
    // Use unordered_map to avoid tree-based comparison issues with NumericEffect
    OperatorIDsByPreEffHashMap grouped_operator_ids;
    // Reuse vectors to save allocations.
    vector<Fact> preconditions;
    vector<Fact> effects;
    vector<NumericEffect> numeric_effects;
    for (OperatorProxy op : task_proxy.get_operators()) {
        /* Skip operators that only induce self-loops. They can be queried
           with operator_induces_self_loop(). */
        effects.clear();
        for (EffectProxy eff : op.get_effects()) {
            Fact e(eff.get_fact().get_variable().get_id(), eff.get_fact().get_value());
            if (e.var < 0 || e.var >= static_cast<int>(variable_to_pattern_index.size())) continue;
            int mapped_var = variable_to_pattern_index[e.var];
            if (mapped_var != -1) {
                assert(e.var >= 0 && e.var < static_cast<int>(domain_mapping.size()));
                assert(e.value >= 0 && e.value < static_cast<int>(domain_mapping[e.var].size()));
                effects.emplace_back(mapped_var, domain_mapping[e.var][e.value]);
            }
        }
        
        numeric_effects.clear();
        for (AssEffectProxy eff : op.get_ass_effects()) {
            NumAssProxy assignment = eff.get_assignment();
            int aff_var = assignment.get_affected_variable().get_id();
            if (aff_var >= static_cast<int>(numeric_variable_to_pattern_index.size())) continue;
            int mapped_var = numeric_variable_to_pattern_index[aff_var];
            if (mapped_var != -1) {
                int ass_var = assignment.get_assigned_variable().get_id();
                // Check if ass_var is constant
                // For now, assume constant if ass_var is not in pattern? No.
                // We need to know if it's a constant value.
                // If ass_var maps to ConstantMapping, we can get the value.
                // If ass_var is not in pattern, we can't easily check mapping unless we have mapping for all vars.
                // Assuming numeric_domain_mapping covers all numeric vars.
                if (ass_var < static_cast<int>(numeric_domain_mapping.size())) {
                    const auto *const_mapping = dynamic_cast<const domain_abstractions::ConstantMapping*>(numeric_domain_mapping[ass_var].get());
                    if (const_mapping) {
                        numeric_effects.push_back({mapped_var, assignment.get_assigment_operator_type(), const_mapping->get_constant_value(), -1});
                    } else {
                        // Variable operand
                        int operand_mapped_var = numeric_variable_to_pattern_index[ass_var];
                        if (operand_mapped_var != -1) {
                             numeric_effects.push_back({mapped_var, assignment.get_assigment_operator_type(), 0.0, operand_mapped_var});
                        } else {
                            // Operand is not in pattern. Treat as unknown?
                            // For now, ignore or handle conservatively.
                        }
                    }
                }
            }
        }

        if (effects.empty() && numeric_effects.empty()) {
            continue;
        }
        sort(effects.begin(), effects.end());
        sort(numeric_effects.begin(), numeric_effects.end());

        preconditions.clear();
        for (FactProxy fact : op.get_preconditions()) {
            Fact p(fact.get_variable().get_id(), fact.get_value());
            if (p.var < 0 || p.var >= static_cast<int>(variable_to_pattern_index.size())) continue;
            int mapped_var = variable_to_pattern_index[p.var];
            if (mapped_var != -1) {
                assert(p.var >= 0 && p.var < static_cast<int>(domain_mapping.size()));
                assert(p.value >= 0 && p.value < static_cast<int>(domain_mapping[p.var].size()));
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

        grouped_operator_ids[make_tuple(move(preconditions), move(effects), move(numeric_effects))].push_back(op.get_id());
    }
    
    // Copy all entries out of the map first to avoid any iterator issues
    std::vector<std::pair<OperatorKey, vector<int>>> all_entries;
    all_entries.reserve(grouped_operator_ids.size());
    for (auto &entry : grouped_operator_ids) {
        all_entries.emplace_back(entry.first, entry.second);
    }
    grouped_operator_ids.clear();  // Release map memory
    
    OperatorGroups groups;
    groups.reserve(all_entries.size());
    std::cerr << "DEBUG: all_entries has " << all_entries.size() << " entries" << std::endl;
    int entry_count = 0;
    for (auto &entry : all_entries) {
        std::cerr << "DEBUG: Entry " << entry_count << ", operator_ids size: " << entry.second.size() << std::endl;
        if (entry.second.size() > 0) {
            std::cerr << "  First few IDs: ";
            for (size_t i = 0; i < std::min(entry.second.size(), (size_t)5); ++i) {
                std::cerr << entry.second[i] << " ";
            }
            std::cerr << std::endl;
        }
        ++entry_count;
        
        if (entry.second.empty()) {
            std::cerr << "WARNING: Skipping empty operator group" << std::endl;
            continue;
        }
        
        auto &key = entry.first;
        OperatorGroup group;
        group.preconditions = get<0>(key);
        group.effects = get<1>(key);
        group.numeric_effects = get<2>(key);
        group.operator_ids = entry.second;
        group.operator_ids = entry.second;   // Copy instead of move
        if (!utils::is_sorted_unique(group.operator_ids)) {
            std::cerr << "ERROR: operator_ids not sorted unique! IDs: ";
            for (int id : group.operator_ids) std::cerr << id << " ";
            std::cerr << std::endl;
        }
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
    const vector<int> &numeric_variable_to_pattern_index,
    const domain_abstractions::DomainMapping &domain_mapping,
    const domain_abstractions::NumericDomainMappingType &numeric_domain_mapping) {
    OperatorGroups groups;
    for (OperatorProxy op : task_proxy.get_operators()) {
        OperatorGroup group;
        vector<int> pre_vals(variable_to_pattern_index.size(), -1);
        group.preconditions.reserve(op.get_preconditions().size());
        for (FactProxy pre : op.get_preconditions()) {
            Fact p(pre.get_variable().get_id(), pre.get_value());
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
            Fact e(eff.get_fact().get_variable().get_id(), eff.get_fact().get_value());
            int mapped_var = variable_to_pattern_index[e.var];
            if (mapped_var != -1) {
                int mapped_val = domain_mapping[e.var][e.value];
                if (mapped_val != pre_vals[e.var]) {
                    group.effects.emplace_back(mapped_var, mapped_val);
                }
            }
        }

        for (AssEffectProxy eff : op.get_ass_effects()) {
            NumAssProxy assignment = eff.get_assignment();
            int aff_var = assignment.get_affected_variable().get_id();
            if (aff_var >= static_cast<int>(numeric_variable_to_pattern_index.size())) continue;
            int mapped_var = numeric_variable_to_pattern_index[aff_var];
            if (mapped_var != -1) {
                int ass_var = assignment.get_assigned_variable().get_id();
                if (ass_var < static_cast<int>(numeric_domain_mapping.size())) {
                    const auto *const_mapping = dynamic_cast<const domain_abstractions::ConstantMapping*>(numeric_domain_mapping[ass_var].get());
                    if (const_mapping) {
                        group.numeric_effects.push_back({mapped_var, assignment.get_assigment_operator_type(), const_mapping->get_constant_value(), -1});
                    } else {
                        int operand_mapped_var = numeric_variable_to_pattern_index[ass_var];
                        if (operand_mapped_var != -1) {
                             group.numeric_effects.push_back({mapped_var, assignment.get_assigment_operator_type(), 0.0, operand_mapped_var});
                        }
                    }
                }
            }
        }
        sort(group.numeric_effects.begin(), group.numeric_effects.end());

        if (group.effects.empty() && group.numeric_effects.empty()) {
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
    if (false) {
        // task_properties::dump_task(task_proxy);
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

    if (false) {
        log << "domain mapping: " << domain_mapping << endl;
        log << "pattern: " << pattern << endl;
        log << "pattern domain sizes: " << pattern_domain_sizes << endl;
        log << "looping operators: " << looping_operators << endl;
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
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        }
    }
    if (false) {
        log << "hash multipliers: " << hash_multipliers << endl;
        log << "num states: " << num_states << endl;
    }
    assert(num_states == domain_abstraction.size());

    abstraction_function = utils::make_unique_ptr<DomainAbstractionFunction>(
        pattern, hash_multipliers, domain_mapping, numeric_domain_mapping);

    match_tree_backward = utils::make_unique_ptr<domain_abstractions::MatchTree>(
        pattern_domain_sizes, hash_multipliers);

    OperatorGroups operator_groups;
    if (combine_labels) {
        operator_groups = group_equivalent_operators(
            task_proxy, variable_to_pattern_index, numeric_variable_to_pattern_index, domain_mapping, numeric_domain_mapping);
    } else {
        operator_groups = get_singleton_operator_groups(
            task_proxy, variable_to_pattern_index, numeric_variable_to_pattern_index, domain_mapping, numeric_domain_mapping);
    }
    int num_ops_covered_by_labels = 0;
    for (const auto &group : operator_groups) {
        num_ops_covered_by_labels += group.operator_ids.size();
    }
    label_to_operators.reserve(operator_groups.size(), num_ops_covered_by_labels);
    for (OperatorGroup &group : operator_groups) {
        const vector<Fact> &preconditions = group.preconditions;
        const vector<Fact> &effects = group.effects;
        const vector<NumericEffect> &numeric_effects = group.numeric_effects;

        int label_id = label_to_operators.size();
        if (false) {
            log << "label: " << label_id << ", preconditions: " << preconditions
            << ", effects: " << effects << ", op ids: " << group.operator_ids
            << endl;
        }
        label_to_operators.push_back(move(group.operator_ids));

        build_ranked_operators(
            preconditions, effects, numeric_effects, pattern.size(),
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
    if (false) {
        for (const auto &op : ranked_operators) {
            log << "label: " << op.label << ", pre: " << op.precondition_hash << ", effect: " << op.hash_effect << endl;
        }
    }

    goal_states = compute_goal_states(variable_to_pattern_index);
    if (false) {
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
                              const vector<NumericEffect> &numeric_effects_without_pre,
                              const OperatorCallback &callback,
                              utils::Log &log) const {
    if (false) {
        log << "recursive call" << endl;
        log << prev_pairs << endl;
        log << pre_pairs << endl;
        log << eff_pairs << endl;
        log << effects_without_pre << endl;
    }
    if (pos < static_cast<int>(effects_without_pre.size())) {
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
                         effects_without_pre, numeric_effects_without_pre, callback, log);
            if (i != eff) {
                pre_pairs.pop_back();
                eff_pairs.pop_back();
            } else {
                prev_pairs.pop_back();
            }
        }
    } else {
        int num_pos = pos - effects_without_pre.size();
        if (num_pos < static_cast<int>(numeric_effects_without_pre.size())) {
            const NumericEffect &eff = numeric_effects_without_pre[num_pos];
            int var_id = eff.var;
            
            int pre_val = -1;
            for (const Fact &f : pre_pairs) { if (f.var == var_id) { pre_val = f.value; break; } }
            if (pre_val == -1) {
                for (const Fact &f : prev_pairs) { if (f.var == var_id) { pre_val = f.value; break; } }
            }
            
            int start_val = 0;
            int end_val = pattern_domain_sizes[var_id];
            if (pre_val != -1) {
                start_val = pre_val;
                end_val = pre_val + 1;
            }
            
            int num_var_id = var_id - domain_mapping.size();
            
            for (int i = start_val; i < end_val; ++i) {
                vector<int> targets;
                if (eff.operand_var == -1) {
                    targets = numeric_domain_mapping[num_var_id]->apply_effect_to_partition(i, eff.op, eff.value);
                } else {
                    // Variable operand. We need the value of the operand variable.
                    // The operand variable must be in the pattern and have a value assigned in this branch.
                    // Check pre_pairs/prev_pairs for operand_var.
                    int operand_val = -1;
                    for (const Fact &f : pre_pairs) { if (f.var == eff.operand_var) { operand_val = f.value; break; } }
                    if (operand_val == -1) {
                        for (const Fact &f : prev_pairs) { if (f.var == eff.operand_var) { operand_val = f.value; break; } }
                    }
                    
                    if (operand_val != -1) {
                        // We have the partition index of the operand.
                        // We need to apply binary operation on partitions.
                        // But apply_effect_to_partition takes float operand.
                        // We need apply_effect_to_partition(source, op, partition_index).
                        // NumericDomainMapping doesn't seem to have that directly.
                        // It has evaluate_partition_comparison.
                        // It has apply_binary_operation for partitions?
                        // Partition::apply_binary_operation(left, right, op).
                        // But we are applying an effect (increase/assign).
                        // If op is assign, it's just the operand partition.
                        // If op is increase, it's source + operand.
                        // We need to implement this logic or assume constant for now.
                        // For now, let's assume all targets are possible if variable operand (fallback).
                        // Or skip.
                        // Let's skip variable operands for now to avoid crash/complexity.
                        targets.push_back(i); // Identity fallback
                    } else {
                        // Operand value unknown.
                        targets.push_back(i); // Identity fallback
                    }
                }

                for (int target : targets) {
                    bool pushed_pre = false;
                    bool pushed_eff = false;
                    bool pushed_prev = false;

                    if (pre_val == -1) {
                        if (i != target) {
                            pre_pairs.emplace_back(var_id, i);
                            eff_pairs.emplace_back(var_id, target);
                            pushed_pre = true;
                            pushed_eff = true;
                        } else {
                            prev_pairs.emplace_back(var_id, i);
                            pushed_prev = true;
                        }
                    } else {
                        if (i != target) {
                            eff_pairs.emplace_back(var_id, target);
                            pushed_eff = true;
                        }
                    }
                    
                    multiply_out(pos + 1, prev_pairs, pre_pairs, eff_pairs,
                                 effects_without_pre, numeric_effects_without_pre, callback, log);
                    
                    if (pushed_pre) pre_pairs.pop_back();
                    if (pushed_eff) eff_pairs.pop_back();
                    if (pushed_prev) prev_pairs.pop_back();
                }
            }
        } else {
            // All effects checked.
            if (!eff_pairs.empty()) {
                callback(prev_pairs, pre_pairs, eff_pairs, hash_multipliers);
            }
        }
    }
}

void DomainAbstraction::build_ranked_operators(
    const vector<Fact> &preconditions,
    const vector<Fact> &effects,
    const vector<NumericEffect> &numeric_effects,
    int num_vars,
    const OperatorCallback &callback,
    utils::Log &log) const {
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
    vector<NumericEffect> numeric_effects_without_pre;

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
    
    for (const NumericEffect &eff : numeric_effects) {
        int var_id = eff.var;
        assert(utils::in_bounds(var_id, pattern));
        if (has_precondition_on_var[var_id]) {
            has_precond_and_effect_on_var[var_id] = true;
        }
        numeric_effects_without_pre.push_back(eff);
    }

    for (Fact pre : preconditions) {
        if (has_precond_and_effect_on_var[pre.var]) {
            pre_pairs.emplace_back(pre.var, pre.value);
        } else {
            prev_pairs.emplace_back(pre.var, pre.value);
        }
    }
    multiply_out(0, prev_pairs, pre_pairs, eff_pairs,
                 effects_without_pre, numeric_effects_without_pre, callback, log);
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
    HeapQueue<int> pq;
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
