#include "projected_task.h"

#include "../numeric_pdbs/types.h"
#include "../task_proxy.h"
#include "../utils/logging.h"

#include <algorithm>
#include <cassert>
#include <numeric>

using namespace std;

namespace tasks {

inline set<int> get_derived_var_ids(const TaskProxy &proxy) {
    set<int> derived_var_ids;
    for (const auto &axiom : proxy.get_axioms()){
        for (const auto &eff : axiom.get_effects()){
            derived_var_ids.insert(eff.get_fact().get_variable().get_id());
        }
    }
    for (const auto &axiom : proxy.get_comparison_axioms()){
        const auto &eff = axiom.get_true_fact();
        assert(axiom.get_false_fact().get_variable() == eff.get_variable());
        derived_var_ids.insert(eff.get_variable().get_id());
    }
    return derived_var_ids;
}

inline void get_regular_numeric_vars_recursive(const TaskProxy &proxy,
                                               const AssignmentAxiomProxy &op,
                                               set<int> &regular_vars) {
    vector<int> var_ids;
    if (op.get_left_variable().get_var_type() == numType::regular){
        regular_vars.insert(op.get_left_variable().get_id());
    } else {
        var_ids.push_back(op.get_left_variable().get_id());
    }
    if (op.get_right_variable().get_var_type() == numType::regular){
        regular_vars.insert(op.get_right_variable().get_id());
    } else {
        var_ids.push_back(op.get_right_variable().get_id());
    }

    for (const auto &ax : proxy.get_assignment_axioms()) {
        for (int var_id : var_ids){
            if (ax.get_assignment_variable().get_id() == var_id){
                get_regular_numeric_vars_recursive(proxy, ax, regular_vars);
            }
        }
    }
}

ProjectedTask::ProjectedTask(const shared_ptr<AbstractTask>& parent,
                             const numeric_pdbs::Pattern &pattern)
        : DelegatingTask(parent),
          variables(pattern.regular),
          numeric_variables(pattern.numeric) {
    // TODO precompute & store some of the expensive to compute structures

    TaskProxy parent_proxy(*parent);

    // Initialize variable index mapping
    var_to_index.resize(parent->get_num_variables(), -1);
    for (size_t i = 0; i < pattern.regular.size(); ++i) {
        var_to_index[pattern.regular[i]] = i;
    }
    for (int var : get_derived_var_ids(parent_proxy)){
        // TODO: instead of adding all derived variables, compute only the relevant ones
        var_to_index[var] = variables.size();
        variables.push_back(var);
    }
    cout << "variables: " << variables << endl;
//    variables.resize(var_to_index.size(), -1);
//    std::iota(var_to_index.begin(), var_to_index.end(), 0);
//    std::iota(variables.begin(), variables.end(), 0);
    num_var_to_index.resize(parent->get_num_numeric_variables(), -1);
    for (size_t i = 0; i < pattern.numeric.size(); ++i) {
        num_var_to_index[pattern.numeric[i]] = i;
    }
    for (const auto &num_var : parent_proxy.get_numeric_variables()){
        if (num_var.get_var_type() != numType::regular){
            // TODO: instead of adding all non-regular variables, compute only the relevant ones
            num_var_to_index[num_var.get_id()] = numeric_variables.size();
            numeric_variables.push_back(num_var.get_id());
        }
    }
    cout << "numeric variables: " << numeric_variables << endl;
//    numeric_variables.resize(num_var_to_index.size(), -1);
//    std::iota(num_var_to_index.begin(), num_var_to_index.end(), 0);
//    std::iota(numeric_variables.begin(), numeric_variables.end(), 0);

    // project initial state
    vector<int> original_initial_state = parent->get_initial_state_values();
    assert(projected_initial_state.empty());
    for (int var_id : variables) {
        projected_initial_state.push_back(original_initial_state[var_id]);
    }

    // project numeric initial state
    vector<ap_float> original_numeric_initial_state = parent->get_initial_state_numeric_values();
    assert(projected_numeric_initial_state.empty());
    for (int var_id : numeric_variables) {
        projected_numeric_initial_state.push_back(original_numeric_initial_state[var_id]);
    }

    // project goal
    for (int goal_id = 0; goal_id < parent->get_num_goals(); ++goal_id) {
        Fact original_fact = parent->get_goal_fact(goal_id);
        if (is_fact_relevant(original_fact)) {
            original_fact.var = var_to_index[original_fact.var];
            projected_goals.push_back(original_fact);
        }
    }

    // project operators
    for (const auto &op : parent_proxy.get_operators()) {
        bool relevant = false;
        for (const auto &eff : op.get_effects()){
            if (is_fact_relevant(eff.get_fact())) {
                projected_op_to_original_op.push_back(op.get_id());
                relevant = true;
                break;
            }
        }
        if (!relevant){
            for (const auto &eff : op.get_ass_effects()){
                int eff_var_id = eff.get_assignment().get_affected_variable().get_id();
                if (is_numeric_var_relevant(eff_var_id)){
                    projected_op_to_original_op.push_back(op.get_id());
                    break;
                }
            }
        }
    }

    // project axioms
    for (const auto &op : parent_proxy.get_axioms()) {
        for (const auto &eff : op.get_effects()){
            if (is_fact_relevant(eff.get_fact())) {
                projected_axiom_to_original_axiom.push_back(op.get_id());
                break;
            }
        }
    }

    // project comparison axioms
    for (const auto &op : parent_proxy.get_comparison_axioms()) {
        assert(op.get_true_fact().get_variable() == op.get_false_fact().get_variable());
        if (is_fact_relevant(op.get_true_fact())) {
            projected_comp_axiom_to_original_comp_axiom.push_back(op.get_id());
        }
    }

    // project assignment axioms
    for (const auto &op : parent_proxy.get_assignment_axioms()) {
        if (is_numeric_var_relevant(op.get_assignment_variable().get_id())) {
            set<int> regular_numeric_vars_in_expression;
            get_regular_numeric_vars_recursive(parent_proxy, op, regular_numeric_vars_in_expression);
            if (std::all_of(regular_numeric_vars_in_expression.begin(),
                            regular_numeric_vars_in_expression.end(),
                            [this] (int var) {
                                return is_numeric_var_relevant(var);})) {
                projected_asgn_axiom_to_original_asgn_axiom.push_back(op.get_id());
            }
        }
    }
}

Fact ProjectedTask::project_fact(const Fact &fact) const {
    assert(fact.var >= 0 && fact.var < parent->get_num_variables());
    assert(var_to_index[fact.var] != -1);
    return {var_to_index[fact.var], fact.value};
}

bool ProjectedTask::is_fact_relevant(const Fact &fact) const {
    assert(fact.var >= 0 && fact.var < parent->get_num_variables());
    return var_to_index[fact.var] != -1;
}

bool ProjectedTask::is_fact_relevant(const FactProxy &fact) const {
    int var_id = fact.get_variable().get_id();
    assert(var_id >= 0 && var_id < parent->get_num_variables());
    return var_to_index[var_id] != -1;
}

bool ProjectedTask::is_numeric_var_relevant(int numeric_var_id) const {
    assert(numeric_var_id >= 0 && numeric_var_id < parent->get_num_numeric_variables());
    return num_var_to_index[numeric_var_id] != -1;
}

int ProjectedTask::get_num_variables() const {
    return variables.size();
}

int ProjectedTask::get_num_numeric_variables() const {
    return numeric_variables.size();
}

const string& ProjectedTask::get_variable_name(int var) const {
    assert(var >= 0 && var < variables.size());
    return parent->get_variable_name(variables[var]);
}

const string& ProjectedTask::get_numeric_variable_name(int var) const {
    assert(var >= 0 && var < numeric_variables.size());
    return parent->get_numeric_variable_name(numeric_variables[var]);
}

int ProjectedTask::get_variable_domain_size(int var) const {
    assert(var >= 0 && var < variables.size());
    return parent->get_variable_domain_size(variables[var]);
}

const string &ProjectedTask::get_fact_name(const Fact &fact) const {
    assert(fact.var >= 0 && fact.var < variables.size());
    return parent->get_fact_name({variables[fact.var], fact.value});
}

bool ProjectedTask::are_facts_mutex(const Fact &fact1, const Fact &fact2) const {
    assert(fact1.var >= 0 && fact1.var < variables.size());
    assert(fact2.var >= 0 && fact2.var < variables.size());
    return parent->are_facts_mutex({variables[fact1.var], fact1.value}, {variables[fact2.var], fact2.value});
}

ap_float ProjectedTask::get_operator_cost(int index, bool is_axiom) const {
    int original_index;
    if (is_axiom){
        original_index = projected_axiom_to_original_axiom[index];
    } else {
        original_index = projected_op_to_original_op[index];
    }
    return parent->get_operator_cost(original_index, is_axiom);
}

const std::string & ProjectedTask::get_operator_name(int index, bool is_axiom) const {
    int original_index;
    if (is_axiom){
        original_index = projected_axiom_to_original_axiom[index];
    } else {
        original_index = projected_op_to_original_op[index];
    }
    return parent->get_operator_name(original_index, is_axiom);
}

int ProjectedTask::get_num_operators() const {
    return static_cast<int>(projected_op_to_original_op.size());
}

int ProjectedTask::get_num_axioms() const {
    return static_cast<int>(projected_axiom_to_original_axiom.size());
}

int ProjectedTask::get_num_operator_preconditions(int index, bool is_axiom) const {
    int original_index;
    if (is_axiom){
        original_index = projected_axiom_to_original_axiom[index];
    } else {
        original_index = projected_op_to_original_op[index];
    }
    int num_projected_preconditions = 0;
    int num_original_preconditions = parent->get_num_operator_preconditions(original_index, is_axiom);
    for (int i = 0; i < num_original_preconditions; ++i) {
        Fact original_fact = parent->get_operator_precondition(original_index, i, is_axiom);
        if (is_fact_relevant(original_fact)) {
            ++num_projected_preconditions;
        }
    }
    return num_projected_preconditions;
}

Fact ProjectedTask::get_operator_precondition(int op_index,
                                              int fact_index,
                                              bool is_axiom) const {
    int original_index;
    if (is_axiom){
        original_index = projected_axiom_to_original_axiom[op_index];
    } else {
        original_index = projected_op_to_original_op[op_index];
    }
    int projected_fact_index = 0;
    int num_original_preconditions = parent->get_num_operator_preconditions(original_index, is_axiom);
    for (int original_fact_index = 0; original_fact_index < num_original_preconditions; ++original_fact_index) {
        Fact original_fact = parent->get_operator_precondition(original_index, original_fact_index, is_axiom);
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

int ProjectedTask::get_num_operator_effects(int op_index, bool is_axiom) const {
    int original_index;
    if (is_axiom){
        original_index = projected_axiom_to_original_axiom[op_index];
    } else {
        original_index = projected_op_to_original_op[op_index];
    }
    int num_original_effects = parent->get_num_operator_effects(original_index, is_axiom);
    int num_projected_effects = 0;
    for (int i = 0; i < num_original_effects; ++i) {
        Fact original_fact = parent->get_operator_effect(original_index, i, is_axiom);
        if (is_fact_relevant(original_fact)) {
            ++num_projected_effects;
        }
    }
    return num_projected_effects;
}

Fact ProjectedTask::get_operator_effect(int op_index,
                                        int eff_index,
                                        bool is_axiom) const {
    int original_index;
    if (is_axiom){
        original_index = projected_axiom_to_original_axiom[op_index];
    } else {
        original_index = projected_op_to_original_op[op_index];
    }
    int num_original_effects = parent->get_num_operator_effects(original_index, is_axiom);
    int projected_eff_index = 0;
    for (int original_eff_index = 0; original_eff_index < num_original_effects; ++original_eff_index) {
        Fact original_fact = parent->get_operator_effect(original_index, original_eff_index, is_axiom);
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

int ProjectedTask::get_num_goals() const {
    return projected_goals.size();
}

Fact ProjectedTask::get_goal_fact(int index) const {
    assert(index >= 0 && index < projected_goals.size());
    return projected_goals[index];
}

vector<int> ProjectedTask::get_initial_state_values() const {
    return projected_initial_state;
}

vector<ap_float> ProjectedTask::get_initial_state_numeric_values() const {
    return projected_numeric_initial_state;
}

vector<int> ProjectedTask::get_state_values(const GlobalState &global_state) const {
    vector<int> state_values(variables.size(), -1);
    vector<int> original_state_values(parent->get_state_values(global_state));
    int i = 0;
    for (int var : variables){
        state_values[i++] = original_state_values[var];
    }
    return state_values;
}

vector<ap_float> ProjectedTask::get_numeric_state_values(const GlobalState &global_state) const {
    vector<ap_float> state_values(numeric_variables.size());
    vector<ap_float> original_state_values(parent->get_numeric_state_values(global_state));
    int i = 0;
    for (int var : numeric_variables){
        state_values[i++] = original_state_values[var];
    }
    return state_values;
}

int ProjectedTask::get_num_operator_ass_effects(int op_index,
                                                bool is_axiom) const {
    int original_index;
    if (is_axiom){
        original_index = projected_axiom_to_original_axiom[op_index];
    } else {
        original_index = projected_op_to_original_op[op_index];
    }
    int num_original_effects = parent->get_num_operator_ass_effects(original_index, is_axiom);
    int num_projected_effects = 0;
    for (int i = 0; i < num_original_effects; ++i) {
        AssEffect original_effect = parent->get_operator_ass_effect(original_index, i, is_axiom);
        if (is_numeric_var_relevant(original_effect.aff_var)) {
            ++num_projected_effects;
        }
    }
    return num_projected_effects;
}

int ProjectedTask::get_num_operator_effect_conditions(
        int op_index, int eff_index, bool is_axiom) const {
    // TODO implement this properly
    return 0;
//    int original_index;
//    if (is_axiom){
//        original_index = projected_axiom_to_original_axiom[op_index];
//    } else {
//        original_index = projected_op_to_original_op[op_index];
//    }
//    int num_original_effects = parent->get_num_operator_effect_conditions(original_index,
//                                                                          original_eff_index,
//                                                                          is_axiom);
//    int num_projected_effects = 0;
//    for (int i = 0; i < num_original_effects; ++i) {
//        AssEffect original_effect = parent->get_num_operator_effect_condition(original_index, i, is_axiom);
//        if (is_numeric_var_relevant(original_effect.aff_var)) {
//            ++num_projected_effects;
//        }
//    }
//    return num_projected_effects;
}

int ProjectedTask::get_num_operator_ass_effect_conditions(
        int op_index, int ass_eff_index, bool is_axiom) const {
    // TODO implement this properly
//    int original_index;
//    if (is_axiom){
//        original_index = projected_axiom_to_original_axiom[op_index];
//    } else {
//        original_index = projected_op_to_original_op[op_index];
//    }
//    int num_op_ass_eff_conds = parent->get_num_operator_ass_effect_conditions(original_index, , is_axiom);
//    for (int i = 0; i < num_op_ass_eff_conds; ++i){
//
//    }
    return 0;
}

Fact ProjectedTask::get_operator_effect_condition(
        int op_index, int eff_index, int cond_index, bool is_axiom) const {
    cerr << "ERROR: not implemented ProjectedTask::get_operator_effect_condition" << endl;
    utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
}

Fact ProjectedTask::get_operator_ass_effect_condition(
        int op_index, int ass_eff_index, int cond_index, bool is_axiom) const {
    cerr << "ERROR: not implemented ProjectedTask::get_operator_ass_effect_condition" << endl;
    utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
}

AssEffect ProjectedTask::get_operator_ass_effect(int op_index,
                                                 int eff_index,
                                                 bool is_axiom) const {
    int original_index;
    if (is_axiom){
        original_index = projected_axiom_to_original_axiom[op_index];
    } else {
        original_index = projected_op_to_original_op[op_index];
    }
    int num_original_effects = parent->get_num_operator_ass_effects(original_index, is_axiom);
    int projected_eff_index = 0;
    for (int original_eff_index = 0; original_eff_index < num_original_effects; ++original_eff_index) {
        AssEffect original_effect = parent->get_operator_ass_effect(original_index, original_eff_index, is_axiom);
        if (is_numeric_var_relevant(original_effect.aff_var)) {
            if (projected_eff_index == eff_index) {
                assert(num_var_to_index[original_effect.aff_var] != -1);
                assert(num_var_to_index[original_effect.ass_var] != -1);
                return {num_var_to_index[original_effect.aff_var],
                        original_effect.op_type,
                        num_var_to_index[original_effect.ass_var]};
            }
            ++projected_eff_index;
        }
    }
    // This should never happen if eff_index is valid.
    ABORT("Invalid effect index in ProjectedTask::get_operator_ass_effect");
}

int ProjectedTask::get_num_ass_axioms() const {
    return projected_asgn_axiom_to_original_asgn_axiom.size();
}

int ProjectedTask::get_num_cmp_axioms() const {
    return projected_comp_axiom_to_original_comp_axiom.size();
}

Fact ProjectedTask::get_comparison_axiom_effect(int axiom_index,
                                                bool evaluation_result) const {
    assert(axiom_index >= 0 && axiom_index < projected_comp_axiom_to_original_comp_axiom.size());
    int original_index = projected_comp_axiom_to_original_comp_axiom[axiom_index];
    return parent->get_comparison_axiom_effect(original_index, evaluation_result);
}

int ProjectedTask::get_comparison_axiom_argument(int axiom_index,
                                                 bool left) const {
    assert(axiom_index >= 0 && axiom_index < projected_comp_axiom_to_original_comp_axiom.size());
    int original_index = projected_comp_axiom_to_original_comp_axiom[axiom_index];
    return parent->get_comparison_axiom_argument(original_index, left);
}

comp_operator ProjectedTask::get_comparison_axiom_operator(int axiom_index) const {
    assert(axiom_index >= 0 && axiom_index < projected_comp_axiom_to_original_comp_axiom.size());
    int original_index = projected_comp_axiom_to_original_comp_axiom[axiom_index];
    return parent->get_comparison_axiom_operator(original_index);
}

int ProjectedTask::get_assignment_axiom_effect(int axiom_index) const {
    assert(axiom_index >= 0 && axiom_index < projected_asgn_axiom_to_original_asgn_axiom.size());
    int original_index = projected_asgn_axiom_to_original_asgn_axiom[axiom_index];
    return parent->get_assignment_axiom_effect(original_index);
}

int ProjectedTask::get_assignment_axiom_argument(int axiom_index,
                                                 bool left) const {
    assert(axiom_index >= 0 && axiom_index < projected_asgn_axiom_to_original_asgn_axiom.size());
    int original_index = projected_asgn_axiom_to_original_asgn_axiom[axiom_index];
    return parent->get_assignment_axiom_argument(original_index, left);
}

cal_operator ProjectedTask::get_assignment_axiom_operator(int axiom_index) const {
    assert(axiom_index >= 0 && axiom_index < projected_asgn_axiom_to_original_asgn_axiom.size());
    return parent->get_assignment_axiom_operator(projected_asgn_axiom_to_original_asgn_axiom[axiom_index]);
}

const GlobalOperator *ProjectedTask::get_global_operator(int index, bool is_axiom) const {
    assert(false);
    cerr << "ERROR: not implemented ProjectedTask::get_global_operator" << endl;
    utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
}

numType ProjectedTask::get_numeric_var_type(int index) const {
    assert(index >= 0 && index < numeric_variables.size());
    return parent->get_numeric_var_type(numeric_variables[index]);
}
}