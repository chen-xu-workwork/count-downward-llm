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


extern std::string get_rovner_et_al_reference();
}

#endif
