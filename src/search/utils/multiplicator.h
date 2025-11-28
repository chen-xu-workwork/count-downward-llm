#ifndef UTILS_MULTIPLICATOR_H
#define UTILS_MULTIPLICATOR_H

#include "../domain_abstractions/domain_abstraction_factory.h"
#include "../domain_abstractions/types.h"
#include "../task_proxy.h"

#include <vector>
#include <memory>
#include <unordered_map>

namespace utils {

struct TransitionInfo {
    std::vector<Fact> source_partition_facts;
    std::vector<Fact> target_partition_facts;
    std::vector<Fact> prevail_facts;
};

class Multiplicator {
    const std::shared_ptr<AbstractTask> task;
    TaskProxy task_proxy;
    const domain_abstractions::DomainMapping &domain_mapping;
    const domain_abstractions::NumericDomainMappingType &numeric_domain_mapping;
    const std::vector<int> &domain_sizes;
    const std::vector<int> &numeric_domain_sizes;
    const std::vector<int> &hash_multipliers;

    int n_numeric_variables;
    int n_propositional_variables;

    // Dependency tracking (reimplemented from NumericHelper)
    std::vector<bool> is_derived_num_var;
    std::vector<bool> is_derived_prop_var;
    std::vector<std::vector<int>> axiom_dependencies;
    std::vector<std::vector<int>> reverse_axiom_dependencies;

    void find_derived_variables();
    void build_axiom_dependencies();

    void multiply_out_recursive(
        int pos, ap_float cost,
        std::vector<Fact> &prev_pairs,
        std::vector<Fact> &pre_pairs,
        std::vector<Fact> &eff_pairs,
        const std::vector<Fact> &effects_without_pre,
        const std::vector<NumAssProxy> &ass_effects,
        int concrete_op_id,
        std::vector<domain_abstractions::AbstractOperator> &operators,
        const OperatorProxy &op);

    std::vector<TransitionInfo> compute_hash_effects_with_preconditions(
        const std::vector<Fact> &pre_pairs,
        const std::vector<Fact> &eff_pairs,
        const std::vector<NumAssProxy> &ass_effects,
        const OperatorProxy &op);

    std::vector<int> compute_reachable_partitions(
        int numeric_var_id,
        int source_partition,
        const NumAssProxy &ass_effect) const;

    std::vector<Fact> compute_affected_comparison_axioms(
        const std::vector<int> &changed_numeric_vars,
        const std::vector<int> &old_partitions,
        const std::vector<int> &new_partitions) const;

    std::vector<Fact> compute_assignment_axiom_cascades(
        const std::vector<int> &changed_numeric_vars,
        const std::vector<int> &old_partitions,
        const std::vector<int> &new_partitions) const;

    int evaluate_comparison_exactly(
        const ComparisonAxiomProxy &axiom,
        int left_partition,
        int right_partition) const;

    std::pair<ap_float, ap_float> apply_range_operation(
        ap_float left_lower, ap_float left_upper,
        ap_float right_lower, ap_float right_upper,
        cal_operator op) const;

public:
    Multiplicator(const std::shared_ptr<AbstractTask> &task,
                  const domain_abstractions::DomainMapping &domain_mapping,
                  const domain_abstractions::NumericDomainMappingType &numeric_domain_mapping,
                  const std::vector<int> &domain_sizes,
                  const std::vector<int> &numeric_domain_sizes,
                  const std::vector<int> &hash_multipliers);

    void multiply_out(const OperatorProxy &op, 
                      std::vector<domain_abstractions::AbstractOperator> &operators);
};

}

#endif
