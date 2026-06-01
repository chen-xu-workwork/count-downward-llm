#include "projected_task.h"

#include "../numeric_pdbs/types.h"
#include "../state_registry.h"
#include "../task_proxy.h"
#include "../utils/logging.h"

#include <algorithm>
#include <cassert>
#include <numeric>

using namespace std;

namespace tasks {

inline bool is_derived_variable(const TaskProxy &task_proxy, const VariableProxy &var) {
    for (auto ax : task_proxy.get_axioms()){
        for (auto eff : ax.get_effects()) {
            if (eff.get_fact().get_variable().get_id() == var.get_id()) {
                return true;
            }
        }
    }
    return false;
}


ProjectedTask::ProjectedTask(
        const std::shared_ptr<AbstractTask>& parent,
        const numeric_pdbs::Pattern &pattern,
        const std::shared_ptr<numeric_pdb_helper::NumericTaskProxy> &numeric_task_proxy)
        : DelegatingTask(parent),
          variables(pattern.regular),
          num_auxiliary_constants(0),
          task_proxy(numeric_task_proxy) {
    // TODO precompute & store some of the expensive-to-compute structures

    TaskProxy parent_proxy(*parent);

    // Initialize variable index mapping for pattern variables
    var_to_index.resize(parent->get_num_variables(), -1);
    // cout << "pattern variables:" << endl;
    for (size_t i = 0; i < pattern.regular.size(); ++i) {
        var_to_index[pattern.regular[i]] = static_cast<int>(i);
        // cout << parent_proxy.get_variables()[pattern.regular[i]].get_fact(0).get_name() << endl;
    }

    // init numeric variables from pattern here
    assert(numeric_variables.empty());
    num_var_to_index.resize(parent->get_num_numeric_variables(), -1);
    is_auxiliary_num_var.resize(parent->get_num_numeric_variables(), false);
    // cout << "numeric pattern variables:" << endl;
    for (size_t i = 0; i < pattern.numeric.size(); ++i) {
        int var_id = pattern.numeric[i];
        if (var_id >= parent->get_num_numeric_variables()) {
            // this is an auxiliary variable added by numeric_pdb_helper::NumericTaskProxy
            var_id = task_proxy->map_to_derived_variable_id(var_id);
            is_auxiliary_num_var[i] = true;
        } else {
            assert(parent->get_numeric_var_type(var_id) == regular);
        }
        assert(var_id < static_cast<int>(num_var_to_index.size()));
        numeric_variables.push_back(var_id);
        assert(num_var_to_index[var_id] == -1);
        num_var_to_index[var_id] = static_cast<int>(i);
//        cout << "map " << var_id << " to " << i << endl;
        //cout << "numeric var: " << var_id << endl;
        // cout << parent_proxy.get_numeric_variables()[var_id].get_name() << endl;
    }

    // cout << "const/inst num vars:" << endl;
    for (auto num_var : parent_proxy.get_numeric_variables()) {
        if (num_var.get_var_type() == numType::instrumentation || num_var.get_var_type() == numType::constant) {
//            cout << "map " << num_var.get_id() << " to " << numeric_variables.size() << endl;
            // cout << parent_proxy.get_numeric_variables()[num_var.get_id()].get_name() << endl;
            num_var_to_index[num_var.get_id()] = static_cast<int>(numeric_variables.size());
            numeric_variables.push_back(num_var.get_id());
        }
    }

    // cout << "relevant vars from assgn ax:" << endl;
    for (auto op : parent_proxy.get_assignment_axioms()) {
        assert(op.get_assignment_variable().get_var_type() == numType::derived);
        if (is_numeric_var_relevant(op.get_assignment_variable().get_id())){
            // don't add the same var twice
            continue;
        }
        set<int> regular_numeric_vars_in_expression;
        get_regular_numeric_vars_recursive(parent_proxy, op, regular_numeric_vars_in_expression);
        if (std::all_of(regular_numeric_vars_in_expression.begin(),
                        regular_numeric_vars_in_expression.end(),
                        [this] (int var) {
                            return is_numeric_var_relevant(var);})) {
            int var_id = op.get_assignment_variable().get_id();
//            cout << "map " << var_id << " to " << numeric_variables.size() << endl;
            num_var_to_index[var_id] = static_cast<int>(numeric_variables.size());
            //cout << "Add new ass var: " << var_id << ", " << regular_numeric_vars_in_expression << endl;
            numeric_variables.push_back(var_id);
            // cout << parent_proxy.get_numeric_variables()[var_id].get_name() << endl;
        }
    }

    // find relevant variables that are results of comparison axioms
    // cout << "relevant variables that are results of comparison axioms:" << endl;
    for (auto op : parent_proxy.get_comparison_axioms()) {
        assert(op.get_true_fact().get_variable() == op.get_false_fact().get_variable());
        if (is_fact_relevant(op.get_true_fact())){
            // don't add the same var twice
            continue;
        }
        int lhs = op.get_left_variable().get_id();
        int rhs = op.get_right_variable().get_id();
        if (is_numeric_var_relevant(lhs) &&
            is_numeric_var_relevant(rhs)) {
            int var_id = op.get_true_fact().get_variable().get_id();
            var_to_index[var_id] = static_cast<int>(variables.size());
            variables.push_back(var_id);
            // cout << parent_proxy.get_variables()[var_id].get_fact(0).get_name() << endl;
        }
    }

    // check for relevant goals
    // cout << "relevant variables that are preconditions of the goal axiom:" << endl;
    for (auto axiom : parent_proxy.get_axioms()) {
        assert(axiom.get_preconditions().empty() || axiom.get_effects().size() == 1);
        // this axiom encodes a set of goal facts (possibly propositional and numeric)
        // if one of the conditions, i.e., the actual goals, is relevant, make the effect relevant
        bool is_relevant = false;
        for (auto pre : axiom.get_preconditions()) {
            if (is_fact_relevant(pre)) {
                is_relevant = true;
            }
        }
        if (is_relevant && !is_fact_relevant(axiom.get_effects()[0].get_fact())){
            // make sure to not add the same variable twice
            assert(axiom.get_effects().size() == 1);
            int var_id = axiom.get_effects()[0].get_fact().get_variable().get_id();
            var_to_index[var_id] = static_cast<int>(variables.size());
            variables.push_back(var_id);
            // cout << parent_proxy.get_variables()[var_id].get_fact(0).get_name() << endl;
        }
    }

    // project goals
    // cout << "relevant goals:" << endl;
    for (int goal_id = 0; goal_id < parent->get_num_goals(); ++goal_id) {
        Fact original_fact = parent->get_goal_fact(goal_id);
        if (is_fact_relevant(original_fact)) {
            // cout << parent->get_fact_name(original_fact) << endl;
            projected_goals.push_back(project_fact(original_fact));
        }
    }

    // project initial state
    vector<int> original_initial_state = parent->get_initial_state_values();
    assert(projected_initial_state.empty());
    for (int var_id : variables) {
        projected_initial_state.push_back(original_initial_state[var_id]);
    }
    // cout << "initial state: " << projected_initial_state << endl;
    // cout << "[" << parent->get_fact_name({variables[0], projected_initial_state[0]});
//    for (size_t i = 1; i < projected_initial_state.size(); ++i){
        // cout << ", " << parent->get_fact_name({variables[i], projected_initial_state[i]});
//    }
    // cout << "]" << endl;

    // project numeric initial state
    // NOTE this might break if parent_task is not the root task
    vector<ap_float> original_numeric_initial_state = g_state_registry->get_numeric_vars(g_initial_state());
    assert(projected_numeric_initial_state.empty());
    for (int var_id : numeric_variables) {
        // assignment axioms have been evaluated on the numeric state from the state registry
        projected_numeric_initial_state.push_back(original_numeric_initial_state[var_id]);
    }
    // cout << "numeric initial state: " << projected_numeric_initial_state << endl;

    // project operators
    for (auto op : parent_proxy.get_operators()) {
        bool relevant = false;
        for (auto eff : op.get_effects()){
            if (is_fact_relevant(eff.get_fact())) {
                // cout << "relevant op eff: " << op.get_name() << endl;
                projected_op_to_original_op.push_back(op.get_id());
                relevant = true;
                break;
            }
        }
        if (!relevant) {
            for (auto eff: op.get_ass_effects()) {
                int eff_var_id = eff.get_assignment().get_affected_variable().get_id();
                if (is_numeric_var_relevant(eff_var_id) &&
                    parent->get_numeric_var_type(eff_var_id) != instrumentation) {
                    // cout << "relevant op neff: " << op.get_name() << " num eff on " << eff.get_assignment().get_affected_variable().get_name() << endl;
                    projected_op_to_original_op.push_back(op.get_id());
                    relevant = true;
                    break;
                }
            }
        }

        // also check for auxiliary variables
        const vector<ap_float> &num_effs = task_proxy->get_action_eff_list(op.get_id());
        for (size_t id = 0; id < numeric_variables.size(); ++id) {
            if (is_auxiliary_num_var[id]) {
                int aux_var = task_proxy->map_to_auxiliary_variable_id(numeric_variables[id]);
                assert(aux_var != -1);
                ap_float eff_val = num_effs[task_proxy->get_regular_var_id(aux_var)];
                if (eff_val != 0) {
                    add_auxiliary_constant_if_needed(eff_val);
                    if (!relevant) {
                        projected_op_to_original_op.push_back(op.get_id());
                        relevant = true;
                    }
                }
            }
        }
        auto num_op = task_proxy->get_operators()[op.get_id()];
        for (auto &[var_id, value]: num_op.get_assign_effects()) {
            if (is_auxiliary_num_var[num_var_to_index[var_id]]) {
                add_auxiliary_constant_if_needed(value);
                if (!relevant){
                    projected_op_to_original_op.push_back(op.get_id());
                    relevant = true;
                }
            }
        }
    }

    // project axioms
    for (auto op : parent_proxy.get_axioms()) {
        for (const auto &eff : op.get_effects()){
            if (is_fact_relevant(eff.get_fact())) {
                // cout << "relevant axiom: " << op.get_name() << endl;
                projected_axiom_to_original_axiom.push_back(op.get_id());
                break;
            }
        }
    }

    // project comparison axioms
    for (auto op : parent_proxy.get_comparison_axioms()) {
        assert(op.get_true_fact().get_variable() == op.get_false_fact().get_variable());
        if (is_fact_relevant(op.get_true_fact()) &&
            is_numeric_var_relevant(op.get_left_variable().get_id()) &&
            is_numeric_var_relevant(op.get_right_variable().get_id())) {
            projected_comp_axiom_to_original_comp_axiom.push_back(op.get_id());
            // cout << "relevant comp axiom: " << op.get_true_fact().get_name() << endl;
        }
    }

    // project assignment axioms
    vector<bool> found_axiom_for_aux_var(is_auxiliary_num_var.size(), false);
    for (auto op : parent_proxy.get_assignment_axioms()) {
        int assgn_var_id = op.get_assignment_variable().get_id();
        if (is_numeric_var_relevant(assgn_var_id)) {
            if (is_auxiliary_num_var[num_var_to_index[assgn_var_id]]){
                // for auxiliary variables x, we need to remove the assign axiom that sets
                // their value (e.g. x = y + z); this does not include changes (e.g. x += 2)
                assert(!found_axiom_for_aux_var[num_var_to_index[assgn_var_id]]);
                found_axiom_for_aux_var[num_var_to_index[assgn_var_id]] = true;
                continue;
            }
            set<int> regular_numeric_vars_in_expression;
            get_regular_numeric_vars_recursive(parent_proxy, op, regular_numeric_vars_in_expression);
            // print regular_numeric_vars_in_expression
            if (std::all_of(regular_numeric_vars_in_expression.begin(),
                            regular_numeric_vars_in_expression.end(),
                            [this] (int var) {
                                return is_numeric_var_relevant(var);})) {
                projected_asgn_axiom_to_original_asgn_axiom.push_back(op.get_id());
//                cout << "relevant assgn axiom: "
//                     << op.get_assignment_variable().get_name()
//                     << " = " << op.get_left_variable().get_name()
//                     << op.get_arithmetic_operator_type()
//                     << op.get_right_variable().get_name() << endl;
            }
        }
    }
}

void ProjectedTask::add_auxiliary_constant_if_needed(ap_float value) {
    for (int cvar_id = 0; cvar_id < static_cast<int>(numeric_variables.size()); ++cvar_id){
        if (get_numeric_var_type(cvar_id) == constant &&
                get_initial_state_numeric_values()[cvar_id] == value){
            return;
        }
    }
    num_var_to_index.push_back(static_cast<int>(numeric_variables.size()));
    numeric_variables.push_back(task_proxy->get_num_numeric_variables() + num_auxiliary_constants); // this includes auxiliary vars
    is_auxiliary_constant.resize(numeric_variables.size(), false);
    is_auxiliary_constant.back() = true;
    projected_numeric_initial_state.push_back(value);
    is_auxiliary_num_var.push_back(false);
    ++num_auxiliary_constants;
}

int ProjectedTask::get_auxiliary_constant_id(ap_float value) const {
    for (int cvar_id = 0; cvar_id < static_cast<int>(numeric_variables.size()); ++cvar_id){
        if (get_numeric_var_type(cvar_id) == constant &&
            get_initial_state_numeric_values()[cvar_id] == value){
            return cvar_id;
        }
    }
    cerr << "No const numeric variable found with value " << value << endl;
    utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
}

void ProjectedTask::get_regular_numeric_vars_recursive(const TaskProxy &proxy,
                                                       const AssignmentAxiomProxy &op,
                                                       set<int> &regular_vars) const {
    set<int> var_ids;
    int lvar_id = op.get_left_variable().get_id();
    if (op.get_left_variable().get_var_type() == numType::regular ||
            (is_numeric_var_relevant(lvar_id) && is_auxiliary_num_var[num_var_to_index[lvar_id]])) {
        regular_vars.insert(lvar_id);
    } else {
        var_ids.insert(lvar_id);
    }
    int rvar_id = op.get_right_variable().get_id();
    if (op.get_right_variable().get_var_type() == numType::regular ||
            (is_numeric_var_relevant(rvar_id) && is_auxiliary_num_var[num_var_to_index[rvar_id]])) {
        regular_vars.insert(rvar_id);
    } else {
        var_ids.insert(rvar_id);
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
    return static_cast<int>(variables.size());
}

int ProjectedTask::get_num_numeric_variables() const {
    return static_cast<int>(numeric_variables.size());
}

const string& ProjectedTask::get_variable_name(int var) const {
    assert(var >= 0 && var < static_cast<int>(variables.size()));
    return parent->get_variable_name(variables[var]);
}

const string& ProjectedTask::get_numeric_variable_name(int var) const {
    assert(var >= 0 && var < static_cast<int>(numeric_variables.size()));
    return parent->get_numeric_variable_name(numeric_variables[var]);
}

int ProjectedTask::get_variable_domain_size(int var) const {
    assert(var >= 0 && var < static_cast<int>(variables.size()));
    return parent->get_variable_domain_size(variables[var]);
}

const string &ProjectedTask::get_fact_name(const Fact &fact) const {
    assert(fact.var >= 0 && fact.var < static_cast<int>(variables.size()));
    return parent->get_fact_name({variables[fact.var], fact.value});
}

bool ProjectedTask::are_facts_mutex(const Fact &fact1, const Fact &fact2) const {
    assert(fact1.var >= 0 && fact1.var < static_cast<int>(variables.size()));
    assert(fact2.var >= 0 && fact2.var < static_cast<int>(variables.size()));
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
    return static_cast<int>(projected_goals.size());
}

Fact ProjectedTask::get_goal_fact(int index) const {
    assert(index >= 0 && index < static_cast<int>(projected_goals.size()));
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
    size_t i = 0;
    for (int var : variables){
        state_values[i++] = original_state_values[var];
    }
    assert(i == state_values.size());
    return state_values;
}

vector<ap_float> ProjectedTask::get_numeric_state_values(const GlobalState &global_state) const {
    vector<ap_float> state_values(numeric_variables.size());
    vector<ap_float> original_state_values(parent->get_numeric_state_values(global_state));
    size_t i = 0;
    for (int var : numeric_variables){
        state_values[i++] = original_state_values[var];
    }
    assert(i == state_values.size());
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
    if (!is_axiom) {
        // also check for indirect effects on auxiliary variables
        const vector<ap_float> &num_effs = task_proxy->get_action_eff_list(original_index);
        // cout << parent->get_operator_name(original_index, is_axiom) << ": " << num_effs << endl;
        for (size_t id = 0; id < numeric_variables.size(); ++id) {
            if (is_auxiliary_num_var[id]) {
                int aux_var = task_proxy->map_to_auxiliary_variable_id(numeric_variables[id]);
                assert(aux_var != -1);
                if (num_effs[task_proxy->get_regular_var_id(aux_var)] != 0) {
                    ++num_projected_effects;
                }
            }
        }
        auto op = task_proxy->get_operators()[original_index];
        for (auto &[var_id, value]: op.get_assign_effects()) {
            if (is_auxiliary_num_var[num_var_to_index[var_id]]) {
                ++num_projected_effects;
            }
        }
    }
    return num_projected_effects;
}

int ProjectedTask::get_num_operator_effect_conditions(
        int /*op_index*/, int /*eff_index*/, bool /*is_axiom*/) const {
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
        int /*op_index*/, int /*ass_eff_index*/, bool /*is_axiom*/) const {
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
        int /*op_index*/, int /*eff_index*/, int /*cond_index*/, bool /*is_axiom*/) const {
    cerr << "ERROR: not implemented ProjectedTask::get_operator_effect_condition" << endl;
    utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
}

Fact ProjectedTask::get_operator_ass_effect_condition(
        int /*op_index*/, int /*ass_eff_index*/, int /*cond_index*/, bool /*is_axiom*/) const {
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
    if (!is_axiom) {
        // also check for auxiliary variables
        const vector<ap_float> &num_effs = task_proxy->get_action_eff_list(original_index);
        for (int id = 0; id < static_cast<int>(numeric_variables.size()); ++id) {
            if (is_auxiliary_num_var[id]) {
                ap_float eff_val = num_effs[task_proxy->get_regular_var_id(task_proxy->map_to_auxiliary_variable_id(numeric_variables[id]))];
                if (eff_val != 0) {
                    if (projected_eff_index == eff_index) {
                        return {id, increase, get_auxiliary_constant_id(eff_val)};
                    }
                    ++projected_eff_index;
                }
            }
        }
        auto op = task_proxy->get_operators()[original_index];
        for (auto &[var, value]: op.get_assign_effects()) {
            int var_id = num_var_to_index[var];
            if (is_auxiliary_num_var[var_id]) {
                if (projected_eff_index == eff_index) {
                    return {var_id, assign, get_auxiliary_constant_id(value)};
                }
                ++projected_eff_index;
            }
        }
    }
    // This should never happen if eff_index is valid.
    ABORT("Invalid effect index in ProjectedTask::get_operator_ass_effect");
}

int ProjectedTask::get_num_ass_axioms() const {
    return static_cast<int>(projected_asgn_axiom_to_original_asgn_axiom.size());
}

int ProjectedTask::get_num_cmp_axioms() const {
    return static_cast<int>(projected_comp_axiom_to_original_comp_axiom.size());
}

Fact ProjectedTask::get_comparison_axiom_effect(int axiom_index,
                                                bool evaluation_result) const {
    assert(axiom_index >= 0 && axiom_index < static_cast<int>(projected_comp_axiom_to_original_comp_axiom.size()));
    int original_index = projected_comp_axiom_to_original_comp_axiom[axiom_index];
    return project_fact(parent->get_comparison_axiom_effect(original_index, evaluation_result));
}

int ProjectedTask::get_comparison_axiom_argument(int axiom_index,
                                                 bool left) const {
    assert(axiom_index >= 0 && axiom_index < static_cast<int>(projected_comp_axiom_to_original_comp_axiom.size()));
    int original_index = projected_comp_axiom_to_original_comp_axiom[axiom_index];
    return num_var_to_index[parent->get_comparison_axiom_argument(original_index, left)];
}

comp_operator ProjectedTask::get_comparison_axiom_operator(int axiom_index) const {
    assert(axiom_index >= 0 && axiom_index < static_cast<int>(projected_comp_axiom_to_original_comp_axiom.size()));
    int original_index = projected_comp_axiom_to_original_comp_axiom[axiom_index];
    return parent->get_comparison_axiom_operator(original_index);
}

int ProjectedTask::get_assignment_axiom_effect(int axiom_index) const {
    assert(axiom_index >= 0 && axiom_index < static_cast<int>(projected_asgn_axiom_to_original_asgn_axiom.size()));
    int original_index = projected_asgn_axiom_to_original_asgn_axiom[axiom_index];
    return num_var_to_index[parent->get_assignment_axiom_effect(original_index)];
}

int ProjectedTask::get_assignment_axiom_argument(int axiom_index,
                                                 bool left) const {
    assert(axiom_index >= 0 && axiom_index < static_cast<int>(projected_asgn_axiom_to_original_asgn_axiom.size()));
    int original_index = projected_asgn_axiom_to_original_asgn_axiom[axiom_index];
    return num_var_to_index[parent->get_assignment_axiom_argument(original_index, left)];
}

cal_operator ProjectedTask::get_assignment_axiom_operator(int axiom_index) const {
    assert(axiom_index >= 0 && axiom_index < static_cast<int>(projected_asgn_axiom_to_original_asgn_axiom.size()));
    return parent->get_assignment_axiom_operator(projected_asgn_axiom_to_original_asgn_axiom[axiom_index]);
}

const GlobalOperator *ProjectedTask::get_global_operator(int /*index*/, bool /*is_axiom*/) const {
    assert(false);
    cerr << "ERROR: not implemented ProjectedTask::get_global_operator" << endl;
    utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
}

numType ProjectedTask::get_numeric_var_type(int index) const {
    assert(index >= 0 && index < static_cast<int>(numeric_variables.size()));
    if (is_auxiliary_num_var[index]) {
        // this is an auxiliary variable added by numeric_pdb_helper::NumericTaskProxy
        // we make it a regular variable in the ProjectedTask and set its value manually
        return numType::regular;
    }
    if (index < static_cast<int>(is_auxiliary_constant.size()) && is_auxiliary_constant[index]){
        return numType::constant;
    }
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
    vector<ap_float> projected_num_state(numeric_variables.size());
    // copy&map variable values from num_state
    for (size_t i = 0; i < pattern.numeric.size(); ++i) {
        int var = pattern.numeric[i];
        if (var >= parent->get_num_numeric_variables()){
            assert(is_auxiliary_num_var[i]);
            // this is an auxiliary variable
            var = task_proxy->map_to_derived_variable_id(var);
            // cout << "is aux: ";
        }
        int projected_id = num_var_to_index[var];
        assert(projected_id != -1);
        projected_num_state[projected_id] = num_state[i];
        assert(!set_numeric_var[projected_id]);
        set_numeric_var[projected_id] = true;
        // cout << "set var " << projected_id << " from " << var << endl;
    }

    //cout << "numeric variables: " << numeric_variables << endl;
    for (int var = 0; var < static_cast<int>(numeric_variables.size()); ++var){
        if (get_numeric_var_type(var) == numType::constant || get_numeric_var_type(var) == numType::instrumentation) {
            projected_num_state[var] = get_initial_state_numeric_values()[var];
            assert(!set_numeric_var[var]);
            set_numeric_var[var] = true;
            // cout << "set var " << var << " const / inst" << endl;
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
        if (get_numeric_var_type(left_var) == numType::regular ||
                get_numeric_var_type(left_var) == numType::derived){
            assert(set_numeric_var[left_var]);
            left_val = projected_num_state[left_var];
        } else if (get_numeric_var_type(left_var) == numType::constant){
            left_val = get_initial_state_numeric_values()[left_var];
        } else {
            assert(get_numeric_var_type(left_var) == numType::instrumentation);
            // must be the costs variable
        }
        ap_float right_val = 0;
        if (get_numeric_var_type(right_var) == numType::regular ||
                get_numeric_var_type(right_var) == numType::derived){
            assert(set_numeric_var[right_var]);
            right_val = projected_num_state[right_var];
        } else if (get_numeric_var_type(right_var) == numType::constant){
            right_val = get_initial_state_numeric_values()[right_var];
        } else {
            assert(get_numeric_var_type(right_var) == numType::instrumentation);
            // must be the costs variable
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

        // cout << "set var " << eff_var << " from ax " << ax_id << endl;
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
        //cout << "left_var: " << left_var << ", right_var: " << right_var << endl;
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
        assert(projected_prop_state[i] != -1);
    }
    //cout << "projected_prop_state: " << projected_prop_state << endl;
    //cout << "projected_num_state: " << projected_num_state << endl;
    //cout << "pre-projected state: " << prop_state << endl;
    //cout << "pre-projected num state: " << num_state << endl;
    return {*this, std::move(projected_prop_state), std::move(projected_num_state)};
}
}
