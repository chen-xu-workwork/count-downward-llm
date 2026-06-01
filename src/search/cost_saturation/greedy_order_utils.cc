#include "greedy_order_utils.h"

#include "types.h"

#include "../option_parser.h"

#include "../utils/collections.h"
#include "../utils/logging.h"
#include "../utils/system.h"

#include <cassert>
#include <cmath>

using namespace std;

namespace cost_saturation {
ap_float compute_stolen_costs(ap_float wanted_by_abs, ap_float surplus_cost) {
    assert(!isinf(wanted_by_abs) || wanted_by_abs < 0);
    assert(!isinf(surplus_cost) || surplus_cost > 0);
    if (isinf(surplus_cost)) {
        return 0;
    }
    assert(!isinf(wanted_by_abs));

    ap_float surplus_for_rest = surplus_cost + wanted_by_abs;
    if (surplus_for_rest >= 0) {
        return max((ap_float)0, wanted_by_abs - surplus_for_rest);
    } else {
        return max(wanted_by_abs, surplus_for_rest);
    }
}

ap_float compute_costs_stolen_by_heuristic(
    const vector<ap_float> &saturated_costs,
    const vector<ap_float> &surplus_costs) {
    assert(saturated_costs.size() == surplus_costs.size());
    int num_operators = surplus_costs.size();
    ap_float sum_stolen_costs = 0;
    for (int op_id = 0; op_id < num_operators; ++op_id) {
        ap_float stolen_costs = compute_stolen_costs(
            saturated_costs[op_id], surplus_costs[op_id]);
        assert(!isinf(stolen_costs) || stolen_costs > 0);
        sum_stolen_costs += stolen_costs;
    }
    return sum_stolen_costs;
}

static ap_float compute_surplus_costs(
    const vector<vector<ap_float>> &saturated_costs_by_abstraction,
    int op_id,
    ap_float remaining_costs) {
    int num_abstractions = saturated_costs_by_abstraction.size();
    ap_float sum_wanted = 0;
    for (int abs = 0; abs < num_abstractions; ++abs) {
        ap_float wanted = saturated_costs_by_abstraction[abs][op_id];
        if (isinf(wanted) && wanted < 0) {
            return INF;
        } else {
            sum_wanted += wanted;
        }
    }
    assert(!isinf(sum_wanted) || sum_wanted > 0);
    if (isinf(remaining_costs)) {
        return INF;
    }
    return remaining_costs - sum_wanted;
}

vector<ap_float> compute_all_surplus_costs(
    const vector<ap_float> &costs,
    const vector<vector<ap_float>> &saturated_costs_by_abstraction) {
    int num_operators = costs.size();
    vector<ap_float> surplus_costs;
    surplus_costs.reserve(num_operators);
    for (int op_id = 0; op_id < num_operators; ++op_id) {
        surplus_costs.push_back(
            compute_surplus_costs(saturated_costs_by_abstraction, op_id, costs[op_id]));
    }
    return surplus_costs;
}

double compute_score(ap_float h, ap_float used_costs, ScoringFunction scoring_function) {
    assert(h >= 0);
    assert(!isinf(used_costs));
    if (scoring_function == ScoringFunction::MAX_HEURISTIC) {
        return h;
    } else if (scoring_function == ScoringFunction::MIN_STOLEN_COSTS) {
        return -used_costs;
    } else if (scoring_function == ScoringFunction::MAX_HEURISTIC_PER_STOLEN_COSTS) {
        return static_cast<double>(h) / max((ap_float)1.0, used_costs);
    } else {
        ABORT("Invalid scoring_function");
    }
}

void add_scoring_function_to_parser(options::OptionParser &parser) {
    parser.add_enum_option(
        "scoring_function",
        {"MAX_HEURISTIC", "MIN_STOLEN_COSTS", "MAX_HEURISTIC_PER_STOLEN_COSTS"},
        "scoring function",
        "MAX_HEURISTIC_PER_STOLEN_COSTS");
}
}
