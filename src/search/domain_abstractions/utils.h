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
    const NumericDomainMappingType &numeric_domain_mapping,
    const std::vector<int> &hash_multipliers);

extern std::string get_rovner_et_al_reference();
}

#endif
