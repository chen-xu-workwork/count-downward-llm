#include "order_generator_greedy.h"

#include "abstraction.h"
#include "utils.h"

#include "../option_parser.h"
#include "../plugin.h"

#include "../utils/collections.h"
#include "../utils/logging.h"
#include "../utils/rng.h"

#include <cassert>
#include <unordered_set>

using namespace std;

namespace cost_saturation {
OrderGeneratorGreedy::OrderGeneratorGreedy(const options::Options &opts)
    : OrderGenerator(opts),
      scoring_function(static_cast<ScoringFunction>(opts.get<int>("scoring_function"))) {
}

double OrderGeneratorGreedy::rate_abstraction(
    const vector<int> &abstract_state_ids, int abs_id) const {
    assert(utils::in_bounds(abs_id, abstract_state_ids));
    int abstract_state_id = abstract_state_ids[abs_id];
    assert(utils::in_bounds(abs_id, h_values_by_abstraction));
    assert(utils::in_bounds(abstract_state_id, h_values_by_abstraction[abs_id]));
    ap_float h = h_values_by_abstraction[abs_id][abstract_state_id];
    assert(h >= 0);

    assert(utils::in_bounds(abs_id, stolen_costs_by_abstraction));
    ap_float stolen_costs = stolen_costs_by_abstraction[abs_id];

    return compute_score(h, stolen_costs, scoring_function);
}

void OrderGeneratorGreedy::initialize(
    const Abstractions &abstractions,
    const vector<ap_float> &costs) {
    utils::Timer timer;
    g_log << "Initialize greedy order generator" << endl;

    vector<vector<ap_float>> saturated_costs_by_abstraction;
    for (const unique_ptr<Abstraction> &abstraction : abstractions) {
        vector<ap_float> h_values = abstraction->compute_goal_distances(costs);
        vector<ap_float> saturated_costs = abstraction->compute_saturated_costs(h_values);
        //print h values and saturated costs 
        for (size_t i = 0; i < h_values.size(); ++i) {
            if (h_values[i] != 0 && h_values[i] != INF) {
                //cout << "Abstraction " << abstractions.size() << ", h_values[" << i << "] = " << h_values[i] << ", saturated_costs[" << i << "] = " << saturated_costs[i] << endl;
            }
        }
        h_values_by_abstraction.push_back(move(h_values));
        saturated_costs_by_abstraction.push_back(move(saturated_costs));
    }
    g_log << "Time for computing h values and saturated costs: "
          << timer << endl;

    vector<ap_float> surplus_costs = compute_all_surplus_costs(
        costs, saturated_costs_by_abstraction);
    g_log << "Done computing surplus costs" << endl;

    g_log << "Compute stolen costs" << endl;
    int num_abstractions = abstractions.size();
    for (int abs = 0; abs < num_abstractions; ++abs) {
        ap_float sum_stolen_costs = compute_costs_stolen_by_heuristic(
            saturated_costs_by_abstraction[abs], surplus_costs);
        stolen_costs_by_abstraction.push_back(sum_stolen_costs);
    }
    g_log << "Time for initializing greedy order generator: "
          << timer << endl;
}

Order OrderGeneratorGreedy::compute_order_for_state(
    const vector<int> &abstract_state_ids,
    bool verbose) {
    assert(abstract_state_ids.size() == h_values_by_abstraction.size());
    utils::Timer greedy_timer;
    int num_abstractions = abstract_state_ids.size();
    Order order = get_default_order(num_abstractions);
    // Shuffle order to break ties randomly.
    rng->shuffle(order);
    vector<double> scores;
    scores.reserve(num_abstractions);
    for (int abs = 0; abs < num_abstractions; ++abs) {
        scores.push_back(rate_abstraction(abstract_state_ids, abs));
    }
    sort(order.begin(), order.end(), [&](int abs1, int abs2) {
             return scores[abs1] > scores[abs2];
         });

    if (verbose) {
        unordered_set<double> unique_scores(scores.begin(), scores.end());
        cout << "Static greedy unique scores: " << unique_scores.size() << endl;
        cout << "Time for computing greedy order: " << greedy_timer << endl;
    }

    assert(order.size() == abstract_state_ids.size());
    return order;
}


static shared_ptr<OrderGenerator> _parse_greedy(options::OptionParser &parser) {
    parser.document_synopsis(
        "Greedy orders",
        "Order abstractions greedily by a given scoring function.");
    add_scoring_function_to_parser(parser);
    add_common_order_generator_options(parser);
    options::Options opts = parser.parse();
    if (parser.dry_run())
        return nullptr;
    else
        return make_shared<OrderGeneratorGreedy>(opts);
}

static PluginShared<OrderGenerator> _plugin_greedy("greedy_orders", _parse_greedy);
}
