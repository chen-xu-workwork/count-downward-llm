#ifndef NUMERIC_PDBS_NUMERIC_CONDITION_H
#define NUMERIC_PDBS_NUMERIC_CONDITION_H

#include "arithmetic_expression.h"

namespace numeric_condition {

class RegularNumericCondition {
    std::shared_ptr<arithmetic_expression::ArithmeticExpression> lhs;
    comp_operator c_op;
    std::shared_ptr<arithmetic_expression::ArithmeticExpression> rhs;
public:
    RegularNumericCondition(const std::shared_ptr<arithmetic_expression::ArithmeticExpression> &lhs,
                            comp_operator c_op,
                            const std::shared_ptr<arithmetic_expression::ArithmeticExpression> &rhs)
            : lhs(lhs->simplify()), c_op(c_op), rhs(rhs->simplify()) {
        // there can only be one regular numeric variable in the expression
        assert(lhs->get_var_id() == -1 || rhs->get_var_id() == -1);
    }

    ~RegularNumericCondition() = default;

    int get_var_id() const {
        int var_id = lhs->get_var_id();
        if (var_id != -1){
            return var_id;
        } else {
            return rhs->get_var_id();
        }
    }

    std::string get_name() const;

    // checks if the condition is satisfied if the variable get_var_id() has this value
    bool satisfied(ap_float value) const;

    ap_float get_constant() const {
        if (lhs->get_var_id() != -1){
            // rhs must be constant
            ap_float c = rhs->evaluate(0);
            ap_float m = lhs->get_multiplier();
            ap_float s = lhs->get_summand();
            return c * m - s;
        } else if (rhs->get_var_id() != -1){
            // lhs must be constant
            ap_float c = lhs->evaluate(0);
            ap_float m = rhs->get_multiplier();
            ap_float s = rhs->get_summand();
            return c * m - s;
        }
        return 0;
    }

    bool is_constant() const {
        return lhs->get_var_id() == -1 && rhs->get_var_id() == -1;
    }
};

}

#endif
