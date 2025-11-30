#ifndef COST_SATURATION_DOMAIN_ABSTRACTION_H
#define COST_SATURATION_DOMAIN_ABSTRACTION_H

#include "abstraction.h"

#include "projection.h" // for TaskInfo and RankedOperator

#include "../abstract_task.h"

#include "../algorithms/array_pool.h"
#include "../domain_abstractions/types.h"
#include "../domain_abstractions/numeric_helper.h"
#include "../pdbs/types.h"
#include "../task_proxy.h"

#include <functional>
#include <vector>

class OperatorProxy;
class VariablesProxy;

namespace domain_abstractions {
class MatchTreeWithPattern;
}

namespace utils {
class LogProxy;
}

namespace cost_saturation {
class DomainAbstractionFunction : public AbstractionFunction {
    const domain_abstractions::DomainMapping domain_mapping;
    domain_abstractions::NumericDomainMappingType numeric_domain_mapping;
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
        domain_abstractions::DomainMapping domain_mapping,
        const domain_abstractions::NumericDomainMappingType &numeric_domain_mapping);

    virtual int get_abstract_state_id(const State &concrete_state) const override;
};


class DomainAbstraction : public Abstraction {
    using Facts = std::vector<Fact>;
    using OperatorCallback =
        std::function<void (Facts &, Facts &, Facts &, const std::vector<int> &)>;

    TaskProxy task_proxy;
    std::shared_ptr<TaskInfo> task_info;
    domain_abstractions::DomainMapping domain_mapping;
    domain_abstractions::NumericDomainMappingType numeric_domain_mapping;
    pdbs::Pattern pattern;
    array_pool_template::ArrayPool<int> label_to_operators;

    std::vector<bool> looping_operators;

    std::vector<RankedOperator> ranked_operators;
    std::unique_ptr<domain_abstractions::MatchTreeWithPattern> match_tree_backward;

    // Number of abstract states in the projection.
    int num_states;

    // Multipliers for each variable for perfect hash function (indexed by pattern position).
    std::vector<int> hash_multipliers;

    // Hash multipliers indexed by original variable ID (for functions that iterate by var_id)
    std::vector<int> hash_multipliers_by_var_id;

    // Domain size of each variable in the pattern.
    std::vector<int> pattern_domain_sizes;

    std::vector<int> goal_states;

    std::vector<int> compute_goal_states(
        const std::vector<int> &variable_to_pattern_index) const;

    std::vector<int> enumerate_states_with_evaluated_comparisons(int base_state_index) const;

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
            // Debug: print operator info
            //std::cout << "DEBUG ranked_operator:\n" << decode_ranked_operator(ranked_operator) << std::endl;
            
            // Choose any operator covered by the label.
            int concrete_op_id = *label_to_operators.get_slice(ranked_operator.label).begin();
            //std::cout << "DEBUG " << decode_mentioned_variables(concrete_op_id) << std::endl;
            
            abstract_facts.clear();
            for (size_t i = 0; i < pattern.size(); ++i) {
                int var = pattern[i];
                if (!task_info->operator_mentions_variable(concrete_op_id, var)) {
                    abstract_facts.emplace_back(i, 0);
                }
            }
            abstract_facts.clear();

            bool has_next_match = true;
            while (has_next_match) {
                int state = ranked_operator.precondition_hash;
                //std::cout << "DEBUG precondition: " << decode_state(state) << std::endl;
                for (const Fact &fact : abstract_facts) {
                    state += hash_multipliers[fact.var] * fact.value;
                    if ( hash_multipliers[fact.var] * fact.value != 0 ) {
                        //std::cout << " + var " << pattern[fact.var]
                        //    << " (pattern idx " << fact.var << ")"
                        //    << " value " << fact.value
                        //    << " multiplier " << hash_multipliers[fact.var]
                        //    << " -> partial state: " << decode_state(state) << std::endl;
                    }
                }
                //std::cout << "DEBUG base state: " << decode_state(state) << std::endl;
                
                if (state >= num_states || state < 0) {
                    //std::cout << "ERROR: state out of bounds in projection with "
                    //          << num_states << " states." << std::endl;
                    //std::cout << decode_domain_abstraction() << std::endl;
                }
                assert(state < num_states && state >= 0);

                bool is_possible_state = false; 
                //TODO: Can be optimized by only considering most optimistic comparison evaluations
                //      and doing logic comparisons instead of enumerating all states.
                std::vector<int> possible_states = enumerate_states_with_evaluated_comparisons(state);
                for (int ps : possible_states) {
                    if (ps == state) {
                        is_possible_state = true;
                        break;
                    }
                }
                if (is_possible_state) {
                    int base_target = state + ranked_operator.hash_effect;
                    //std::cout << "DEBUG base target: " << decode_state(base_target) << std::endl;
                    std::vector<int> successors = enumerate_states_with_evaluated_comparisons(base_target);
                    for (int succ : successors) {
                        //std::cout << "DEBUG succ state: " << decode_state(succ) << std::endl;
                        //std::cout << "Successor: " << succ << ", Num states: " << num_states << std::endl;
                        assert(succ < num_states && succ >= 0);
                        if (succ == state) {
                            //std::cout << "DEBUG self-loop detected." << std::endl;
                            continue; // Skip self-loops
                        }
                        callback(Transition(state,
                                            ranked_operator.label,
                                            succ));
                    }
                
                } 
                break;
                has_next_match = increment_to_next_state(abstract_facts);
                //std::cout << std::endl;
            }
        }
    }

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
    
    // Debug helper: decode state index into human-readable variable->value mapping
    std::string decode_state(int state_index) const;
    
    // Debug helper: decode ranked operator into human-readable format
    std::string decode_ranked_operator(const RankedOperator &ranked_op) const;
    
    // Debug helper: print names of variables mentioned by an operator (complement of abstract_facts)
    std::string decode_mentioned_variables(int concrete_op_id) const;
    
    // Debug helper: print complete domain abstraction info (domains, numeric ranges, partitions)
    std::string decode_domain_abstraction() const;
};
}

#endif
