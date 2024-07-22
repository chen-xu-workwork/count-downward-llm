#include "numeric_helper.h"

#include "../axioms.h"
#include "numeric_condition.h"
#include "../utils/logging.h"

#include <sstream>

using namespace std;
using namespace numeric_condition;

namespace numeric_pdb_helper {

ostream &operator<<(ostream &os, const LinearNumericCondition &lnc) {
    for (ap_float coeff : lnc.coefficients) {
        os << " " << coeff;
    }
    os << " " << lnc.constant;
    return os;
}

NumericTaskProxy::NumericTaskProxy(const TaskProxy &task) :
        task_proxy(task),
        n_numeric_variables(0) {
    build_numeric_variables();
    build_artificial_variables();
    find_derived_numeric_variables();
    build_actions();
    build_numeric_goals();
}

void NumericTaskProxy::build_numeric_variables() {
    NumericVariablesProxy num_variables = task_proxy.get_numeric_variables();
    reg_num_var_id_to_glob_var_id.assign(num_variables.size(), -1);
    glob_var_id_to_reg_num_var_id.assign(num_variables.size(), -1);
    for (size_t num_id = 0; num_id < num_variables.size(); ++num_id) {
        if (num_variables[num_id].get_var_type() == regular) {
            reg_num_var_id_to_glob_var_id[n_numeric_variables] = num_id;
            glob_var_id_to_reg_num_var_id[num_id] = n_numeric_variables;
            ++n_numeric_variables;
        }
    }
}

void NumericTaskProxy::build_artificial_variables() {
    // variables initialization
    NumericVariablesProxy num_variables = task_proxy.get_numeric_variables();
    AssignmentAxiomsProxy assignment_axioms = task_proxy.get_assignment_axioms();
    artificial_variables.assign(num_variables.size(), LinearNumericCondition(n_numeric_variables));
    for (size_t num_id = 0; num_id < num_variables.size(); ++num_id) {
        // artificial_variables[num_id].coefficients.assign(n_numeric_variables,0);
        if (num_variables[num_id].get_var_type() == regular) {
            artificial_variables[num_id].coefficients[glob_var_id_to_reg_num_var_id[num_id]] = 1;
            // cout << num_id << " regular " << artificial_variables[num_id] << " " <<
            // num_variables[num_id].get_name() << endl;
        } else if (num_variables[num_id].get_var_type()) {
            artificial_variables[num_id].constant = num_variables[num_id].get_initial_state_value();
            // cout << num_id << " constant : " <<
            // num_variables[num_id].get_initial_state_value() << " " <<
            // artificial_variables[num_id] << " " << num_variables[num_id].get_name()
            // << endl;
        }
    }

    // populate artificial variables using ass axioms
    // initialize artificial variables
    for (size_t ax_id = 0; ax_id < assignment_axioms.size(); ++ax_id) {
        int affected_variable =
                assignment_axioms[ax_id].get_assignment_variable().get_id();
        int lhs = assignment_axioms[ax_id].get_left_variable().get_id();
        int rhs = assignment_axioms[ax_id].get_right_variable().get_id();

        switch (assignment_axioms[ax_id].get_arithmetic_operator_type()) {
            case sum:
                for (size_t num_id = 0; num_id < n_numeric_variables; ++num_id) {
                    artificial_variables[affected_variable].coefficients[num_id] =
                            artificial_variables[lhs].coefficients[num_id] +
                            artificial_variables[rhs].coefficients[num_id];
                }
                artificial_variables[affected_variable].constant =
                        artificial_variables[lhs].constant +
                        artificial_variables[rhs].constant;
                break;
            case diff:
                for (size_t num_id = 0; num_id < n_numeric_variables; ++num_id) {
                    artificial_variables[affected_variable].coefficients[num_id] =
                            artificial_variables[lhs].coefficients[num_id] -
                            artificial_variables[rhs].coefficients[num_id];
                }
                artificial_variables[affected_variable].constant =
                        artificial_variables[lhs].constant -
                        artificial_variables[rhs].constant;
                break;
            case mult:
                assert((num_variables[lhs].get_var_type() != constant) ||
                       (num_variables[rhs].get_var_type() != constant));
                if (num_variables[lhs].get_var_type() == constant) {
                    for (size_t num_id = 0; num_id < n_numeric_variables; ++num_id) {
                        artificial_variables[affected_variable].coefficients[num_id] =
                                artificial_variables[lhs].constant *
                                artificial_variables[rhs].coefficients[num_id];
                    }
                    artificial_variables[affected_variable].constant =
                            artificial_variables[lhs].constant *
                            artificial_variables[rhs].constant;
                } else {
                    for (size_t num_id = 0; num_id < n_numeric_variables; ++num_id) {
                        artificial_variables[affected_variable].coefficients[num_id] =
                                artificial_variables[lhs].coefficients[num_id] *
                                artificial_variables[rhs].constant;
                    }
                    artificial_variables[affected_variable].constant =
                            artificial_variables[lhs].constant *
                            artificial_variables[rhs].constant;
                }
                break;
            case divi:
                assert(num_variables[rhs].get_var_type() != constant);
                for (size_t num_id = 0; num_id < n_numeric_variables; ++num_id) {
                    artificial_variables[affected_variable].coefficients[num_id] =
                            artificial_variables[lhs].coefficients[num_id] /
                            artificial_variables[rhs].constant;
                }
                artificial_variables[affected_variable].constant =
                        artificial_variables[lhs].constant /
                        artificial_variables[rhs].constant;
                break;
            default:
                cerr << "Error: No assignment operators are allowed here." << endl;
                utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        }
    }
}

void NumericTaskProxy::build_action(const OperatorProxy &op, size_t op_id) {

    for (const AssEffectProxy &eff : op.get_ass_effects()) {
        assert(eff.get_conditions().empty());

        int lhs = eff.get_assignment().get_affected_variable().get_id();

        if (task_proxy.get_numeric_variables()[lhs].get_var_type() == instrumentation) {
            continue;
        }

        int id_num = glob_var_id_to_reg_num_var_id[lhs];

        assert(id_num != -1);

        int rhs = eff.get_assignment().get_assigned_variable().get_id();

        f_operator oper = eff.get_assignment().get_assigment_operator_type();

        const LinearNumericCondition &av = artificial_variables[rhs];
        bool is_simple_effect = oper == increase || oper == decrease;

        if (is_simple_effect) {
            if (oper == increase)
                actions[op_id].eff_list[id_num] = av.constant;
            if (oper == decrease)
                actions[op_id].eff_list[id_num] = -av.constant;
        } else {
            switch (oper) {
                case (assign):
                    cout << task_proxy.get_numeric_variables()[lhs].get_name() << " := " << av.constant << endl;
                    actions[op_id].asgn_effs.emplace_back(lhs, av.constant);
                    break;
//                case (increase): {
//                    for (int var = 0; var < n_numeric_variables; ++var) {
//                        coefficients[var] = av.coefficients[var];
//                        if (id_num == var) coefficients[var] += 1.0;
//                    }
//                    actions[op_id].linear_eff_coefficeints.push_back(coefficients);
//                    actions[op_id].linear_eff_constants.push_back(av.constant);
//                    break;
//                }
//                case (decrease): {
//                    for (int var = 0; var < n_numeric_variables; ++var) {
//                        coefficients[var] = -av.coefficients[var];
//                        if (id_num == var) coefficients[var] += 1.0;
//                    }
//                    actions[op_id].linear_eff_coefficeints.push_back(coefficients);
//                    actions[op_id].linear_eff_constants.push_back(-av.constant);
//                    break;
//                }
                default: {
                    cerr << "non-linear numeric effect, only assignment effects are supported" << endl;
                    utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
                }
            }
        }
    }
}

void NumericTaskProxy::build_actions() {
    OperatorsProxy ops = task_proxy.get_operators();
    AxiomsProxy axioms = task_proxy.get_axioms();
    actions.assign(ops.size() + axioms.size(), Action(n_numeric_variables));
    for (size_t op_id = 0; op_id < ops.size(); ++op_id)
        build_action(ops[op_id], op_id);
    for (size_t op_id = 0; op_id < axioms.size(); ++op_id)
        build_action(axioms[op_id], ops.size() + op_id);
}

inline int get_achieving_comp_axiom(const TaskProxy &proxy, const FactProxy &condition) {
    for (auto op : proxy.get_comparison_axioms()) {
        if (condition.get_variable() == op.get_true_fact().get_variable()) {
            return op.get_id();
        }
    }
    return -1;
}

inline int get_achieving_assgn_axiom(const TaskProxy &proxy, int var_id) {
    for (auto op: proxy.get_assignment_axioms()) {
        if (var_id == op.get_assignment_variable().get_id()) {
            return op.get_id();
        }
    }
    return -1;
}

bool NumericTaskProxy::is_derived_numeric_variable(const VariableProxy &var_proxy) const {
    return is_derived_num_var[var_proxy.get_id()];
}

int NumericTaskProxy::get_regular_var_id(int num_var_id) const {
    assert(glob_var_id_to_reg_num_var_id[num_var_id] != -1);
    return glob_var_id_to_reg_num_var_id[num_var_id];
}

int NumericTaskProxy::get_number_propositional_variables() const {
    // TODO precompute and cache this
    int num_prop_variables = 0;
    for (auto var: task_proxy.get_variables()) {
        if (!is_derived_numeric_variable(var) && !task_proxy.is_derived_variable(var)) {
            ++num_prop_variables;
        }
    }
    return num_prop_variables;
}

int NumericTaskProxy::get_number_regular_numeric_variables() const {
    return n_numeric_variables;
}

shared_ptr<RegularNumericCondition> NumericTaskProxy::get_regular_numeric_condition(const FactProxy &condition) /*const*/ {
    assert(!task_proxy.is_derived_variable(condition.get_variable()) &&
                   is_derived_numeric_variable(condition.get_variable()));

    int var_id = condition.get_variable().get_id();

    if (static_cast<size_t>(var_id) >= regular_numeric_conditions.size()){
        regular_numeric_conditions.resize(task_proxy.get_variables().size());
    }
    if (static_cast<size_t>(condition.get_value()) >= regular_numeric_conditions[var_id].size()){
        regular_numeric_conditions[var_id].resize(task_proxy.get_variables()[var_id].get_domain_size());
    }
    if (regular_numeric_conditions[var_id][condition.get_value()]){
        return regular_numeric_conditions[var_id][condition.get_value()];
    }

    int c_op_id = get_achieving_comp_axiom(task_proxy, condition);

    if (c_op_id == -1){
        cerr << "ERROR: could not find a comparison axiom that achieves this fact, is it propositional? " << condition.get_name() << endl;
        utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
    }

    ComparisonAxiomProxy c_op = task_proxy.get_comparison_axioms()[c_op_id];
//    cout << endl << "found comparison op: " << endl
//         << c_op.get_left_variable().get_name() << c_op.get_left_variable().get_var_type() << endl
//         << c_op.get_right_variable().get_name() << c_op.get_right_variable().get_var_type() << endl
//         << c_op.get_comparison_operator_type()
//         << endl;

    ap_float const_ = numeric_limits<double>::max();
    switch (c_op.get_right_variable().get_var_type()) {
//        case regular:
//            // this does not seem to happen due to the normal form of the translated task
//            num_var_id = c_op.get_left_variable().get_id();
//            break;
        case constant:
            const_ = c_op.get_right_variable().get_initial_state_value();
            break;

        default: // could be instrumentation or unkonwn
            cerr << "ERROR: not sure what to do here3; got numeric variable of type " << c_op.get_right_variable().get_var_type() << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
    }

    shared_ptr<RegularNumericCondition> num_condition;

    int num_var_id = -1;
    ap_float l_const;
    cal_operator arth_op;
    switch(c_op.get_left_variable().get_var_type()) {
        case regular:
            num_var_id = c_op.get_left_variable().get_id();
            num_condition = make_shared<RegularNumericConditionVar>(num_var_id,
                                                                    c_op.get_comparison_operator_type(),
                                                                    const_);
            break;
        case derived: {
            int assgn_op_id = get_achieving_assgn_axiom(task_proxy,
                                                        c_op.get_left_variable().get_id());
            if (assgn_op_id == -1) {
                cerr << "ERROR: could not find a assigment axiom that achieves this fact, is it propositional? "
                     << condition.get_name() << endl;
                utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
            }
            AssignmentAxiomProxy assgn_ax = task_proxy.get_assignment_axioms()[assgn_op_id];

            ap_float op_const;
            comp_operator new_c_op;
            if (assgn_ax.get_left_variable().get_var_type() == regular) {
                // var arth_op op_const c_op const_; e.g. var - 2 >= 0
                assert(assgn_ax.get_right_variable().get_var_type() == constant);
                num_var_id = assgn_ax.get_left_variable().get_id();
                op_const = assgn_ax.get_right_variable().get_initial_state_value();
                new_c_op = c_op.get_comparison_operator_type();
            } else if (assgn_ax.get_right_variable().get_var_type() == regular) {
                // op_const arth_op var c_op const_; e.g. 2 - var >= 1
                // transformed to var arth_op op_const new_c_op -const_; e.g. var - 2 <= -1
                assert(assgn_ax.get_left_variable().get_var_type() == constant);
                num_var_id = assgn_ax.get_right_variable().get_id();
                op_const = assgn_ax.get_left_variable().get_initial_state_value();
                new_c_op = get_mirror_op(c_op.get_comparison_operator_type());
                const_ = -const_;
            } else {
                cerr << assgn_ax.get_left_variable().get_name()
                     << assgn_ax.get_arithmetic_operator_type()
                     << assgn_ax.get_right_variable().get_name() << endl;
                cerr << "ERROR: not sure what to do here; no regular numeric variable in assignment axiom "
                     << assgn_ax.get_id() << endl;
                utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
            }
            arth_op = assgn_ax.get_arithmetic_operator_type();
            num_condition = make_shared<RegularNumericConditionVarOpC>(num_var_id,
                                                                       arth_op,
                                                                       op_const,
                                                                       new_c_op,
                                                                       const_);
            break;
        }
        case constant:
            l_const = c_op.get_left_variable().get_initial_state_value();
            num_var_id = c_op.get_left_variable().get_id();
            num_condition = make_shared<RegularNumericConditionConst>(num_var_id,
                                                                      l_const,
                                                                      c_op.get_comparison_operator_type(),
                                                                      const_);

            break;
        default: // could be instrumentation or unkonwn
            cerr << "ERROR: not sure what to do here2; got numeric variable of type " << c_op.get_left_variable().get_var_type() << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
    }

    regular_numeric_conditions[var_id][condition.get_value()] = num_condition;

    return regular_numeric_conditions[var_id][condition.get_value()];
}

void NumericTaskProxy::find_derived_numeric_variables() {
    is_derived_num_var.assign(task_proxy.get_variables().size(), false);
    for (auto op : task_proxy.get_comparison_axioms()) {
        is_derived_num_var[op.get_true_fact().get_variable().get_id()] = true;
    }
}

void NumericTaskProxy::build_numeric_goals() {
    // reconstruct regular numeric goals
    for (auto axiom : task_proxy.get_axioms()){
        if (!axiom.get_preconditions().empty()) {
            for (auto pre: axiom.get_preconditions()) {
                regular_numeric_goals.push_back(get_regular_numeric_condition(pre));
            }
            assert(axiom.get_effects().size() == 1);
        }
    }
}

const vector<shared_ptr<RegularNumericCondition>> &NumericTaskProxy::get_numeric_goals() const {
    return regular_numeric_goals;
}

int NumericTaskProxy::get_approximate_domain_size(NumericVariableProxy num_var) {
    assert(g_numeric_var_types[num_var.get_id()] == numType::regular);
    if (approximate_num_var_domain_sizes.empty()){
        approximate_num_var_domain_sizes.resize(task_proxy.get_numeric_variables().size(), -1);
    }
    if (approximate_num_var_domain_sizes[num_var.get_id()] == -1){
        // TODO precompute this on construction
        ap_float min_increment = numeric_limits<ap_float>::max();
        ap_float min_const = numeric_limits<ap_float>::max();
        ap_float max_const = numeric_limits<ap_float>::lowest();

        for (const auto &op : task_proxy.get_operators()){
            for (const auto &pre : op.get_preconditions()){
                if (!task_proxy.is_derived_variable(pre.get_variable()) &&
                        is_derived_numeric_variable(pre.get_variable())) {
                    const auto &num_cond = get_regular_numeric_condition(pre);
                    if (num_cond->has_constant()) {
                        ap_float c = num_cond->get_constant();
                        min_const = min(min_const, c);
                        max_const = max(max_const, c);
                    }
                }
            }
            const auto &num_effs = get_action_eff_list(op.get_id());
            ap_float eff = num_effs[get_regular_var_id(num_var.get_id())];
            if (eff != 0) {
                min_increment = min(min_increment, abs(num_effs[get_regular_var_id(num_var.get_id())]));
            }
        }

        ap_float ini_val = g_initial_state_numeric[num_var.get_id()];
        min_const = min(min_const, ini_val);
        max_const = max(max_const, ini_val);

        for (const auto & goal : get_numeric_goals()){
            if (num_var.get_id() == goal->get_var_id() && goal->has_constant()){
                ap_float c = goal->get_constant();
                min_const = min(min_const, c);
                max_const = max(max_const, c);
            }
        }

        assert(abs((max_const - min_const) / min_increment) <= numeric_limits<int>::max());
        approximate_num_var_domain_sizes[num_var.get_id()] = static_cast<int>(abs((max_const - min_const) / min_increment));
    }
    return approximate_num_var_domain_sizes[num_var.get_id()];
}
}
