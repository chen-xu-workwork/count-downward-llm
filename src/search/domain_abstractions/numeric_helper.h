#ifndef DOMAIN_ABSTRACTIONS_NUMERIC_HELPER_H
#define DOMAIN_ABSTRACTIONS_NUMERIC_HELPER_H

#include "types.h"
#include "../numeric_pdbs/numeric_condition.h"
#include "../task_proxy.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace arithmetic_expression {
class ArithmeticExpression;
}

namespace domain_abstractions {

/**
 * Helper class for handling numeric variables, axioms, and effects
 * in domain abstractions.
 * 
 * Key responsibilities:
 * 1. Track numeric variables (both regular and derived)
 * 2. Handle assignment axioms (e.g., a := b + c)
 * 3. Handle comparison axioms (e.g., a > b + 3)
 * 4. Compute which variables are affected by changes (axiom dependencies)
 */

struct ResNumericVariable {
    int var_id;  // global numeric variable id
    std::string name;
    std::shared_ptr<arithmetic_expression::ArithmeticExpression> expr;
    
    ResNumericVariable(int var_id,
                      std::string name,
                      std::shared_ptr<arithmetic_expression::ArithmeticExpression> expr)
        : var_id(var_id), name(std::move(name)), expr(std::move(expr)) {}
};

// Forward declaration - AbstractOperator is defined in domain_abstraction_factory.h
class AbstractOperator;

/**
 * Main helper class for domain abstractions with numeric variables.
 * 
 * Key responsibilities:
 * 1. Build AbstractOperators that account for ALL cascading effects:
 *    - Direct propositional effects
 *    - Direct numeric effects (assignment/additive)
 *    - Derived propositional variables (from comparison axioms)
 *    - Derived numeric variables (from assignment axioms)
 * 
 * 2. Handle axiom dependencies and cascading updates:
 *    - When a numeric variable changes, which comparison axioms are affected?
 *    - When a numeric variable changes, which assignment axioms are affected?
 *    - Compute transitive closure of all affected variables
 * 
 * The DomainAbstractionFactory uses this helper to get properly constructed
 * AbstractOperators instead of building them directly.
 */
class DomainAbstractionNumericHelper {
public:
    /**
     * Constructor.
     * @param task The planning task
     * @param domain_mapping Mapping for propositional variables (from factory)
     * @param numeric_domain_mapping Mapping for numeric variables (from factory)
     * @param domain_sizes Abstract domain sizes for propositional variables
     * @param numeric_domain_sizes Number of partitions per numeric variable
     * @param hash_multipliers Hash multipliers for perfect hashing
     */
    explicit DomainAbstractionNumericHelper(
        const std::shared_ptr<AbstractTask> &task,
        const DomainMapping &domain_mapping,
        const NumericDomainMappingType &numeric_domain_mapping,
        const std::vector<int> &domain_sizes,
        const std::vector<int> &numeric_domain_sizes,
        const std::vector<int> &hash_multipliers);
    
    // Access to numeric task information
    int get_num_numeric_variables() const {
        return n_numeric_variables;
    }
    
    const std::string &get_numeric_variable_name(int var_id) const {
        return task_proxy.get_numeric_variables()[var_id].get_name();
    }
    
    numType get_numeric_var_type(int var_id) const {
        return task_proxy.get_numeric_variables()[var_id].get_var_type();
    }
    
    // Check if a numeric variable is derived (computed via assignment axioms)
    bool is_derived_numeric_variable(int num_var_id) const {
        return is_derived_num_var[num_var_id];
    }
    
    // Check if a propositional variable is derived (computed via comparison axioms)
    bool is_derived_propositional_variable(int var_id) const {
        return is_derived_prop_var[var_id];
    }
    
