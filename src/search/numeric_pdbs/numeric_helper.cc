#include "numeric_helper.h"

#include "arithmetic_expression.h"
#include "causal_graph.h"
#include "numeric_condition.h"

#include "../axioms.h"

#include <sstream>
#include <unordered_set>

using namespace std;
using namespace arithmetic_expression;
using namespace numeric_condition;

namespace numeric_pdb_helper {

ostream &operator<<(ostream &os, const LinearNumericCondition &lnc) {
    for (ap_float coeff : lnc.coefficients) {
        os << " " << coeff;
    }
    os << " " << lnc.constant;
    return os;
}

NumericTaskProxy::NumericTaskProxy(shared_ptr<AbstractTask> task_) :
        task(std::move(task_)),
        task_proxy(*task),
        n_numeric_variables(0),
        initial_state_values(task->get_initial_state_numeric_values()) {
    verify_is_restricted_numeric_task(task_proxy);
    double start_time = utils::g_timer();
    build_numeric_variables();
    find_derived_numeric_variables();
    build_numeric_preconditions();
    build_goals();
    build_actions();
    cout << "Time to build restricted numeric task: " << utils::g_timer() - start_time << "s" << endl;
    cout << "Number auxiliary numeric variables: " << auxiliary_numeric_variables.size() << endl;
}

bool NumericTaskProxy::is_derived_variable(const VariableProxy &var) const {
    for (auto ax : task_proxy.get_axioms()){
        for (auto eff : ax.get_effects()) {
            if (eff.get_fact().get_variable().get_id() == var.get_id()) {
                return true;
            }
        }
    }
    return false;
}

const numeric_pdbs::CausalGraph &NumericTaskProxy::get_numeric_causal_graph() const {
    return ::get_numeric_causal_graph(this);
}

void NumericTaskProxy::verify_is_restricted_numeric_task(const TaskProxy &task_proxy) {
    verify_no_non_numeric_axioms(task_proxy);
    verify_no_conditional_effects(task_proxy);
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
    reg_num_var_id_to_glob_var_id.resize(n_numeric_variables);
}

void NumericTaskProxy::build_action(const OperatorProxy &op, size_t op_id) {

    auto &action = actions[op_id];

    for (FactProxy pre : op.get_preconditions()){
        assert(!is_derived_variable(pre.get_variable()));
        if (is_derived_numeric_variable(pre.get_variable())) {
            action.numeric_preconditions.push_back(build_condition(pre));
        } else {
            action.preconditions.push_back(pre);
        }
    }

    for (const AssEffectProxy &eff : op.get_ass_effects()) {
        assert(eff.get_conditions().empty());

        int lhs = eff.get_assignment().get_affected_variable().get_id();

        if (task_proxy.get_numeric_variables()[lhs].get_var_type() == instrumentation) {
            // we ignore the cost function here
            continue;
        }

        int id_num = get_regular_var_id(lhs);

        assert(id_num != -1);

        auto rhs = eff.get_assignment().get_assigned_variable();

        f_operator oper = eff.get_assignment().get_assigment_operator_type();

        const auto expr = parse_arithmetic_expression(rhs);

        if (!expr->is_constant()){
            cerr << "non-simple numeric effect in action " << op.get_name() << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        }

        ap_float eff_value = expr->evaluate();

        switch (oper) {
            case (assign):
                action.asgn_effs.emplace_back(lhs, eff_value);
                break;
            case (increase):
                action.eff_list[id_num] = eff_value;
//                for (size_t var = 0; var < n_numeric_variables; ++var) {
//                        coefficients[var] = av.coefficients[var];
//                        if (id_num == var) coefficients[var] += 1.0;
//                }
//                    action.linear_eff_coefficeints.push_back(coefficients);
//                    action.linear_eff_constants.push_back(av.constant);
                break;
            case (decrease):
                action.eff_list[id_num] = -eff_value;
//                for (size_t var = 0; var < n_numeric_variables; ++var) {
//                        coefficients[var] = -av.coefficients[var];
//                        if (id_num == var) coefficients[var] += 1.0;
//                }
//                    action.linear_eff_coefficeints.push_back(coefficients);
//                    action.linear_eff_constants.push_back(-av.constant);
                break;
            default: {
                cerr << "non-linear numeric effect in action " << op.get_name() << endl;
                utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
            }
        }
    }
    vector<ap_float> eff_values(task->get_num_numeric_variables() + auxiliary_numeric_variables.size());
    //        cout << "action effects:" << endl;
    for (int reg_var_id = 0; reg_var_id < static_cast<int>(action.eff_list.size()); ++reg_var_id){
        assert(eff_values[get_global_var_id(reg_var_id)] == 0);
        eff_values[get_global_var_id(reg_var_id)] = action.eff_list[reg_var_id];
//            if (action.eff_list[reg_var_id] != 0) {
//                cout << "var" << get_global_var_id(reg_var_id) << " += " << action.eff_list[reg_var_id] << endl;
//            }
    }
    for (const auto &aux_var : auxiliary_numeric_variables){
        if (action.asgn_effs.empty()) {
            ap_float val = aux_var.expr->evaluate_ignore_additive_consts(eff_values);
            action.eff_list[get_regular_var_id(aux_var.var_id)] = val;
        } else {
            cerr << "ERROR: actions that assign and inc/decrement variables " << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);

            for (const auto &[asgn_var, asgn_val] : action.asgn_effs){
                if (find(affected_vars.begin(), affected_vars.end(), asgn_var) != affected_vars.end()) {
                    eff_values[asgn_var] = asgn_val;
                }
            }

            ap_float val = aux_var.expr->evaluate_ignore_additive_consts(eff_values);

            action.asgn_effs.emplace_back(aux_var.var_id, val);

            for (const auto &[asgn_var, asgn_val] : action.asgn_effs){
                if (find(affected_vars.begin(), affected_vars.end(), asgn_var) != affected_vars.end()) {
                    eff_values[asgn_var] = 0;
                }
            }
        }
//        cout << aux_var.expr->get_name() << endl;
//
//        cout << "effect value for var" << aux_var.var_id << " is " << val << endl;
//        cout << "------------------" << endl;
    }
//    cout << "final effs of action: " << action.eff_list << endl;
}

