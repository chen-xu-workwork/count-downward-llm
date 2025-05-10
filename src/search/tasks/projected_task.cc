#include "projected_task.h"

#include "../numeric_pdbs/types.h"
#include "../task_proxy.h"
#include "../utils/logging.h"

#include <algorithm>
#include <cassert>
#include <numeric>

using namespace std;

namespace tasks {

//inline set<int> get_derived_var_ids(const TaskProxy &proxy) {
//    set<int> derived_var_ids;
//    for (const auto &axiom : proxy.get_axioms()) {
//        for (const auto &eff : axiom.get_effects()) {
//            derived_var_ids.insert(eff.get_fact().get_variable().get_id());
//        }
//    }
//    for (const auto &axiom : proxy.get_comparison_axioms()) {
//        const auto &eff = axiom.get_true_fact();
//        assert(axiom.get_false_fact().get_variable() == eff.get_variable());
//        derived_var_ids.insert(eff.get_variable().get_id());
//    }
//    return derived_var_ids;
//}

inline void get_regular_numeric_vars_recursive(const TaskProxy &proxy,
                                               const AssignmentAxiomProxy &op,
                                               set<int> &regular_vars) {
    set<int> var_ids;
    if (op.get_left_variable().get_var_type() == numType::regular){
        regular_vars.insert(op.get_left_variable().get_id());
    } else {
        var_ids.insert(op.get_left_variable().get_id());
    }
    if (op.get_right_variable().get_var_type() == numType::regular){
        regular_vars.insert(op.get_right_variable().get_id());
    } else {
        var_ids.insert(op.get_right_variable().get_id());
    }

    if (!var_ids.empty()) {
        for (const auto &ax: proxy.get_assignment_axioms()) {
            for (int var_id: var_ids) {
                if (ax.get_assignment_variable().get_id() == var_id) {
                    get_regular_numeric_vars_recursive(proxy, ax, regular_vars);
                }
            }
        }
    }
}

inline bool is_derived_variable(const VariableProxy &var, const TaskProxy &task_proxy) {
    for (auto ax : task_proxy.get_axioms()){
        for (auto eff : ax.get_effects()) {
            if (eff.get_fact().get_variable().get_id() == var.get_id()) {
                return true;
            }
        }
    }
    return false;
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
    //    cout << "variables:" << endl;
    for (size_t i = 0; i < pattern.regular.size(); ++i) {
        var_to_index[pattern.regular[i]] = i;
        //        cout << parent_proxy.get_variables()[pattern.regular[i]].get_fact(0).get_name() << endl;
    }

    num_var_to_index.resize(parent->get_num_numeric_variables(), -1);
    for (size_t i = 0; i < pattern.numeric.size(); ++i) {
        if (pattern.numeric[i] >= num_var_to_index.size()){
            // this is an auxiliary variable added by numeric_pdb_helper::NumericTaskProxy
            // TODO check if these are handled correctly everywhere
            num_var_to_index.resize(pattern.numeric[i] + 1, -1);
        }
        num_var_to_index[pattern.numeric[i]] = i;

        //        cout << parent_proxy.get_numeric_variables()[pattern.numeric[i]].get_name() << endl;
    }

    for (const auto &num_var : parent_proxy.get_numeric_variables()) {
        if (num_var.get_var_type() == numType::instrumentation || num_var.get_var_type() == numType::constant){
            num_var_to_index[num_var.get_id()] = numeric_variables.size();
            numeric_variables.push_back(num_var.get_id());
        }
    }

    for (const auto &op : parent_proxy.get_assignment_axioms()) {
        assert(op.get_assignment_variable().get_var_type() == numType::derived);
        set<int> regular_numeric_vars_in_expression;
        get_regular_numeric_vars_recursive(parent_proxy, op, regular_numeric_vars_in_expression);
        if (std::all_of(regular_numeric_vars_in_expression.begin(),
                        regular_numeric_vars_in_expression.end(),
                        [this] (int var) {
                            return is_numeric_var_relevant(var);})) {
            num_var_to_index[op.get_assignment_variable().get_id()] = numeric_variables.size();
            numeric_variables.push_back(op.get_assignment_variable().get_id());
        }
    }

    // project goals
    for (int goal_id = 0; goal_id < parent->get_num_goals(); ++goal_id) {
        Fact original_fact = parent->get_goal_fact(goal_id);
        cout << parent->get_fact_name(original_fact);
        cout << "goal id: " << original_fact.var << endl;
        if (is_fact_relevant(original_fact)) {
            projected_goals.push_back(project_fact(original_fact));
        }
        if (is_derived_variable(parent_proxy.get_variables()[original_fact.var], parent_proxy)){
            // TODO if there are numeric goals in the pattern, then we need to add the derived variable that represents the numeric goals here
        }
    }
    cout << "projected goals: (size): " << projected_goals.size() << ", ";
    for (const auto &goal : projected_goals) {
        cout << goal.var << ", " << goal.value << " --";
    }

    for (const auto &op : parent_proxy.get_comparison_axioms()) {
        assert(op.get_true_fact().get_variable() == op.get_false_fact().get_variable());
        int lhs = op.get_left_variable().get_id();
        int rhs = op.get_right_variable().get_id();
        if (is_numeric_var_relevant(lhs) &&
            is_numeric_var_relevant(rhs)) {
                    
            var_to_index[op.get_true_fact().get_variable().get_id()] = variables.size();
            variables.push_back(op.get_true_fact().get_variable().get_id());
        }
    }

    // TODO this is not doing anything!?
//    for (const OperatorProxy  &var : parent_proxy.get_axioms()) {
//        vector<int> var_ids;
//        for (const auto &eff : var.get_effects()){
//            if (is_fact_relevant(eff.get_fact())){
//                var_ids.push_back(eff.get_fact().get_variable().get_id());
//            }
//        }
//        for (const FactProxy &pre : var.get_preconditions()){
//            if (is_fact_relevant(pre)) {
//                var_ids.push_back(pre.get_variable().get_id());
//            }
//        }
//    }



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
        if (var_id >= original_numeric_initial_state.size()) {
            //TODO: figure out if placeholder is correct. Auxiliary variables are out of range
            projected_numeric_initial_state.push_back(-1);
            continue;
        }
        projected_numeric_initial_state.push_back(original_numeric_initial_state[var_id]);
    }

//    cout << "initial state: " << projected_initial_state << projected_numeric_initial_state << endl;

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
        if (is_fact_relevant(op.get_true_fact()) &&
            is_numeric_var_relevant(op.get_left_variable().get_id()) &&
            is_numeric_var_relevant(op.get_right_variable().get_id())) {
            projected_comp_axiom_to_original_comp_axiom.push_back(op.get_id());
        }
    }

    // project assignment axioms
    for (const auto &op : parent_proxy.get_assignment_axioms()) {
        if (is_numeric_var_relevant(op.get_assignment_variable().get_id())) {
            set<int> regular_numeric_vars_in_expression;
            get_regular_numeric_vars_recursive(parent_proxy, op, regular_numeric_vars_in_expression);
            // print regular_numeric_vars_in_expression
            if (std::all_of(regular_numeric_vars_in_expression.begin(),
                            regular_numeric_vars_in_expression.end(),
                            [this] (int var) {
                                return is_numeric_var_relevant(var);})) {
                projected_asgn_axiom_to_original_asgn_axiom.push_back(op.get_id());
            }
        }
    }
}

