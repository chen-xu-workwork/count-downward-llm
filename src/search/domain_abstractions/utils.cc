#include "utils.h"

#include "domain_abstraction.h"

#include "../task_proxy.h"


#include "../task_tools.h"
#include "../causal_graph.h"


#include "../utils/logging.h"
#include "../utils/markup.h"
#include "../utils/math.h"
#include "../utils/rng.h"

#include <limits>

using namespace std;

namespace domain_abstractions {
vector<Fact> get_goals_in_random_order(
    const TaskProxy &task_proxy, utils::RandomNumberGenerator &rng) {
    //vector<Fact> goals = task_tools::get_facts(task_proxy.get_goals());
    GoalsProxy goals_proxy = task_proxy.get_goals();
    vector<Fact> goals;
    goals.reserve(goals_proxy.size());
    for (size_t i = 0; i < goals_proxy.size(); ++i) {
        FactProxy goal = goals_proxy[i];
        goals.push_back(Fact(goal.get_variable().get_id(), goal.get_value()));
    }
    rng.shuffle(goals);
    return goals;
}

string get_rovner_et_al_reference() {
    return " (Rovner, Helmert, and Domshlak 2019, https://doi.org/10.1007/978-3-030-30244-3_22).";
}

vector<int> get_non_goal_variables(const TaskProxy &task_proxy) {
    size_t num_vars = task_proxy.get_variables().size();
    GoalsProxy goals = task_proxy.get_goals();
    vector<bool> is_goal(num_vars, false);
    for (FactProxy goal : goals) {
        is_goal[goal.get_variable().get_id()] = true;
    }

    vector<int> non_goal_variables;
    non_goal_variables.reserve(num_vars - goals.size());
    for (int var_id = 0; var_id < static_cast<int>(num_vars); ++var_id) {
        if (!is_goal[var_id]) {
            non_goal_variables.push_back(var_id);
        }
    }
    return non_goal_variables;
}

vector<vector<int>> compute_cg_neighbors(
    const TaskProxy &task_proxy,
    bool bidirectional) {
    const CausalGraph &cg = task_proxy.get_causal_graph();
    int num_vars = task_proxy.get_variables().size();
    vector<vector<int>> cg_neighbors(num_vars);
    for (int var_id = 0; var_id < num_vars; ++var_id) {
        cg_neighbors[var_id] = cg.get_predecessors(var_id);
        if (bidirectional) {
            const vector<int> &successors = cg.get_successors(var_id);
            cg_neighbors[var_id].insert(cg_neighbors[var_id].end(), successors.begin(), successors.end());
        }
        std::sort(cg_neighbors[var_id].begin(), cg_neighbors[var_id].end());
        cg_neighbors[var_id].erase(
            std::unique(cg_neighbors[var_id].begin(), cg_neighbors[var_id].end()),
            cg_neighbors[var_id].end());
    }
    return cg_neighbors;
}
}