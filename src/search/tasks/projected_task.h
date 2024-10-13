#ifndef TASKS_PROJECTED_TASK_H
#define TASKS_PROJECTED_TASK_H

#include <memory>
#include <string>
#include <vector>

#include "delegating_task.h"
#include "../types.h"

namespace tasks {

class ProjectedTask : public DelegatingTask {
 private:
  const Pattern& pattern;
  std::vector<int> var_to_index;
  std::vector<int> num_var_to_index;

  Fact project_fact(const Fact& fact) const;
  bool is_fact_relevant(const Fact& fact) const;
  bool is_numeric_fact_relevant(const NumericFact& fact) const;

 public:
  ProjectedTask(const std::shared_ptr<AbstractTask>& parent,
                const Pattern& pattern);

  int get_num_variables() const override;
  int get_num_numeric_variables() const override;
  const std::string& get_variable_name(int var) const override;
  const std::string& get_numeric_variable_name(int var) const override;
  int get_variable_domain_size(int var) const override;

  int get_num_operators() const override;
  int get_num_operator_preconditions(int index, bool is_axiom) const override;
  Fact get_operator_precondition(int op_index, int fact_index,
                                 bool is_axiom) const override;
  int get_num_operator_effects(int op_index, bool is_axiom) const override;
  Fact get_operator_effect(int op_index, int eff_index,
                           bool is_axiom) const override;

  int get_num_goals() const override;
  Fact get_goal_fact(int index) const override;
  std::vector<int> get_initial_state_values() const override;
  std::vector<double> get_initial_state_numeric_values() const override;
  int get_num_operator_ass_effects(int op_index, bool is_axiom) const override;
  AssEffect get_operator_ass_effect(int op_index, int eff_index,
                                   bool is_axiom) const override;
  int get_num_ass_axioms() const override;
  int get_num_cmp_axioms() const override;
  Fact get_comparison_axiom_effect(int axiom_index,
                                     bool evaluation_result) const override;
  int get_comparison_axiom_argument(int axiom_index,
                                      bool left) const override;
  comp_operator get_comparison_axiom_operator(
            int axiom_index) const override;
  int get_assignment_axiom_effect(int axiom_index) const override;
  int get_assignment_axiom_argument(int axiom_index,
                                      bool left) const override;
  cal_operator get_assignment_axiom_operator(
            int axiom_index) const override;

};
}  // namespace tasks

#endif  // TASKS_PROJECTED_TASK_H