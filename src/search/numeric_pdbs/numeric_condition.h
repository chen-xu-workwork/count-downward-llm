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
        // there can be at most one regular numeric variable in the expression
#ifndef NDEBUG
        std::vector<int> var_ids;
        lhs->add_var_ids(var_ids);
        rhs->add_var_ids(var_ids);
        assert(var_ids.size() <= 1);
#endif
    }

    ~RegularNumericCondition() = default;

    int get_var_id() const {
        std::vector<int> var_ids;
        if (rhs->is_constant()){
            assert(!lhs->is_constant());
            lhs->add_var_ids(var_ids);
        } else {
            rhs->add_var_ids(var_ids);
        }
        assert(var_ids.size() == 1);
        return var_ids[0];
    }

    std::string get_name() const;

    // checks if the condition is satisfied if the variable get_var_id() has this value
    bool satisfied(ap_float value) const;

    ap_float get_constant() const {
        if (!lhs->is_constant()){
            assert(rhs->is_constant());
            // rhs must be constant
            ap_float c = rhs->evaluate();
            ap_float m = lhs->get_multiplier();
            ap_float s = lhs->get_summand();
            return c * m - s;
        } else if (!rhs->is_constant()){
            assert(lhs->is_constant());
            // lhs must be constant
            ap_float c = lhs->evaluate();
            ap_float m = rhs->get_multiplier();
            ap_float s = rhs->get_summand();
            return c * m - s;
        }
        return 0;
    }

    bool is_constant() const {
        return lhs->is_constant() && rhs->is_constant();
    }
};

}

#endif
