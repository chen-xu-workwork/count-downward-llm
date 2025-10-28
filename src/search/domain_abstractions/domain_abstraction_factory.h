#ifndef DOMAIN_ABSTRACTION_DOMAIN_ABSTRACTION_FACTORY_H
#define DOMAIN_ABSTRACTION_DOMAIN_ABSTRACTION_FACTORY_H

#include "types.h"

#include "../task_proxy.h"

namespace utils {
class LogProxy;
class RandomNumberGenerator;
}

namespace domain_abstractions {
class MatchTree;

class AbstractOperator {
    /*
      This class represents an abstract operator how it is needed for
      the regression search performed during the construction of the
      domain abstraction. As all abstract states are represented as a
      number, abstract operators don't have "usual" effects but "hash
      effects", i.e. the change (as number) the abstract operator
      implies on a given abstract state.
    */
    int concrete_op_id;

    int cost;

    /*
      Preconditions for the regression search, corresponds to normal
      effects and prevail of concrete operators.
    */
    std::vector<Fact> regression_preconditions;
    std::vector<NumAssProxy> regression_numeric_preconditions;

    /*
      Effects of the operator during regression search on a given
      abstract state number.
      
      For propositional-only operators: single hash effect
      For operators with numeric effects: ONE hash effect per numeric partition transition
      (comparison axiom cascades are handled on-the-fly in Dijkstra, not pre-computed)
    */
    std::vector<int> hash_effects;
    
    /*
      Information about numeric variable transitions for cascade enumeration.
      Only populated for operators with numeric effects.
      
      changed_numeric_vars[i] = ID of the i-th numeric variable modified by this operator
      source_partitions[i] = source partition for changed_numeric_vars[i] (predecessor state)
      target_partitions[i] = target partition for changed_numeric_vars[i] (current state)
    */
    std::vector<int> changed_numeric_vars;
    std::vector<int> source_partitions;
    std::vector<int> target_partitions;

public:
    /*
      Abstract operators are built from concrete operators. The
      parameters follow the usual name convention of SAS+ operators,
      meaning prevail, preconditions and effects are all related to
      progression search.
    */
    AbstractOperator(const std::vector<Fact> &prevail,
                     const std::vector<Fact> &preconditions,
                     const std::vector<Fact> &effects,
                     const std::vector<NumAssProxy> &ass_effects,
                     int cost,
                     const std::vector<int> &hash_multipliers,
                     const NumericDomainMappingType &numeric_domain_mapping,
                     const std::vector<int> &numeric_domain_sizes,
                     int concrete_op_id);
    
    /*
      Constructor that accepts pre-computed hash effects WITHOUT cascades.
      This is used by the numeric helper which computes hash effects for
      numeric partition transitions only. Comparison axiom cascades are
      handled on-the-fly during Dijkstra search.
      
      Parameters include numeric transition information for cascade enumeration.
    */
    AbstractOperator(const std::vector<Fact> &prevail,
                     const std::vector<Fact> &preconditions,
                     const std::vector<Fact> &effects,
                     const std::vector<NumAssProxy> &ass_effects,
                     int cost,
                     const std::vector<int> &pre_computed_hash_effects,
                     int concrete_op_id,
                     const std::vector<int> &changed_numeric_vars,
                     const std::vector<int> &source_partitions,
                     const std::vector<int> &target_partitions);
    ~AbstractOperator() = default;

    /*
      Returns variable value pairs which represent the preconditions of
      the abstract operator in a regression search
    */
    const std::vector<Fact> &get_regression_preconditions() const {
        return regression_preconditions;
    }

    /*
      Returns the effects of the abstract operator in form of value
      changes (+ or -) to abstract state indices.
      
      For propositional-only: returns vector with single hash effect
      For numeric operators: returns vector with multiple hash effects
    */
    const std::vector<int> &get_hash_effects() const {return hash_effects;}

    int get_concrete_op_id() const {
        return concrete_op_id;
    }

    /*
      Returns the cost of the abstract operator (same as the cost of
      the original concrete operator)
    */
    int get_cost() const {return cost;}
    
    /*
      Returns information about numeric variable transitions for cascade enumeration.
      Empty vectors if operator has no numeric effects.
    */
    const std::vector<int> &get_changed_numeric_vars() const {return changed_numeric_vars;}
    const std::vector<int> &get_source_partitions() const {return source_partitions;}
    const std::vector<int> &get_target_partitions() const {return target_partitions;}
    
