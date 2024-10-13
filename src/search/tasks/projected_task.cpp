#include "projected_task.h"

#include <cassert>

#include "../utils/logging.h"

using namespace std;

namespace tasks {

ProjectedTask::ProjectedTask(const shared_ptr<AbstractTask>& parent,
                               const Pattern &pattern)
      : DelegatingTask(parent), pattern(pattern) {
    // Initialize variable index mapping
    var_to_index.resize(parent->get_num_variables(), -1);
    for (size_t i = 0; i < pattern.regular.size(); ++i) {
      var_to_index[pattern.regular[i]] = i;
    }
    num_var_to_index.resize(parent->get_num_numeric_variables(), -1);
    for (size_t i = 0; i < pattern.numeric.size(); ++i) {
      num_var_to_index[pattern.numeric[i]] = i;
    }
}

Fact ProjectedTask::project_fact(const Fact& fact) const {
  int original_var_id = fact.var; // Accessing 'var' directly from Fact
  int new_var_id = var_to_index[original_var_id];
  if (new_var_id == -1) {
    // This variable is not part of the pattern, so the fact is irrelevant.
    return Fact(0, 0);  // Use a dummy fact
  }
  return Fact(new_var_id, fact.value); // Accessing 'value' directly from Fact
}

bool ProjectedTask::is_fact_relevant(const Fact& fact) const {
  int var_id = fact.var; // Accessing 'var' directly from Fact
  return var_to_index[var_id] != -1;
}

int ProjectedTask::get_num_variables() const {
  return pattern.regular.size();
}

int ProjectedTask::get_num_numeric_variables() const {
  return pattern.numeric.size();
}

const string& ProjectedTask::get_variable_name(int var) const  {
  int original_var_id = pattern.regular[var];
  return parent->get_variable_name(original_var_id);
}

const string& ProjectedTask::get_numeric_variable_name(int var) const {
  int original_var_id = pattern.numeric[var];
  return parent->get_numeric_variable_name(original_var_id);
}

int ProjectedTask::get_variable_domain_size(int var) const  {
  int original_var_id = pattern.regular[var];
  return parent->get_variable_domain_size(original_var_id);
}

int ProjectedTask::get_num_operators() const  {
  int num_ops = 0;
  for (int i = 0; i < parent->get_num_operators(); ++i) {
    if (get_num_operator_effects(i, false) > 0) {
      ++num_ops;
    }
  }
  return num_ops;
}

int ProjectedTask::get_num_operator_preconditions(int index,
                                                 bool is_axiom) const  {
  assert(!is_axiom);  // Axioms are not currently supported
  int num_original_preconditions =
      parent->get_num_operator_preconditions(index, is_axiom);
  int num_projected_preconditions = 0;
  for (int i = 0; i < num_original_preconditions; ++i) {
    Fact original_fact = parent->get_operator_precondition(index, i, is_axiom);
    if (is_fact_relevant(original_fact)) {
      ++num_projected_preconditions;
    }
  }
  return num_projected_preconditions;
}

Fact ProjectedTask::get_operator_precondition(int op_index, int fact_index,
                                             bool is_axiom) const  {
  assert(!is_axiom);  // Axioms are not currently supported
  int num_original_preconditions =
      parent->get_num_operator_preconditions(op_index, is_axiom);
  int projected_fact_index = 0;
  for (int original_fact_index = 0; original_fact_index < num_original_preconditions;
       ++original_fact_index) {
    Fact original_fact =
        parent->get_operator_precondition(op_index, original_fact_index, is_axiom);
    if (is_fact_relevant(original_fact)) {
      if (projected_fact_index == fact_index) {
        return project_fact(original_fact);
      }
      ++projected_fact_index;
    }
  }
  // This should never happen if fact_index is valid.
  ABORT("Invalid fact index in ProjectedTask::get_operator_precondition");
}

int ProjectedTask::get_num_operator_effects(int op_index, bool is_axiom) const  {
  assert(!is_axiom);  // Axioms are not currently supported
  int num_original_effects =
      parent->get_num_operator_effects(op_index, is_axiom);
  int num_projected_effects = 0;
  for (int i = 0; i < num_original_effects; ++i) {
    Fact original_fact = parent->get_operator_effect(op_index, i, is_axiom);
    if (is_fact_relevant(original_fact)) {
      ++num_projected_effects;
    }
  }
  return num_projected_effects;
}

Fact ProjectedTask::get_operator_effect(int op_index, int eff_index,
                                       bool is_axiom) const  {
  assert(!is_axiom);  // Axioms are not currently supported
  int num_original_effects =
      parent->get_num_operator_effects(op_index, is_axiom);
  int projected_eff_index = 0;
  for (int original_eff_index = 0; original_eff_index < num_original_effects;
       ++original_eff_index) {
    Fact original_fact =
        parent->get_operator_effect(op_index, original_eff_index, is_axiom);
    if (is_fact_relevant(original_fact)) {
      if (projected_eff_index == eff_index) {
        return project_fact(original_fact);
      }
      ++projected_eff_index;
    }
  }
  // This should never happen if eff_index is valid.
  ABORT("Invalid effect index in ProjectedTask::get_operator_effect");
}

int ProjectedTask::get_num_goals() const  {
  int num_projected_goals = 0;
  for (int i = 0; i < parent->get_num_goals(); ++i) {
    Fact goal = parent->get_goal_fact(i);
    if (is_fact_relevant(goal)) {
      ++num_projected_goals;
    }
  }
  return num_projected_goals;
}

Fact ProjectedTask::get_goal_fact(int index) const  {
  int num_original_goals = parent->get_num_goals();
  int projected_goal_index = 0;
  for (int original_goal_index = 0; original_goal_index < num_original_goals;
       ++original_goal_index) {
    Fact original_fact = parent->get_goal_fact(original_goal_index);
    if (is_fact_relevant(original_fact)) {
      if (projected_goal_index == index) {
        return project_fact(original_fact);
      }
      ++projected_goal_index;
    }
  }
  // This should never happen if index is valid.
  ABORT("Invalid goal index in ProjectedTask::get_goal_fact");
}

std::vector<int> ProjectedTask::get_initial_state_values() const  {
  std::vector<int> original_initial_state =
      parent->get_initial_state_values();
  std::vector<int> projected_initial_state;
  projected_initial_state.reserve(pattern.regular.size());
  for (int var_id : pattern.regular) {
    projected_initial_state.push_back(original_initial_state[var_id]);
  }
  return projected_initial_state;
}

std::vector<double> ProjectedTask::get_initial_state_numeric_values()
    const  {
  std::vector<double> original_initial_state =
      parent->get_initial_state_numeric_values();
  std::vector<double> projected_initial_state;
  projected_initial_state.reserve(pattern.numeric.size());
  for (int var_id : pattern.numeric) {
    projected_initial_state.push_back(original_initial_state[var_id]);
  }
  return projected_initial_state;
}

int ProjectedTask::get_num_operator_ass_effects(int op_index,
                                               bool is_axiom) const  {
  assert(!is_axiom);  // Axioms are not currently supported
  int num_original_effects =
      parent->get_num_operator_ass_effects(op_index, is_axiom);
  int num_projected_effects = 0;
  for (int i = 0; i < num_original_effects; ++i) {
    AssEffect original_effect =
        parent->get_operator_ass_effect(op_index, i, is_axiom);
    if (is_fact_relevant(Fact(original_effect.aff_var, 0))) {
      ++num_projected_effects;
    }
  }
  return num_projected_effects;
}

AssEffect ProjectedTask::get_operator_ass_effect(int op_index, int eff_index,
                                                 bool is_axiom) const  {
  assert(!is_axiom);  // Axioms are not currently supported
  int num_original_effects =
      parent->get_num_operator_ass_effects(op_index, is_axiom);
  int projected_eff_index = 0;
  for (int original_eff_index = 0; original_eff_index < num_original_effects;
       ++original_eff_index) {
    AssEffect original_effect =
        parent->get_operator_ass_effect(op_index, original_eff_index, is_axiom);
    if (is_fact_relevant(Fact(original_effect.aff_var, 0)))  {
      if (projected_eff_index == eff_index) {
        Fact projected_fact = project_fact(Fact(original_effect.aff_var, 0));
        int projected_var_id = projected_fact.var; // Extract the variable ID

        return AssEffect(projected_var_id,
                          original_effect.op_type,
                          project_fact({original_effect.ass_var, 0}).var);
      }

      ++projected_eff_index;
    }
  }
  // This should never happen if eff_index is valid.
  ABORT("Invalid effect index in ProjectedTask::get_operator_ass_effect");
}

