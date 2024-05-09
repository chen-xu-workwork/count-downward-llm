#include "variable_order_finder.h"

#include "causal_graph.h"
#include "numeric_condition.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

using namespace std;
using utils::ExitCode;

namespace numeric_pdbs {
VariableOrderFinder::VariableOrderFinder(const shared_ptr<AbstractTask> task,
                                         shared_ptr<numeric_pdb_helper::NumericTaskProxy> num_task_proxy,
                                         VariableOrderType variable_order_type,
                                         bool is_pdb)
        : task(task),
          variable_order_type(variable_order_type),
          is_pdb(is_pdb) {

    // TODO: add options that determine the order of variable types, i.e. first prop, then numeric or vice versa
    //  this should then be used for numeric CG predecessors and for goals

    TaskProxy task_proxy(*task);
    for (auto var : task_proxy.get_variables()){
        if (!num_task_proxy->is_numeric_variable(var) &&
            !task_proxy.is_derived_variable(var)){
            remaining_vars.emplace_back(var.get_id(), false);
        }
    }
    for (auto var : task_proxy.get_numeric_variables()){
        if (var.get_var_type() == regular){
            remaining_vars.emplace_back(var.get_id(), true);
        }
    }

    if (variable_order_type == REVERSE_LEVEL) {
        reverse(remaining_vars.begin(), remaining_vars.end());
    } else if (variable_order_type == CG_GOAL_RANDOM ||
        variable_order_type == RANDOM) {
        // TODO add random to options
        shuffle(remaining_vars.begin(), remaining_vars.end(), std::mt19937(std::random_device()()));
    }

    is_causal_predecessor.resize(task->get_num_variables() + task->get_num_numeric_variables(), false);
    is_goal_variable.resize(task->get_num_variables(), false);
    for (FactProxy goal: task_proxy.get_goals()) {
        if (!task_proxy.is_derived_variable(goal.get_variable())) {
            is_goal_variable[goal.get_variable().get_id()] = true;
        }
    }
    is_numeric_goal_variable.resize(task->get_num_numeric_variables(), false);
    for (const auto &n_goal : num_task_proxy->get_numeric_goals()){
        is_numeric_goal_variable[n_goal->get_var_id()] = true;
    }
}

void VariableOrderFinder::select_next(int position, int var_no, bool is_numeric) {
    assert(remaining_vars[position].first == var_no && remaining_vars[position].second == is_numeric);
    remaining_vars.erase(remaining_vars.begin() + position);
    selected_vars.push_back(var_no);

    const numeric_pdbs::CausalGraph &cg = get_numeric_causal_graph(task.get());
    if (is_numeric) {
        for (int var: cg.get_num_eff_to_prop_pre(var_no)){
            is_causal_predecessor[var] = true;
        }
        for (int var: cg.get_num_eff_to_num_pre(var_no)){
            is_causal_predecessor[task->get_num_variables() + var] = true;
        }
    } else {
        for (int var: cg.get_prop_eff_to_prop_pre(var_no)){
            is_causal_predecessor[var] = true;
        }
        for (int var: cg.get_prop_eff_to_num_pre(var_no)){
            is_causal_predecessor[task->get_num_variables() + var] = true;
        }
    }
}

bool VariableOrderFinder::done() const {
    return remaining_vars.empty();
}

pair<int, bool> VariableOrderFinder::next() {
    assert(!done());
    if (variable_order_type == CG_GOAL_LEVEL || variable_order_type
                                                == CG_GOAL_RANDOM) {
        // First run: Try to find a causally connected variable.
        for (size_t i = 0; i < remaining_vars.size(); ++i) {
            auto [var_no, is_num] = remaining_vars[i];
            int id = is_num ? task->get_num_variables() + var_no : var_no;
            if (is_causal_predecessor[id]) {
                select_next(i, var_no, is_num);
                return {var_no, is_num};
            }
        }
        // Second run: Try to find a goal variable.
        for (size_t i = 0; i < remaining_vars.size(); ++i) {
            auto [var_no, is_num] = remaining_vars[i];
            if (!is_num && is_goal_variable[var_no]) {
                select_next(i, var_no, is_num);
                return {var_no, is_num};
            }
            if (is_num && is_numeric_goal_variable[var_no]) {
                select_next(i, var_no, is_num);
                return {var_no, is_num};
            }
        }
    } else if (variable_order_type == GOAL_CG_LEVEL) {
        // First run: Try to find a goal variable.
        for (size_t i = 0; i < remaining_vars.size(); ++i) {
            auto [var_no, is_num] = remaining_vars[i];
            if (!is_num && is_goal_variable[var_no]) {
                select_next(i, var_no, is_num);
                return {var_no, is_num};
            }
            if (is_num && is_numeric_goal_variable[var_no]) {
                select_next(i, var_no, is_num);
                return {var_no, is_num};
            }
        }
        // Second run: Try to find a causally connected variable.
        for (size_t i = 0; i < remaining_vars.size(); ++i) {
            auto [var_no, is_num] = remaining_vars[i];
            int id = is_num ? task->get_num_variables() + var_no : var_no;
            if (is_causal_predecessor[id]) {
                select_next(i, var_no, is_num);
                return {var_no, is_num};
            }
        }
    } else if (variable_order_type == RANDOM ||
               variable_order_type == LEVEL ||
               variable_order_type == REVERSE_LEVEL) {
        auto [var_no, is_num] = remaining_vars[0];
        select_next(0, var_no, is_num);
        return {var_no, is_num};
    }
    if (is_pdb) {
        return {-1, false}; // Avoiding derived variables in PDB, as they are not causally connected.
    }
    cerr << "Relevance analysis has not been performed." << endl;
    utils::exit_with(ExitCode::INPUT_ERROR);
}

void VariableOrderFinder::dump() const {
    cout << "Variable order type: ";
    switch (variable_order_type) {
        case CG_GOAL_LEVEL:
            cout << "CG/GOAL, tie breaking on level (main)";
            break;
        case CG_GOAL_RANDOM:
            cout << "CG/GOAL, tie breaking random";
            break;
        case GOAL_CG_LEVEL:
            cout << "GOAL/CG, tie breaking on level";
            break;
        case RANDOM:
            cout << "random";
            break;
        case LEVEL:
            cout << "by level";
            break;
        case REVERSE_LEVEL:
            cout << "by reverse level";
            break;
        default:
            ABORT("Unknown variable order type.");
    }
    cout << endl;
}
}
