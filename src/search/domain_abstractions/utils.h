#ifndef DOMAIN_ABSTRACTIONS_UTILS_H
#define DOMAIN_ABSTRACTIONS_UTILS_H

#include "types.h"

#include "../task_proxy.h"

#include "../utils/timer.h"

#include <memory>
#include <string>

namespace utils {
class RandomNumberGenerator;
}

namespace domain_abstractions {
extern std::vector<Fact> get_goals_in_random_order(
    const TaskProxy &task_proxy, utils::RandomNumberGenerator &rng);
extern std::vector<int> get_non_goal_variables(const TaskProxy &task_proxy);

/*
  Compute the causal graph neighbors for each variable of the task. If
  bidirectional is false, then only predecessors of variables are considered
  neighbors. If bidirectional is true, then the causal graph is treated as
  undirected graph and also successors of variables are considered neighbors.
*/
extern std::vector<std::vector<int>> compute_cg_neighbors(
    const TaskProxy &task_proxy,
    bool bidirectional);

/*
  Enumerate all possible abstract predecessor state indices considering
  comparison axiom cascades. This function evaluates comparison axioms
  optimistically:
  - For DEFINITELY_TRUE/FALSE comparisons, use the fixed value
  - For UNKNOWN comparisons, optimistically try TRUE possibility
  
  For the initial state (no numeric changes), the returned vector has size 1.
*/
extern std::vector<int> enumerate_cascade_predecessors(
    int base_predecessor_index,
    const std::vector<int> &changed_numeric_vars,
    const std::vector<int> &source_partitions,
    const std::vector<int> &target_partitions,
    const TaskProxy &task_proxy,
    const DomainMapping &domain_mapping,
    const NumericDomainMappings &numeric_domain_mapping,
    const std::vector<int> &hash_multipliers);

/*
  Compute the abstract state hash for a concrete state, including:
  1. Propositional variables (with comparison axioms set to UNKNOWN initially)
  2. Numeric variable partitions
  3. Evaluation of comparison axioms using CONCRETE state values
  
  This version uses the actual evaluated comparison axiom values from the state,
  only falling back to range-based evaluation if the axiom is not yet evaluated.
*/
extern size_t compute_abstract_state_hash(
    const State &state,
    const TaskProxy &task_proxy,
    const DomainMapping &domain_mapping,
    const NumericDomainMappings &numeric_domain_mapping,
    const std::vector<int> &hash_multipliers);

/*
  BACKUP VERSION: Compute the abstract state hash using OPTIMISTIC range-based
  evaluation for comparison axioms. This version evaluates comparison axioms
  based on the ranges computed from assignment axioms, optimistically choosing
  TRUE when ranges permit both TRUE and FALSE.
  
  This approach is useful for exploring possibilities during predecessor
  enumeration, but should NOT be used for hashing concrete states (use
  compute_abstract_state_hash instead for that).
*/
extern size_t compute_abstract_state_hash_backup(
    const State &state,
    const TaskProxy &task_proxy,
    const DomainMapping &domain_mapping,
    const NumericDomainMappings &numeric_domain_mapping,
    const std::vector<int> &hash_multipliers);

extern std::string get_rovner_et_al_reference();
}

#endif