    /**
     * Build all abstract operators for the given task.
     * This computes operators that account for:
     * - Direct propositional effects
     * - Direct numeric effects
     * - Cascading effects through comparison axioms (prop derived vars)
     * - Cascading effects through assignment axioms (numeric derived vars)
     * 
     * Returns vector of AbstractOperators ready for use in regression search.
     */
    std::vector<AbstractOperator> build_abstract_operators(const TaskProxy &task_proxy);
    
    // Get numeric goals
    const std::vector<numeric_condition::RegularNumericCondition> &get_numeric_goals() const {
        return numeric_goals;
    }
    
    // Get propositional goals
    const std::vector<FactProxy> &get_propositional_goals() const {
        return propositional_goals;
    }
    
    // Get initial numeric values
    const std::vector<ap_float> &get_initial_numeric_values() const {
        return initial_numeric_values;
    }
    
    /**
     * Get all numeric variables that are transitively affected by a change to var_id.
     * This includes variables computed via assignment axioms that depend on var_id.
     * 
     * Example: If we have axioms:
     *   derived1 := x + y
     *   derived2 := derived1 * 2
     * Then get_affected_variables(x) returns {x, derived1, derived2}
     */
    std::vector<int> get_affected_variables(int var_id) const;
    
    /**
     * Given a numeric variable and its new value, compute all derived variables
     * that need to be updated and return their new values.
     * 
     * Returns: vector of pairs (var_id, new_value) for all affected derived variables
     */
    std::vector<std::pair<int, ap_float>> compute_derived_updates(
        int changed_var_id,
        ap_float new_value,
        const std::vector<ap_float> &current_state) const;
    
    /**
     * Apply numeric effects of an operator to a state.
     * Returns the new numeric state after applying additive and assignment effects,
     * including updates to derived variables.
     */
    std::vector<ap_float> apply_numeric_effects(
        int op_id,
        const std::vector<ap_float> &current_numeric_state) const;
    
    // Conversion between regular (non-derived) and global numeric variable IDs
    int get_regular_var_id(int global_num_var_id) const {
        return glob_var_id_to_reg_num_var_id[global_num_var_id];
    }
    
    int get_global_var_id(int regular_num_var_id) const {
        return reg_num_var_id_to_glob_var_id[regular_num_var_id];
    }
    
    const TaskProxy &get_task_proxy() const {
        return task_proxy;
    }

private:
    const std::shared_ptr<AbstractTask> task;
    const TaskProxy task_proxy;
    
    // Domain mappings from factory
    const DomainMapping &domain_mapping;
    const NumericDomainMappingType &numeric_domain_mapping;
    const std::vector<int> &domain_sizes;
    const std::vector<int> &numeric_domain_sizes;
    const std::vector<int> &hash_multipliers;
    
    int n_numeric_variables;
    int n_propositional_variables;
    
    // Mapping between regular (non-derived) and global numeric variable IDs
    std::vector<int> reg_num_var_id_to_glob_var_id;
    std::vector<int> glob_var_id_to_reg_num_var_id;
    
    // Track which variables are derived
    std::vector<bool> is_derived_num_var;        // Numeric (assignment axioms)
    std::vector<bool> is_derived_prop_var;       // Propositional (comparison axioms)
    
    // Goals
    std::vector<numeric_condition::RegularNumericCondition> numeric_goals;
    std::vector<FactProxy> propositional_goals;
    
    // Initial state
    std::vector<ap_float> initial_numeric_values;
    
    // Axiom dependency information
    // For each variable, which other variables does it depend on?
    std::vector<std::vector<int>> axiom_dependencies;
    
    // Reverse dependencies: for each variable, which derived variables depend on it?
    std::vector<std::vector<int>> reverse_axiom_dependencies;
    
    // Auxiliary variables for complex expressions
    std::vector<ResNumericVariable> auxiliary_numeric_variables;
    std::unordered_map<std::string, size_t> auxiliary_num_vars_expressions;
    
    // Initialization methods
    void build_numeric_variables();
    void find_derived_variables();  // Find both numeric and propositional derived vars
    void build_goals();
    void build_axiom_dependencies();
    
