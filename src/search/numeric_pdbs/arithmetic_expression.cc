#include "arithmetic_expression.h"

#include "numeric_helper.h"

#include "../utils/system.h"

#include <iostream>
#include <sstream>

using namespace std;

namespace arithmetic_expression {
ap_float ArithmeticExpressionVar::evaluate(const State &state, const numeric_pdb_helper::NumericTaskProxy &task) const {
    return task.get_numeric_state_value(state, var_id);
}

string ArithmeticExpressionOp::get_name() const {
    stringstream ss;
    ss << lhs->get_name() << c_op << rhs->get_name();
    return ss.str();
}

ap_float ArithmeticExpressionOp::evaluate() const {
    assert(is_constant());
    switch (c_op){
        case cal_operator::sum:
            return lhs->evaluate() + rhs->evaluate();
        case cal_operator::diff:
            return lhs->evaluate() - rhs->evaluate();
        case cal_operator::mult:
            return lhs->evaluate() * rhs->evaluate();
        case cal_operator::divi:
            assert(rhs->evaluate() != 0);
            return lhs->evaluate() / rhs->evaluate();
        default:
            cerr << "ERROR: unknown cal_operator: " << c_op << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
    }
}

ap_float ArithmeticExpressionOp::evaluate(ap_float value) const {
#ifndef NDEBUG
    vector<int> var_ids;
    lhs->add_var_ids(var_ids);
    rhs->add_var_ids(var_ids);
    assert(var_ids.size() <= 1);
#endif
    switch (c_op){
        case cal_operator::sum:
            return lhs->evaluate(value) + rhs->evaluate(value);
        case cal_operator::diff:
            return lhs->evaluate(value) - rhs->evaluate(value);
        case cal_operator::mult:
            return lhs->evaluate(value) * rhs->evaluate(value);
        case cal_operator::divi:
            assert(rhs->evaluate(value) != 0);
            return lhs->evaluate(value) / rhs->evaluate(value);
        default:
            cerr << "ERROR: unknown cal_operator: " << c_op << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
    }
}

ap_float ArithmeticExpressionOp::evaluate(const vector<ap_float> &num_values) const {
    switch (c_op){
        case cal_operator::sum:
            return lhs->evaluate(num_values) + rhs->evaluate(num_values);
        case cal_operator::diff:
            return lhs->evaluate(num_values) - rhs->evaluate(num_values);
        case cal_operator::mult:
            return lhs->evaluate(num_values) * rhs->evaluate(num_values);
        case cal_operator::divi:
            assert(rhs->evaluate(num_values) != 0);
            return lhs->evaluate(num_values) / rhs->evaluate(num_values);
        default:
            cerr << "ERROR: unknown cal_operator: " << c_op << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
    }
}

ap_float ArithmeticExpressionOp::evaluate_ignore_additive_consts(const vector<ap_float> &num_values) const {
    switch (c_op){
        case cal_operator::sum:
            return lhs->evaluate_ignore_additive_consts(num_values) + rhs->evaluate_ignore_additive_consts(num_values);
        case cal_operator::diff:
            return lhs->evaluate_ignore_additive_consts(num_values) - rhs->evaluate_ignore_additive_consts(num_values);
        case cal_operator::mult:
            return lhs->evaluate_ignore_additive_consts(num_values) * rhs->evaluate_ignore_additive_consts(num_values);
        case cal_operator::divi:
            assert(rhs->evaluate_ignore_additive_consts(num_values) != 0);
            return lhs->evaluate_ignore_additive_consts(num_values) / rhs->evaluate_ignore_additive_consts(num_values);
        default:
            cerr << "ERROR: unknown cal_operator: " << c_op << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
    }
}

ap_float ArithmeticExpressionOp::evaluate(const State &state,
                                          const numeric_pdb_helper::NumericTaskProxy &task) const {
    switch (c_op){
        case cal_operator::sum:
            return lhs->evaluate(state, task) + rhs->evaluate(state, task);
        case cal_operator::diff:
            return lhs->evaluate(state, task) - rhs->evaluate(state, task);
        case cal_operator::mult:
            return lhs->evaluate(state, task) * rhs->evaluate(state, task);
        case cal_operator::divi:
            assert(rhs->evaluate(state, task) != 0);
            return lhs->evaluate(state, task) / rhs->evaluate(state, task);
        default:
            cerr << "ERROR: unknown cal_operator: " << c_op << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
    }
}

ap_float ArithmeticExpressionOp::get_multiplier() const {
    // TODO the current implementation does not support this
    assert(c_op != cal_operator::divi || rhs->is_constant());

    if (!lhs->is_constant()){
        assert(rhs->is_constant());
        // rhs must be constant
        ap_float lhs_m = lhs->get_multiplier();
        switch (c_op){
            case cal_operator::sum: // fall through
            case cal_operator::diff:
                return lhs_m;
            case cal_operator::mult:
                assert(lhs_m != 0);
                assert(rhs->evaluate() != 0);
                return 1.0 / (lhs_m * rhs->evaluate());
            case cal_operator::divi:
                assert(lhs_m != 0);
                return rhs->evaluate() / lhs_m;
            default:
                cerr << "ERROR: unknown cal_operator: " << c_op << endl;
                utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        }
    } else if (!rhs->is_constant()){
        assert(lhs->is_constant());
        // lhs must be constant
        ap_float rhs_m = rhs->get_multiplier();
        switch (c_op){
            case cal_operator::sum: // fall through
                return rhs_m;
            case cal_operator::diff:
                return -rhs_m;
            case cal_operator::mult:
                assert(rhs_m != 0);
                assert(lhs->evaluate() != 0);
                return 1.0 / (rhs_m * lhs->evaluate());
            case cal_operator::divi:
                assert(rhs_m != 0);
                return lhs->evaluate() / rhs_m;
            default:
                cerr << "ERROR: unknown cal_operator: " << c_op << endl;
                utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        }

    }
    cerr << "ERROR: both sides of arithmetic expression are constant, simplify() has not been called." << endl;
    utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
}

ap_float ArithmeticExpressionOp::get_summand() const {
    // TODO the current implementation does not support this
    assert(c_op != cal_operator::divi || rhs->is_constant());

    if (!lhs->is_constant()){
        assert(rhs->is_constant());
        // rhs must be constant
        ap_float lhs_m = lhs->get_multiplier();
        ap_float rhs_c = rhs->evaluate();
        switch (c_op){
            case cal_operator::sum:
                return lhs->get_summand() + rhs_c * lhs_m;
            case cal_operator::diff:
                return lhs->get_summand() - rhs_c * lhs_m;
            case cal_operator::mult: // fall through
            case cal_operator::divi:
                return lhs->get_summand();
            default:
                cerr << "ERROR: unknown cal_operator: " << c_op << endl;
                utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        }
    } else if (!rhs->is_constant()){
        assert(lhs->is_constant());
        // lhs must be constant
        ap_float rhs_m = rhs->get_multiplier();
        ap_float lhs_c = lhs->evaluate();
        switch (c_op){
            case cal_operator::sum:
                return rhs->get_summand() + lhs_c * rhs_m;
            case cal_operator::diff:
                return -lhs_c * rhs_m - rhs->get_summand();
            case cal_operator::mult: // fall through
            case cal_operator::divi:
                return rhs->get_summand();
            default:
                cerr << "ERROR: unknown cal_operator: " << c_op << endl;
                utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        }
    }
    cerr << "ERROR: both sides of arithmetic expression are constant, simplify() has not been called." << endl;
    utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
}
}
