#ifndef RMAX_HEURISTIC_H
#define RMAX_HEURISTIC_H

#include "../heuristic.h"
#include "../priority_queue.h"

#include "../numeric_operator_counting/numeric_helper.h"

#include <vector>
#include <set>


namespace numeric_pdbs {
class CanonicalPDBs;
class PatternDatabase;
}

namespace rmax_heuristic {

class RMaxHeuristic : public Heuristic {
    friend class numeric_pdbs::CanonicalPDBs;
    friend class numeric_pdbs::PatternDatabase;

    explicit RMaxHeuristic(const std::shared_ptr<AbstractTask> &task);
protected:
	virtual void initialize();
	virtual ap_float compute_heuristic(const GlobalState &global_state);
    ap_float compute_heuristic(const State &state);
	virtual ap_float update_cost(ap_float old_cost, ap_float new_cost) {return std::max(old_cost, new_cost);}

    bool restrict_achievers;
    bool ceiling_less_than_one;

    std::vector<std::set<int>> condition_to_action; // index condition, value set of action with that preconditions
    std::vector<double> cond_dist;
    std::vector<double> cond_num_dist;
    std::vector<double> action_dist;
    std::vector<bool> is_init_state; // TODO erease, for debug only
    std::vector<bool> closed;

    std::vector<std::set<int>> possible_achievers; // index: action, value, set of numeric conditions that can be achieved by the action
    std::vector<std::set<int>> possible_achievers_inverted; // index: numeric condition, value, set actions that can modify the numeric achiever
    std::vector<std::set<int>> all_achievers;
    numeric_helper::NumericTaskProxy numeric_task;
    void update_reachable_conditions_actions(const State &s_0, int gr, HeapQueue<int>& a_plus);
    void update_reachable_actions(int gr, int cond, HeapQueue<int>& a_plus);
    std::vector<std::vector<double>> net_effects; // index: action, index n_condition, value: net effect;
    std::vector<std::vector<double>> action_comp_number_execution;

    double check_conditions(int gr_id);
    double max_float;
    void generate_possible_achievers();
    void generate_preconditions();
    double check_goal();
    double get_number_of_execution(int gr, const State &s_0, int n_condition);
    double min_over_possible_achievers(int nc_id);
public:
	explicit RMaxHeuristic(const options::Options &options);
	~RMaxHeuristic() override = default;
    
};
}

#endif
