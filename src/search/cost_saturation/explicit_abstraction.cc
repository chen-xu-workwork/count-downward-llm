#include "explicit_abstraction.h"

#include "types.h"

#include "../utils/collections.h"
#include "../utils/logging.h"
#include "../utils/strings.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_set>

using namespace std;

namespace cost_saturation {

using PriorityQueue = std::priority_queue<
    std::pair<ap_float, int>,
    std::vector<std::pair<ap_float, int>>,
    std::greater<std::pair<ap_float, int>>>;

static void dijkstra_search(
    const vector<vector<Successor>> &graph,
    const vector<ap_float> &costs,
    PriorityQueue &queue,
    vector<ap_float> &distances) {
    while (!queue.empty()) {
        pair<ap_float, int> top_pair = queue.top();
        queue.pop();
        ap_float distance = top_pair.first;
        int state = top_pair.second;
        ap_float state_distance = distances[state];
        
        if (state_distance < distance) {
            continue;
        }
        for (const Successor &transition : graph[state]) {
            int successor = transition.state;
            int op = transition.op;
            ap_float cost = costs[op];
            ap_float successor_distance = (isinf(cost)) ? INF : state_distance + cost;
            
            if (distances[successor] > successor_distance) {
                distances[successor] = successor_distance;
                queue.push({successor_distance, successor});
            }
        }
    }
}

ostream &operator<<(ostream &os, const Successor &successor) {
    os << "(" << successor.op << ", " << successor.state << ")";
    return os;
}

static vector<bool> get_active_operators_from_graph(
    const vector<vector<Successor>> &backward_graph, int num_ops) {
    vector<bool> active_operators(num_ops, false);
    int num_states = backward_graph.size();
    for (int target = 0; target < num_states; ++target) {
        for (const Successor &transition : backward_graph[target]) {
            int op_id = transition.op;
            active_operators[op_id] = true;
        }
    }
    return active_operators;
}

ExplicitAbstraction::ExplicitAbstraction(
    unique_ptr<AbstractionFunction> abstraction_function,
    vector<vector<Successor>> &&backward_graph_,
    vector<bool> &&looping_operators,
    vector<int> &&goal_states)
    : Abstraction(move(abstraction_function)),
      backward_graph(move(backward_graph_)),
      active_operators(get_active_operators_from_graph(
                           backward_graph, looping_operators.size())),
      looping_operators(move(looping_operators)),
      goal_states(move(goal_states)) {
#ifndef NDEBUG
    for (int target = 0; target < get_num_states(); ++target) {
        // Check that no transition is stored multiple times.
        vector<Successor> copied_transitions = this->backward_graph[target];
        sort(copied_transitions.begin(), copied_transitions.end());
        assert(utils::is_sorted_unique(copied_transitions));
        // Check that we don't store self-loops.
        assert(all_of(copied_transitions.begin(), copied_transitions.end(),
                      [target](const Successor &succ) {return succ.state != target;}));
    }
#endif
}

vector<ap_float> ExplicitAbstraction::compute_goal_distances(const vector<ap_float> &costs) const {
    vector<ap_float> goal_distances(get_num_states(), INF);
    PriorityQueue queue;
    for (int goal_state : goal_states) {
        goal_distances[goal_state] = 0;
        queue.push({0, goal_state});
    }
    dijkstra_search(backward_graph, costs, queue, goal_distances);
    return goal_distances;
}

vector<ap_float> ExplicitAbstraction::compute_saturated_costs(
    const vector<ap_float> &h_values) const {
    int num_operators = get_num_operators();
    vector<ap_float> saturated_costs(num_operators, -INF);

    /* To prevent negative cost cycles we ensure that all operators
       inducing self-loops have non-negative costs. */
    for (int op_id = 0; op_id < num_operators; ++op_id) {
        if (looping_operators[op_id]) {
            saturated_costs[op_id] = 0;
        }
    }

    int num_states = backward_graph.size();
    for (int target = 0; target < num_states; ++target) {
        ap_float target_h = h_values[target];
        if (isinf(target_h)) {
            continue;
        }

        for (const Successor &transition : backward_graph[target]) {
            int op_id = transition.op;
            int src = transition.state;
            ap_float src_h = h_values[src];
            if (isinf(src_h)) {
                continue;
            }

            const ap_float needed = src_h - target_h;
            if (saturated_costs[op_id] < needed) {
                saturated_costs[op_id] = needed;
            }
        }
    }
    return saturated_costs;
}

int ExplicitAbstraction::get_num_operators() const {
    return looping_operators.size();
}

int ExplicitAbstraction::get_num_states() const {
    return backward_graph.size();
}

bool ExplicitAbstraction::operator_is_active(int op_id) const {
    return active_operators[op_id];
}

bool ExplicitAbstraction::operator_induces_self_loop(int op_id) const {
    return looping_operators[op_id];
}

void ExplicitAbstraction::for_each_transition(const TransitionCallback &callback) const {
    int num_states = get_num_states();
    for (int target = 0; target < num_states; ++target) {
        for (const Successor &transition : backward_graph[target]) {
            int op_id = transition.op;
            int src = transition.state;
            callback(Transition(src, op_id, target));
        }
    }
}

const vector<int> &ExplicitAbstraction::get_goal_states() const {
    return goal_states;
}

void ExplicitAbstraction::dump() const {
    int num_states = get_num_states();

    cout << "States: " << num_states << endl;
    cout << "Goal states: " << goal_states.size() << endl;
    cout << "Operators inducing state-changing transitions: "
         << count(active_operators.begin(), active_operators.end(), true) << endl;
    cout << "Operators inducing self-loops: "
         << count(looping_operators.begin(), looping_operators.end(), true) << endl;

    vector<bool> is_goal(num_states, false);
    for (int goal : goal_states) {
        is_goal[goal] = true;
    }

    cout << "digraph transition_system";
    cout << " {" << endl;
    for (int i = 0; i < num_states; ++i) {
        cout << "    node [shape = " << (is_goal[i] ? "doublecircle" : "circle")
             << "] " << i << ";" << endl;
    }
    for (int target = 0; target < num_states; ++target) {
        unordered_map<int, vector<int>> parallel_transitions;
        for (const Successor &succ : backward_graph[target]) {
            int src = succ.state;
            parallel_transitions[src].push_back(succ.op);
        }
        for (const auto &pair : parallel_transitions) {
            int src = pair.first;
            const vector<int> &operators = pair.second;
            cout << "    " << src << " -> " << target
                 << " [label = \"" << utils::join(operators, "_") << "\"];" << endl;
        }
    }
    cout << "}" << endl;
}
}
