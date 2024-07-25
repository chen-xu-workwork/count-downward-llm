#ifndef NUMERIC_PDBS_ARITHMETIC_EXPRESSION_H
#define NUMERIC_PDBS_ARITHMETIC_EXPRESSION_H

#include "../globals.h"

#include <cassert>

namespace arithmetic_expression {

class ArithmeticExpression {
    // an arithmetic expression over a single regular numeric variable and constants
public:
    virtual ~ArithmeticExpression() = default;

    virtual int get_var_id() const = 0;

    virtual std::string get_name() const = 0;

    virtual ap_float evaluate(ap_float value) const = 0;
};

class ArithmeticExpressionVar : public ArithmeticExpression {
    // this class encodes the basic expressions with just a variable
    int var_id; // global id of a regular numeric variable
public:
    explicit ArithmeticExpressionVar(int var_id)
            : var_id(var_id) {
        assert(var_id >= 0);
    };

    int get_var_id() const override {
        return var_id;
    }

    std::string get_name() const override {
        return "var";
    }

    ap_float evaluate(ap_float value) const override {
        return value;
    }
};

class ArithmeticExpressionConst : public ArithmeticExpression {
    // this class encodes the basic expressions with just a constant
    ap_float const_;
public:
    explicit ArithmeticExpressionConst(ap_float const_)
            : const_(const_) {
    }

    int get_var_id() const override {
        return -1;
    }

    std::string get_name() const override {
        return std::to_string(const_);
    }

    ap_float evaluate(ap_float /*value*/) const override {
        return const_;
    }
};

class ArithmeticExpressionOp : public ArithmeticExpression {
    std::shared_ptr<ArithmeticExpression> lhs;
    cal_operator c_op;
    std::shared_ptr<ArithmeticExpression> rhs;
public:
    ArithmeticExpressionOp(std::shared_ptr<ArithmeticExpression> lhs,
                           cal_operator c_op,
                           std::shared_ptr<ArithmeticExpression> rhs)
            : lhs(std::move(lhs)), c_op(c_op), rhs(std::move(rhs)) {
    }

    int get_var_id() const override {
        int var_id = lhs->get_var_id();
        if (var_id != -1){
            assert(rhs->get_var_id() == -1); // there can only be one variable in the expression
            return var_id;
        } else {
            return rhs->get_var_id();
        }
    }

    std::string get_name() const override;

    ap_float evaluate(ap_float value) const override;
};
}

#endif