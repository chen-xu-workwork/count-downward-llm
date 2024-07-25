#include "arithmetic_expression.h"

#include "../utils/system.h"

#include <iostream>
#include <sstream>

using namespace std;

namespace arithmetic_expression {

string ArithmeticExpressionOp::get_name() const {
    stringstream ss;
    ss << lhs->get_name() << c_op << rhs->get_name();
    return ss.str();
}

ap_float ArithmeticExpressionOp::evaluate(ap_float value) const {
    switch (c_op){
        case cal_operator::sum:
            return lhs->evaluate(value) + rhs-> evaluate(value);
        case cal_operator::diff:
            return lhs->evaluate(value) - rhs->evaluate(value);
        case cal_operator::mult:
            return lhs->evaluate(value) * rhs->evaluate(value);
        case cal_operator::divi :
            return lhs->evaluate(value) / rhs->evaluate(value);
        default:
            cerr << "ERROR: unknown cal_operator: " << c_op << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
    }
}
}