#include "multiplicator.h"

#include "../numeric_pdbs/arithmetic_expression.h"
#include "../numeric_pdbs/numeric_condition.h"
#include "../task_tools.h"

#include <algorithm>
#include <cassert>
#include <queue>
#include <unordered_set>
#include <optional>

using namespace std;
using namespace arithmetic_expression;
using namespace numeric_condition;
using namespace domain_abstractions;

namespace utils {

Multiplicator::Multiplicator(
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
    
    n_numeric_variables = task_proxy.get_numeric_variables().size();
    n_propositional_variables = task_proxy.get_variables().size();
    
    is_derived_num_var.resize(n_numeric_variables, false);
    is_derived_prop_var.resize(n_propositional_variables, false);
    
    axiom_dependencies.resize(n_numeric_variables);
    reverse_axiom_dependencies.resize(n_numeric_variables);
    
    find_derived_variables();
    build_axiom_dependencies();
}

void Multiplicator::find_derived_variables() {
    AssignmentAxiomsProxy assignment_axioms = task_proxy.get_assignment_axioms();
    for (AssignmentAxiomProxy axiom : assignment_axioms) {
        NumericVariableProxy derived_var = axiom.get_assignment_variable();
        int derived_id = derived_var.get_id();
        if (derived_id >= 0 && derived_id < static_cast<int>(is_derived_num_var.size())) {
            is_derived_num_var[derived_id] = true;
        }
    }
    
    ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
    for (ComparisonAxiomProxy axiom : comparison_axioms) {
        FactProxy true_fact = axiom.get_true_fact();
        FactProxy false_fact = axiom.get_false_fact();
        int var_id = true_fact.get_variable().get_id();
        if (var_id >= 0 && var_id < static_cast<int>(is_derived_prop_var.size())) {
            is_derived_prop_var[var_id] = true;
        }
        assert(var_id == false_fact.get_variable().get_id());
    }
}

void Multiplicator::build_axiom_dependencies() {
    AssignmentAxiomsProxy assignment_axioms = task_proxy.get_assignment_axioms();
    for (AssignmentAxiomProxy axiom : assignment_axioms) {
        int derived_id = axiom.get_assignment_variable().get_id();
        int left_id = axiom.get_left_variable().get_id();
        int right_id = axiom.get_right_variable().get_id();
        
        if (derived_id >= 0 && derived_id < static_cast<int>(axiom_dependencies.size())) {
            if (left_id >= 0) {
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
}

void Multiplicator::multiply_out(const OperatorProxy &op, 
                  std::vector<AbstractOperator> &operators) {
    
    vector<Fact> prev_pairs;
    vector<Fact> pre_pairs;
    vector<Fact> eff_pairs;
    vector<Fact> effects_without_pre;

    vector<bool> is_comparison_axiom_var(n_propositional_variables, false);
    ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
    for (ComparisonAxiomProxy axiom : comparison_axioms) {
        is_comparison_axiom_var[axiom.get_true_fact().get_variable().get_id()] = true;
    }

    int num_variables = task_proxy.get_variables().size();
    vector<int> has_precondition_on_var(num_variables, -1);
    vector<int> has_effect_on_var(num_variables, -1);

    for (FactProxy pre : op.get_preconditions()) {
        int var_id = pre.get_variable().get_id();
        if (!domain_mapping[var_id].empty()) {
            int abstract_val = domain_mapping[var_id][pre.get_value()];
            has_precondition_on_var[var_id] = abstract_val;
        } else {
             has_precondition_on_var[var_id] = 0;
        }
    }

    for (EffectProxy eff : op.get_effects()) {
        int var_id = eff.get_fact().get_variable().get_id();
        bool is_var_id_in_comparison_axioms = is_comparison_axiom_var[var_id];
        assert(!is_var_id_in_comparison_axioms);
        
        if (domain_mapping[var_id].empty()) continue;
        
        int val = domain_mapping[var_id][eff.get_fact().get_value()];
        int pre_val = has_precondition_on_var[var_id];
        if (pre_val < 0) {
            effects_without_pre.emplace_back(var_id, val);
        } else if (pre_val != val) {
            has_effect_on_var[var_id] = val;
            eff_pairs.emplace_back(var_id, val);
        }
    }
    
    for (FactProxy pre : op.get_preconditions()) {
        int var_id = pre.get_variable().get_id();
        bool is_var_id_in_comparison_axioms = is_comparison_axiom_var[var_id];
        
        if (domain_mapping[var_id].empty()) continue;
        
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
        bool is_var_id_in_comparison_axioms = is_comparison_axiom_var[var_id];
        
        if (domain_mapping[var_id].empty()) continue;
        
        int val = domain_mapping[var_id][pre.get_value()];
        if (is_var_id_in_comparison_axioms) {
            pre_pairs.emplace_back(var_id, val);
            eff_pairs.emplace_back(var_id, domain_mapping[var_id][2]); // unknown value
        } 
    }
    
    vector<NumAssProxy> ass_effects;
    for (auto ass_eff : op.get_ass_effects()) {
        ass_effects.push_back(ass_eff.get_assignment());
    }
    
    multiply_out_recursive(
        0, op.get_cost(), prev_pairs, pre_pairs, eff_pairs,
        effects_without_pre, ass_effects, op.get_id(), operators, op);
}

void Multiplicator::multiply_out_recursive(
    int pos, ap_float cost, vector<Fact> &prev_pairs,
    vector<Fact> &pre_pairs,
    vector<Fact> &eff_pairs,
    const vector<Fact> &effects_without_pre,
    const vector<NumAssProxy> &ass_effects,
    int concrete_op_id,
    vector<AbstractOperator> &operators,
    const OperatorProxy &op) {
    
    if (pos == static_cast<int>(effects_without_pre.size())) {
        if (!eff_pairs.empty() || !ass_effects.empty()) {
            vector<TransitionInfo> transitions = 
                compute_hash_effects_with_preconditions(pre_pairs, eff_pairs, ass_effects, op);
            
            for (const TransitionInfo &trans : transitions) {
                vector<Fact> extended_pre_pairs = pre_pairs;
                vector<Fact> extended_eff_pairs = eff_pairs;
                vector<Fact> extended_prev_pairs = prev_pairs;

                extended_pre_pairs.insert(extended_pre_pairs.end(),
                                         trans.source_partition_facts.begin(),
                                         trans.source_partition_facts.end());
                extended_eff_pairs.insert(extended_eff_pairs.end(),
                                         trans.target_partition_facts.begin(),
                                         trans.target_partition_facts.end());
                extended_prev_pairs.insert(extended_prev_pairs.end(),
                                         trans.prevail_facts.begin(),
                                         trans.prevail_facts.end());

                operators.emplace_back(
                    extended_prev_pairs,
                    extended_pre_pairs,
                    extended_eff_pairs,
                    cost,
                    hash_multipliers,
                    concrete_op_id
                );                            
            }
        }
    } else {
        int var_id = effects_without_pre[pos].var;
        int eff = effects_without_pre[pos].value;
        
        for (int i = 0; i < this->domain_sizes[var_id]; ++i) {
            if (i != eff) {
                pre_pairs.emplace_back(var_id, i);
                eff_pairs.emplace_back(var_id, eff);
            } else {
                prev_pairs.emplace_back(var_id, i);
            }

            multiply_out_recursive(
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

vector<TransitionInfo> Multiplicator::compute_hash_effects_with_preconditions(
    const vector<Fact> &pre_pairs,
    const vector<Fact> &eff_pairs,
    const vector<NumAssProxy> &ass_effects,
    const OperatorProxy &op) {
    
    vector<TransitionInfo> transitions;
    assert(pre_pairs.size() == eff_pairs.size());
    
    vector<bool> affected_numeric_vars(numeric_domain_mapping.size(), false);
    for (const NumAssProxy &ass_eff : ass_effects) {
        int num_var_id = ass_eff.get_affected_variable().get_id();
        if (numeric_domain_sizes[num_var_id] > 1) {
            affected_numeric_vars[num_var_id] = true;
        } 
    }
    
    function<void(size_t, vector<Fact>&, vector<Fact>&, vector<int>&, vector<int>&, vector<int>&)> enumerate_targets =
        [&](size_t var_idx, 
            vector<Fact> &source_facts, vector<Fact> &target_facts,
            vector<int> &changed_vars, vector<int> &old_parts, vector<int> &new_parts) {
        
        if (var_idx == numeric_domain_mapping.size()) {
            // 1. Build ranges from source_facts
            unordered_map<int, int> partition_assignment;
            for (const Fact &fact : source_facts) {
                int num_var_id = fact.var - domain_sizes.size();
                partition_assignment[num_var_id] = fact.value;
            }
            
            unordered_map<int, NumericRange> ranges;
            NumericVariablesProxy num_vars = task_proxy.get_numeric_variables();
            
            for (size_t nvar_id = 0; nvar_id < num_vars.size(); ++nvar_id) {
                NumericVariableProxy var = num_vars[nvar_id];
                if (var.get_var_type() == numType::constant) {
                    ap_float val = var.get_initial_state_value();
                    ranges[nvar_id] = NumericRange(val, val, true, true);
                } else if (var.get_var_type() == numType::regular) {
                    auto it = partition_assignment.find(nvar_id);
                    if (it != partition_assignment.end()) {
                        const NumericRange *rng = numeric_domain_mapping[nvar_id]->get_range_for_partition(it->second);
                        if (rng) ranges[nvar_id] = *rng;
                    } else if (numeric_domain_sizes[nvar_id] == 1) {
                        ranges[nvar_id] = NumericRange(
                            -numeric_limits<ap_float>::infinity(),
                            numeric_limits<ap_float>::infinity(),
                            false, false
                        );
                    }
                }
            }
            
            // 2. Propagate ranges through assignment axioms
            AssignmentAxiomsProxy assignment_axioms = task_proxy.get_assignment_axioms();
            bool changed = true;
            int iterations = 0;
            const int MAX_ITERATIONS = 100;
            
            while (changed && iterations++ < MAX_ITERATIONS) {
                changed = false;
                for (AssignmentAxiomProxy axiom : assignment_axioms) {
                    int derived_id = axiom.get_assignment_variable().get_id();
                    int ax_left_id = axiom.get_left_variable().get_id();
                    int ax_right_id = axiom.get_right_variable().get_id();
                    
                    bool left_known = false;
                    NumericRange left_range;
                    if (axiom.get_left_variable().get_var_type() == numType::constant) {
                        ap_float val = axiom.get_left_variable().get_initial_state_value();
                        left_range = NumericRange(val, val, true, true);
                        left_known = true;
                    } else if (ranges.count(ax_left_id)) {
                        left_range = ranges[ax_left_id];
                        left_known = true;
                    }
                    
                    bool right_known = false;
                    NumericRange right_range;
                    if (axiom.get_right_variable().get_var_type() == numType::constant) {
                        ap_float val = axiom.get_right_variable().get_initial_state_value();
                        right_range = NumericRange(val, val, true, true);
                        right_known = true;
                    } else if (ranges.count(ax_right_id)) {
                        right_range = ranges[ax_right_id];
                        right_known = true;
                    }
                    
                    if (left_known && right_known) {
                        NumericRange res = NumericDomainMapping::apply_range_operation(
                            left_range, right_range, axiom.get_arithmetic_operator_type());
                        
                        auto it = ranges.find(derived_id);
                        if (it == ranges.end() || 
                            it->second.lower != res.lower || it->second.upper != res.upper ||
                            it->second.lower_inclusive != res.lower_inclusive || it->second.upper_inclusive != res.upper_inclusive) {
                            ranges[derived_id] = res;
                            changed = true;
                        }
                    }
                }
            }
            
            // 3. Check preconditions
            bool satisfies_preconditions = true;
            ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
            
            for (FactProxy concrete_pre : op.get_preconditions()) {
                int var_id = concrete_pre.get_variable().get_id();
                int concrete_val = concrete_pre.get_value();
                
                bool is_comparison_var = false;
                optional<ComparisonAxiomProxy> matching_axiom_opt = std::nullopt;

                for (ComparisonAxiomProxy axiom : comparison_axioms) {
                    if (axiom.get_true_fact().get_variable().get_id() == var_id) {
                        is_comparison_var = true;
                        matching_axiom_opt = axiom;
                        break;
                    }
                }
                
                if (!is_comparison_var) continue;

                ComparisonAxiomProxy matching_axiom = matching_axiom_opt.value();
                int left_id = matching_axiom.get_left_variable().get_id();
                int right_id = matching_axiom.get_right_variable().get_id();
                comp_operator comp_op = matching_axiom.get_comparison_operator_type();
                
                NumericRange left_range;
                if (matching_axiom.get_left_variable().get_var_type() == numType::constant) {
                    ap_float val = matching_axiom.get_left_variable().get_initial_state_value();
                    left_range = NumericRange(val, val, true, true);
                } else if (ranges.count(left_id)) {
                    left_range = ranges[left_id];
                }
                
                NumericRange right_range;
                if (matching_axiom.get_right_variable().get_var_type() == numType::constant) {
                    ap_float val = matching_axiom.get_right_variable().get_initial_state_value();
                    right_range = NumericRange(val, val, true, true);
                } else if (ranges.count(right_id)) {
                    right_range = ranges[right_id];
                }
                
                int eval = NumericDomainMapping::evaluate_comparison(comp_op, left_range, right_range);
                int true_val = matching_axiom.get_true_fact().get_value();
                int required_eval = (concrete_val == true_val) ? 0 : 1;
                
                if (eval != 2 && eval != required_eval) {
                    satisfies_preconditions = false;
                    break;
                }
            }
            
            if (satisfies_preconditions) {
                TransitionInfo trans;
                trans.source_partition_facts = source_facts;
                trans.target_partition_facts = target_facts;
                
                if (!changed_vars.empty()) {
                    vector<Fact> affected_facts = 
                        compute_affected_comparison_axioms(changed_vars, old_parts, new_parts);
                    for (const Fact &f : affected_facts) {
                        trans.source_partition_facts.emplace_back(f.var, 1 - f.value);
                        trans.target_partition_facts.emplace_back(f.var, f.value);
                    }
                    
                    vector<Fact> assignment_cascade_facts = 
                        compute_assignment_axiom_cascades(changed_vars, old_parts, new_parts);
                    for (const Fact &f : assignment_cascade_facts) {
                        trans.source_partition_facts.emplace_back(f.var, 1 - f.value);
                        trans.target_partition_facts.emplace_back(f.var, f.value);
                    }
                }

                transitions.push_back(trans);
            }
            return;
        }
        
        if (affected_numeric_vars[var_idx]) {
            int num_partitions = numeric_domain_sizes[var_idx];
            const NumAssProxy *ass_eff_for_var = nullptr;
            for (const NumAssProxy &ass_eff : ass_effects) {
                if (ass_eff.get_affected_variable().get_id() == static_cast<int>(var_idx)) {
                    ass_eff_for_var = &ass_eff;
                    break;
                }
            }
            
            for (int source_partition = 0; source_partition < num_partitions; ++source_partition) {
                vector<int> reachable_targets;
                if (ass_eff_for_var) {
                    reachable_targets = compute_reachable_partitions(var_idx, source_partition, *ass_eff_for_var);
                } else {
                    // Should not happen
                    reachable_targets.push_back(source_partition);
                }
                
                for (int target_partition : reachable_targets) {
                    int abstract_num_var_id = domain_sizes.size() + var_idx;
                    source_facts.emplace_back(abstract_num_var_id, source_partition);
                    target_facts.emplace_back(abstract_num_var_id, target_partition);
                    
                    changed_vars.push_back(var_idx);
                    old_parts.push_back(source_partition);
                    new_parts.push_back(target_partition);
                    
                    enumerate_targets(var_idx + 1, source_facts, target_facts, changed_vars, old_parts, new_parts);
                    
                    source_facts.pop_back();
                    target_facts.pop_back();
                    changed_vars.pop_back();
                    old_parts.pop_back();
                    new_parts.pop_back();
                }
            }
        } else {
            int num_partitions = numeric_domain_sizes[var_idx];
            if (num_partitions > 1) {
                int abstract_num_var_id = domain_sizes.size() + var_idx;
                for (int p = 0; p < num_partitions; ++p) {
                    source_facts.emplace_back(abstract_num_var_id, p);
                    target_facts.emplace_back(abstract_num_var_id, p);
                    enumerate_targets(var_idx + 1, source_facts, target_facts, changed_vars, old_parts, new_parts);
                    source_facts.pop_back();
                    target_facts.pop_back();
                }
            } else {
                enumerate_targets(var_idx + 1, source_facts, target_facts, changed_vars, old_parts, new_parts);
            }
        }
    };
    
    vector<Fact> source_facts, target_facts;
    vector<int> changed_vars, old_parts, new_parts;
    enumerate_targets(0, source_facts, target_facts, changed_vars, old_parts, new_parts);
    
    return transitions;
}

vector<int> Multiplicator::compute_reachable_partitions(
    int numeric_var_id,
    int source_partition,
    const NumAssProxy &ass_effect) const {
    
    const NumericDomainMapping &mapping = *numeric_domain_mapping[numeric_var_id];
    f_operator op_type = ass_effect.get_assigment_operator_type();
    NumericVariableProxy assigned_var = ass_effect.get_assigned_variable();
    ap_float operand_value = assigned_var.get_initial_state_value();
    return mapping.compute_reachable_partitions(source_partition, op_type, operand_value);
}

vector<Fact> Multiplicator::compute_affected_comparison_axioms(
    const vector<int> &changed_numeric_vars,
    const vector<int> &old_partitions,
    const vector<int> &new_partitions) const {
    
    vector<Fact> affected_facts;
    ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
    for (ComparisonAxiomProxy axiom : comparison_axioms) {
        int left_var_id = axiom.get_left_variable().get_id();
        int right_var_id = axiom.get_right_variable().get_id();
        
        bool depends_on_changed_var = false;
        for (size_t i = 0; i < changed_numeric_vars.size(); ++i) {
            int changed_id = changed_numeric_vars[i];
            if (left_var_id == changed_id || right_var_id == changed_id) {
                depends_on_changed_var = true;
                break;
            }
        }
        
        if (!depends_on_changed_var) continue;
        
        int left_partition_old = -1, left_partition_new = -1;
        int right_partition_old = -1, right_partition_new = -1;
        
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
        
        FactProxy true_fact = axiom.get_true_fact();
        FactProxy false_fact = axiom.get_false_fact();
        int prop_var_id = true_fact.get_variable().get_id();
        
        bool partition_changed = (left_partition_old != left_partition_new || 
                                  right_partition_old != right_partition_new);
        
        if (partition_changed) {
            int eval_left_partition = (left_partition_new != -1) ? left_partition_new : left_partition_old;
            int eval_right_partition = (right_partition_new != -1) ? right_partition_new : right_partition_old;
            
            int eval_result = evaluate_comparison_exactly(axiom, eval_left_partition, eval_right_partition);
            
            if (eval_result == 0) {
                affected_facts.emplace_back(prop_var_id, false_fact.get_value());
            } else if (eval_result == 1) {
                affected_facts.emplace_back(prop_var_id, true_fact.get_value());
            }
        }
    }
    return affected_facts;
}

vector<Fact> Multiplicator::compute_assignment_axiom_cascades(
    const vector<int> &changed_numeric_vars,
    const vector<int> &old_partitions,
    const vector<int> &new_partitions) const {
    
    vector<Fact> affected_facts;
    vector<int> derived_changed_vars;
    vector<int> derived_old_partitions;
    vector<int> derived_new_partitions;
    
    AssignmentAxiomsProxy assignment_axioms = task_proxy.get_assignment_axioms();
    for (AssignmentAxiomProxy axiom : assignment_axioms) {
        int derived_var_id = axiom.get_assignment_variable().get_id();
        int left_var_id = axiom.get_left_variable().get_id();
        int right_var_id = axiom.get_right_variable().get_id();
        cal_operator op = axiom.get_arithmetic_operator_type();
        
        bool depends_on_changed = false;
        for (int changed_id : changed_numeric_vars) {
            if (left_var_id == changed_id || right_var_id == changed_id) {
                depends_on_changed = true;
                break;
            }
        }
        
        if (!depends_on_changed) continue;
        
        int left_partition_old = -1, right_partition_old = -1;
        for (size_t i = 0; i < changed_numeric_vars.size(); ++i) {
            if (changed_numeric_vars[i] == left_var_id) left_partition_old = old_partitions[i];
            if (changed_numeric_vars[i] == right_var_id) right_partition_old = old_partitions[i];
        }
        
        if (left_partition_old == -1 || right_partition_old == -1) continue;
        
        const NumericDomainMapping &left_mapping = *numeric_domain_mapping[left_var_id];
        const NumericDomainMapping &right_mapping = *numeric_domain_mapping[right_var_id];
        
        ap_float left_lower_old, left_upper_old, right_lower_old, right_upper_old;
        bool found_left = false, found_right = false;
        
        for (const auto &r : left_mapping.get_ranges()) {
            if (r.partition_index == left_partition_old) { left_lower_old = r.lower; left_upper_old = r.upper; found_left = true; break; }
        }
        for (const auto &r : right_mapping.get_ranges()) {
            if (r.partition_index == right_partition_old) { right_lower_old = r.lower; right_upper_old = r.upper; found_right = true; break; }
        }
        
        if (!found_left || !found_right) continue;
        
        pair<ap_float, ap_float> old_derived_range = apply_range_operation(left_lower_old, left_upper_old, right_lower_old, right_upper_old, op);
        
        int left_partition_new = -1, right_partition_new = -1;
        for (size_t i = 0; i < changed_numeric_vars.size(); ++i) {
            if (changed_numeric_vars[i] == left_var_id) left_partition_new = new_partitions[i];
            if (changed_numeric_vars[i] == right_var_id) right_partition_new = new_partitions[i];
        }
        
        if (left_partition_new == -1 || right_partition_new == -1) continue;
        
        ap_float left_lower_new, left_upper_new, right_lower_new, right_upper_new;
        found_left = false; found_right = false;
        
        for (const auto &r : left_mapping.get_ranges()) {
            if (r.partition_index == left_partition_new) { left_lower_new = r.lower; left_upper_new = r.upper; found_left = true; break; }
        }
        for (const auto &r : right_mapping.get_ranges()) {
            if (r.partition_index == right_partition_new) { right_lower_new = r.lower; right_upper_new = r.upper; found_right = true; break; }
        }
        
        if (!found_left || !found_right) continue;
        
        pair<ap_float, ap_float> new_derived_range = apply_range_operation(left_lower_new, left_upper_new, right_lower_new, right_upper_new, op);
        
        const NumericDomainMapping &derived_mapping = *numeric_domain_mapping[derived_var_id];
        int old_derived_partition = -1, new_derived_partition = -1;
        
        for (const auto &range : derived_mapping.get_ranges()) {
            if (old_derived_partition == -1 && range.overlaps_with(old_derived_range.first, old_derived_range.second, true, false)) {
                old_derived_partition = range.partition_index;
            }
            if (new_derived_partition == -1 && range.overlaps_with(new_derived_range.first, new_derived_range.second, true, false)) {
                new_derived_partition = range.partition_index;
            }
        }
        
        if (old_derived_partition != -1 && new_derived_partition != -1 && old_derived_partition != new_derived_partition) {
            derived_changed_vars.push_back(derived_var_id);
            derived_old_partitions.push_back(old_derived_partition);
            derived_new_partitions.push_back(new_derived_partition);
        }
    }
    
    if (!derived_changed_vars.empty()) {
        vector<Fact> comparison_facts = compute_affected_comparison_axioms(derived_changed_vars, derived_old_partitions, derived_new_partitions);
        affected_facts.insert(affected_facts.end(), comparison_facts.begin(), comparison_facts.end());
    }
    
    return affected_facts;
}

int Multiplicator::evaluate_comparison_exactly(
    const ComparisonAxiomProxy &axiom,
    int left_partition,
    int right_partition) const {
    
    comp_operator comp_op = axiom.get_comparison_operator_type();
    int left_var_id = axiom.get_left_variable().get_id();
    int right_var_id = axiom.get_right_variable().get_id();
    
    if (left_var_id >= static_cast<int>(numeric_domain_mapping.size()) ||
        right_var_id >= static_cast<int>(numeric_domain_mapping.size())) {
        return 2;
    }
    
    const NumericDomainMapping &left_mapping = *numeric_domain_mapping[left_var_id];
    const NumericDomainMapping &right_mapping = *numeric_domain_mapping[right_var_id];
    
    return left_mapping.evaluate_comparison_with(right_mapping, left_partition, right_partition, comp_op);
}

pair<ap_float, ap_float> Multiplicator::apply_range_operation(
    ap_float left_lower, ap_float left_upper,
    ap_float right_lower, ap_float right_upper,
    cal_operator op) const {
    return NumericDomainMapping::apply_range_operation(left_lower, left_upper, right_lower, right_upper, op);
}

}
