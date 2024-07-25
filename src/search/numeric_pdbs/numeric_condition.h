#ifndef NUMERIC_PDBS_NUMERIC_CONDITION_H
#define NUMERIC_PDBS_NUMERIC_CONDITION_H

#include "arithmetic_expression.h"

#include "../globals.h"

namespace numeric_condition {

class RegularNumericCondition {
    std::shared_ptr<arithmetic_expression::ArithmeticExpression> lhs;
    comp_operator c_op;
    std::shared_ptr<arithmetic_expression::ArithmeticExpression> rhs;
public:
    RegularNumericCondition(std::shared_ptr<arithmetic_expression::ArithmeticExpression> lhs,
                            comp_operator c_op,
                            std::shared_ptr<arithmetic_expression::ArithmeticExpression> rhs)
            : lhs(lhs), c_op(c_op), rhs(rhs) {
    }

    ~RegularNumericCondition() = default;

    int get_var_id() const {
        int var_id = lhs->get_var_id();
        if (var_id != -1){
            assert(rhs->get_var_id() == -1); // there can only be one variable in the expression
            return var_id;
        } else {
            return rhs->get_var_id();
        }
    }

    std::string get_name() const;

    bool satisfied(ap_float value) const;

    ap_float get_constant() const {
        if (rhs->get_var_id() == -1){
            return rhs->evaluate(0);
        } else if (lhs->get_var_id() == -1){
            return lhs->evaluate(0);
        }
        return 0;
    }

    bool is_constant() const {
        return lhs->get_var_id() == -1 && rhs->get_var_id() == -1;
    }
};

}

#endif
