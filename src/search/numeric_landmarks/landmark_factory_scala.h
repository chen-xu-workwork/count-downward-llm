#ifndef NUMERIC_LANDMARKS_LANDMARK_FACTORY_SCALA_H
#define NUMERIC_LANDMARKS_LANDMARK_FACTORY_SCALA_H

#include "../landmarks/landmark_factory.h"
#include "../landmarks/landmark_graph.h"
#include "../numeric_operator_counting/numeric_helper.h"

#include "../globals.h"

#include "../utils/hash.h"

#include <unordered_set>
#include <utility>
#include <vector>
#include <stack>

namespace landmarks {

class LandmarkFactoryScala {
private:
    TaskProxy task;
    numeric_helper::NumericTaskProxy numeric_task;
    std::vector<std::set<int>> achieve; // index: action, value, std::set of conditions are achieved by the action // this may be useless, just get the post-conditions!
    
    std::vector<std::set<int>> possible_achievers; // index: action, value, std::set of numeric conditions that can be achieved by the action
    std::vector<std::set<int>> possible_achievers_inverted; // index: numeric condition, value, std::set actions that can modify the numeric achiever
    std::vector<std::vector<double>> net_effects; // index: action, index n_condition, value: net effect;

    std::vector<std::set<int>> condition_to_action;
    std::vector<double> cond_dist;
    std::vector<double> cond_num_dist;
    std::vector<bool> is_init_state; // TODO erease, for debug only
    std::vector<bool> set_lm; // this is created to check if the vector lm is initialised or it's actually empty;
    std::vector<bool> set_never_active;
    std::vector<std::set<int>> reach_achievers; // this is actually not essential at the moment TODO: add this to the cost-partitioning
    std::vector<FactProxy> facts_collection;
    std::vector<std::set<int>> lm;
    
    void update_actions_conditions(const State &s0, OperatorProxy &gr, std::stack<OperatorProxy> & a_plus, std::vector<bool> &never_active, std::vector<std::set<int> > &lm);
    void update_action_condition(OperatorProxy &gr, int comp, std::vector<std::set<int> > &lm, std::vector<bool> &never_active, std::stack<OperatorProxy> & a_plus);
    bool update_lm(int p, OperatorProxy &gr, std::vector<std::set<int> > &lm);
    std::set<int> metric_sensitive_intersection(std::set<int> & previous, std::set<int> & temp);
    bool check_conditions(int gr2);
    void generate_link_precondition_action();
    void generate_possible_achievers();
    bool check_if_smark_intersection_needed();
    std::set<int> reachable;
    std::set<int> goal_landmarks;
    std::set<int> newset;
    std::set<int> action_landmarks;
public:
    LandmarkFactoryScala(const std::shared_ptr<AbstractTask> t);
    ~LandmarkFactoryScala() {
        
    }
    
    std::set<int> & compute_landmarks(const State &state);
    std::set<int> & compute_action_landmarks(std::set<int> &fact_landmarks);
    //TODO, improve this, this can be done only if compute_landmarks(const State &state) has been called.
    std::vector<std::set<int>> & get_landmarks_table(){
        return lm;

    }
};
}

#endif