    int ProjectedTask::get_num_ass_axioms() const  {
    int num_ass_axioms = 0;
    for (int i = 0; i < parent->get_num_ass_axioms(); ++i) {
    if (is_numeric_fact_relevant(
            Fact(parent->get_assignment_axiom_effect(i), 0))) {
    ++num_ass_axioms;
}
}
return num_ass_axioms;
}

int ProjectedTask::get_num_cmp_axioms() const  {
int num_cmp_axioms = 0;
for (int i = 0; i < parent->get_num_cmp_axioms(); ++i) {
if (is_fact_relevant(
        Fact(parent->get_comparison_axiom_effect(i, true), 0))) {
++num_cmp_axioms;
}
}
return num_cmp_axioms;
}

Fact ProjectedTask::get_comparison_axiom_effect(int axiom_index,
                                                bool evaluation_result) const  {
Fact original_effect =
        parent->get_comparison_axiom_effect(axiom_index, evaluation_result);
return project_fact(original_effect);
}

int ProjectedTask::get_comparison_axiom_argument(int axiom_index,
                                                 bool left) const  {
int original_argument =
        parent->get_comparison_axiom_argument(axiom_index, left);
if (left) {
return num_var_to_index[original_argument];
} else {
return num_var_to_index[original_argument];
}
}

comp_operator ProjectedTask::get_comparison_axiom_operator(
        int axiom_index) const  {
return parent->get_comparison_axiom_operator(axiom_index);
}

int ProjectedTask::get_assignment_axiom_effect(int axiom_index) const  {
int original_effect_var = parent->get_assignment_axiom_effect(axiom_index);
return num_var_to_index[original_effect_var];
}

int ProjectedTask::get_assignment_axiom_argument(int axiom_index,
                                                 bool left) const  {
int original_argument =
        parent->get_assignment_axiom_argument(axiom_index, left);
if (left) {
return num_var_to_index[original_argument];
} else {
return num_var_to_index[original_argument];
}
}

cal_operator ProjectedTask::get_assignment_axiom_operator(
        int axiom_index) const  {
return parent->get_assignment_axiom_operator(axiom_index);
}

}