float ProjectedTask::calculate_derived_variable_value(const int var_id, const vector<ap_float> &state) const {
    //Input is id of projected variable

    numType var_type = get_numeric_var_type(var_id);
    if (var_type != numType::constant) {
        return get_initial_state_numeric_values()[var_id];
    } else if (var_type == numType::regular) {
        return state[var_id];
    } 
    assert(var_type == numType::derived);

    int eff_var = get_assignment_axiom_effect(var_id);
    int left_var = get_assignment_axiom_argument(var_id, true);
    int right_var = get_assignment_axiom_argument(var_id, false);
    //TODO: Make check that lhs and rhs are relevant
    cal_operator op = get_assignment_axiom_operator(var_id);

    switch (op) {
        case cal_operator::sum:
            return state[left_var] + state[right_var];
        case cal_operator::diff:
            return state[left_var] - state[right_var];
        case cal_operator::mult:
            return state[left_var] * state[right_var];
        case cal_operator::divi:
            if (state[right_var] == 0) {
                throw runtime_error("Division by zero in derived variable calculation");
            }
            return state[left_var] / state[right_var];
        default:
            throw runtime_error("Unknown operator in derived variable calculation");
    }
    throw runtime_error("Should be unreachable");
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
    return project_fact(parent->get_comparison_axiom_effect(original_index, evaluation_result));
}

int ProjectedTask::get_comparison_axiom_argument(int axiom_index,
                                                 bool left) const {
    assert(axiom_index >= 0 && axiom_index < projected_comp_axiom_to_original_comp_axiom.size());
    int original_index = projected_comp_axiom_to_original_comp_axiom[axiom_index];
    return num_var_to_index[parent->get_comparison_axiom_argument(original_index, left)];
}