    // Helper methods for building abstract operators
    void build_abstract_operator(
        const OperatorProxy &op,
        std::vector<AbstractOperator> &operators);
    
    /**
     * Given a concrete operator, compute all possible abstract transitions.
     * This includes:
     * 1. Direct propositional effects
     * 2. Direct numeric effects (which may transition between partitions)
     * 3. Cascading effects on comparison axioms (derived propositional vars)
     * 4. Cascading effects on assignment axioms (derived numeric vars)
     * 
     * For operators with numeric effects, this enumerates all possible
     * partition transitions and their cascading effects.
     */
    void enumerate_abstract_transitions(
        const OperatorProxy &op,
        std::vector<Fact> &prev_pairs,
        std::vector<Fact> &pre_pairs,
        std::vector<Fact> &eff_pairs,
        const std::vector<Fact> &effects_without_pre,
        const std::vector<NumAssProxy> &ass_effects,
        int concrete_op_id,
        std::vector<AbstractOperator> &operators);
    
    /**
     * Recursive helper to multiply out propositional effects without preconditions.
     * This implements the multiply_out pattern from the factory.
     */
    void multiply_out_propositional(
        int pos, int cost,
        std::vector<Fact> &prev_pairs,
        std::vector<Fact> &pre_pairs,
        std::vector<Fact> &eff_pairs,
        const std::vector<Fact> &effects_without_pre,
        const std::vector<NumAssProxy> &ass_effects,
        int concrete_op_id,
        std::vector<AbstractOperator> &operators);
    
    /**
     * Compute all hash effects for an operator, including cascading effects
     * from axioms. This enumerates all possible abstract state transitions
     * considering:
     * 1. Direct propositional effects
     * 2. Direct numeric effects (partition transitions)
     * 3. Cascading effects on derived numeric variables (assignment axioms)
     * 4. Cascading effects on derived propositional variables (comparison axioms)
     */
    std::vector<int> compute_hash_effects_with_cascades(
        const std::vector<Fact> &eff_pairs,
        const std::vector<NumAssProxy> &ass_effects);
    
    /**
     * Compute which derived propositional variables (from comparison axioms)
     * are affected when numeric variables change values/partitions.
     * 
     * Example: If comparison axiom is "var_x > 5", and we transition from
     * partition [-inf, 5) to [5, inf), the derived boolean may flip.
     */
    std::vector<Fact> compute_affected_comparison_axioms(
        const std::vector<int> &changed_numeric_vars,
        const std::vector<int> &old_partitions,
        const std::vector<int> &new_partitions) const;
    
    /**
     * Compute which target partitions are reachable from a source partition
     * when applying a numeric assignment effect.
     * 
     * Example: For a' ∈ (-inf, 3) and effect a += 2:
     *   Result range: (-inf, 5)
     *   This overlaps with partitions 0: (-inf, 3) and 1: [3, inf)
     *   Returns: [0, 1]
     * 
     * This implements correct range arithmetic from the reference document.
     */
    std::vector<int> compute_reachable_partitions(
        int numeric_var_id,
        int source_partition,
        const NumAssProxy &ass_effect) const;
    
    /**
     * Evaluate a comparison axiom exactly based on partition ranges.
     * 
     * Returns:
     *   0 = definitely false (all values in range fail comparison)
     *   1 = definitely true (all values in range satisfy comparison)
     *   2 = unknown (range spans comparison threshold, both possible)
     * 
     * Example: For comparison (x > 5) with x ∈ [6, 10):
     *   All values > 5, so returns 1 (definitely true)
     */
    int evaluate_comparison_exactly(
        const ComparisonAxiomProxy &axiom,
        int left_partition,
        int right_partition) const;
    
    // Helper for parsing arithmetic expressions
    std::shared_ptr<arithmetic_expression::ArithmeticExpression> 
        parse_arithmetic_expression(NumericVariableProxy num_var);
};

} // namespace domain_abstractions

#endif
