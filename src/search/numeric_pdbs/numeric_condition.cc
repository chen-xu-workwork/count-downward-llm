#include "numeric_condition.h"

#include "../utils/system.h"

#include <sstream>

using namespace std;

namespace numeric_condition {
string RegularNumericCondition::get_name() const {
    ostringstream s;
    s << lhs->get_name() << c_op << rhs->get_name();
    return s.str();
}

bool RegularNumericCondition::satisfied(ap_float value) const {
    switch (c_op){
        case lt:
            return lhs->evaluate(value) < rhs->evaluate(value);
        case le:
            return lhs->evaluate(value) <= rhs->evaluate(value);
        case eq:
            return lhs->evaluate(value) == rhs->evaluate(value);
        case ge:
            return lhs->evaluate(value) >= rhs->evaluate(value);
        case gt:
            return lhs->evaluate(value) > rhs->evaluate(value);
        default:
            cerr << "ERROR: unsupported comparison operator " << c_op << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
    }
}
}