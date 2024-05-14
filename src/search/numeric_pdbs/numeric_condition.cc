#include "numeric_condition.h"

#include "../utils/system.h"

#include <iostream>
#include <sstream>

using namespace std;

namespace numeric_condition {
string RegularNumericConditionVar::get_name() const {
    ostringstream s;
    s << "var" << var_id << c_op << _const << endl;
    return s.str();
}

bool RegularNumericConditionVar::satisfied(ap_float value) const {
    switch (c_op){
        case lt:
            return value < _const;
        case le:
            return value <= _const;
        case eq:
            return value == _const;
        case ge:
            return value >= _const;
        case gt:
            return value > _const;
        default:
            cerr << "ERROR: unsupported comparison operator " << c_op << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
    }
}

RegularNumericConditionConst::RegularNumericConditionConst(
        int var_id,
        ap_float const_l,
        comp_operator c_op,
        ap_float const_r) : RegularNumericCondition(var_id, c_op, const_r),
                            const_l(const_l) {
    switch (c_op){
        case lt:
            is_satisfied = const_l < _const;
            break;
        case le:
            is_satisfied = const_l <= _const;
            break;
        case eq:
            is_satisfied = const_l == _const;
            break;
        case ge:
            is_satisfied = const_l >= _const;
            break;
        case gt:
            is_satisfied = const_l > _const;
            break;
        default:
            cerr << "ERROR: unsupported comparison operator " << c_op << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
    }
}

string RegularNumericConditionConst::get_name() const {
    ostringstream s;
    s << const_l << c_op << _const << endl;
    return s.str();
}

string RegularNumericConditionVarOpC::get_name() const {
    ostringstream s;
    s << "var" << var_id << cal_op << op_const << c_op << _const << endl;
    return s.str();
}

bool RegularNumericConditionVarOpC::satisfied(ap_float value) const {
    ap_float lh_value;
    switch (cal_op){
        case sum:
            lh_value = value + op_const;
            break;
        case diff:
            lh_value = value - op_const;
            break;
        default:
            cerr << "ERROR: unsupported cal_operator " << cal_op << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
    }
    switch (c_op){
        case lt:
            return lh_value < _const;
        case le:
            return lh_value <= _const;
        case eq:
            return lh_value == _const;
        case ge:
            return lh_value >= _const;
        case gt:
            return lh_value > _const;
        default:
            cerr << "ERROR: unsupported comparison operator " << c_op << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
    }
}
}