    void dump(const VariablesProxy &variables,
              utils::LogProxy &log) const;
};

// Structure to store numeric goal conditions
// For each numeric variable that appears in goals, stores the partition indices that satisfy the goal
struct NumericGoalCondition {
    int numeric_var_id;  // ID of the numeric variable
    comp_operator op;    // comparison operator (lt, le, eq, ge, gt)
    ap_float constant;   // constant value to compare against
    
    NumericGoalCondition(int var_id, comp_operator op, ap_float constant)
        : numeric_var_id(var_id), op(op), constant(constant) {}
};

class DomainAbstractionFactory {
    TaskProxy task_proxy;
    DomainMapping domain_mapping;
    NumericDomainMappingType numeric_domain_mapping;
    std::vector<int> numeric_domain_sizes;
    
    /*
      final h-values for abstract-states.
      dead-ends are represented by numeric_limits<int>::max()
    */
    std::vector<int> distances;

    std::vector<int> generating_op_ids;
    std::vector<std::vector<int>> wildcard_plan;

    // multipliers for each variable for perfect hash function
    // Includes multipliers for both propositional and numeric variables
    std::vector<int> hash_multipliers;

    int num_states;
    
    // Operators that have numeric effects
    std::vector<int> numeric_operators;
    
    // Numeric goal conditions extracted from comparison axioms
    std::vector<NumericGoalCondition> numeric_goal_conditions;

    std::vector<AbstractOperator> compute_abstract_operators(
        const TaskProxy &task_proxy, const std::vector<int> &domain_sizes);
    MatchTree build_match_tree(const std::vector<int> &domain_sizes,
                               const std::vector<AbstractOperator> &operators);
    std::vector<Fact> compute_abstract_goals(const TaskProxy &task_proxy);
    
    void compute_distances(
        const TaskProxy &task_proxy,
        const std::vector<AbstractOperator> &operators,
        const MatchTree &match_tree,
        const std::vector<Fact> &abstract_goals,
        const std::vector<int> &domain_sizes, bool compute_plan);
    void compute_abstract_plan(
        const TaskProxy &task_proxy,
        const std::vector<AbstractOperator> &operators,
        const MatchTree &match_tree,
        const std::vector<Fact> &abstract_goals,
        const std::vector<int> &domain_sizes,
        const std::shared_ptr<utils::RandomNumberGenerator> &rng,
        bool compute_wildcard_plan);

    // NOTE: multiply_out() and build_abstract_operators() moved to DomainAbstractionNumericHelper
    
    bool is_goal_state(int state_index,
                       const std::vector<Fact> &abstract_goals,
                       const std::vector<int> &domain_sizes) const;
    int hash_index(const std::vector<int> &state) const;
    bool variable_is_trivial(int var_id) const;
    
    // Helper methods for numeric variables
    bool operator_has_numeric_effects(const OperatorProxy &op) const;
    std::vector<int> compute_abstract_numeric_predecessors(
        int state_index,
        const OperatorProxy &op,
        const std::vector<int> &domain_sizes) const;

public:
    DomainAbstractionFactory(
        const TaskProxy &task_proxy,
        const DomainMapping &domain_mapping,
        const std::vector<int> &domain_sizes,
        const NumericDomainMappingType &numeric_domain_mapping,
        const std::vector<int> &numeric_domain_sizes,
        bool compute_plan,
        const std::shared_ptr<utils::RandomNumberGenerator> &rng,
        bool compute_wildcard_plan);

    DomainAbstraction generate();

    std::vector<std::vector<int>> &&extract_wildcard_plan() {
        return std::move(wildcard_plan);
    };
    
    /*
      Enumerate all possible predecessor states that could result from applying
      an operator with numeric effects, considering cascading comparison axioms
      and assignment axioms.
      
      Parameters:
        - base_predecessor_index: The predecessor index from numeric partition transitions only
        - changed_numeric_vars: IDs of numeric variables modified by the operator
        - source_partitions: Source partitions for each changed variable (predecessor state)
        - target_partitions: Target partitions for each changed variable (current state)
        - task_proxy: Task for accessing axioms
        
      Returns:
        Vector of possible predecessor indices accounting for all comparison axiom combinations
        
      For the initial state (no numeric changes), returns a vector of size 1.
    */
    std::vector<int> enumerate_cascade_predecessors(
        int base_predecessor_index,
        const std::vector<int> &changed_numeric_vars,
        const std::vector<int> &source_partitions,
        const std::vector<int> &target_partitions,
        const TaskProxy &task_proxy) const;
};
}

#endif
