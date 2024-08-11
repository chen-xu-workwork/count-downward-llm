#ifndef NUMERIC_PDBS_NUMERIC_HELPER
#define NUMERIC_PDBS_NUMERIC_HELPER

#include "numeric_condition.h"
#include "numeric_task_proxy.h"

#include "../option_parser.h"
#include "../plugin.h"
#include "../task_tools.h"

#include <cmath>
#include <iostream>
#include <list>
#include <set>
#include <unordered_map>
#include <vector>

namespace arithmetic_expression {
class ArithmeticExpression;
}

namespace numeric_condition {
class RegularNumericCondition;
}

namespace numeric_pdbs {
class CausalGraph;
}

namespace numeric_pdb_helper {

/* An action is an operator where effects are espressed as add and eff of
 proposition. A proposition is an atom of the form Var = Val */

struct Action {
    std::vector<FactProxy> preconditions; // propositional preconditions (no derived vars)
    std::vector<std::shared_ptr<numeric_condition::RegularNumericCondition>> numeric_preconditions;
    std::vector<ap_float> eff_list;  // additive numeric effects
    // list of assignment effects: first is global numeric var id, second is assigned value
    std::vector<std::pair<int, ap_float>> asgn_effs;

    explicit Action(int size_eff) : eff_list(size_eff, 0) {
    }
};

/* Linear Numeric Conditions */

struct LinearNumericCondition {
    std::vector<ap_float> coefficients;
    ap_float constant;

    explicit LinearNumericCondition(int size_coefficients) {
        coefficients.assign(size_coefficients, 0);
        constant = 0;
    }
};

struct ResNumericVariable {
    int var_id; // global var id
    std::string name;
    std::shared_ptr<arithmetic_expression::ArithmeticExpression> expr;
    ResNumericVariable(int var_id,
                       std::string name,
                       std::shared_ptr<arithmetic_expression::ArithmeticExpression> expr)
            : var_id(var_id), name(std::move(name)), expr(std::move(expr)) {
    }
};

struct RestrictedState {
    std::vector<int> values;
    std::vector<ap_float> num_values;
    RestrictedState(const std::vector<int> &values,
                    const std::vector<ap_float> &num_values) : values(values),
                                                               num_values(num_values) {
    }

    int operator[](std::size_t var_id) const {
        assert(var_id < values.size());
        return values[var_id];
    }

    ap_float nval(std::size_t var_id) const {
        assert(var_id < num_values.size());
        return num_values[var_id];
    }
};

std::ostream &operator<<(std::ostream &os, const LinearNumericCondition &lnc);


/* NumericTaskProxy */
class NumericTaskProxy {
    friend class NumericOperatorProxy; // access to task
public:
    explicit NumericTaskProxy(const std::shared_ptr<AbstractTask> task);

    const std::vector<ap_float> &get_action_eff_list(int op_id) const {
        // TODO: get rid of this
        return actions[op_id].eff_list;
    }

    bool is_derived_numeric_variable(const VariableProxy &var_proxy) const;

    int get_regular_var_id(int num_var_id) const;

    int get_global_var_id(int regular_num_var_id) const;

    const std::vector<numeric_condition::RegularNumericCondition> &get_numeric_goals() const;

    const std::vector<FactProxy> &get_propositional_goals() const;

    int get_approximate_domain_size(const ResNumericVariableProxy &num_var);

    bool is_derived_variable(const VariableProxy &var) const;

    const TaskProxy &get_task_proxy() const {
        // TODO try to get rid of this
        return task_proxy;
    }

    int get_num_variables() const {
        return task->get_num_variables();
    }

    int get_num_numeric_variables() const {
        return task->get_num_numeric_variables() + auxiliary_numeric_variables.size();
    }

    size_t get_num_operators() const {
        return task->get_num_operators();
    }

    VariablesProxy get_variables() const {
        return task_proxy.get_variables();
    }

    ResNumericVariablesProxy get_numeric_variables() const {
        return ResNumericVariablesProxy(*this);
    }

    const std::string &get_numeric_variable_name(int var) const {
        return task_proxy.get_numeric_variables()[var].get_name();
    }

    numType get_numeric_var_type(int var) const {
        if (var < task->get_num_numeric_variables()){
            return task_proxy.get_numeric_variables()[var].get_var_type();
        } else {
            return numType::regular;
        }
    }

    const std::vector<ap_float> &get_initial_state_numeric_values() const {
        return initial_state_values;
    }

    const Action &get_operator(int op_id) const {
        return actions[op_id];
    }

    NumericOperatorsProxy get_operators() const {
        return NumericOperatorsProxy(*this);
    }

    ap_float get_operator_cost(int op_id) const {
        return task->get_operator_cost(op_id, false);
    }

    const std::string &get_operator_name(int op_id) const {
        return task->get_operator_name(op_id, false);
    }

    State get_original_initial_state() const {
        return task_proxy.get_initial_state();
    }

    RestrictedState get_restricted_initial_state() const {
        return RestrictedState(task->get_initial_state_values(), initial_state_values);
    }

    ap_float get_numeric_state_value(const State &state, int num_var) const {
        if (num_var < task->get_num_numeric_variables()){
            return state.nval(num_var);
        } else {
            return auxiliary_numeric_variables[num_var - task->get_num_numeric_variables()].expr->evaluate(state, *this);
        }
    }

    const numeric_pdbs::CausalGraph &get_numeric_causal_graph() const;

    static void verify_is_restricted_numeric_task(const TaskProxy &task_proxy);

private:
    const std::shared_ptr<AbstractTask> task;
    const TaskProxy task_proxy;

    void build_numeric_variables();
    void build_artificial_variables();

    void find_derived_numeric_variables();

    void build_action(const OperatorProxy &op, size_t op_id);
    void build_actions();

    std::shared_ptr<numeric_condition::RegularNumericCondition> build_condition(FactProxy pre);
    void build_numeric_preconditions();

    void build_goals();

    std::shared_ptr<arithmetic_expression::ArithmeticExpression> parse_arithmetic_expression(NumericVariableProxy num_var);

    std::shared_ptr<arithmetic_expression::ArithmeticExpressionVar> create_auxiliary_variable(
            const std::string &name,
            std::shared_ptr<arithmetic_expression::ArithmeticExpression> expr);


    std::vector<std::vector<std::shared_ptr<numeric_condition::RegularNumericCondition>>> regular_numeric_conditions;
    std::vector<numeric_condition::RegularNumericCondition> regular_numeric_goals;

    std::vector<FactProxy> propositional_goals;

    std::vector<int> approximate_num_var_domain_sizes;

    // numeric variables
    size_t n_numeric_variables;  // number of regular numeric variables
    std::vector<int> reg_num_var_id_to_glob_var_id; // map regular numeric variable id to global variable id
    std::vector<int> glob_var_id_to_reg_num_var_id; // map global variable id to regular numeric variable id
    std::vector<LinearNumericCondition> artificial_variables;

    std::unordered_map<std::string, size_t> auxiliary_num_vars_expressions;
    std::vector<ResNumericVariable> auxiliary_numeric_variables;

    std::vector<ap_float> initial_state_values;

    std::vector<bool> is_derived_num_var; // true for propositional variables that encodes derived numeric facts

    std::vector<Action> actions;
};
}
#endif