void NumericTaskProxy::build_actions() {
    OperatorsProxy ops = task_proxy.get_operators();
    actions.assign(ops.size(), Action(n_numeric_variables));
    for (size_t op_id = 0; op_id < ops.size(); ++op_id) {
        build_action(ops[op_id], op_id);
    }
}

inline int get_achieving_comp_axiom(const TaskProxy &proxy, const FactProxy &condition) {
    for (auto op : proxy.get_comparison_axioms()) {
        if (condition.get_variable() == op.get_true_fact().get_variable()) {
            return op.get_id();
        }
    }
    return -1;
}

shared_ptr<ArithmeticExpressionVar> NumericTaskProxy::create_auxiliary_variable(
        const string &name,
        shared_ptr<ArithmeticExpression> expr) {

    expr = expr->simplify();

    auto it = auxiliary_num_vars_expressions.find(name);
    if (it != auxiliary_num_vars_expressions.end()){
        int var_id = auxiliary_numeric_variables[it->second].var_id;
        return make_shared<ArithmeticExpressionVar>(var_id);
    }

    int var_id = task->get_num_numeric_variables() + auxiliary_numeric_variables.size();

//    cout << "new aux var for " << name << endl;
//    cout << "expression: " << expr->get_name() << endl;

    reg_num_var_id_to_glob_var_id.push_back(var_id);
    glob_var_id_to_reg_num_var_id.push_back(n_numeric_variables);
    ++n_numeric_variables;

    initial_state_values.push_back(expr->evaluate(initial_state_values));

    auxiliary_num_vars_expressions[name] = auxiliary_numeric_variables.size();
    auxiliary_numeric_variables.emplace_back(var_id, name, std::move(expr));
    return make_shared<ArithmeticExpressionVar>(var_id);
}

shared_ptr<RegularNumericCondition> NumericTaskProxy::build_condition(FactProxy pre) {
    assert(!is_derived_variable(pre.get_variable()) &&
           is_derived_numeric_variable(pre.get_variable()));

    int var_id = pre.get_variable().get_id();

    if (regular_numeric_conditions[var_id][pre.get_value()]) {
        return regular_numeric_conditions[var_id][pre.get_value()];
    }

    int c_op_id = get_achieving_comp_axiom(task_proxy, pre);

    if (c_op_id == -1) {
        cerr << "ERROR: could not find a comparison axiom that achieves this fact, is it propositional? "
             << pre.get_name() << endl;
        utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
    }

    ComparisonAxiomProxy c_op = task_proxy.get_comparison_axioms()[c_op_id];

//    cout << endl << "found comparison op: " << endl
//         << c_op.get_left_variable().get_name() << c_op.get_left_variable().get_var_type() << endl
//         << c_op.get_comparison_operator_type() << endl
//         << c_op.get_right_variable().get_name() << c_op.get_right_variable().get_var_type()
//         << endl;

    shared_ptr<ArithmeticExpression> lhs = parse_arithmetic_expression(c_op.get_left_variable());
    shared_ptr<ArithmeticExpression> rhs = parse_arithmetic_expression(c_op.get_right_variable());

//    cout << lhs->get_name() << endl;
//    cout << c_op.get_comparison_operator_type() << endl;
//    cout << rhs->get_name() << endl;

    if (!lhs->is_constant() && !rhs->is_constant()) {
        auto expr = make_shared<ArithmeticExpressionOp>(lhs, cal_operator::diff, rhs);
        auto var = create_auxiliary_variable(pre.get_name(), expr);
        auto zero = make_shared<ArithmeticExpressionConst>(0);

        return make_shared<RegularNumericCondition>(var, c_op.get_comparison_operator_type(), zero);
    }

//    cout << make_shared<RegularNumericCondition>(lhs, c_op.get_comparison_operator_type(), rhs)->get_name()
//         << "; constant = " << make_shared<RegularNumericCondition>(lhs, c_op.get_comparison_operator_type(), rhs)->get_constant()
//         << endl;

    return make_shared<RegularNumericCondition>(lhs, c_op.get_comparison_operator_type(), rhs);
}