comp_operator ProjectedTask::get_comparison_axiom_operator(int axiom_index) const {
    assert(axiom_index >= 0 && axiom_index < projected_comp_axiom_to_original_comp_axiom.size());
    int original_index = projected_comp_axiom_to_original_comp_axiom[axiom_index];
    return parent->get_comparison_axiom_operator(original_index);
}

int ProjectedTask::get_assignment_axiom_effect(int axiom_index) const {
    assert(axiom_index >= 0 && axiom_index < projected_asgn_axiom_to_original_asgn_axiom.size());
    int original_index = projected_asgn_axiom_to_original_asgn_axiom[axiom_index];
    return num_var_to_index[parent->get_assignment_axiom_effect(original_index)];
}

int ProjectedTask::get_assignment_axiom_argument(int axiom_index,
                                                 bool left) const {
    assert(axiom_index >= 0 && axiom_index < projected_asgn_axiom_to_original_asgn_axiom.size());
    int original_index = projected_asgn_axiom_to_original_asgn_axiom[axiom_index];
    return num_var_to_index[parent->get_assignment_axiom_argument(original_index, left)];
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

State ProjectedTask::get_projected_state(const State &state) const {
    vector<int> projected_prop_state(variables.size(), -1);
    for (size_t i = 0; i < variables.size(); ++i){
        int var = variables[i];
        assert(var == state[var].get_variable().get_id());
        projected_prop_state[i] = state[var].get_value();
    }
    vector<ap_float> projected_num_state(numeric_variables.size());
    for (size_t i = 0; i < numeric_variables.size(); ++i){
        int var = numeric_variables[i];
        projected_num_state[i] = state.nval(var);
    }
    return {*this, std::move(projected_prop_state), std::move(projected_num_state)};
}

State ProjectedTask::get_projected_state(const std::vector<int> &prop_state,
                                         const std::vector<ap_float> &num_state,
                                         const numeric_pdbs::Pattern &pattern) const {
    //cout << "get_projected_state: " << prop_state << num_state << endl;
    vector<int> projected_prop_state(variables.size(), -1);
    vector<bool> set_var(variables.size(), false);
    // copy&map variable values from prop_state
    for (size_t i = 0; i < pattern.regular.size(); ++i){
        int var = pattern.regular[i];
        int projected_id = var_to_index[var];
        assert(projected_id != -1);
        projected_prop_state[projected_id] = prop_state[i];
        assert(!set_var[projected_id]);
        set_var[projected_id] = true;
    }
    vector<bool> set_numeric_var(numeric_variables.size(), false);
    vector<ap_float> projected_num_state(numeric_variables.size(), -11.11);
    // copy&map variable values from num_state
    for (size_t i = 0; i < pattern.numeric.size(); ++i){
        int var = pattern.numeric[i];
        int projected_id = num_var_to_index[var];
        assert(projected_id != -1);
        projected_num_state[projected_id] = num_state[i];
        assert(!set_numeric_var[projected_id]);
        set_numeric_var[projected_id] = true;
    }
    for (size_t var = 0; var < numeric_variables.size(); ++var){
        if (get_numeric_var_type(var) == numType::constant){
            projected_num_state[var] = get_initial_state_numeric_values()[var];
            assert(!set_numeric_var[var]);
            set_numeric_var[var] = true;
        } 
    }
    // evaluate assignment axioms
    for (int ax_id = 0; ax_id < get_num_ass_axioms(); ++ax_id){
        // all these variables are numeric
        int eff_var = get_assignment_axiom_effect(ax_id);
        int left_var = get_assignment_axiom_argument(ax_id, true);
        int right_var = get_assignment_axiom_argument(ax_id, false);
        cal_operator op = get_assignment_axiom_operator(ax_id);

        ap_float left_val = 0;
        //TODO: entire next blocks can be sinplified by using the "calculate_derived_variable_value" function
        if (get_numeric_var_type(left_var) == numType::regular){
            assert(std::find(pattern.numeric.begin(), pattern.numeric.end(), numeric_variables[left_var]) != pattern.numeric.end());
            left_val = projected_num_state[left_var];
            assert(set_numeric_var[left_var]);
        } else if (get_numeric_var_type(left_var) == numType::derived) {
            const vector<ap_float> state_argument = projected_num_state;
            left_val = calculate_derived_variable_value(left_var, state_argument);
        } else {
            if (get_numeric_var_type(left_var) == numType::constant){
                left_val = get_initial_state_numeric_values()[left_var];
            } else {
                assert(get_numeric_var_type(left_var) == numType::instrumentation);
                // must be the costs variable
            }
        }
        ap_float right_val = 0;
        if (get_numeric_var_type(right_var) == numType::regular){
            assert(std::find(pattern.numeric.begin(), pattern.numeric.end(), numeric_variables[right_var]) != pattern.numeric.end());
            right_val = projected_num_state[right_var];
            assert(set_numeric_var[right_var]);
        } else if (get_numeric_var_type(right_var) == numType::derived) {
            const vector<ap_float> state_argument = projected_num_state;
            right_val = calculate_derived_variable_value(right_var, state_argument);
            cout << "right_val: " << right_val << endl;
        } else {
            if (get_numeric_var_type(right_var) == numType::constant){
                right_val = get_initial_state_numeric_values()[right_var];
            } else {
                assert(get_numeric_var_type(right_var) == numType::instrumentation);
                // must be the costs variable
            }
        }

        ap_float res;
        switch (op) {
            case cal_operator::diff:
                res = left_val - right_val;
                break;
            case cal_operator::sum:
                res = left_val + right_val;
                break;
            case cal_operator::mult:
                res = left_val * right_val;
                break;
            default:
                cerr << "unsupported cal_operator in ProjectedTask: " << op << endl;
                utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        }

        projected_num_state[eff_var] = res;
        assert(!set_numeric_var[eff_var]);
        set_numeric_var[eff_var] = true;
    }
    
    // evaluate comparison axioms
    for (int ax_id = 0; ax_id < get_num_cmp_axioms(); ++ax_id){
        // these are numeric variables
        int left_var = get_comparison_axiom_argument(ax_id, true);
        int right_var = get_comparison_axiom_argument(ax_id, false);
        comp_operator op = get_comparison_axiom_operator(ax_id);
        assert(set_numeric_var[left_var]);
        assert(set_numeric_var[right_var]);

        bool res;
        switch (op) {
            case comp_operator::le:
                res = projected_num_state[left_var] <= projected_num_state[right_var];
                break;
            case comp_operator::eq:
                res = projected_num_state[left_var] == projected_num_state[right_var];
                break;
            case comp_operator::ge:
                res = projected_num_state[left_var] >= projected_num_state[right_var];
                break;
            case comp_operator::gt:
                res = projected_num_state[left_var] > projected_num_state[right_var];
                break;
            case comp_operator::lt:
                res = projected_num_state[left_var] < projected_num_state[right_var];
                break;
            case comp_operator::ue:
                res = projected_num_state[left_var] != projected_num_state[right_var];
                break;
            default:
                cerr << "unsupported comp_operator in ProjectedTask: " << op << endl;
                utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        }

        Fact eff = get_comparison_axiom_effect(ax_id, res); // this is a propositional var
        assert(projected_prop_state[eff.var] == -1);

        projected_prop_state[eff.var] = eff.value;

        assert(!set_var[eff.var]);
        set_var[eff.var] = true;
    }

    // evaluate axioms to get values of derived variables
    // we need to do this here because this might have numeric preconditions
    for (int ax_id = 0; ax_id < get_num_axioms(); ++ax_id){
        bool applicable = true;
        int num_pre = get_num_operator_preconditions(ax_id, true);
        for (int pre_id = 0; pre_id < num_pre; ++pre_id){
            Fact pre = get_operator_precondition(ax_id, pre_id, true);
            //cout << "TEST1: " << pre.var << ", " << pre.value << ", " << projected_prop_state.size() << endl;
            //cout << "TEST2: " << projected_prop_state[pre.var] << endl;
            assert(set_var[pre.var]);
            assert(projected_prop_state[pre.var] != -1);
            if (projected_prop_state[pre.var] != pre.value){
                applicable = false;
                break;
            }
        }

        int num_eff = get_num_operator_effects(ax_id, true);
        for (int eff_id = 0; eff_id < num_eff; ++eff_id){
            // NOTE: the default value for derived variables in NFD seems to be 1
            Fact eff = get_operator_effect(ax_id, eff_id, true);
            if (applicable){
                assert(projected_prop_state[eff.var] == -1);
                assert(eff.value == 0);
                projected_prop_state[eff.var] = eff.value;
            } else {
                projected_prop_state[eff.var] = 1;
            }

            assert(!set_var[eff.var]);
            set_var[eff.var] = true;
        }
    }
    for (size_t i = 0; i < variables.size(); ++i){
        if (projected_prop_state[i] == -1){
            // TODO verify that this variable is the result of a comparison axiom that is not relevant for the variables in this ProjectedTask
            //  therefore, we set its value to 0, which indicates that the condition is true
            projected_prop_state[i] = 0;
        }
    }
    //cout << "projected_prop_state: " << projected_prop_state << endl;
    //cout << "projected_num_state: " << projected_num_state << endl;
    //cout << "pre-projected state: " << prop_state << endl;
    //cout << "pre-projected num state: " << num_state << endl;
    return {*this, std::move(projected_prop_state), std::move(projected_num_state)};
}
}
