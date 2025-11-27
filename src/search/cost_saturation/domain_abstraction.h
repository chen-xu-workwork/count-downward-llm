#ifndef COST_SATURATION_DOMAIN_ABSTRACTION_H
#define COST_SATURATION_DOMAIN_ABSTRACTION_H

#include "abstraction.h"

#include "projection.h" // for TaskInfo and RankedOperator

#include "../abstract_task.h"

#include "../algorithms/array_pool.h"
#include "../domain_abstractions/types.h"
#include "../pdbs/types.h"

#include <functional>
#include <vector>

class OperatorProxy;
class TaskProxy;
class VariablesProxy;

namespace domain_abstractions {
class MatchTree;
}

namespace utils {
class LogProxy;
}

namespace cost_saturation {
class DomainAbstractionFunction : public AbstractionFunction {
    const domain_abstractions::DomainMapping domain_mapping;
    const domain_abstractions::NumericDomainMappingType numeric_domain_mapping;
    struct VariableAndMultiplier {
        int pattern_var;
        int hash_multiplier;

        VariableAndMultiplier(int pattern_var, int hash_multiplier)
            : pattern_var(pattern_var),
              hash_multiplier(hash_multiplier) {
        }
    };
    std::vector<VariableAndMultiplier> variables_and_multipliers;

public:
    DomainAbstractionFunction(
        const pdbs::Pattern &pattern,
        const std::vector<int> &hash_multipliers,
        domain_abstractions::DomainMapping domain_mapping);

    virtual int get_abstract_state_id(const State &concrete_state) const override;
};


class DomainAbstraction : public Abstraction {
    using Facts = std::vector<Fact>;
    using OperatorCallback =
        std::function<void (Facts &, Facts &, Facts &, const std::vector<int> &)>;

    std::shared_ptr<TaskInfo> task_info;
    domain_abstractions::DomainMapping domain_mapping;
    domain_abstractions::NumericDomainMappingType numeric_domain_mapping;
    pdbs::Pattern pattern;
    array_pool_template::ArrayPool<int> label_to_operators;

    std::vector<bool> looping_operators;

    std::vector<RankedOperator> ranked_operators;
    std::unique_ptr<domain_abstractions::MatchTree> match_tree_backward;

    // Number of abstract states in the projection.
    int num_states;

    // Multipliers for each variable for perfect hash function.
    std::vector<int> hash_multipliers;

    // Domain size of each variable in the pattern.
    std::vector<int> pattern_domain_sizes;

    std::vector<int> goal_states;

    std::vector<int> compute_goal_states(
        const std::vector<int> &variable_to_pattern_index) const;

    /*
      Given an abstract state (represented as a vector of facts), compute the
      "next" fact. Return true iff there is a next fact.
    */
    bool increment_to_next_state(std::vector<Fact> &facts) const;

    /*
      Apply a function to all state-changing transitions in the projection
      (including unreachable and unsolvable transitions).
    */
    template<class Callback>
    void for_each_label_transition(const Callback &callback) const {
        // Reuse vector to save allocations.
        std::vector<Fact> abstract_facts;

        for (const RankedOperator &ranked_operator : ranked_operators) {
            // Choose any operator covered by the label.
            int concrete_op_id = *label_to_operators.get_slice(ranked_operator.label).begin();
            abstract_facts.clear();
            for (size_t i = 0; i < pattern.size(); ++i) {
                int var = pattern[i];
                if (!task_info->operator_mentions_variable(concrete_op_id, var)) {
                    abstract_facts.emplace_back(i, 0);
                }
            }

            bool has_next_match = true;
            while (has_next_match) {
                int state = ranked_operator.precondition_hash;
                for (const Fact &fact : abstract_facts) {
                    state += hash_multipliers[fact.var] * fact.value;
                }
                callback(Transition(state,
                                    ranked_operator.label,
                                    state + ranked_operator.hash_effect));
                has_next_match = increment_to_next_state(abstract_facts);
            }
        }
    }

    /*
      Recursive method; called by build_abstract_operators. In the case
      of a precondition with value = -1 in the concrete operator, all
      multiplied-out abstract operators are computed, i.e., for all
      possible values of the variable (with precondition = -1), one
      abstract operator with a concrete value (!= -1) is computed.
    */
    void multiply_out(
        int pos,
        std::vector<Fact> &prev_pairs,
        std::vector<Fact> &pre_pairs,
        std::vector<Fact> &eff_pairs,
        const std::vector<Fact> &effects_without_pre,
        const OperatorCallback &callback,
        utils::Log &log) const;

    /*
      Compute all abstract operators for a given concrete operator. Initialize
      data structures for initial call to recursive method multiply_out.
      variable_to_index maps variables in the task to their index in the
      pattern or -1.
    */
    void build_ranked_operators(
        const std::vector<Fact> &preconditions,
        const std::vector<Fact> &effects,
        int num_vars,
        const OperatorCallback &callback,
        utils::Log &log) const;

    /*
      Return true iff all abstract facts hold in the given state.
    */
    bool is_consistent(
        int state_index,
        const std::vector<Fact> &abstract_facts) const;

public:
    DomainAbstraction(
        const TaskProxy &task_proxy,
        const std::shared_ptr<TaskInfo> &task_info,
        domain_abstractions::DomainAbstraction &domain_abstraction,
        bool combine_labels,
        utils::Log &log);
    virtual ~DomainAbstraction() override;

    virtual std::vector<ap_float> compute_goal_distances(
        const std::vector<ap_float> &operator_costs) const override;
    virtual std::vector<ap_float> compute_saturated_costs(
        const std::vector<ap_float> &h_values) const override;
    virtual int get_num_operators() const override;
    virtual bool operator_is_active(int op_id) const override;
    virtual bool operator_induces_self_loop(int op_id) const override;
    virtual void for_each_transition(const TransitionCallback &callback) const override;
    virtual int get_num_states() const override;
    virtual const std::vector<int> &get_goal_states() const override;

    const pdbs::Pattern &get_pattern() const;
    virtual void dump() const override;
};
}

#endif
