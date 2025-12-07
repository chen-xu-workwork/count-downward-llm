#include "cost_partitioning_heuristic_collection_generator.h"

#include "cost_partitioning_heuristic.h"
#include "diversifier.h"
#include "order_generator.h"
#include "order_optimizer.h"
#include "utils.h"

#include "../task_proxy.h"

#include "../sampling.h"
#include "../task_tools.h"
#include "../successor_generator.h"
#include "../utils/collections.h"
#include "../utils/countdown_timer.h"
#include "../utils/logging.h"
#include "../utils/memory.h"

#include <cassert>
#include <cmath>

using namespace std;

namespace cost_saturation {

static vector<vector<int>> convert_samples_to_abstract_ids(
    const Abstractions &abstractions,
    const vector<State> &samples) {
    vector<vector<int>> abstract_state_ids;
    abstract_state_ids.reserve(samples.size());
    for (const State &state : samples) {
        abstract_state_ids.push_back(get_abstract_state_ids(abstractions, state));
    }
    return abstract_state_ids;
}

CostPartitioningHeuristicCollectionGenerator::CostPartitioningHeuristicCollectionGenerator(
    const shared_ptr<OrderGenerator> &order_generator,
    int max_orders,
    int max_size_kb,
    double max_time,
    bool diversify,
    int num_samples,
    double max_optimization_time,
    const shared_ptr<utils::RandomNumberGenerator> &rng)
    : order_generator(order_generator),
      max_orders(max_orders),
      max_size_kb(max_size_kb),
      max_time(max_time),
      diversify(diversify),
      num_samples(num_samples),
      max_optimization_time(max_optimization_time),
      rng(rng) {
}

vector<CostPartitioningHeuristic>
CostPartitioningHeuristicCollectionGenerator::generate_cost_partitionings(
    const shared_ptr<AbstractTask> &task,
    const TaskProxy &task_proxy,
    const Abstractions &abstractions,
    const vector<ap_float> &costs,
    const CPFunction &cp_function) const {
    utils::CountdownTimer timer(max_time);

    State initial_state = task_proxy.get_initial_state();
    SuccessorGenerator successor_generator(task);

    order_generator->initialize(abstractions, costs);

    vector<int> abstract_state_ids_for_init = get_abstract_state_ids(
        abstractions, initial_state);
    Order order_for_init = order_generator->compute_order_for_state(
        abstract_state_ids_for_init, true);
    vector<ap_float> remaining_costs = costs;
    //print remaining costs


    CostPartitioningHeuristic cp_for_init = cp_function(
        abstractions, order_for_init, remaining_costs, abstract_state_ids_for_init);
    ap_float init_h = cp_for_init.compute_heuristic(abstract_state_ids_for_init);

    if (isinf(init_h)) {
        g_log << "Initial state is unsolvable." << endl;
        return {
                   cp_for_init
        };
    }

    function<bool(State)> is_dead_end =
        [&abstractions, &cp_for_init](const State &state) {
            return isinf(cp_for_init.compute_heuristic(
                get_abstract_state_ids(abstractions, state)));
        };

    ap_float avg_cost = get_average_operator_cost(task_proxy);

    unique_ptr<Diversifier> diversifier;
    if (diversify) {
        double max_sampling_time = timer.get_remaining_time();
        utils::CountdownTimer sampling_timer(max_sampling_time);
        g_log << "Start sampling" << endl;
        
        vector<State> samples = sample_states_with_random_walks(
            task_proxy, successor_generator, num_samples, init_h, avg_cost, is_dead_end, &sampling_timer);
            
        g_log << "Samples: " << samples.size() << endl;
        g_log << "Sampling time: " << sampling_timer.get_elapsed_time() << endl;

        diversifier = utils::make_unique_ptr<Diversifier>(
            convert_samples_to_abstract_ids(abstractions, samples));
    }

    g_log << "Start computing cost partitionings" << endl;
    vector<CostPartitioningHeuristic> cp_heuristics;
    int evaluated_orders = 0;
    int size_kb = 0;
    while (static_cast<int>(cp_heuristics.size()) < max_orders &&
           (!timer.is_expired() || cp_heuristics.empty()) &&
           (size_kb < max_size_kb)) {
        bool is_first_order = (evaluated_orders == 0);

        vector<int> abstract_state_ids;
        Order order;
        CostPartitioningHeuristic cp_heuristic;
        if (is_first_order) {
            // Use initial state as first sample.
            abstract_state_ids = abstract_state_ids_for_init;
            order = order_for_init;
            cp_heuristic = cp_for_init;
        } else {
            vector<State> sample = sample_states_with_random_walks(
                task_proxy, successor_generator, 1, init_h, avg_cost, is_dead_end);
            
            if (sample.empty()) {
                // Fallback to initial state if sampling failed (e.g. dead ends)
                abstract_state_ids = abstract_state_ids_for_init;
            } else {
                abstract_state_ids = get_abstract_state_ids(abstractions, sample[0]);
            }

            order = order_generator->compute_order_for_state(
                abstract_state_ids, false);
            remaining_costs = costs;
            cp_heuristic = cp_function(abstractions, order, remaining_costs, abstract_state_ids);
        }

        // Optimize order.
        double optimization_time = min(
            static_cast<double>(timer.get_remaining_time()), max_optimization_time);
        if (optimization_time > 0) {
            utils::CountdownTimer opt_timer(optimization_time);
            ap_float incumbent_h_value = cp_heuristic.compute_heuristic(abstract_state_ids);
            optimize_order_with_hill_climbing(
                cp_function, opt_timer, abstractions, costs, abstract_state_ids, order,
                cp_heuristic, incumbent_h_value, is_first_order);
            if (is_first_order) {
                g_log << "Time for optimizing order: " << opt_timer.get_elapsed_time()
                    << endl;
            }
        }

        // If diversify=true, only add order if it improves upon previously
        // added orders.
        if (!diversifier || diversifier->is_diverse(cp_heuristic)) {
            size_kb += cp_heuristic.estimate_size_in_kb();
            cp_heuristics.push_back(move(cp_heuristic));
            if (diversifier) {
                g_log << "Average finite h-value for " << num_samples
                    << " samples after " << timer.get_elapsed_time()
                    << " of diversification: "
                    << diversifier->compute_avg_finite_sample_h_value()
                    << endl;
            }
        }

        ++evaluated_orders;
    }

    g_log << "Evaluated orders: " << evaluated_orders << endl;
    g_log << "Cost partitionings: " << cp_heuristics.size() << endl;
    g_log << "Time for computing cost partitionings: " << timer.get_elapsed_time()
        << endl;
    g_log << "Estimated heuristic size: " << size_kb << " KiB" << endl;
    return cp_heuristics;
}
}
