#ifndef NUMERIC_PDBS_NUMERIC_CONDITION_H
#define NUMERIC_PDBS_NUMERIC_CONDITION_H

#include "../globals.h"

namespace numeric_condition {
class RegularNumericCondition {
protected:
    int var_id; // this is always a regular numeric variable
    comp_operator c_op;
    ap_float _const;
public:
    RegularNumericCondition(int var_id,
                            comp_operator c_op,
                            ap_float _const) : var_id(var_id), c_op(c_op), _const(_const) {};

    virtual ~RegularNumericCondition() = default;

    int get_var_id() const {
        return var_id;
    }

    virtual std::string get_name() const = 0;

    virtual bool satisfied(ap_float value) const = 0;
};

class RegularNumericConditionVar : public RegularNumericCondition {
public:
    RegularNumericConditionVar(int var_id,
                               comp_operator c_op,
                               ap_float _const) : RegularNumericCondition(var_id, c_op, _const) {};

    std::string get_name() const override;

    bool satisfied(ap_float value) const override;
};

class RegularNumericConditionConst : public RegularNumericCondition {
    bool is_satisfied;
    ap_float const_l;
public:
    RegularNumericConditionConst(int var_id,
                                 ap_float const_l,
                                 comp_operator c_op,
                                 ap_float const_r);

    std::string get_name() const override;

    bool satisfied(ap_float /*value*/) const override {
        // this is a constant condition that is evaluated on creation
        return is_satisfied;
    }
};

class RegularNumericConditionVarOpC : public RegularNumericCondition {
    cal_operator cal_op;
    ap_float op_const;
public:
    RegularNumericConditionVarOpC(int var_id,
                                  cal_operator cal_op,
                                  ap_float op_const,
                                  comp_operator comp_op,
                                  ap_float const_r) : RegularNumericCondition(var_id, comp_op, const_r),
                                                      cal_op(cal_op),
                                                      op_const(op_const) {};

    std::string get_name() const override;

    bool satisfied(ap_float value) const override;
};
}

#endif