void NumericTaskProxy::build_numeric_preconditions() {
    regular_numeric_conditions.resize(task_proxy.get_variables().size());
    for (size_t var = 0; var < task_proxy.get_variables().size(); ++var) {
        regular_numeric_conditions[var].resize(task_proxy.get_variables()[var].get_domain_size());
    }
    for (OperatorProxy op : task_proxy.get_operators()) {
        for (FactProxy pre : op.get_preconditions()) {
            // check if proper numeric condition
            assert(!is_derived_variable(pre.get_variable()));
            if (is_derived_numeric_variable(pre.get_variable())) {
                auto num_condition = build_condition(pre);
                regular_numeric_conditions[pre.get_variable().get_id()][pre.get_value()] = num_condition;
            }
        }
    }
}

void NumericTaskProxy::build_goals() {
    // there should be at most two axioms, one that is always generated is a dummy axiom that seems to do nothing (in
    // particular, it does not have preconditions),
    // the (optional) second one encodes the numeric goals (and possibly propositional ones) into a single derived variable
    assert(task_proxy.get_axioms().size() <= 2);

    for (FactProxy goal : task_proxy.get_goals()) {
        if (!is_derived_variable(goal.get_variable())) {
            assert(!is_derived_numeric_variable(goal.get_variable()));
            propositional_goals.push_back(goal);
        }
    }

    // reconstruct regular numeric goals
    for (auto axiom : task_proxy.get_axioms()){
        assert(axiom.get_preconditions().empty() || axiom.get_effects().size() == 1);
        if (!axiom.get_preconditions().empty()) {
            for (auto pre: axiom.get_preconditions()) {
                assert(!is_derived_variable(pre.get_variable()));
                if (is_derived_numeric_variable(pre.get_variable())){
                    shared_ptr<RegularNumericCondition> goal(build_condition(pre));
                    if (!goal->is_constant()){
                        regular_numeric_goals.push_back(*goal);
                    }
                } else {
                    assert(all_of(propositional_goals.begin(),
                                  propositional_goals.end(),
                                  [&pre](FactProxy a) {
                        return a.get_variable().get_id() != pre.get_variable().get_id();
                    }));
                    propositional_goals.push_back(pre);
                }
            }
        }
    }
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
    assert(num_var_id >= 0);
    assert(static_cast<size_t>(num_var_id) < glob_var_id_to_reg_num_var_id.size());
    assert(glob_var_id_to_reg_num_var_id[num_var_id] != -1);
    return glob_var_id_to_reg_num_var_id[num_var_id];
}

int NumericTaskProxy::get_global_var_id(int regular_num_var_id) const {
    assert(regular_num_var_id >= 0);
    assert(static_cast<size_t>(regular_num_var_id) < reg_num_var_id_to_glob_var_id.size());
    assert(reg_num_var_id_to_glob_var_id[regular_num_var_id] != -1);
    return reg_num_var_id_to_glob_var_id[regular_num_var_id];
}

shared_ptr<arithmetic_expression::ArithmeticExpression> NumericTaskProxy::parse_arithmetic_expression(
        NumericVariableProxy num_var) {

    switch (num_var.get_var_type()) {
        case regular:
            return make_shared<ArithmeticExpressionVar>(num_var.get_id());
        case constant:
            return make_shared<ArithmeticExpressionConst>(num_var.get_initial_state_value());
        case derived: {
            int assgn_op_id = get_achieving_assgn_axiom(task_proxy,
                                                        num_var.get_id());
            if (assgn_op_id == -1) {
                cerr << "ERROR: could not find a assigment axiom that achieves this fact, is it propositional? "
                     << num_var.get_name() << endl;
                utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
            }
            AssignmentAxiomProxy assgn_ax = task_proxy.get_assignment_axioms()[assgn_op_id];

            auto lhs = parse_arithmetic_expression(assgn_ax.get_left_variable());
            auto rhs = parse_arithmetic_expression(assgn_ax.get_right_variable());

            auto expr = make_shared<ArithmeticExpressionOp>(
                    lhs,
                    assgn_ax.get_arithmetic_operator_type(),
                    rhs);

            if (!lhs->is_constant() && !rhs->is_constant()){
                return create_auxiliary_variable(num_var.get_name(), expr);
            }

            return expr;
        }
        default: // could be instrumentation or unknown
            cerr << "ERROR: unsupported numeric variable type " << num_var.get_var_type() << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
    }
}

