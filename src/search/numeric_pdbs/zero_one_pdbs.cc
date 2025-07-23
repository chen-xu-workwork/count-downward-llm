#include "zero_one_pdbs.h"

#include "pattern_database.h"

#include "../task_proxy.h"

#include "../utils/logging.h"

#include <iostream>
#include <limits>
#include <memory>
#include <vector>

using namespace std;

namespace numeric_pdbs {
ZeroOnePDBs::ZeroOnePDBs(
    const shared_ptr<numeric_pdb_helper::NumericTaskProxy> &task_proxy, 
    const PatternCollection &patterns,
    std::size_t max_number_states,
    bool extend_abstract_state_space,
    bool need_goal,
    double f_layer_offset_ratio,
    bool keep_parent_pointers,
    double max_h_factor,
    InnerHeuristic exploration_h,
    InnerHeuristic frontier_h,
    InnerHeuristic failed_lookup_h
) : 
    extend_abstract_state_space(extend_abstract_state_space),
    f_layer_offset_ratio(f_layer_offset_ratio),
    keep_parent_pointers(keep_parent_pointers),
    max_h_factor(max_h_factor),
    need_goal(need_goal),
    exploration_h(exploration_h),
    frontier_h(frontier_h),
    failed_lookup_h(failed_lookup_h) {

    vector<ap_float> operator_costs;
    numeric_pdb_helper::NumericOperatorsProxy operators = task_proxy->get_operators();
    operator_costs.reserve(operators.size());
    for (size_t i = 0; i < operators.size(); ++i)
        operator_costs.push_back(operators[i].get_cost());

    pattern_databases.reserve(patterns.size());
    for (const Pattern &pattern : patterns) {
        shared_ptr<PatternDatabase> pdb = make_shared<PatternDatabase>(
            task_proxy, 
            pattern, 
            max_number_states,
            extend_abstract_state_space,
            need_goal,
            f_layer_offset_ratio,
            keep_parent_pointers,
            max_h_factor,
            exploration_h,
            frontier_h,
            failed_lookup_h,
            operator_costs,
            false
        );

        /* Set cost of relevant operators to 0 for further iterations
           (action cost partitioning). */
        for (size_t i = 0; i < operators.size(); ++i) {
            const numeric_pdb_helper::NumericOperatorProxy &op = operators[i];
            if (pdb->is_operator_relevant(op))
                operator_costs[i] = 0;
        }

        pattern_databases.push_back(pdb);
    }
}

ap_float ZeroOnePDBs::get_value(const State &state) const {
    /*
      Because we use cost partitioning, we can simply add up all
      heuristic values of all patterns in the pattern collection.
    */
    ap_float h_val = 0;
    for (const shared_ptr<PatternDatabase> &pdb : pattern_databases) {
        pair<bool, ap_float> pair = pdb->get_value(state);
        ap_float pdb_value = pair.second;
        if (pdb_value == numeric_limits<ap_float>::max())
            return numeric_limits<ap_float>::max();
        h_val += pdb_value;
    }
    return h_val;
}

ap_float ZeroOnePDBs::compute_approx_mean_finite_h() const {
    ap_float approx_mean_finite_h = 0;
    for (const shared_ptr<PatternDatabase> &pdb : pattern_databases) {
        approx_mean_finite_h += pdb->compute_mean_finite_h();
    }
    return approx_mean_finite_h / pattern_databases.size();
}

void ZeroOnePDBs::dump() const {
    for (const shared_ptr<PatternDatabase> &pdb : pattern_databases) {
        cout << pdb->get_pattern() << endl;
    }
}
}
