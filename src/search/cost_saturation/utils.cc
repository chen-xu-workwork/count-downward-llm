#include "utils.h"

#include "abstraction.h"
#include "abstraction_generator.h"
#include "cost_partitioning_heuristic.h"
#include "cost_partitioning_heuristic_collection_generator.h"
#include "max_cost_partitioning_heuristic.h"

#include "../option_parser.h"

#include "../algorithms/partial_state_tree.h"
#include "../cegar/utils.h"
#include "../utils/collections.h"
#include "../utils/logging.h"
#include "../utils/rng.h"
#include "../utils/rng_options.h"

#include <cassert>
#include <cmath>
#include <numeric>

using namespace std;

namespace cost_saturation {
Abstractions generate_abstractions(
    const shared_ptr<AbstractTask> &task,
    const vector<shared_ptr<AbstractionGenerator>> &abstraction_generators,
    DeadEnds *dead_ends) {
    Abstractions abstractions;
    vector<int> abstractions_per_generator;
    for (const shared_ptr<AbstractionGenerator> &generator : abstraction_generators) {
        int abstractions_before = abstractions.size();
        for (auto &abstraction : generator->generate_abstractions(task, dead_ends)) {
            abstractions.push_back(move(abstraction));
        }
        abstractions_per_generator.push_back(abstractions.size() - abstractions_before);
    }
    g_log << "Abstractions: " << abstractions.size() << endl;
    g_log << "Abstractions per generator: " << abstractions_per_generator << endl;
    return abstractions;
}

Order get_default_order(int num_abstractions) {
    vector<int> indices(num_abstractions);
    iota(indices.begin(), indices.end(), 0);
    return indices;
}

bool is_sum_within_range(ap_float a, ap_float b) {
    // For floating point, we mainly need to check for NaN
    return !isnan(a) && !isnan(b);
}

ap_float left_addition(ap_float a, ap_float b) {
    if (isinf(a)) {
        return a;
    } else if (isinf(b)) {
        return b;
    } else {
        assert(is_sum_within_range(a, b));
        return a + b;
    }
}

ap_float compute_max_h(
    const CPHeuristics &cp_heuristics,
    const vector<int> &abstract_state_ids,
    vector<int> *num_best_order) {
    ap_float max_h = 0;
    int best_id = -1;
    int current_id = 0;
    for (const CostPartitioningHeuristic &cp_heuristic : cp_heuristics) {
        ap_float sum_h = cp_heuristic.compute_heuristic(abstract_state_ids);
        if (sum_h > max_h) {
            max_h = sum_h;
            best_id = current_id;
        }
        if (isinf(max_h)) {
            break;
        }
        ++current_id;
    }
    assert(max_h >= 0);

    if (num_best_order) {
        num_best_order->resize(cp_heuristics.size(), 0);
        if (best_id != -1) {
            ++(*num_best_order)[best_id];
        }
    }

    return max_h;
}

void reduce_costs(vector<ap_float> &remaining_costs, const vector<ap_float> &saturated_costs) {
    assert(remaining_costs.size() == saturated_costs.size());
    for (size_t i = 0; i < remaining_costs.size(); ++i) {
        ap_float &remaining = remaining_costs[i];
        ap_float saturated = saturated_costs[i];

        //HAcky fix due to double precision issues when subtracting very small values from very big values
        if (!(isinf(remaining) && isinf(saturated))) {
            saturated = min(remaining, saturated);
        }

        assert(remaining >= 0);
        assert(saturated <= remaining || (isinf(remaining) && isinf(saturated)));
        if (isinf(remaining)) {
            // Left addition: x - y = x for all values y if x is infinite.
        } else if (isinf(saturated) && saturated < 0) {
            remaining = INF;
        } else {
            assert(!isinf(saturated) || saturated > 0);
            remaining -= saturated;
        }
        assert(remaining >= 0);
    }
}

std::vector<ap_float> get_operator_costs(const TaskProxy &task_proxy) {
    std::vector<ap_float> costs;
    costs.reserve(task_proxy.get_operators().size());
    for (OperatorProxy op : task_proxy.get_operators()) {
        costs.push_back(op.get_cost());
    }
    return costs;
}

void add_order_options_to_parser(options::OptionParser &parser) {
    parser.add_option<shared_ptr<OrderGenerator>>(
        "orders",
        "order generator",
        "greedy_orders()");
    parser.add_option<int>(
        "max_orders",
        "maximum number of orders",
        "infinity",
        options::Bounds("0", "infinity"));
    parser.add_option<int>(
        "max_size",
        "maximum heuristic size in KiB",
        "infinity",
        options::Bounds("0", "infinity"));
    parser.add_option<double>(
        "max_time",
        "maximum time in seconds for finding orders",
        "200",
        options::Bounds("0", "infinity"));
    parser.add_option<bool>(
        "diversify",
        "only keep orders that have a higher heuristic value than all previous "
        "orders for any of the samples",
        "true");
    parser.add_option<int>(
        "samples",
        "number of samples for diversification",
        "1000",
        options::Bounds("1", "infinity"));
    parser.add_option<double>(
        "max_optimization_time",
        "maximum time in seconds for optimizing each order with hill climbing",
        "2",
        options::Bounds("0", "infinity"));
    utils::add_rng_options(parser);
}

CostPartitioningHeuristicCollectionGenerator
get_cp_heuristic_collection_generator_from_options(const options::Options &opts) {
    return CostPartitioningHeuristicCollectionGenerator(
        opts.get<shared_ptr<OrderGenerator>>("orders"),
        opts.get<int>("max_orders"),
        opts.get<int>("max_size"),
        opts.get<double>("max_time"),
        opts.get<bool>("diversify"),
        opts.get<int>("samples"),
        opts.get<double>("max_optimization_time"),
        utils::parse_rng_from_options(opts));
}

void prepare_parser_for_cost_partitioning_heuristic(
    options::OptionParser &parser, bool consistent) {
    parser.document_language_support("action costs", "supported");
    parser.document_language_support(
        "conditional effects",
        "not supported (the heuristic supports them in theory, but none of "
        "the currently implemented abstraction generators do)");
    parser.document_language_support(
        "axioms",
        "not supported (the heuristic supports them in theory, but none of "
        "the currently implemented abstraction generators do)");
    parser.document_property("admissible", "yes");
    parser.document_property("consistent", consistent ? "yes" : "no");
    parser.document_property("safe", "yes");
    parser.document_property("preferred operators", "no");

    parser.add_list_option<shared_ptr<AbstractionGenerator>>(
        "abstractions",
        "abstraction generators",
        "[projections(hillclimbing(max_time=60)), "
        "projections(systematic(2)), "
        "cartesian()]");
    Heuristic::add_options_to_parser(parser);
}


shared_ptr<ScalarEvaluator> get_max_cp_heuristic(options::OptionParser &parser, const CPFunction &cp_function) {
    prepare_parser_for_cost_partitioning_heuristic(parser);
    add_order_options_to_parser(parser);
    Heuristic::add_options_to_parser(parser);

    options::Options opts = parser.parse();
    if (parser.help_mode())
        return nullptr;

    if (parser.dry_run())
        return nullptr;

    shared_ptr<AbstractTask> task = get_task_from_options(opts);
    TaskProxy task_proxy(*task);
    vector<int> int_costs = cegar::get_operator_costs(task_proxy);
    vector<ap_float> costs(int_costs.begin(), int_costs.end());
    unique_ptr<DeadEnds> dead_ends = utils::make_unique_ptr<DeadEnds>();
    Abstractions abstractions = generate_abstractions(
        task, opts.get_list<shared_ptr<AbstractionGenerator>>("abstractions"), dead_ends.get());
    vector<CostPartitioningHeuristic> cp_heuristics =
        get_cp_heuristic_collection_generator_from_options(opts).generate_cost_partitionings(
            task, task_proxy, abstractions, costs, cp_function);
    return make_shared<MaxCostPartitioningHeuristic>(
        opts,
        move(abstractions),
        move(cp_heuristics),
        move(dead_ends));
}
}
