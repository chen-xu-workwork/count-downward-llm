#ifndef TASKS_PROJECTED_TASK_H
#define TASKS_PROJECTED_TASK_H

#include <memory>
#include <string>
#include <vector>

#include "delegating_task.h"

class FactProxy;
class State;

namespace numeric_pdbs {
class Pattern;
}

namespace tasks {

class ProjectedTask : public DelegatingTask {
private:
    std::vector<int> variables;
    std::vector<int> numeric_variables;

    std::vector<int> var_to_index;
    std::vector<int> num_var_to_index;

    std::vector<int> projected_initial_state;
    std::vector<ap_float> projected_numeric_initial_state;
    std::vector<Fact> projected_goals;

    std::vector<int> projected_op_to_original_op;
    std::vector<int> projected_axiom_to_original_axiom;
    std::vector<int> projected_comp_axiom_to_original_comp_axiom;
    std::vector<int> projected_asgn_axiom_to_original_asgn_axiom;

    float calculate_derived_variable_value(const int var_id, const std::vector<ap_float> &state) const;

    Fact project_fact(const Fact &fact) const;
    bool is_fact_relevant(const Fact &fact) const;
    bool is_fact_relevant(const FactProxy &fact) const;
    bool is_numeric_var_relevant(int numeric_var_id) const;

public:
    ProjectedTask(const std::shared_ptr<AbstractTask>& parent,
                  const numeric_pdbs::Pattern &pattern);

    const std::vector<int> &get_projected_variables() const {
        return variables;
    }

    const std::vector<int> &get_projected_numeric_variables() const {
        return numeric_variables;
    }

    int get_num_variables() const override;
    int get_num_numeric_variables() const override;
    const std::string &get_variable_name(int var) const override;
    const std::string &get_numeric_variable_name(int var) const override;
    int get_variable_domain_size(int var) const override;
    const std::string &get_fact_name(const Fact &fact) const override;

    bool are_facts_mutex(const Fact &fact1, const Fact &fact2) const override;

    ap_float get_operator_cost(int index, bool is_axiom) const override;
    const std::string &get_operator_name(int index, bool is_axiom) const override;
    int get_num_operators() const override;
    int get_num_operator_preconditions(int index, bool is_axiom) const override;
    Fact get_operator_precondition(int op_index, int fact_index,
                                           bool is_axiom) const override;
    int get_num_operator_effects(int op_index, bool is_axiom) const override;

    int get_num_operator_ass_effects(int op_index, bool is_axiom) const override;
    int get_num_operator_effect_conditions(
            int op_index, int eff_index, bool is_axiom) const override;
    int get_num_operator_ass_effect_conditions(
            int op_index, int ass_eff_index, bool is_axiom) const override;
    Fact get_operator_effect_condition(
            int op_index, int eff_index, int cond_index, bool is_axiom) const override;
    Fact get_operator_ass_effect_condition(
            int op_index, int ass_eff_index, int cond_index, bool is_axiom) const override;
    Fact get_operator_effect(
            int op_index, int eff_index, bool is_axiom) const override;
    AssEffect get_operator_ass_effect(
            int op_index, int eff_index, bool is_axiom) const override;
    Fact get_comparison_axiom_effect(int axiom_index, bool evaluation_result) const override;
    int get_comparison_axiom_argument(int axiom_index, bool left) const override; // true: left, false: right
    comp_operator get_comparison_axiom_operator(int axiom_index) const override;
    int get_assignment_axiom_effect(int axiom_index) const override;
    int get_assignment_axiom_argument(int axiom_index, bool left) const override; // true: left, false: right
    cal_operator get_assignment_axiom_operator(int axiom_index) const override;

    const GlobalOperator *get_global_operator(int index, bool is_axiom) const override;

    int get_num_axioms() const override;
    int get_num_ass_axioms() const override;
    int get_num_cmp_axioms() const override;

    int get_num_goals() const override;
    Fact get_goal_fact(int index) const override;

    std::vector<int> get_initial_state_values() const override;
    std::vector<ap_float> get_initial_state_numeric_values() const override;
    std::vector<int> get_state_values(const GlobalState &global_state) const override;
    std::vector<ap_float> get_numeric_state_values(const GlobalState &global_state) const override;

    numType get_numeric_var_type(int index) const override;

    State get_projected_state(const State &state) const;

    State get_projected_state(const std::vector<int> &prop_state,
                              const std::vector<ap_float> &num_state,
                              const numeric_pdbs::Pattern &pattern) const;

};
}  // namespace tasks

#endif  // TASKS_PROJECTED_TASK_H