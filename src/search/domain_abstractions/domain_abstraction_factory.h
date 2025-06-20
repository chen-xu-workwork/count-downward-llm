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

    /*
      Effect of the operator during regression search on a given
      abstract state number.
    */
    int hash_effect;
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
      Returns the effect of the abstract operator in form of a value
      change (+ or -) to an abstract state index
    */
    int get_hash_effect() const {return hash_effect;}

    int get_concrete_op_id() const {
        return concrete_op_id;
    }

    /*
      Returns the cost of the abstract operator (same as the cost of
      the original concrete operator)
    */
    int get_cost() const {return cost;}
    void dump(const VariablesProxy &variables,
              utils::LogProxy &log) const;
};

class DomainAbstractionFactory {
    DomainMapping domain_mapping;
    /*
      final h-values for abstract-states.
      dead-ends are represented by numeric_limits<int>::max()
    */
    std::vector<int> distances;

    std::vector<int> generating_op_ids;
    std::vector<std::vector<int>> wildcard_plan;

    // multipliers for each variable for perfect hash function
    std::vector<int> hash_multipliers;

    int num_states;

    std::vector<AbstractOperator> compute_abstract_operators(
        const TaskProxy &task_proxy, const std::vector<int> &domain_sizes);
    MatchTree build_match_tree(const std::vector<int> &domain_sizes,
                               const std::vector<AbstractOperator> &operators);
    std::vector<Fact> compute_abstract_goals(const TaskProxy &task_proxy);
    void compute_distances(
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

    void multiply_out(int pos, int cost,
                      std::vector<Fact> &prev_pairs,
                      std::vector<Fact> &pre_pairs,
                      std::vector<Fact> &eff_pairs,
                      const std::vector<Fact> &effects_without_pre,
                      int concrete_op_id,
                      const std::vector<int> &domain_sizes,
                      std::vector<AbstractOperator> &operators);
    void build_abstract_operators(const OperatorProxy &op,
                                  int num_variables,
                                  const std::vector<int> &domain_sizes,
                                  std::vector<AbstractOperator> &operators);
    bool is_goal_state(int state_index,
                       const std::vector<Fact> &abstract_goals,
                       const std::vector<int> &domain_sizes) const;
    int hash_index(const std::vector<int> &state) const;
    bool variable_is_trivial(int var_id) const;

public:
    DomainAbstractionFactory(
        const TaskProxy &task_proxy,
        const DomainMapping &domain_mapping,
        const std::vector<int> &domain_sizes,
        bool compute_plan,
        const std::shared_ptr<utils::RandomNumberGenerator> &rng,
        bool compute_wildcard_plan);

    DomainAbstraction generate();

    std::vector<std::vector<int>> &&extract_wildcard_plan() {
        return std::move(wildcard_plan);
    };
};
}

#endif
