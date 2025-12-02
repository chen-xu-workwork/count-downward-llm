#ifndef DOMAIN_ABSTRACTION_DOMAIN_ABSTRACTION_FACTORY_H
#define DOMAIN_ABSTRACTION_DOMAIN_ABSTRACTION_FACTORY_H

#include "cegar_logger.h"
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
    int hash_effect;
    ap_float cost;

    /*
      Preconditions for the regression search, corresponds to normal
      effects and prevail of concrete operators.
    */
    std::vector<Fact> regression_preconditions;
    std::vector<Fact> pre;

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
  AbstractOperator(const std::vector<Fact> &prev_pairs,
                    const std::vector<Fact> &pre_pairs,
                    const std::vector<Fact> &eff_pairs,
                    ap_float cost,
                    const std::vector<int> &hash_multipliers,
                    int concrete_op_id);
    

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
    const int &get_hash_effect() const {return hash_effect;}

    int get_concrete_op_id() const {
        return concrete_op_id;
    }

    /*
      Returns the cost of the abstract operator (same as the cost of
      the original concrete operator)
    */
    ap_float get_cost() const {return cost;}
    
    /*
      Returns information about numeric variable transitions for cascade enumeration.
      Empty vectors if operator has no numeric effects.
    */
    const std::vector<int> &get_changed_numeric_vars() const {return changed_numeric_vars;}
    const std::vector<int> &get_source_partitions() const {return source_partitions;}
    const std::vector<int> &get_target_partitions() const {return target_partitions;}

  // Preconditions that must hold in the predecessor state (progression preconditions
  // and numeric source-partition facts).
  
  const std::vector<Fact> &get_preconditions() const { return pre; }

  void dump(const TaskProxy &task_proxy, DomainMapping &domain_mapping, NumericDomainMappingType &numeric_domain_mapping) const;
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

// Forward declare CEGARLogger
class CEGARLogger;

class DomainAbstractionFactory {
    TaskProxy task_proxy;
    DomainMapping domain_mapping;
    NumericDomainMappingType numeric_domain_mapping;
    std::vector<int> numeric_domain_sizes;
    std::shared_ptr<CEGARLogger> logger;
    
    /*
      final h-values for abstract-states.
      dead-ends are represented by numeric_limits<ap_float>::max()
    */
    std::vector<ap_float> distances;

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
    
    // Dump domain mapping information for debugging
    void dump_domain_mappings(const std::vector<int> &domain_sizes) const;

public:
    DomainAbstractionFactory(
        const TaskProxy &task_proxy,
        const DomainMapping &domain_mapping,
        const std::vector<int> &domain_sizes,
        const NumericDomainMappingType &numeric_domain_mapping,
        const std::vector<int> &numeric_domain_sizes,
        bool compute_plan,
        const std::shared_ptr<utils::RandomNumberGenerator> &rng,
        bool compute_wildcard_plan,
        std::shared_ptr<CEGARLogger> logger = nullptr);

    DomainAbstraction generate();

    std::vector<std::vector<int>> &&extract_wildcard_plan() {
        return std::move(wildcard_plan);
    };
    
    /*
      Enumerate all possible state indices for a given base state, evaluating
      comparison axioms based on numeric variable partition ranges.
      
      This function:
      1. Computes ranges for changed numeric variables (from partitions)
      2. Propagates ranges through assignment axioms to derived numeric variables
      3. Evaluates comparison axioms using the computed ranges
      4. Returns all possible state indices with different comparison axiom truth values
      
      Used both in regression (Dijkstra) and progression (plan extraction).
      
      Parameters:
        - base_state_index: The state index with comparison axioms set to UNKNOWN
        - changed_numeric_vars: IDs of numeric variables that changed (empty for initial state)
        - source_partitions: Source partitions for each changed variable (for regression/predecessors)
        - target_partitions: Target partitions for each changed variable (for progression/successors)
        - task_proxy: Task for accessing axioms
        
      Returns:
        Vector of possible state indices accounting for all comparison axiom combinations.
        For states with no numeric changes (e.g., initial state), returns vector of size 1.
      
      The source/target partition interpretation depends on the direction:
        - In REGRESSION (Dijkstra): source=predecessor partitions, target=current partitions
        - In PROGRESSION (plan extraction): source=current partitions, target=successor partitions
    */
    std::vector<int> enumerate_states_with_evaluated_comparisons(
        int base_state_index,
        const TaskProxy &task_proxy) const;
};
}

#endif