void NumericTaskProxy::find_derived_numeric_variables() {
    is_derived_num_var.assign(task_proxy.get_variables().size(), false);
    for (auto op : task_proxy.get_comparison_axioms()) {
        is_derived_num_var[op.get_true_fact().get_variable().get_id()] = true;
    }
}

const vector<RegularNumericCondition> &NumericTaskProxy::get_numeric_goals() const {
    return regular_numeric_goals;
}

const vector<FactProxy> &NumericTaskProxy::get_propositional_goals() const {
    return propositional_goals;
}

int NumericTaskProxy::get_approximate_domain_size(const ResNumericVariableProxy &num_var) {
    // TODO: maybe have different variants
    assert(num_var.get_id() >= 0);
    assert(static_cast<size_t>(num_var.get_id()) >= g_numeric_var_types.size() ||
           g_numeric_var_types[num_var.get_id()] == numType::regular);
    if (approximate_num_var_domain_sizes.empty()){
        approximate_num_var_domain_sizes.resize(get_num_numeric_variables(), -1);
    }
    if (approximate_num_var_domain_sizes[num_var.get_id()] == -1){
        // TODO precompute this on construction
        unordered_set<ap_float> increments;
        unordered_set<ap_float> decrements;
        ap_float min_const = 0;
        ap_float max_const = 0;

        ap_float min_change = numeric_limits<ap_float>::max();
        ap_float max_pos_change = 0;
        ap_float max_neg_change = 0;

        for (const auto &op : get_operators()){
            for (const auto &num_pre : op.get_numeric_preconditions()){
                if (!num_pre->is_constant() && num_var.get_id() == num_pre->get_var_id()) {
                    ap_float c = num_pre->get_constant();
                    min_const = min(min_const, c);
                    max_const = max(max_const, c);
                }
            }
            const auto &num_effs = get_action_eff_list(op.get_id());
            ap_float eff = num_effs[get_regular_var_id(num_var.get_id())];
            if (eff > 0) {
                increments.insert(eff);
                min_change = min(min_change, eff);
                max_pos_change = max(max_pos_change, eff);
            } else if (eff < 0){
                decrements.insert(eff);
                min_change = min(min_change, abs(eff));
                max_neg_change = min(max_neg_change, eff);
            }

            for (const auto &[var, val] : op.get_assign_effects()){
                if (var == num_var.get_id()) {
                    min_const = min(min_const, val);
                    max_const = max(max_const, val);
                }
            }
        }

        ap_float ini_val = get_initial_state_numeric_values()[num_var.get_id()];
        min_const = min(min_const, ini_val);
        max_const = max(max_const, ini_val);

        for (const auto &goal : get_numeric_goals()){
            if (num_var.get_id() == goal.get_var_id()){
                ap_float c = goal.get_constant();
                min_const = min(min_const, c);
                max_const = max(max_const, c);
            }
        }

        min_const += max_neg_change;
        max_const += max_pos_change;

        ap_float min_increment = numeric_limits<ap_float>::max();
        if (!increments.empty() && !decrements.empty()) {
            for (ap_float inc: increments) {
                for (ap_float dec: decrements) {
                    min_increment = min(min_increment, abs(inc + dec));
                }
            }
            if (min_increment == 0){
                min_increment = min_change;
            }
        } else {
            min_increment = min_change;
        }

//        cout << "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++" << endl;
//        cout << "ENI of " << num_var.get_name() << ":" << endl;
//        cout << "[" << min_const << ", " << max_const << "] increment "
//             << min_increment << " => " << static_cast<int>(abs((max_const - min_const) / min_increment) + 1)
//             << " values" << endl;

        assert(abs((max_const - min_const) / min_increment) + 1 <= numeric_limits<int>::max());
        approximate_num_var_domain_sizes[num_var.get_id()] = static_cast<int>(abs((max_const - min_const) / min_increment) + 1);
    }
    return approximate_num_var_domain_sizes[num_var.get_id()];
}
}
