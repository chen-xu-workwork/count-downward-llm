#include "cegar.h"

#include "cegar_logger.h"
#include "domain_abstraction.h"
#include "domain_abstraction_factory.h"

#include "../axioms.h"
#include "utils.h"

#include <variant>
#include <cmath>
#include <optional>
#include "../globals.h"
#include "../option_parser.h"
#include "../task_proxy.h"

#include "../utils/countdown_timer.h"
#include "../utils/logging.h"
#include "../utils/rng.h"
#include "../utils/rng_options.h"

#include "../utils/markup.h"
#include "../utils/math.h"
#include "../utils/memory.h"
#include "../utils/system.h"
#include "../utils/timer.h"
#include "../utils/collections.h"

using namespace std;

namespace domain_abstractions {
static const int memory_padding_in_mb = 75;

static Verbosity log_verbosity = Verbosity::NONE;

class CEGAR {

    using NumericFlaw = std::tuple<int, ap_float, bool>;
    using PropFlaw = std::pair<Fact, std::vector<NumericFlaw>>;
    using Flaw = std::variant<PropFlaw, NumericFlaw>;
private:
    
    std::vector<int> local_to_global_regular_numeric_var_ids;
    std::vector<int> global_to_local_regular_numeric_var_ids; // NOTE: Not used yet(?)
    std::vector<std::unordered_set<ap_float>> already_split;
    mutable std::vector<std::unordered_set<ap_float>> regular_numeric_var_values;
    
    const int max_abstraction_size;
    const double max_time;
    const bool use_wildcard_plans;
    const bool deviation_flaws;
    const ExecEntirePlanMode exec_entire_plan;
    const FlawTreatment flaw_treatment;
    const InitSplitMethod init_split_method;
    const NumericSplitStrategy numeric_split_strategy;
    const shared_ptr<utils::RandomNumberGenerator> &rng;
    const std::unordered_set<int> init_split_var_ids;
    std::unordered_set<int> blacklisted_variables;
    
    shared_ptr<CEGARLogger> logger;

    std::vector<int> abstract_domain_sizes;
    std::vector<int> real_domain_sizes;
    
    NumericDomainMappingType numeric_domain_mapping;
    std::vector<int> numeric_domain_sizes;
    std::unordered_set<int> blacklisted_numeric_variables;
    
    mutable std::unordered_map<int, std::unordered_map<int, ap_float>>
        last_concrete_values_by_prop_var;
    
    std::unordered_map<int, std::unordered_set<int>> comparison_axiom_dependencies;
    std::unordered_set<int> comparison_axiom_var_ids;

    DomainMapping compute_initial_domain_mapping(const TaskProxy &task_proxy);
    vector<int> compute_initial_split(
        int var_id, const TaskProxy &task_proxy, int &abstraction_size);
    bool initialization_fits_size_limit(int abstraction_size, int var_id);
    pair<int, vector<int>> get_goal_value_split(
        int var_id, const TaskProxy &task_proxy);
    pair<int, vector<int>> get_init_value_split(
        int var_id, const TaskProxy &task_proxy);
    pair<int, vector<int>> get_random_value_split(
        int var_id, const TaskProxy &task_proxy);
    pair<int, vector<int>> get_identity_split(
        int var_id, const TaskProxy &task_proxy);
    pair<int, vector<int>> get_random_partition_split(
        int var_id, const TaskProxy &task_proxy, int abstraction_size);
    pair<int, vector<int>> get_random_init_goal_partition_split(
        int var_id, const TaskProxy &task_proxy);

    bool termination_criterion_satisfied(utils::CountdownTimer &timer);

    std::vector<Flaw> get_flaws(const TaskProxy &task_proxy,
                                const State &concrete_init,
                                const DomainAbstraction &abstraction,
                                bool execute_entire_plan) const;
    bool fix_flaws(std::vector<Flaw> &&flaws,
                DomainMapping &domain_mapping, int abstraction_size, NumericDomainMappingType &numeric_domain_mapping);
    bool fix_single_random_flaw(std::vector<Flaw> &&flaws,
                            DomainMapping &domain_mapping,
                            int abstraction_size, NumericDomainMappingType &numeric_domain_mapping  );
    bool fix_single_flaw_max_refined(
        vector<Flaw> &&flaws, DomainMapping &domain_mapping,
        int abstraction_size, NumericDomainMappingType &numeric_domain_mapping);
    bool fix_flaws_per_atom(std::vector<Flaw> &&flaws,
                        DomainMapping &domain_mapping,
                        int abstraction_size, NumericDomainMappingType &numeric_domain_mapping);
    bool fix_flaws_per_variable(std::vector<Flaw> &&flaws,
                            DomainMapping &domain_mapping,
                            int abstraction_size, NumericDomainMappingType &numeric_domain_mapping);

    bool can_refine_variable(int old_abstraction_size, int var_id);
    bool can_refine_numeric_variable(int old_abstraction_size, int numeric_var_id, const TaskProxy &task_proxy);

    void add_variable_to_abstraction_if_necessary(
        int var, DomainMapping &abstraction);

    void print_statistics(const TaskProxy &task_proxy, const DomainMapping &domain_mapping);
    
    NumericDomainMappingType compute_initial_numeric_domain_mapping(
        const TaskProxy &task_proxy);
    
    
    // Check if a propositional variable is derived from a comparison axiom
    bool is_comparison_axiom_variable(int var_id) const {
        return comparison_axiom_dependencies.count(var_id) > 0;
    }
    
    // Determine split direction for numeric refinement
    bool determine_include_in_lower(
        int prop_var_id,
        int split_var_id,
        ap_float split_value,
        const std::vector<ap_float> &concrete_values,
        const TaskProxy &task_proxy) const;

    std::optional<ap_float> choose_unsplit_value(
        int numeric_var_id, int local_idx, ap_float concrete_value) const;

    std::vector<Flaw> get_precondition_flaws(
        const OperatorProxy &op,
        const std::vector<int> &current_state,
        const std::vector<ap_float> &numeric_state,
        const TaskProxy &task_proxy) const;
    std::vector<Flaw> get_deviation_flaws(
        const std::vector<int> &successor_state,
        const std::vector<ap_float> &numeric_successor_state,
        const std::vector<int> &abstract_successor_state,
        const std::vector<int> &abstract_numeric_successor_state,
        const DomainMapping &domain_mapping,
        const NumericDomainMappingType &numeric_domain_mapping,
        const TaskProxy &task_proxy) const;
    std::vector<Flaw> get_goal_flaws(
        const TaskProxy &task_proxy,
        const std::vector<int> &current_state,
        const std::vector<ap_float> &numeric_state) const;
public:
                CEGAR(int max_abstraction_size,
                    double max_time,
                    bool use_wildcard_plans,
                    bool deviation_flaws,
                        ExecEntirePlanMode exec_entire_plan,
                    FlawTreatment flaw_treatment,
                    InitSplitMethod init_split_method,
                    NumericSplitStrategy numeric_split_strategy,
                    const shared_ptr<utils::RandomNumberGenerator> &rng,
                    const TaskProxy &task_proxy,
                    unordered_set<int> &&init_split_var_ids,
                    unordered_set<int> &&blacklisted_variables,
                    unordered_set<int> &&blacklisted_numeric_variables);

    DomainAbstraction build_abstraction(const TaskProxy &task_proxy);
    void build_comparison_axiom_mapping(const TaskProxy &task_proxy);

};

CEGAR::CEGAR(
                int max_abstraction_size,
                double max_time,
                bool use_wildcard_plans,
                bool deviation_flaws,
                ExecEntirePlanMode exec_entire_plan,
                FlawTreatment flaw_treatment,
                InitSplitMethod init_split_method,
                NumericSplitStrategy numeric_split_strategy,
                const shared_ptr<utils::RandomNumberGenerator> &rng,
                const TaskProxy &task_proxy,
                unordered_set<int> &&init_split_var_ids,
                unordered_set<int> &&blacklisted_variables,
                unordered_set<int> &&blacklisted_numeric_variables)
        : max_abstraction_size(max_abstraction_size),
            max_time(max_time),
            use_wildcard_plans(use_wildcard_plans),
            deviation_flaws(deviation_flaws),
        exec_entire_plan(exec_entire_plan),
            flaw_treatment(flaw_treatment),
            init_split_method(init_split_method),
            numeric_split_strategy(numeric_split_strategy),
            rng(rng),
            init_split_var_ids(move(init_split_var_ids)),
            blacklisted_variables(move(blacklisted_variables)),
            blacklisted_numeric_variables(move(blacklisted_numeric_variables)),
            logger(make_shared<CEGARLogger>(log_verbosity)) {
    /* TODO: Should we check somewhere that *init_split_var_ids* does not
        contain elements that are blacklisted? */

    global_to_local_regular_numeric_var_ids.resize(
        task_proxy.get_numeric_variables().size(), -1);

    for (size_t i = 0; i < task_proxy.get_numeric_variables().size(); ++i) {
        NumericVariableProxy num_var = task_proxy.get_numeric_variables()[i];
        if (num_var.get_var_type() == numType::regular || num_var.get_var_type() == numType::constant) {
            local_to_global_regular_numeric_var_ids.push_back(i);
            already_split.push_back(std::unordered_set<ap_float>());
            global_to_local_regular_numeric_var_ids[i] =
                static_cast<int>(local_to_global_regular_numeric_var_ids.size()) - 1;
        }
    }
}

DomainMapping CEGAR::compute_initial_domain_mapping(
    const TaskProxy &task_proxy) {
    const int num_variables = task_proxy.get_variables().size();
    abstract_domain_sizes.resize(num_variables, 1);
    real_domain_sizes.reserve(num_variables);
    for (int i = 0; i < num_variables; ++i) {
        real_domain_sizes.push_back(
            task_proxy.get_variables()[i].get_domain_size());
    }

    DomainMapping domain_mapping(num_variables);
    if (!init_split_var_ids.empty()) {
        // Separate propositional and numeric var IDs
        // Numeric vars are encoded as num_prop_vars + numeric_var_id
        vector<int> prop_split_vars;
        for (int var_id : init_split_var_ids) {
            if (var_id < num_variables) {
                prop_split_vars.push_back(var_id);
            }
            // Numeric var IDs (>= num_variables) are handled in compute_initial_numeric_domain_mapping
        }
        
        rng->shuffle(prop_split_vars);
        int abstraction_size = 1;
        for (const int var_id : prop_split_vars) {
            if (blacklisted_variables.count(var_id) == 0) {
                domain_mapping[var_id] = compute_initial_split(
                    var_id, task_proxy, abstraction_size);
            }
        }
    }
    return domain_mapping;
}

#ifndef NDEBUG
static bool variable_specified_in_goal(int var_id,
                                       const TaskProxy &task_proxy) {
    bool is_goal = false;
    for (const FactProxy &goal: task_proxy.get_goals()) {
        if (goal.get_variable().get_id() == var_id) {
            is_goal = true;
            break;
        }
    }
    return is_goal;
}
#endif

vector<int> CEGAR::compute_initial_split(
    int var_id, const TaskProxy &task_proxy, int &abstraction_size) {
    logger->log_no_endl(Verbosity::DEBUG, "Initial split for variable ", var_id, ": with initialization method ");
    switch (init_split_method) {
    case InitSplitMethod::GOAL_VALUE:
        logger->log(Verbosity::DEBUG, "GOAL_VALUE");
        assert(variable_specified_in_goal(var_id, task_proxy));
        break;
    case InitSplitMethod::GOAL_VALUE_OR_RANDOM_IF_NON_GOAL:
        logger->log(Verbosity::DEBUG, "GOAL_VALUE_OR_RANDOM_IF_NON_GOAL");
        break;
    case InitSplitMethod::INIT_VALUE:
        logger->log(Verbosity::DEBUG, "INIT_VALUE");
        break;
    case InitSplitMethod::RANDOM_VALUE: 
        logger->log(Verbosity::DEBUG, "RANDOM_VALUE");
        break;
    case InitSplitMethod::RANDOM_PARTITION:
        logger->log(Verbosity::DEBUG, "RANDOM_PARTITION");
        break;
    case InitSplitMethod::RANDOM_BINARY_PARTITION_SEPARATING_INIT_GOAL:
        logger->log(Verbosity::DEBUG, "RANDOM_BINARY_PARTITION_SEPARATING_INIT_GOAL");
        break;
    case InitSplitMethod::IDENTITY:
        logger->log(Verbosity::DEBUG, "IDENTITY");
        break;
    }
    logger->log(Verbosity::DEBUG, "");
    if (init_split_method == InitSplitMethod::RANDOM_PARTITION
        || initialization_fits_size_limit(abstraction_size, var_id)) {
        pair<int, vector<int>> init_split;
        switch (init_split_method) {
        case InitSplitMethod::GOAL_VALUE:
            assert(variable_specified_in_goal(var_id, task_proxy));
        case InitSplitMethod::GOAL_VALUE_OR_RANDOM_IF_NON_GOAL:
            init_split = get_goal_value_split(var_id, task_proxy);
            break;
        case InitSplitMethod::INIT_VALUE:
            init_split = get_init_value_split(var_id, task_proxy);
            break;
        case InitSplitMethod::RANDOM_VALUE:
            init_split = get_random_value_split(var_id, task_proxy);
            break;
        case InitSplitMethod::IDENTITY:
            init_split = get_identity_split(var_id, task_proxy);
            break;
        case InitSplitMethod::RANDOM_PARTITION:
            init_split = get_random_partition_split(var_id, task_proxy, abstraction_size);
            break;
        case InitSplitMethod::RANDOM_BINARY_PARTITION_SEPARATING_INIT_GOAL:
            init_split = get_random_init_goal_partition_split(
                var_id, task_proxy);
            break;
        }
        abstract_domain_sizes[var_id] = init_split.first;
        abstraction_size *= init_split.first;
        return init_split.second;
    }
    return {};
}

bool CEGAR::initialization_fits_size_limit(int old_abstraction_size,
                                           int var_id) {
    int split_domain_size = -1;
    switch (init_split_method) {
    case InitSplitMethod::GOAL_VALUE:
    case InitSplitMethod::INIT_VALUE:
    case InitSplitMethod::RANDOM_VALUE:
    case InitSplitMethod::GOAL_VALUE_OR_RANDOM_IF_NON_GOAL:
    case InitSplitMethod::RANDOM_BINARY_PARTITION_SEPARATING_INIT_GOAL:
        split_domain_size = 2;
        break;
    case InitSplitMethod::IDENTITY:
        //TODO: Add this back in
        //assert(utils::in_bounds(var_id, real_domain_sizes));
        split_domain_size = real_domain_sizes[var_id];
        break;
    case InitSplitMethod::RANDOM_PARTITION:
        assert(false);
        break;
    }
    assert(split_domain_size > 0);
    if (utils::is_product_within_limit(
        old_abstraction_size, split_domain_size, max_abstraction_size)) {
        return true;
    }
    // if (log.is_at_least_debug()) {
    //     log << "Initial split for variable " << var_id
    //         << " exceeds size limit." << endl;
    // }
    // Do not blacklist because we might still be able to fix single flaws.
    //blacklisted_variables.insert(var_id);
    return false;
}

pair<int, vector<int>> CEGAR::get_goal_value_split(
    int var_id, const TaskProxy &task_proxy) {
    vector<int> init_split(
        task_proxy.get_variables()[var_id].get_domain_size(), 0);

    int goal_value = -1;
    for (FactProxy goal : task_proxy.get_goals()) {
        if (goal.get_variable().get_id() == var_id) {
            goal_value = goal.get_value();
            break;
        }
    }

    // we cannot choose the axiom goal, prevented before. Otherwise goal axiom has only comparisons as precondition, so we are fine. 
    if (goal_value < 0) {
        // Variable not relevant in goal.
        return get_random_value_split(var_id, task_proxy);
    } else {
        init_split[goal_value] = 1;
        return make_pair(2, move(init_split));
    }
}

pair<int, vector<int>> CEGAR::get_init_value_split(
    int var_id, const TaskProxy &task_proxy) {
    vector<int> init_split(
        task_proxy.get_variables()[var_id].get_domain_size(), 0);

    State init_state = task_proxy.get_initial_state();

    int init_value = task_proxy.get_initial_state()[var_id].get_value();
    logger->log(Verbosity::DEBUG, "init value ", init_value);
    assert(utils::in_bounds(init_value, init_split));
    //TODO: Do the same for numeric variables
    init_split[init_value] = 1;
    return make_pair(2, move(init_split));
}

pair<int, vector<int>> CEGAR::get_random_value_split(
    int var_id, const TaskProxy &task_proxy) {
    int domain_size = task_proxy.get_variables()[var_id].get_domain_size();
    
    // For comparison axiom variables, always split at value 0 (true) instead of random
    if (comparison_axiom_var_ids.count(var_id) > 0) {
        logger->log(Verbosity::DEBUG, "  Variable ", var_id, " is a comparison axiom - splitting at value 0 instead of random");
        vector<int> init_split(domain_size, 0);
        if (domain_size > 1) {
            init_split[0] = 1;  // Value 0 (true) goes to partition 1
        }
        return make_pair(min(2, domain_size), move(init_split));
    }
    
    int split_val = rng->random(domain_size);
    vector<int> init_split(domain_size, 0);
    init_split[split_val] = 1;
    return make_pair(2, move(init_split));
}

pair<int, vector<int>> CEGAR::get_identity_split(
    int var_id, const TaskProxy &task_proxy) {
    int domain_size = task_proxy.get_variables()[var_id].get_domain_size();
    
    // For comparison axiom variables, a "full" identity split doesn't make sense 
    // since they're binary (true/false). Instead, split at value 0 (true in numeric fd).
    if (comparison_axiom_var_ids.count(var_id) > 0) {
        logger->log(Verbosity::DEBUG, "  Variable ", var_id, " is a comparison axiom - using binary split at value 0");
        vector<int> init_split(domain_size, 0);
        if (domain_size > 1) {
            init_split[0] = 1;  // Value 0 (true) goes to partition 1
        }
        return make_pair(min(2, domain_size), move(init_split));
    }
    
    // Standard identity split: each value maps to its own partition
    vector<int> init_split(domain_size);
    for (int i = 0; i < domain_size; ++i) {
        init_split[i] = i;
    }
    return make_pair(domain_size, move(init_split));
}

pair<int, vector<int>> CEGAR::get_random_partition_split(
    int var_id, const TaskProxy &task_proxy, int abstraction_size) {
    int domain_size = task_proxy.get_variables()[var_id].get_domain_size();
    int abstract_domain_size = 0;
    vector<int> init_split(domain_size);
    vector<int> value_map(domain_size, -1);
    for (int i = 0; i < domain_size; ++i) {
        int r = rng->random(domain_size);
        if (value_map[r] == -1) {
            value_map[r] = abstract_domain_size++;
        }
        init_split[i] = value_map[r];
    }
    assert(abstract_domain_size > 0);
    if (abstract_domain_size > 1) {
        if (abstract_domain_size > 1 && utils::is_product_within_limit(
            abstraction_size, abstract_domain_size, max_abstraction_size)) {
            return make_pair(abstract_domain_size, move(init_split));
        } else {
            logger->log(Verbosity::DEBUG, "Initial split for variable ", var_id, " exceeds size limit.");
        }
    }
    return make_pair(1, vector<int>{});
}

pair<int, vector<int>> CEGAR::get_random_init_goal_partition_split(
    int var_id, const TaskProxy &task_proxy) {
    int domain_size = task_proxy.get_variables()[var_id].get_domain_size();
    vector<int> init_split(domain_size);
    for (int i = 0; i < domain_size; ++i) {
        init_split[i] = rng->random(2);
    }

    /*
      If a goal value is specified for the given variable, and if it is
      different from the initial value, make sure both values end up in
      different partitions.
    */
    for (FactProxy goal : task_proxy.get_goals()) {
        if (goal.get_variable().get_id() == var_id) {
            int init_val =
                task_proxy.get_initial_state()[var_id].get_value();
            int goal_val = goal.get_value();
            if (init_val != goal_val) {
                init_split[init_val] = 0;
                init_split[goal_val] = 1;
            }
        }
    }

    int first_val = init_split[0];
    for (int i = 1; i < domain_size; ++i) {
        if (init_split[i] != first_val) {
            return make_pair(2, move(init_split));
        }
    }
    // Partition resulted in trivial variable.
    return make_pair(1, vector<int>{});
}

//TODO: Get rid of that. With deviation flaws, we always want the "latest" value
std::optional<ap_float> CEGAR::choose_unsplit_value(
    int numeric_var_id, int local_idx, ap_float concrete_value) const {
    if (local_idx < 0 || local_idx >= static_cast<int>(already_split.size())) {
        return std::nullopt;
    }
    if (already_split[local_idx].count(concrete_value) == 0) {
        return concrete_value;
    }
    if (local_idx >= static_cast<int>(regular_numeric_var_values.size())) {
        return std::nullopt;
    }
    for (ap_float val : regular_numeric_var_values[local_idx]) {
        if (already_split[local_idx].count(val) == 0) {
            return val;
        }
    }
    if (numeric_var_id < 0 ||
        numeric_var_id >= static_cast<int>(numeric_domain_mapping.size())) {
        return std::nullopt;
    }
    const auto &ranges = numeric_domain_mapping[numeric_var_id]->get_ranges();
    for (const NumericRange &range : ranges) {
        if (!range.contains(concrete_value)) {
            continue;
        }
        ap_float lower = range.lower;
        ap_float upper = range.upper;
        bool has_lower = lower != -numeric_limits<ap_float>::infinity();
        bool has_upper = upper != numeric_limits<ap_float>::infinity();
        if (has_lower && has_upper && lower == upper) {
            return std::nullopt;
        }
        ap_float candidate = concrete_value;
        if (has_lower && has_upper) {
            candidate = (lower + upper) / 2;
        } else if (has_lower) {
            candidate = lower + 1;
        } else if (has_upper) {
            candidate = upper - 1;
        }
        if (has_lower && candidate <= lower) {
            return std::nullopt;
        }
        if (has_upper && candidate >= upper) {
            return std::nullopt;
        }
        if (already_split[local_idx].count(candidate) == 0) {
            return candidate;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

vector<CEGAR::Flaw> CEGAR::get_precondition_flaws(
    const OperatorProxy &op, const vector<int> &current_state,
    const vector<ap_float> &numeric_state, const TaskProxy &task_proxy) const {
    vector<Flaw> flaws;
    for (FactProxy pre : op.get_preconditions()) {
        int var_id = pre.get_variable().get_id();
        if (blacklisted_variables.count(var_id) == 0
            && current_state[var_id] != pre.get_value()) {
            if (is_comparison_axiom_variable(var_id)) {
                auto it = comparison_axiom_dependencies.find(var_id);
                assert(it != comparison_axiom_dependencies.end());
                vector<NumericFlaw> numeric_flaws;
                for (int dep_var_id : it->second) {
                    ap_float concrete_value = numeric_state[dep_var_id];
                    bool is_lower = determine_include_in_lower(var_id, dep_var_id, concrete_value, numeric_state, task_proxy);
                    NumericFlaw numeric_flaw{dep_var_id, concrete_value, is_lower};
                    numeric_flaws.push_back(numeric_flaw);
                }

                flaws.emplace_back(
                    std::in_place_type<PropFlaw>,
                    Fact(var_id, pre.get_value()),
                    move(numeric_flaws)
                );
            } else {
                flaws.emplace_back(
                    std::in_place_type<PropFlaw>,
                    Fact(var_id, pre.get_value()),
                    std::vector<NumericFlaw>{}
                );
            }
        }
    }
    return flaws;
}

vector<CEGAR::Flaw> CEGAR::get_deviation_flaws(
    const vector<int> &successor_state, const vector<ap_float> &numeric_successor_state,
    const vector<int> &abstract_successor_state, const vector<int> &abstract_numeric_successor_state,
    const DomainMapping &domain_mapping,
    const NumericDomainMappingType &numeric_domain_mapping,
    const TaskProxy &task_proxy) const {
    // TODO: Blacklisting????!!!!!
    vector<Flaw> flaws;

    for (size_t var_id = 0; var_id < successor_state.size(); ++var_id) {
        if (domain_mapping[var_id].empty()) {
            continue; // trivial variable
        }
        int abstract_value = abstract_successor_state[var_id];
        int correct_abstract_value = domain_mapping[var_id][successor_state[var_id]];
        if (abstract_value != correct_abstract_value) {
            assert(is_comparison_axiom_variable(static_cast<int>(var_id)));
            auto it = comparison_axiom_dependencies.find(static_cast<int>(var_id));
            assert(it != comparison_axiom_dependencies.end());
            bool added = false;
            vector<NumericFlaw> numeric_flaws;
            for (int dep_var_id : it->second) {
                int local_idx = global_to_local_regular_numeric_var_ids[dep_var_id];
                assert(local_idx >= 0 && local_idx < static_cast<int>(already_split.size()));
                ap_float concrete_value = numeric_successor_state[dep_var_id];
                bool is_lower = determine_include_in_lower(var_id, dep_var_id, concrete_value, numeric_successor_state, task_proxy);
                NumericFlaw numeric_flaw{dep_var_id, concrete_value, is_lower};
                numeric_flaws.push_back(numeric_flaw);
            }
            assert(added);
            PropFlaw pf{Fact(static_cast<int>(var_id), successor_state[var_id]), numeric_flaws};
            flaws.push_back(pf);
        }
    }

    for (size_t var_id = 0; var_id < numeric_successor_state.size(); ++var_id) {
        int abstract_value = abstract_numeric_successor_state[var_id];
        int correct_abstract_value = numeric_domain_mapping[var_id]->get_partition_index(
            numeric_successor_state[var_id]);
        if (abstract_value != correct_abstract_value) {
            ap_float concrete_value = numeric_successor_state[var_id];
            // TODO: Not sure whether to split lower or upper bound but that might be important to determine.
            bool is_lower = true;
            NumericFlaw numeric_flaw{static_cast<int>(var_id), concrete_value, is_lower};
            flaws.push_back(numeric_flaw);
        }
    }
    return flaws;
}


// Helper function to check if a variable is derived (appears in axiom effects)
static bool is_derived_variable(const TaskProxy &task_proxy, int var_id) {
    for (OperatorProxy ax : task_proxy.get_axioms()) {
        for (EffectProxy eff : ax.get_effects()) {
            if (eff.get_fact().get_variable().get_id() == var_id) {
                return true;
            }
        }
    }
    return false;
}

vector<CEGAR::Flaw> CEGAR::get_goal_flaws(
    const TaskProxy &task_proxy, const vector<int> &current_state,
    const vector<ap_float> &numeric_state) const {
    vector<Flaw> flaws;
    
    // First, collect non-derived goals directly
    for (const FactProxy &goal : task_proxy.get_goals()) {
        int var_id = goal.get_variable().get_id();
        if (!is_derived_variable(task_proxy, var_id)) {
            if (blacklisted_variables.count(var_id) == 0
                && current_state[var_id] != goal.get_value()) {
                if (is_comparison_axiom_variable(var_id)) {
                    auto it = comparison_axiom_dependencies.find(var_id);
                    assert(it != comparison_axiom_dependencies.end());
                    vector<NumericFlaw> numeric_flaws;
                    for (int dep_var_id : it->second) {
                        ap_float concrete_value = numeric_state[dep_var_id];
                        bool is_lower = determine_include_in_lower(var_id, dep_var_id, concrete_value, numeric_state, task_proxy);
                        NumericFlaw numeric_flaw{dep_var_id, concrete_value, is_lower};
                        numeric_flaws.push_back(numeric_flaw);
                    }
                    flaws.emplace_back(
                        std::in_place_type<PropFlaw>,
                        Fact(var_id, goal.get_value()),
                        move(numeric_flaws)
                    );
                } else {
                    flaws.emplace_back(
                        std::in_place_type<PropFlaw>,
                        Fact(var_id, goal.get_value()),
                        std::vector<NumericFlaw>{}
                    );
                }
            }
        }
    }
    
    // Reconstruct goals from goal axioms (numeric goals are compiled into axioms)
    // There should be at most two axioms: one dummy axiom (no preconditions),
    // and one optional goal axiom that encodes numeric/propositional goals
    assert(task_proxy.get_axioms().size() <= 2);
    
    for (OperatorProxy axiom : task_proxy.get_axioms()) {
        if (!axiom.get_preconditions().empty() && axiom.get_effects().size() == 1) {
            for (FactProxy pre : axiom.get_preconditions()) {
                int var_id = pre.get_variable().get_id();
                if (blacklisted_variables.count(var_id) == 0
                    && current_state[var_id] != pre.get_value()) {

                    if (is_comparison_axiom_variable(var_id)) {
                        auto it = comparison_axiom_dependencies.find(var_id);
                        assert(it != comparison_axiom_dependencies.end());
                        vector<NumericFlaw> numeric_flaws;
                        for (int dep_var_id : it->second) {
                            ap_float concrete_value = numeric_state[dep_var_id];
                            bool is_lower = determine_include_in_lower(var_id, dep_var_id, concrete_value, numeric_state, task_proxy);
                            NumericFlaw numeric_flaw{dep_var_id, concrete_value, is_lower};
                            numeric_flaws.push_back(numeric_flaw);
                        }
                        flaws.emplace_back(
                            std::in_place_type<PropFlaw>,
                            Fact(var_id, pre.get_value()),
                            move(numeric_flaws)
                        );
                    } else {
                        flaws.emplace_back(
                            std::in_place_type<PropFlaw>,
                            Fact(var_id, pre.get_value()),
                            std::vector<NumericFlaw>{}
                        );
                    }
                }
            }
        }
    }
    
    return flaws;
}

/*
  Apply the operator to the given state, ignoring that the operator is
  potentially not applicable in the state, and expecting that the operator
  has not conditional effects.
*/
static void apply_op_to_state(vector<int> &state, const OperatorProxy &op) {
    assert(!op.is_axiom());
    for (EffectProxy effect : op.get_effects()) {
        assert(effect.get_conditions().empty());
        FactProxy effect_fact = effect.get_fact();
        state[effect_fact.get_variable().get_id()] = effect_fact.get_value();
    }
}

/*
  Apply numeric effects of the operator to the numeric state.
*/
static void apply_numeric_effects(vector<ap_float> &numeric_state, const OperatorProxy &op) {
    assert(!op.is_axiom());
    
    // Apply numeric assignment effects
    for (auto ass_eff_proxy : op.get_ass_effects()) {
        NumAssProxy effect = ass_eff_proxy.get_assignment();
        
        int affected_var_id = effect.get_affected_variable().get_id();
        
        // Get the assigned variable (operand)
        NumericVariableProxy assigned_var = effect.get_assigned_variable();
        ap_float operand = numeric_state[assigned_var.get_id()];
        
        // Apply the effect based on assignment type
        f_operator op_type = effect.get_assigment_operator_type();
        switch (op_type) {
            case assign:
                numeric_state[affected_var_id] = operand;
                break;
            case increase:
                numeric_state[affected_var_id] += operand;
                break;
            case decrease:
                numeric_state[affected_var_id] -= operand;
                break;
            case scale_up:
                numeric_state[affected_var_id] *= operand;
                break;
            case scale_down:
                if (operand != 0) {
                    numeric_state[affected_var_id] /= operand;
                }
                break;
        }
    }
}


vector<CEGAR::Flaw> CEGAR::get_flaws(
    const TaskProxy &task_proxy, const State &concrete_init,
    const DomainAbstraction &abstraction, bool execute_entire_plan) const {

    vector<Flaw> flaws; 

    // Initialize propositional state
    vector<int> current_prop_state;
    current_prop_state.reserve(concrete_init.size());
    for (int i = 0; i < concrete_init.size(); ++i) {
        current_prop_state.push_back(concrete_init[i].get_value());
    }
    
    // Initialize numeric state
    vector<ap_float> current_numeric_state = g_root_task()->get_initial_state_numeric_values();
    
    g_axiom_evaluator->evaluate_arithmetic_axioms(current_numeric_state);
    g_axiom_evaluator->evaluate(current_prop_state, current_numeric_state);

    vector<vector<int>> wildcard_plan = abstraction.get_plan();

    // Required for our favorite deviation flaws
    const vector<vector<int>> &abstract_prop_states = abstraction.get_abstract_prop_states();
    const vector<vector<int>> &abstract_numeric_states = abstraction.get_abstract_numeric_states();
    assert(abstract_prop_states.size() == abstract_numeric_states.size());

    // Helper: decode current abstract state (propositional + numeric partitions)
    auto decode_abstract_state_compact = [&](const vector<int> &prop_state,
                                             const vector<ap_float> &num_state) -> string {
        const DomainMapping &dm = abstraction.get_domain_mapping();
        const NumericDomainMappingType &ndm = abstraction.get_numeric_domain_mapping();
        stringstream ss;
        ss << "[";
        bool first = true;
        for (size_t i = 0; i < dm.size(); ++i) {
            if (!dm[i].empty()) {
                if (!first) ss << ", ";
                first = false;
                int concrete_val = prop_state[i];
                ss << "v" << i << "=" << concrete_val << " (" << dm[i][concrete_val] << ")";
            }
        }
        for (size_t i = 0; i < ndm.size(); ++i) {
            if (ndm[i]->get_num_partitions() == 1) continue; // Skip trivial numeric variables
            if (!first) ss << ", ";
            first = false;
            ss << "num" << i << "=" << num_state[i];
        }
        ss << "]";
        return ss.str();
    };

    logger->log(Verbosity::DEBUG, "PLAN: Validating abstract plan (", wildcard_plan.size(), " steps)");

    // Debug domain mapping
    const DomainMapping &dm = abstraction.get_domain_mapping();
    const NumericDomainMappingType &ndm = abstraction.get_numeric_domain_mapping();
    
    logger->log(Verbosity::DEBUG, "  Domain mapping (propositional):");
    for (size_t i = 0; i < dm.size(); ++i) {
        if (!dm[i].empty()) {
            logger->log_no_endl(Verbosity::DEBUG, "    var ", i, ": ");
            for (size_t j = 0; j < dm[i].size(); ++j) {
                if (dm[i][j] == 0) continue;
                logger->log_no_endl(Verbosity::DEBUG, j, " -> ", dm[i][j], ", ");
            }
            logger->log(Verbosity::DEBUG, "");
        }
    }
    logger->log(Verbosity::DEBUG, "  Domain mapping (numeric):");
    for (size_t i = 0; i < ndm.size(); ++i) {
        if (ndm[i]->get_num_partitions() > 1) {
            logger->log_no_endl(Verbosity::DEBUG, "    num var ", i, ": { ");
            // Iterate through actual ranges and show partition assignment
            const auto &ranges = ndm[i]->get_ranges();
            for (size_t range_idx = 0; range_idx < ranges.size(); ++range_idx) {
                const NumericRange &range = ranges[range_idx];
                logger->log_no_endl(Verbosity::DEBUG, "partition ", range.partition_index, ": ", range.to_string(), " ");
            }   
            logger->log(Verbosity::DEBUG, "}");
        }
    }
    logger->log(Verbosity::DEBUG, "PLAN: State 0 (start): ", decode_abstract_state_compact(current_prop_state, current_numeric_state));

    const DomainMapping &domain_mapping = abstraction.get_domain_mapping();
    const NumericDomainMappingType &numeric_domain_mapping = abstraction.get_numeric_domain_mapping();
    
    int step_num = 0;
    for (vector<int> &equivalent_ops : wildcard_plan) {
        assert(flaws.empty()); 
        assert(abstract_prop_states.size() == step_num + 1);
        assert(abstract_numeric_states.size() == step_num + 1);

        const vector<int> &abstract_state = abstract_prop_states[step_num];
        const vector<int> &abstract_numeric_state = abstract_numeric_states[step_num];
        
        for (int op_id : equivalent_ops) {
            assert(op_id < task_proxy.get_operators().size());
            OperatorProxy op = task_proxy.get_operators()[op_id];
            string op_name = op.get_name();

            vector<Flaw> operator_flaws =
                get_precondition_flaws(op, current_prop_state, current_numeric_state, task_proxy);

            vector<Flaw> deviation_flaws = get_deviation_flaws(
                current_prop_state, current_numeric_state,
                abstract_state, abstract_numeric_state,
                domain_mapping, numeric_domain_mapping, task_proxy);
            operator_flaws.insert(operator_flaws.end(), deviation_flaws.begin(), deviation_flaws.end());

            if (operator_flaws.empty()) {
                flaws.clear();
                string decoded_state = decode_abstract_state_compact(current_prop_state, current_numeric_state);
                logger->log(Verbosity::DEBUG, "[PLAN] ", decoded_state, ", ", op_name);
                apply_op_to_state(current_prop_state, op);
                apply_numeric_effects(current_numeric_state, op);
                g_axiom_evaluator->evaluate_arithmetic_axioms(current_numeric_state);
                g_axiom_evaluator->evaluate(current_prop_state, current_numeric_state);
                break;
            } else {
                // We have precondition or deviation flaws
                flaws.insert(flaws.end(), operator_flaws.begin(), operator_flaws.end());
            }
        }
        
        if (!flaws.empty()) {
            return flaws;
        }
        step_num++;
    }

    string decoded_state = decode_abstract_state_compact(current_prop_state, current_numeric_state);
    logger->log(Verbosity::DEBUG, "[PLAN] ", decoded_state);

    assert(flaws.empty());

    vector<Flaw> goal_flaws =
        get_goal_flaws(task_proxy, current_prop_state, current_numeric_state);
    flaws.insert(flaws.end(), goal_flaws.begin(), goal_flaws.end());
    return flaws;
}

bool CEGAR::fix_flaws(
    vector<Flaw> &&flaws, DomainMapping &domain_mapping,
    int abstraction_size, NumericDomainMappingType &numeric_domain_mapping) {
    switch (flaw_treatment) {
        case FlawTreatment::RANDOM_SINGLE_ATOM:
            return fix_single_random_flaw(move(flaws), domain_mapping, abstraction_size, numeric_domain_mapping);
        case FlawTreatment::ONE_SPLIT_PER_ATOM:
            return fix_flaws_per_atom(move(flaws), domain_mapping, abstraction_size, numeric_domain_mapping);
        case FlawTreatment::ONE_SPLIT_PER_VARIABLE:
            return fix_flaws_per_variable(move(flaws), domain_mapping, abstraction_size, numeric_domain_mapping);
        case FlawTreatment::MAX_REFINED_SINGLE_ATOM:
            return fix_single_flaw_max_refined(move(flaws), domain_mapping, abstraction_size, numeric_domain_mapping);
    }
    assert(false);
    return false;
}

bool CEGAR::fix_single_random_flaw(
    vector<Flaw> &&flaws, DomainMapping &domain_mapping,
    int abstraction_size, NumericDomainMappingType &numeric_domain_mapping) {
    // TODO: Number of repetitions set to log(|flaws|) + 1 is somewhat arbitrary...
    int repetitions = ceil(1 + std::log(flaws.size()));
    for (int i = 0; i < repetitions; ++i) {
        int chosen_idx = rng->random(flaws.size());
        const Flaw &chosen = flaws[chosen_idx];

        // TODO: Proper check of can_refine_numeric_var
        visit([&](auto &&f) {
            using T = std::decay_t<decltype(f)>;

            if constexpr (std::is_same_v<T, PropFlaw>) {
                const Fact &fact = f.first;

                if (can_refine_variable(abstraction_size, fact.var)) {
                    add_variable_to_abstraction_if_necessary(fact.var, domain_mapping);

                    if (is_comparison_axiom_variable(fact.var)) {
                        const std::vector<NumericFlaw> &numeric_flaws = f.second;
                        domain_mapping[fact.var][0] = 1;
                        abstract_domain_sizes[fact.var] = 2;
                        for (int j = i; j < repetitions; ++j) {
                            chosen_idx = rng->random(numeric_flaws.size());
                            NumericFlaw chosen_numeric_flaw = numeric_flaws[chosen_idx];
                            // TODO: Implement check if the flaw has been already split at that position
                            int id              = std::get<0>(chosen_numeric_flaw);
                            const ap_float &val = std::get<1>(chosen_numeric_flaw);
                            bool flag           = std::get<2>(chosen_numeric_flaw);
                            if (can_refine_numeric_variable(abstraction_size, id)) {
                                // TODO: Here was LOCAL id used. Get rid of it. 
                                numeric_domain_mapping[id]->split_at(val, flag);
                                return true;
                            }
                            // TODO: Think more carefully if that really makes sense here
                        }
                        return false;
                        
                    } else {
                        int old_size = abstract_domain_sizes[fact.var];
                        domain_mapping[fact.var][fact.value] = abstract_domain_sizes[fact.var];
                        abstract_domain_sizes[fact.var] += 1;
                        logger->log(Verbosity::INFO, "Refined propositional var ", fact.var,
                                " at value ", fact.value,
                                " (abstract domain size: ", old_size, " -> ", abstract_domain_sizes[fact.var], ")");
                    }
                } 
            } else if constexpr (std::is_same_v<T, NumericFlaw>) {
                int id              = std::get<0>(f);
                const ap_float &val = std::get<1>(f);
                bool flag           = std::get<2>(f);

                // TODO: Implement check if the flaw has been already split at that position
                numeric_domain_mapping[id]->split_at(val, flag);
                if (can_refine_numeric_variable(abstraction_size, id)) {
                    // TODO: Here was LOCAL id used. Get rid of it. 
                    numeric_domain_mapping[id]->split_at(val, flag);
                    return true;
                }

            }
        }, chosen);
    }

    return false;
}

/* Chooses a flaw for that the increase in abstraction size is the smallest among all given ones
 * -> Leads to the smallest possible increase in abstraction size in every iteration 
 */
bool CEGAR::fix_single_flaw_max_refined(
    vector<Flaw> &&flaws, DomainMapping &domain_mapping,
    int abstraction_size, NumericDomainMappingType &numeric_domain_mapping) {

    int current_max_domain_size = 0;
    vector<int> current_flaw_candidates;
    vector<vector<int>> dependent_numeric_flaw_candidates;
    int num_flaws = (int) flaws.size();

    //Select max-refined flaws
    for (int i = 0; i < num_flaws; ++i) {
        Flaw flaw = flaws[i];
        int domain_size = 0;
        vector<int> dep_candidates;
        visit([&](auto &&f) {
            using T = std::decay_t<decltype(f)>;
            if constexpr (std::is_same_v<T, PropFlaw>) { 
                const Fact &fact = f.first;
                domain_size = abstract_domain_sizes[fact.var];
                if (is_comparison_axiom_variable(fact.var)) {
                    const std::vector<NumericFlaw> &numeric_flaws = f.second;
                    int max_numeric_domain_size = 0;
                    int old_domain_size = domain_size;
                    for (int j = 0; j < numeric_flaws.size(); ++j) {
                        NumericFlaw numeric_flaw = numeric_flaws[j];
                        int id = std::get<0>(numeric_flaw);
                        int num_partitions = numeric_domain_mapping[id]->get_num_partitions();
                        if (num_partitions > max_numeric_domain_size) {
                            dep_candidates.clear();
                            max_numeric_domain_size = num_partitions;
                            dep_candidates.push_back(j);
                        } else if (num_partitions == max_numeric_domain_size) {
                            dep_candidates.push_back(j);
                        }
                    }
                    domain_size = domain_size + max_numeric_domain_size;
                } 
                
            } else if constexpr (std::is_same_v<T, NumericFlaw>) {
                int id = std::get<0>(f);
                int num_partitions = numeric_domain_mapping[id]->get_num_partitions();
                domain_size = num_partitions;

            } 
            if (domain_size > current_max_domain_size) {
                current_flaw_candidates.clear();
                dependent_numeric_flaw_candidates.clear();
                current_flaw_candidates.emplace_back(i);
                dependent_numeric_flaw_candidates.push_back(dep_candidates);
                current_max_domain_size = domain_size;
            } else if (domain_size == current_max_domain_size) {
                current_flaw_candidates.emplace_back(i);
                dependent_numeric_flaw_candidates.push_back(dep_candidates);
            }

        }, flaw);
    }

    // Refine max-refined flaw
    Flaw chosen_flaw = flaws[*rng->choose(current_flaw_candidates)];
    visit([&](auto &&f) {
        using T = std::decay_t<decltype(f)>;
        if constexpr (std::is_same_v<T, PropFlaw>) { 
            const Fact &fact = f.first;
            if (can_refine_variable(abstraction_size, fact.var)) {
                add_variable_to_abstraction_if_necessary(fact.var, domain_mapping);
                if (is_comparison_axiom_variable(fact.var)) {
                    const std::vector<NumericFlaw> &numeric_flaws = f.second;
                    domain_mapping[fact.var][0] = 1;
                    abstract_domain_sizes[fact.var] = 2;

                    NumericFlaw numeric_flaw = numeric_flaws[*rng->choose(dependent_numeric_flaw_candidates[0])];
                    int id = std::get<0>(numeric_flaw);
                    const ap_float &val = std::get<1>(numeric_flaw);
                    bool flag = std::get<2>(numeric_flaw);
                    if (can_refine_numeric_variable(abstraction_size, id)) {
                        numeric_domain_mapping[id]->split_at(val, flag);
                        return true;
                    }
                }
            } 
        } else if constexpr (std::is_same_v<T, NumericFlaw>) {
            int id = std::get<0>(f);
            ap_float val = std::get<1>(f);
            bool flag = std::get<2>(f);
            if (can_refine_numeric_variable(abstraction_size, id)) {
                numeric_domain_mapping[id]->split_at(val, flag);
                return true;
            }
        }

    }, chosen_flaw);

    return false;
}

bool CEGAR::fix_flaws_per_atom(
    vector<Flaw> &&flaws, DomainMapping &domain_mapping,
    int abstraction_size, NumericDomainMappingType &numeric_domain_mapping) {
    // FIXME: Bias for variables with low index.

    // NOTE: unsure if that is the optimal strategy. Assume a comparison depending on n numeric variables. In that case, we refine all of them at once. 
    
    sort(flaws.begin(), flaws.end(), [](const auto &lhs, const auto &rhs) {
        auto get_id = [](const auto& element) -> int {
            using T = std::decay_t<decltype(element)>;
            if constexpr (std::is_same_v<T, PropFlaw>) {
                return element.first.var; 
            } else if constexpr (std::is_same_v<T, NumericFlaw>) {
                return std::get<0>(element); 
            }
        };
        return std::visit(get_id, lhs) < std::visit(get_id, rhs);
    });

    PropFlaw last_prop_flaw(Fact(-1, -1), {});
    NumericFlaw last_numeric_flaw{-1, 0.0, false};
    for (Flaw flaw : flaws) {
    
        visit([&](auto &&f) {
            using T = std::decay_t<decltype(f)>;
            if constexpr (std::is_same_v<T, PropFlaw>) { 
                const Fact &fact = f.first;
                if (can_refine_variable(abstraction_size, fact.var)) {
                    add_variable_to_abstraction_if_necessary(fact.var, domain_mapping);
                    if (is_comparison_axiom_variable(fact.var)) {
                        const std::vector<NumericFlaw> &numeric_flaws = f.second;
                        domain_mapping[fact.var][0] = 1;
                        abstract_domain_sizes[fact.var] = 2;
                        sort(numeric_flaws.begin(), numeric_flaws.end());
                        for (int j = 0; j < numeric_flaws.size(); ++j) {
                            NumericFlaw numeric_flaw = numeric_flaws[j];
                            if (last_numeric_flaw == numeric_flaw) {
                                continue; // Duplicate flaw, already refined
                            }   
                            int id = std::get<0>(numeric_flaw);
                            const ap_float &val = std::get<1>(numeric_flaw);
                            bool flag = std::get<2>(numeric_flaw);
                            if (can_refine_numeric_variable(abstraction_size, id)) {
                                numeric_domain_mapping[id]->split_at(val, flag);
                                last_numeric_flaw = numeric_flaw;
                            }
                        }
                    } else {
                        if (last_prop_flaw == PropFlaw(fact, f.second)) {
                            return; // Duplicate flaw, already refined
                        }
                        domain_mapping[fact.var][fact.value] =
                            abstract_domain_sizes[fact.var];
                        abstract_domain_sizes[fact.var] += 1;
                        last_prop_flaw = PropFlaw(fact, f.second);
                    }
                }
            } else if constexpr (std::is_same_v<T, NumericFlaw>) {
                if (last_numeric_flaw == f) {
                    return; // Duplicate flaw, already refined
                }
                int id = std::get<0>(f);
                const ap_float &val = std::get<1>(f);
                bool flag = std::get<2>(f);
                if (can_refine_numeric_variable(abstraction_size, id)) {
                    numeric_domain_mapping[id]->split_at(val, flag);
                    last_numeric_flaw = f;
                }
            }
        }, flaw);
    }
    return last_prop_flaw.first != Fact(-1, -1) || std::get<0>(last_numeric_flaw) != -1;
}

bool CEGAR::fix_flaws_per_variable(
    vector<Flaw> &&flaws, DomainMapping &domain_mapping,
    int abstraction_size, NumericDomainMappingType &numeric_domain_mapping) {
    // FIXME: Bias for variables with low index.
    sort(flaws.begin(), flaws.end(), [](const auto &lhs, const auto &rhs) {
        auto get_id = [](const auto& element) -> int {
            using T = std::decay_t<decltype(element)>;
            if constexpr (std::is_same_v<T, PropFlaw>) {
                return element.first.var; 
            } else if constexpr (std::is_same_v<T, NumericFlaw>) {
                return std::get<0>(element); 
            }
        };
        return std::visit(get_id, lhs) < std::visit(get_id, rhs);
    });

    PropFlaw last_prop_flaw(Fact(-1, -1), {});
    NumericFlaw last_numeric_flaw{-1, 0.0, false};
    for (Flaw flaw : flaws) {
    
        visit([&](auto &&f) {
            using T = std::decay_t<decltype(f)>;
            if constexpr (std::is_same_v<T, PropFlaw>) { 
                const Fact &fact = f.first;
                if (can_refine_variable(abstraction_size, fact.var)) {
                    add_variable_to_abstraction_if_necessary(fact.var, domain_mapping);
                    if (is_comparison_axiom_variable(fact.var)) {
                        const std::vector<NumericFlaw> &numeric_flaws = f.second;
                        domain_mapping[fact.var][0] = 1;
                        abstract_domain_sizes[fact.var] = 2;
                        sort(numeric_flaws.begin(), numeric_flaws.end());
                        for (int j = 0; j < numeric_flaws.size(); ++j) {
                            NumericFlaw numeric_flaw = numeric_flaws[j];
                            if (last_numeric_flaw == numeric_flaw) {
                                continue; // Duplicate flaw, already refined
                            }   
                            int id = std::get<0>(numeric_flaw);
                            const ap_float &val = std::get<1>(numeric_flaw);
                            bool flag = std::get<2>(numeric_flaw);
                            if (can_refine_numeric_variable(abstraction_size, id)) {
                                numeric_domain_mapping[id]->split_at(val, flag);
                                last_numeric_flaw = numeric_flaw;
                            }
                        }
                    } else {
                        if (fact.var > last_prop_flaw.first.var && last_prop_flaw == PropFlaw(fact, f.second)) {
                            return; // Duplicate flaw, already refined
                        }
                        domain_mapping[fact.var][fact.value] =
                            abstract_domain_sizes[fact.var];
                        abstract_domain_sizes[fact.var] += 1;
                        last_prop_flaw = PropFlaw(fact, f.second);
                    }
                }
            } else if constexpr (std::is_same_v<T, NumericFlaw>) {
                if (last_numeric_flaw == f) {
                    return; // Duplicate flaw, already refined
                }
                int id = std::get<0>(f);
                const ap_float &val = std::get<1>(f);
                bool flag = std::get<2>(f);
                if (can_refine_numeric_variable(abstraction_size, id)) {
                    numeric_domain_mapping[id]->split_at(val, flag);
                    last_numeric_flaw = f;
                }
            }
        }, flaw);
    }
    return last_prop_flaw.first != Fact(-1, -1) || std::get<0>(last_numeric_flaw) != -1;
}

void CEGAR::print_statistics(
    const TaskProxy &task_proxy, const DomainMapping &domain_mapping) {
    //assert(log.is_at_least_normal());

    // Propositional variable statistics
    int num_variables = task_proxy.get_variables().size();
    int abstraction_size = 1;
    int num_trivial_variables = 0;
    int num_complete_variables = 0;
    double avg_domain_size = 0;
    for (int i = 0; i < num_variables; ++i) {
        abstraction_size *= abstract_domain_sizes[i];
        int original_domain_size =
            task_proxy.get_variables()[i].get_domain_size();
        if (abstract_domain_sizes[i] == 1) {
            ++num_trivial_variables;
        }
        if (abstract_domain_sizes[i] == original_domain_size) {
            ++num_complete_variables;
        }
        double domain_size_ratio =
            ((double) abstract_domain_sizes[i]) / original_domain_size;
        avg_domain_size += domain_size_ratio / num_variables;
    }

    // Numeric variable statistics
    int num_numeric_variables = task_proxy.get_numeric_variables().size();
    int num_trivial_numeric_vars = 0;
    int num_refined_numeric_vars = 0;
    int total_numeric_partitions = 0;
    double avg_numeric_partitions = 0;
    
    for (int i = 0; i < num_numeric_variables; ++i) {
        int num_partitions = numeric_domain_sizes[i];
        total_numeric_partitions += num_partitions;
        abstraction_size *= num_partitions;
        
        if (num_partitions == 1) {
            ++num_trivial_numeric_vars;
        } else {
            ++num_refined_numeric_vars;
        }
    }
    
    if (num_numeric_variables > 0) {
        avg_numeric_partitions = ((double) total_numeric_partitions) / num_numeric_variables;
    }
    if(logger && logger->should_log(Verbosity::INFO)) {
        logger->log(Verbosity::INFO, "\n=== CEGAR Statistics ===");
        logger->log(Verbosity::INFO, "Final abstraction size: ", abstraction_size);
        logger->log(Verbosity::INFO, "\nPropositional variables:");
        logger->log(Verbosity::INFO, "  Total: ", num_variables);
        logger->log(Verbosity::INFO, "  Trivial (size 1): ", num_trivial_variables);
        logger->log(Verbosity::INFO, "  Complete (not abstracted): ", num_complete_variables);
        logger->log(Verbosity::INFO, "  Average domain size ratio: ", avg_domain_size);
        
        // Print details of non-trivial propositional variables
        logger->log(Verbosity::INFO, "\n  Non-trivial propositional variables:");
        for (int i = 0; i < num_variables; ++i) {
            if (abstract_domain_sizes[i] > 1) {
                VariableProxy var = task_proxy.get_variables()[i];
                int original_size = var.get_domain_size();
                logger->log(Verbosity::INFO, "    var", i, " (", var.get_name(), "): ",
                                "abstract_size=", abstract_domain_sizes[i],
                                ", original_size=", original_size);
                
                // Print the domain mapping if it's not too large
                if (abstract_domain_sizes[i] <= 10 && original_size <= 20) {
                    logger->log_no_endl(Verbosity::INFO, "      mapping: [");
                    for (int val = 0; val < original_size; ++val) {
                        if (val > 0) logger->log_no_endl(Verbosity::INFO, ", ");
                        logger->log_no_endl(Verbosity::INFO, val, "->", domain_mapping[i][val]);
                    }
                    logger->log(Verbosity::INFO, "]");
                }
            }
        }
        
        logger->log(Verbosity::INFO, "\nNumeric variables:");
        logger->log(Verbosity::INFO, "  Total: ", num_numeric_variables);
        logger->log(Verbosity::INFO, "  Trivial (1 partition): ", num_trivial_numeric_vars);
        logger->log(Verbosity::INFO, "  Refined (>1 partition): ", num_refined_numeric_vars);
        logger->log(Verbosity::INFO, "  Total partitions: ", total_numeric_partitions);
        logger->log(Verbosity::INFO, "  Average partitions per variable: ", avg_numeric_partitions);
        
        // Print details of refined numeric variables
        logger->log(Verbosity::INFO, "\n  Refined numeric variables:");
        for (int i = 0; i < num_numeric_variables; ++i) {
            if (numeric_domain_sizes[i] > 1) {
                NumericVariableProxy num_var = task_proxy.get_numeric_variables()[i];
                logger->log(Verbosity::INFO, "    var", i, " (", num_var.get_name(), "): ",
                                numeric_domain_sizes[i], " partitions");
                
                // Print the ranges for this variable
                const vector<NumericRange> &ranges = numeric_domain_mapping[i]->get_ranges();
                for (size_t j = 0; j < ranges.size(); ++j) {
                    logger->log_no_endl(Verbosity::INFO, "      partition ", ranges[j].partition_index, ": ");
                    // Print lower bound with correct bracket
                    logger->log_no_endl(Verbosity::INFO, (ranges[j].lower_inclusive ? "[" : "("));
                    if (ranges[j].lower == -numeric_limits<ap_float>::infinity()) {
                        logger->log_no_endl(Verbosity::INFO, "-inf");
                    } else {
                        logger->log_no_endl(Verbosity::INFO, ranges[j].lower);
                    }
                    logger->log_no_endl(Verbosity::INFO, ", ");
                    // Print upper bound
                    if (ranges[j].upper == numeric_limits<ap_float>::infinity()) {
                        logger->log_no_endl(Verbosity::INFO, "inf");
                    } else {
                        logger->log_no_endl(Verbosity::INFO, ranges[j].upper);
                    }
                    // Print upper bracket
                    logger->log(Verbosity::INFO, (ranges[j].upper_inclusive ? "]" : ")"));
                }
            }
        }
        
        logger->log(Verbosity::INFO, "========================\n");
    }
}

void CEGAR::add_variable_to_abstraction_if_necessary(
    int var, DomainMapping &abstraction) {
    if (abstraction[var].empty()) {
        int real_domain_size = real_domain_sizes[var];
        abstraction[var].resize(real_domain_size, 0);
    }
}

NumericDomainMappingType CEGAR::compute_initial_numeric_domain_mapping(
    const TaskProxy &task_proxy) {
    // Get number of numeric variables
    int num_numeric_variables = task_proxy.get_numeric_variables().size();
    
    // Initialize numeric domain mapping with full range (-inf, inf) for regular/derived variables
    // Constants should have a single partition at their exact value
    // Choose strategy based on configuration
    logger->log(Verbosity::DEBUG, "DEBUG: NumericSplitStrategy = ", 
                   (numeric_split_strategy == NumericSplitStrategy::EXCLUSION ? "EXCLUSION" : "STANDARD"));
    NumericDomainMappingType numeric_domain_mapping;
    numeric_domain_mapping.reserve(num_numeric_variables);
    
    NumericVariablesProxy num_vars = task_proxy.get_numeric_variables();
    for (int i = 0; i < num_numeric_variables; ++i) {
        NumericVariableProxy num_var = num_vars[i];
        numType var_type = num_var.get_var_type();
        
        if (var_type == numType::constant) {
            ap_float const_value = num_var.get_initial_state_value();
            
            numeric_domain_mapping.push_back(std::make_unique<ConstantMapping>(const_value));
        } else if (var_type == numType::derived) {
            numeric_domain_mapping.push_back(std::make_unique<ConstantMapping>(0));
        } else if (var_type == numType::regular) {
            
            if (numeric_split_strategy == NumericSplitStrategy::EXCLUSION) {
                numeric_domain_mapping.push_back(std::make_unique<ExclusionSplitMapping>());
            } else {
                numeric_domain_mapping.push_back(std::make_unique<StandardSplitMapping>());
            }
        } else {
            
            if (numeric_split_strategy == NumericSplitStrategy::EXCLUSION) {
                numeric_domain_mapping.push_back(std::make_unique<ExclusionSplitMapping>());
            } else {
                numeric_domain_mapping.push_back(std::make_unique<StandardSplitMapping>());
            }
        }
    }
    
    int num_prop_vars = task_proxy.get_variables().size();
    
    for (int encoded_var_id : init_split_var_ids) {
        if (encoded_var_id < num_prop_vars) {
            continue;
        }
        
        int numeric_var_id = encoded_var_id - num_prop_vars;
        if (numeric_var_id < 0 || numeric_var_id >= num_numeric_variables) {
            continue;
        }
        
        if (blacklisted_numeric_variables.count(numeric_var_id) > 0) {
            continue;
        }
        
        NumericVariableProxy num_var = num_vars[numeric_var_id];
        numType var_type = num_var.get_var_type();
        
        if (var_type != numType::regular) {
            continue;
        }
        
        if (init_split_method == InitSplitMethod::IDENTITY ||
            init_split_method == InitSplitMethod::GOAL_VALUE_OR_RANDOM_IF_NON_GOAL) {
            
            ap_float init_value = num_var.get_initial_state_value();
            bool include_in_lower = rng->random(2) == 0;  // Random flip
            
            logger->log(Verbosity::DEBUG, "Initial split for numeric variable num_", numeric_var_id,
                           " (", num_var.get_name(), ") at init value ", init_value,
                           ", include_in_lower=", include_in_lower);
            
            numeric_domain_mapping[numeric_var_id]->split_at(init_value, include_in_lower);
            
            // Record in already_split to prevent duplicate splits at this value
            int local_id = global_to_local_regular_numeric_var_ids[numeric_var_id];
            if (local_id >= 0 && static_cast<size_t>(local_id) < already_split.size()) {
                already_split[local_id].insert(init_value);
                logger->log(Verbosity::DEBUG, "  Added init value ", init_value, 
                           " to already_split for local_id=", local_id);
            }
        }
    }
    
    return numeric_domain_mapping;
}

void CEGAR::build_comparison_axiom_mapping(const TaskProxy &task_proxy) {
    // Build mapping from propositional variables (derived from comparison axioms)
    // to the REGULAR (non-derived) numeric variables they ultimately depend on.
    // 
    // This is critical because:
    // 1. Comparison axioms can use derived numeric variables (from assignment axioms)
    // 2. We can only refine REGULAR numeric variables (derived ones have implicit partitions)
    // 3. We need to trace through the assignment axiom hierarchy to find base variables
    //
    // Example:
    //   sum := x + y          (assignment axiom, sum is derived)
    //   comp := (sum > 10)    (comparison axiom, comp is propositional)
    //   => comp depends on regular variables x and y, not on sum
    
    comparison_axiom_dependencies.clear();
    
    // First, build a helper to track which variables are derived
    int num_numeric_vars = task_proxy.get_numeric_variables().size();
    vector<bool> is_regular(num_numeric_vars, false); //regular or constant
    
    // Build dependency graph: derived_var -> [source_vars]
    vector<vector<int>> axiom_dependencies(num_numeric_vars);
    
    AssignmentAxiomsProxy assignment_axioms = task_proxy.get_assignment_axioms();

    for (AssignmentAxiomProxy axiom : assignment_axioms) {
        int derived_id = axiom.get_assignment_variable().get_id();
        int left_id = axiom.get_left_variable().get_id();
        int right_id = axiom.get_right_variable().get_id();

        assert(derived_id >= 0 && derived_id < num_numeric_vars);
        assert(left_id >= 0 && left_id < num_numeric_vars);
        assert(right_id >= 0 && right_id < num_numeric_vars);
        
        axiom_dependencies[derived_id].push_back(left_id);
        axiom_dependencies[derived_id].push_back(right_id);
    }

    for (int i = 0; i < num_numeric_vars; ++i) {
        NumericVariableProxy num_var = task_proxy.get_numeric_variables()[i];
        int var_id = num_var.get_id();
        numType num_type = num_var.get_var_type();
        if (num_type == numType::regular) {
            is_regular[i] = true;
        }
    }

    
    auto find_regular_dependencies = [&](int var_id, auto& find_regular_dependencies_ref) -> unordered_set<int> {
        unordered_set<int> regular_vars;
        
        assert(var_id >= 0 && var_id < num_numeric_vars);
        
        if (is_regular[var_id]) {
            regular_vars.insert(var_id);
        } else {
            for (int dep_id : axiom_dependencies[var_id]) {
                unordered_set<int> deps = find_regular_dependencies_ref(dep_id, find_regular_dependencies_ref);
                regular_vars.insert(deps.begin(), deps.end());
            }
        }
        
        return regular_vars;
    };
    
    GoalsProxy goal = task_proxy.get_goals();
    for (FactProxy goal_fact : goal) {
        logger->log(Verbosity::DEBUG, "DEBUG:   Goal: ",
                        goal_fact.get_variable().get_name(), "=", goal_fact.get_value());
    }

    ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
    logger->log(Verbosity::DEBUG, "DEBUG: Building comparison axiom mapping, total axioms: ", comparison_axioms.size());
    for (ComparisonAxiomProxy axiom : comparison_axioms) {
        FactProxy true_fact = axiom.get_true_fact();
        FactProxy false_fact = axiom.get_false_fact();
        assert(true_fact.get_variable().get_id() == false_fact.get_variable().get_id());
        
        int prop_var_id = true_fact.get_variable().get_id();
        
        logger->log(Verbosity::DEBUG, "DEBUG: Processing comparison axiom for fdr_", prop_var_id,
                        " (", true_fact.get_variable().get_name(), ")");
        
        int left_var_id = axiom.get_left_variable().get_id();
        int right_var_id = axiom.get_right_variable().get_id();
        assert(left_var_id >= 0 && left_var_id < num_numeric_vars);
        assert(right_var_id >= 0 && right_var_id < num_numeric_vars);
        
        unordered_set<int> regular_vars;
        
        unordered_set<int> left_deps = find_regular_dependencies(left_var_id, find_regular_dependencies);
        regular_vars.insert(left_deps.begin(), left_deps.end());
        
        unordered_set<int> right_deps = find_regular_dependencies(right_var_id, find_regular_dependencies);
        regular_vars.insert(right_deps.begin(), right_deps.end());
        
        comparison_axiom_dependencies[prop_var_id] = regular_vars;
        
        logger->log(Verbosity::DEBUG, "  fdr_", prop_var_id, " depends on:");
        logger->log(Verbosity::DEBUG, "    left_var=num_", left_var_id,
                        (left_var_id >= 0 && is_regular[left_var_id] ? " (regular)" : " (DERIVED)"));
        logger->log(Verbosity::DEBUG, "    right_var=num_", right_var_id,
                        (right_var_id >= 0 && is_regular[right_var_id] ? " (regular)" : " (DERIVED)"));
        logger->log_no_endl(Verbosity::DEBUG, "    Regular dependencies: {");
        for (int reg_var : regular_vars) {
            logger->log_no_endl(Verbosity::DEBUG, "num_", reg_var, " ");
        }
        logger->log(Verbosity::DEBUG, "}");
    }
    
    logger->log(Verbosity::DEBUG, "DEBUG: Total comparison axiom dependencies stored: ",
                    comparison_axiom_dependencies.size());
    
    logger->log(Verbosity::DEBUG, "\n=== COMPLETE comparison_axiom_dependencies mapping ===");
    NumericVariablesProxy num_vars_for_print = task_proxy.get_numeric_variables();
    for (const auto &entry : comparison_axiom_dependencies) {
        int prop_var_id = entry.first;
        const unordered_set<int> &reg_vars = entry.second;
        VariableProxy prop_var = task_proxy.get_variables()[prop_var_id];
        logger->log_no_endl(Verbosity::DEBUG, "  fdr_", prop_var_id, " (", prop_var.get_name(), ") -> {");
        for (int reg_var_id : reg_vars) {
            NumericVariableProxy num_var = num_vars_for_print[reg_var_id];
            numType var_type = num_var.get_var_type();
            string type_str;
            switch (var_type) {
                case numType::regular: type_str = "REGULAR"; break;
                case numType::constant: type_str = "CONSTANT"; break;
                case numType::derived: type_str = "DERIVED"; break;
                default: type_str = "UNKNOWN"; break;
            }
            logger->log_no_endl(Verbosity::DEBUG, " num_", reg_var_id, "(", num_var.get_name(), ",", type_str, ")");
        }
        logger->log(Verbosity::DEBUG, " }");
    }
    logger->log(Verbosity::DEBUG, "======================================================\n");
    std::unordered_set<int> operator_modified_numeric_vars;
    
    logger->log(Verbosity::DEBUG, "DEBUG PHASE2: Collecting operator-modified numeric variables...");
    OperatorsProxy operators = task_proxy.get_operators();
    for (OperatorProxy op : operators) {
        for (auto ass_eff_proxy : op.get_ass_effects()) {
            NumAssProxy ass_eff = ass_eff_proxy.get_assignment();
            int affected_var_id = ass_eff.get_affected_variable().get_id();
            
            if (affected_var_id >= 0 && affected_var_id < num_numeric_vars) {
                if (is_regular[affected_var_id]) {
                    operator_modified_numeric_vars.insert(affected_var_id);
                }
            }
        }
    }
    
    logger->log_no_endl(Verbosity::DEBUG, "DEBUG PHASE2: Operator-modified numeric variables: ");
    vector<int> sorted_op_vars(operator_modified_numeric_vars.begin(), 
                                operator_modified_numeric_vars.end());
    sort(sorted_op_vars.begin(), sorted_op_vars.end());
    for (int var_id : sorted_op_vars) {
        logger->log_no_endl(Verbosity::DEBUG, "var", var_id, " ");
    }
    logger->log(Verbosity::DEBUG, "");
    logger->log(Verbosity::DEBUG, "DEBUG PHASE2: Total: ", operator_modified_numeric_vars.size(), " variables");
}

DomainAbstraction CEGAR::build_abstraction(
    const TaskProxy &task_proxy) {
    logger->log(Verbosity::INFO, "Building domain abstraction...");
    utils::reserve_extra_memory_padding(memory_padding_in_mb);
    utils::CountdownTimer timer(max_time);

    // Blacklist logic axiom variables (derived variables that are NOT comparison axioms)
    // Only comparison axioms should be refinable
    comparison_axiom_var_ids.clear();
    ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
    for (ComparisonAxiomProxy axiom : comparison_axioms) {
        int var_id = axiom.get_true_fact().get_variable().get_id();
        comparison_axiom_var_ids.insert(var_id);
    }
    
    // NOTE: There are always 2 logic axioms that we ignore
    logger->log(Verbosity::DEBUG, "Blacklisting logic axiom variables:");
    for (OperatorProxy axiom : task_proxy.get_axioms()) {
        for (EffectProxy eff : axiom.get_effects()) {
            int var_id = eff.get_fact().get_variable().get_id();
            // Only blacklist if this is NOT a comparison axiom
            if (comparison_axiom_var_ids.count(var_id) == 0) {
                blacklisted_variables.insert(var_id);
                logger->log(Verbosity::DEBUG, "  Blacklisted logic axiom variable ", var_id,
                               " (", eff.get_fact().get_variable().get_name(), ")");
            }
        }
    }

    DomainMapping domain_mapping =
        compute_initial_domain_mapping(task_proxy);
    logger->log(Verbosity::DEBUG, "Initial domain mapping: ", domain_mapping);
    
    // Initialize numeric domain mapping with full range (-inf, inf) for all numeric variables
    numeric_domain_mapping = compute_initial_numeric_domain_mapping(task_proxy);
    numeric_domain_sizes.resize(numeric_domain_mapping.size(), 1);
    
    // Update numeric_domain_sizes to reflect any initial splits that were applied
    for (size_t i = 0; i < numeric_domain_mapping.size(); ++i) {
        numeric_domain_sizes[i] = numeric_domain_mapping[i]->get_num_partitions();
    }
    
    DomainAbstractionFactory factory(
        task_proxy, domain_mapping, abstract_domain_sizes,
        numeric_domain_mapping, numeric_domain_sizes,
        true, rng, use_wildcard_plans, logger);
    DomainAbstraction abstraction = factory.generate();

    int iteration = 1;
    State concrete_init = task_proxy.get_initial_state();
    while (!termination_criterion_satisfied(timer)) {
        logger->log(Verbosity::INFO, "iteration #", iteration);

        // Decide whether to execute the entire plan for this iteration
        bool exec_entire_for_this_iteration;
        if (exec_entire_plan == ExecEntirePlanMode::EXECUTE_ENTIRE_PLAN) {
            exec_entire_for_this_iteration = true;
        } else if (exec_entire_plan == ExecEntirePlanMode::STOP_AT_FIRST_FLAW) {
            exec_entire_for_this_iteration = false;
        } else { // RANDOMIZE
            exec_entire_for_this_iteration = (rng->random(2) == 0);
        }

        vector<Flaw> flaws =
                get_flaws(task_proxy, concrete_init, abstraction, exec_entire_for_this_iteration);

        if (flaws.empty()) {
            logger->log(Verbosity::DEBUG, "No more flaws found, terminating CEGAR refinement.");
            break;
        }

        bool flaws_fixed = fix_flaws(move(flaws), domain_mapping, abstraction.size());
        if (!flaws_fixed) {
            logger->log(Verbosity::DEBUG, "No flaws could be fixed, terminating CEGAR refinement.");
            break;
        }
        
        DomainAbstractionFactory new_factory(
            task_proxy, domain_mapping, abstract_domain_sizes,
            numeric_domain_mapping, numeric_domain_sizes,
            true, rng, true, logger);
        
        abstraction = new_factory.generate();
        ++iteration;
        
        // (trimmed legacy debug)
    }

    if (utils::extra_memory_padding_is_reserved()) {
        utils::release_extra_memory_padding();
    }


    print_statistics(task_proxy, domain_mapping);
    logger->log(Verbosity::INFO, "Number of CEGAR iterations: ", iteration);

    return abstraction;
}

bool CEGAR::termination_criterion_satisfied(
    utils::CountdownTimer &timer) {
    if (timer.is_expired()) {
        logger->log(Verbosity::INFO, "Terminating CEGAR; time limit reached.");
        return true;
    }
    if (!utils::extra_memory_padding_is_reserved()) {
        logger->log(Verbosity::INFO, "Terminating CEGAR; memory limit reached.");
        return true;
    }
    return false;
}

bool CEGAR::can_refine_variable(
    int old_abstraction_size, int var_id) {
    // TODO: ideally, at this point, we would have checked if var_id is already
    // blacklisted. However, this doesn't seem to work with all of the fix
    // methods.
//    assert(!blacklisted_variables.count(var_id));
    if (blacklisted_variables.count(var_id)) {
        return false;
    }
    
    // Comparison axiom variables can only be refined once (size 1 -> 2).
    if (is_comparison_axiom_variable(var_id) && abstract_domain_sizes[var_id] >= 2) {
        logger->log(Verbosity::DEBUG, "Comparison axiom var ", var_id,
                   " already refined, allowing for potential numeric refinement");
        return true;
    }
    
    int domain_size = abstract_domain_sizes[var_id];
    int abs_size_without_var = old_abstraction_size / domain_size;
    if (utils::is_product_within_limit(abs_size_without_var, domain_size + 1,
                                       max_abstraction_size)) {
        return true;
    }
    blacklisted_variables.insert(var_id);
    return false;
}

bool CEGAR::can_refine_numeric_variable(
    int old_abstraction_size, int numeric_var_id, const TaskProxy &task_proxy) {
    if (blacklisted_numeric_variables.count(numeric_var_id)) {
        return false;
    }
    
    // CRITICAL: Don't refine constants or derived variables!
    // Constants should always have exactly 1 partition at their value
    // Derived variables are implicitly abstracted based on their source variables
    NumericVariablesProxy num_vars = task_proxy.get_numeric_variables();
    if (numeric_var_id >= 0 && static_cast<size_t>(numeric_var_id) < num_vars.size()) {
        NumericVariableProxy num_var = num_vars[numeric_var_id];
        numType var_type = num_var.get_var_type();
        
        logger->log(Verbosity::DEBUG, "DEBUG can_refine: num_", numeric_var_id,
                        " has type=", static_cast<int>(var_type),
                        " (1=constant, 2=derived, 4=regular)");
        
        if (var_type == numType::constant) {
            logger->log(Verbosity::DEBUG, "Cannot refine num_", numeric_var_id, " (CONSTANT); ignoring");
            return false;
        }
        
        if (var_type == numType::derived) {
            logger->log(Verbosity::DEBUG, "Cannot refine num_", numeric_var_id, " (DERIVED); ignoring");
            return false;
        }
    }
    
    // Check if this numeric variable is in the abstraction
    if (numeric_var_id < 0 || numeric_var_id >= static_cast<int>(numeric_domain_mapping.size())) {
        return false;
    }
    
    int current_partitions = numeric_domain_mapping[numeric_var_id]->get_num_partitions();
    int abs_size_without_var = old_abstraction_size / current_partitions;

    logger->log(Verbosity::DEBUG, "Numeric variable ", numeric_var_id,
                    " has ", current_partitions, " partitions.");
    logger->log(Verbosity::DEBUG, "Old abstraction size: ", old_abstraction_size);
    logger->log(Verbosity::DEBUG, "Abstraction size without this variable: ", abs_size_without_var);
    logger->log(Verbosity::DEBUG, "Max abstraction size: ", max_abstraction_size);
    
    // Splitting will create one more partition
    if (utils::is_product_within_limit(abs_size_without_var, current_partitions + 1,
                                       max_abstraction_size)) {
        return true;
    }
    
    logger->log(Verbosity::DEBUG, "Cannot refine numeric variable ", numeric_var_id, "; blacklisting");
    blacklisted_numeric_variables.insert(numeric_var_id);
    return false;
}

static std::unordered_map<int, NumericRange> compute_all_numeric_ranges(
    const std::unordered_map<int, NumericRange> &base_ranges,
    const TaskProxy &task_proxy) {
    
    std::unordered_map<int, NumericRange> ranges = base_ranges;
    
    // Add constants
    NumericVariablesProxy num_vars = task_proxy.get_numeric_variables();
    for (size_t i = 0; i < num_vars.size(); ++i) {
        if (num_vars[i].get_var_type() == numType::constant) {
            ap_float val = num_vars[i].get_initial_state_value();
            ranges[i] = NumericRange(val, val, true, true);
        }
    }
    
    // Propagate through assignment axioms (fixpoint)
    AssignmentAxiomsProxy assignment_axioms = task_proxy.get_assignment_axioms();
    bool changed = true;
    
    while (changed) {
        changed = false;
        
        for (AssignmentAxiomProxy axiom : assignment_axioms) {
            int derived_id = axiom.get_assignment_variable().get_id();
            int left_id = axiom.get_left_variable().get_id();
            int right_id = axiom.get_right_variable().get_id();
            
            auto left_it = ranges.find(left_id);
            auto right_it = ranges.find(right_id);
            if (left_it == ranges.end() || right_it == ranges.end()) {
                continue;
            }
            
            const NumericRange &l_range = left_it->second;
            const NumericRange &r_range = right_it->second;
            
            NumericRange res = NumericDomainMapping::apply_range_operation(
                l_range, r_range, axiom.get_arithmetic_operator_type());
            
            auto it = ranges.find(derived_id);
            if (it == ranges.end() || 
                it->second.lower != res.lower || it->second.upper != res.upper ||
                it->second.lower_inclusive != res.lower_inclusive || 
                it->second.upper_inclusive != res.upper_inclusive) {
                ranges[derived_id] = res;
                changed = true;
            }
        }
    }
    
    return ranges;
}

static int evaluate_comparison_with_ranges(
    int prop_var_id,
    const std::unordered_map<int, NumericRange> &ranges,
    const TaskProxy &task_proxy) {
    
    // Find the comparison axiom for this prop_var_id
    ComparisonAxiomsProxy comp_axioms = task_proxy.get_comparison_axioms();
    for (ComparisonAxiomProxy axiom : comp_axioms) {
        if (axiom.get_true_fact().get_variable().get_id() != prop_var_id) {
            continue;
        }
        
        int left_id = axiom.get_left_variable().get_id();
        int right_id = axiom.get_right_variable().get_id();
        
        // Get ranges for left and right operands
        auto left_it = ranges.find(left_id);
        auto right_it = ranges.find(right_id);
        
        if (left_it == ranges.end() || right_it == ranges.end()) {
            // Missing range - return unknown
            return 2;
        }
        
        return NumericDomainMapping::evaluate_comparison(
            axiom.get_comparison_operator_type(), left_it->second, right_it->second);
    }
    
    // Comparison axiom not found
    return 2;
}

bool CEGAR::determine_include_in_lower(
    int prop_var_id,
    int split_var_id,
    ap_float split_value,
    const std::vector<ap_float> &concrete_values,
    const TaskProxy &task_proxy) const {
    
    // Get the set of regular variables this comparison depends on
    auto deps_it = comparison_axiom_dependencies.find(prop_var_id);
    bool found = false;
    assert(deps_it != comparison_axiom_dependencies.end());
    
    const std::unordered_set<int> &dependent_vars = deps_it->second;
    
    // Build base ranges: singleton [v, v] for all variables except split_var
    std::unordered_map<int, NumericRange> base_ranges_lower;  // For include_in_lower=true
    std::unordered_map<int, NumericRange> base_ranges_upper;  // For include_in_lower=false
    
    //TODO: Take the lower/upper bound values based on the current intervals. Does it make a difference?
    for (int var_id : dependent_vars) {
        if (var_id == split_var_id) {
            base_ranges_lower[var_id] = NumericRange(
                -std::numeric_limits<ap_float>::infinity(), split_value, 
                false, true);  // (-inf, split_value]
            
            base_ranges_upper[var_id] = NumericRange(
                split_value, std::numeric_limits<ap_float>::infinity(),
                true, false);  // [split_value, inf)
        } else {
            assert(var_id >= 0 && static_cast<size_t>(var_id) < concrete_values.size());
            ap_float val = concrete_values[var_id];
            NumericRange singleton(val, val, true, true);
            base_ranges_lower[var_id] = singleton;
            base_ranges_upper[var_id] = singleton;
        } 
    }
    
    std::unordered_map<int, NumericRange> all_ranges_lower = 
        compute_all_numeric_ranges(base_ranges_lower, task_proxy);
    std::unordered_map<int, NumericRange> all_ranges_upper = 
        compute_all_numeric_ranges(base_ranges_upper, task_proxy);
    
    int eval_lower = evaluate_comparison_with_ranges(prop_var_id, all_ranges_lower, task_proxy);
    int eval_upper = evaluate_comparison_with_ranges(prop_var_id, all_ranges_upper, task_proxy);
    
    logger->log(Verbosity::DEBUG, "determine_include_in_lower for prop_var_id=", prop_var_id,
               ", split_var=", split_var_id, ", split_value=", split_value);
    logger->log(Verbosity::DEBUG, "  eval with (-inf, ", split_value, "]: ", 
               (eval_lower == 0 ? "TRUE" : (eval_lower == 1 ? "FALSE" : "UNKNOWN")));
    logger->log(Verbosity::DEBUG, "  eval with [", split_value, ", inf): ",
               (eval_upper == 0 ? "TRUE" : (eval_upper == 1 ? "FALSE" : "UNKNOWN")));
    
    // TODO: add assertion that true should never be possible
    // TODO: If todo asumption does not hold, return enum result
    // Prefer FALSE (=1) over UNKNOWN (=2) over TRUE (=0)
    // We want the comparison to be FALSE in the abstract state
    if (eval_lower == 1 && eval_upper != 1) {
        // Only include_in_lower=true gives FALSE
        logger->log(Verbosity::DEBUG, "  -> Choosing include_in_lower=true (gives FALSE)");
        return true;
    } else if (eval_upper == 1 && eval_lower != 1) {
        // Only include_in_lower=false gives FALSE
        logger->log(Verbosity::DEBUG, "  -> Choosing include_in_lower=false (gives FALSE)");
        return false;
    } else if (eval_lower == 1 && eval_upper == 1) {
        // Both give FALSE - either works, default to false
        logger->log(Verbosity::DEBUG, "  -> Both give FALSE, defaulting to include_in_lower=false");
        utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        return false;
    } else if (eval_lower == 2 && eval_upper == 2) {
        // Both give UNKNOWN - this shouldn't happen in theory, but default to false
        logger->log(Verbosity::DEBUG, "  -> Both give UNKNOWN, defaulting to include_in_lower=false");
        utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        return false;
    } else if (eval_lower == 2) {
        // Only lower gives UNKNOWN (upper gives TRUE) - prefer UNKNOWN over TRUE
        logger->log(Verbosity::DEBUG, "  -> Choosing include_in_lower=true (gives UNKNOWN over TRUE)");
        utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        return true;
    } else if (eval_upper == 2) {
        // Only upper gives UNKNOWN (lower gives TRUE) - prefer UNKNOWN over TRUE
        logger->log(Verbosity::DEBUG, "  -> Choosing include_in_lower=false (gives UNKNOWN over TRUE)");
        utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        return false;
    } else {
        // Both give TRUE - doesn't matter, default to false
        logger->log(Verbosity::DEBUG, "  -> Both give TRUE, defaulting to include_in_lower=false");
        return false;
    }
}

bool CEGAR::fix_numeric_flaws(
    const vector<SelectedNumericFlaw> &numeric_flaws, int abstraction_size, const TaskProxy &task_proxy) {
    
    if (numeric_flaws.empty()) {
        return true;
    }
    
    // Build a map of concrete values for each numeric variable and prop_var_id
    // This is needed for determine_include_in_lower
    std::unordered_map<int, std::unordered_map<int, ap_float>> concrete_values_by_prop_var;
    for (const SelectedNumericFlaw &flaw : numeric_flaws) {
        if (!flaw.prop_var_id) {
            continue;
        }
        auto it = last_concrete_values_by_prop_var.find(*flaw.prop_var_id);
        if (it != last_concrete_values_by_prop_var.end()) {
            concrete_values_by_prop_var[*flaw.prop_var_id] = it->second;
        } else {
            concrete_values_by_prop_var[*flaw.prop_var_id][flaw.numeric_var_id] = flaw.concrete_value;
        }
    }
    
    // Build list of valid candidates: non-blacklisted numeric vars with split values not already split
    struct Candidate {
        int numeric_var_id;
        ap_float split_value;
        int local_id;
        std::optional<int> prop_var_id;  // Which comparison axiom this flaw is from, if any
    };
    vector<Candidate> valid_candidates;
    
    for (const SelectedNumericFlaw &flaw : numeric_flaws) {
        int numeric_var_id = flaw.numeric_var_id;
        ap_float split_value = flaw.concrete_value;
        
        // Check if we can refine this numeric variable
        if (!can_refine_numeric_variable(abstraction_size, numeric_var_id, task_proxy)) {
            logger->log(Verbosity::DEBUG, "DEBUG: Cannot refine num_", numeric_var_id,
                           " (blacklisted or size limit)");
            continue;
        }
        
        // Bounds check
        assert(numeric_var_id >= 0 && numeric_var_id < static_cast<int>(numeric_domain_mapping.size()));
        
        int local_id = global_to_local_regular_numeric_var_ids[numeric_var_id];
        assert(local_id >= 0 && already_split.size() > static_cast<size_t>(local_id));
        
        // Check if this split value has already been used
        assert(already_split[local_id].count(split_value) == 0);
        
        valid_candidates.push_back({numeric_var_id, split_value, local_id, flaw.prop_var_id});
    }
    
    // If no valid candidates, return false (no blacklisting - candidates may exist in future iterations)
    if (valid_candidates.empty()) {
        logger->log(Verbosity::INFO, "No valid numeric flaw candidates to refine (all already split or blacklisted)");
        return false;
    }
    
    // Select ONE random candidate
    const Candidate &selected = *rng->choose(valid_candidates);
    int numeric_var_id = selected.numeric_var_id;
    ap_float split_value = selected.split_value;
    int local_id = selected.local_id;
    std::optional<int> prop_var_id = selected.prop_var_id;
    
    // Assert that split_value is not NaN (it should have been replaced in get_flaws)
    assert(!std::isnan(split_value));
    
    // Determine split direction to ensure the comparison evaluates to FALSE
    // in the abstract state containing the concrete flaw state
    bool include_in_lower = false;
    if (prop_var_id) {
        assert(comparison_axiom_info.count(*prop_var_id) > 0);
        const std::unordered_map<int, ap_float> &concrete_values =
            concrete_values_by_prop_var[*prop_var_id];
        include_in_lower = determine_include_in_lower(
            *prop_var_id, numeric_var_id, split_value, concrete_values, task_proxy);
    }
    
    logger->log(Verbosity::DEBUG, "Split direction for num_", numeric_var_id,
               " at ", split_value, ": include_in_lower=", include_in_lower);
    
    int old_num_partitions = numeric_domain_mapping[numeric_var_id]->get_num_partitions();
    
    int after_concrete_split = numeric_domain_mapping[numeric_var_id]->split_at(split_value, include_in_lower);
    int new_num_partitions = after_concrete_split;
    
    if (new_num_partitions > old_num_partitions) {
        // Successfully split - created at least one new partition
        numeric_domain_sizes[numeric_var_id] = new_num_partitions;
        already_split[local_id].insert(split_value);
        
        // Increment refinement counter
        
        logger->log(Verbosity::INFO, "Refined num_", numeric_var_id,
                       " at value ", split_value,
                       " (partitions: ", old_num_partitions, " -> ", new_num_partitions, ")",
                       " include_in_lower=", include_in_lower);
        return true;
    } else {
        // No new partitions created - splits already exist
        logger->log(Verbosity::DEBUG, "DEBUG: Flaw for num_", numeric_var_id,
                       " at value ", split_value,
                       " - splits already exist (no refinement needed)");
        return false;
    }
}

DomainAbstraction generate_domain_abstraction_with_cegar(
        int max_abstraction_size,
        double max_time,
        bool use_wildcard_plans,
    bool deviation_flaws,
        FlawTreatment flaw_treatment,
        InitSplitMethod init_split_method,
        NumericSplitStrategy numeric_split_strategy,
        ExecEntirePlanMode exec_entire_plan,
        const shared_ptr<utils::RandomNumberGenerator> &rng,
        const TaskProxy &task_proxy,
        unordered_set<int> &&init_split_var_ids,
        unordered_set<int> &&blacklisted_variables,
        unordered_set<int> &&blacklisted_numeric_variables) {
    CEGAR cegar(
        max_abstraction_size,
        max_time,
        use_wildcard_plans,
        deviation_flaws,
        exec_entire_plan,
        flaw_treatment,
        init_split_method,
        numeric_split_strategy,
        rng,
        task_proxy,
        move(init_split_var_ids),
        move(blacklisted_variables),
        move(blacklisted_numeric_variables));  
        
    // Build mapping from comparison axiom propositional variables to numeric variables
    cegar.build_comparison_axiom_mapping(task_proxy);

    return cegar.build_abstraction(task_proxy);
}

void add_cegar_implementation_notes_to_parser(options::OptionParser &parser) {
    parser.document_note(
        "Short description of the CEGAR algorithm",
        "The CEGAR algorithm computes a pattern collection for a given planning "
        "task and a given (sub)set of its goals in a randomized order as "
        "follows. Starting from the pattern collection consisting of a singleton "
        "pattern for each goal variable, it repeatedly attempts to execute an "
        "optimal plan of each pattern in the concrete task, collects reasons why "
        "this is not possible (so-called flaws) and refines the pattern in "
        "question by adding a variable to it.\n"
        "Further parameters allow blacklisting a (sub)set of the non-goal "
        "variables which are then never added to the collection, limiting PDB "
        "and collection size, setting a time limit and switching between "
        "computing regular or wildcard plans, where the latter are sequences of "
        "parallel operators inducing the same abstract transition.",
        true);
    parser.document_note(
        "Implementation notes about the CEGAR algorithm",
        "The following describes differences of the implementation to "
        "the original implementation used and described in the paper.\n\n"
        "Conceptually, there is one larger difference which concerns the "
        "computation of (regular or wildcard) plans for PDBs. The original "
        "implementation used an enforced hill-climbing (EHC) search with the "
        "PDB as the perfect heuristic, which ensured finding strongly optimal "
        "plans, i.e., optimal plans with a minimum number of zero-cost "
        "operators, in domains with zero-cost operators. The original "
        "implementation also slightly modified EHC to search for a best-"
        "improving successor, chosen uniformly at random among all best-"
        "improving successors.\n\n"
        "In contrast, the current implementation computes a plan alongside the "
        "computation of the PDB itself. A modification to Dijkstra's algorithm "
        "for computing the PDB values stores, for each state, the operator "
        "leading to that state (in a regression search). This generating "
        "operator is updated only if the algorithm found a cheaper path to "
        "the state. After Dijkstra finishes, the plan computation starts at the "
        "initial state and iteratively follows the generating operator, computes "
        "all operators of the same cost inducing the same transition, until "
        "reaching a goal. This constitutes a wildcard plan. It is turned into a "
        "regular one by randomly picking a single operator for each transition. "
        "\n\n"
        "Note that this kind of plan extraction does not consider all successors "
        "of a state uniformly at random but rather uses the previously deterministically "
        "chosen generating operator to settle on one successor state, which is "
        "biased by the number of operators leading to the same successor from "
        "the given state. Further note that in the presence of zero-cost "
        "operators, this procedure does not guarantee that the computed plan is "
        "strongly optimal because it does not minimize the number of used "
        "zero-cost operators leading to the state when choosing a generating "
        "operator. Experiments have shown (issue1007) that this speeds up the "
        "computation significantly while not having a strongly negative effect "
        "on heuristic quality due to potentially computing worse plans.\n\n"
        "Two further changes fix bugs of the original implementation to match "
        "the description in the paper. The first bug fix is to raise a flaw "
        "for all goal variables of the task if the plan for a PDB can be "
        "executed on the concrete task but does not lead to a goal state. "
        "Previously, such flaws would not have been raised because all goal "
        "variables are part of the collection from the start on and therefore "
        "not considered. This means that the original implementation "
        "accidentally disallowed merging patterns due to goal violation "
        "flaws. The second bug fix is to actually randomize the order of "
        "parallel operators in wildcard plan steps.",
        true);
}

void add_domain_abstraction_cegar_options_to_parser(
    options::OptionParser &parser) {
    parser.add_option<bool>(
        "use_wildcard_plans",
        "Consider parallel transitions in abstraction.",
        "true");
    parser.add_option<bool>(
        "deviation_flaws",
        "Enable deviation flaw detection when operator preconditions match.",
        "true");
    vector<string> init_split_method;
    init_split_method.emplace_back("goal_value");
    init_split_method.emplace_back("goal_value_or_random_if_non_goal");
    init_split_method.emplace_back("init_value");
    init_split_method.emplace_back("random_value");
    init_split_method.emplace_back("random_partition");
    init_split_method.emplace_back(
        "random_binary_partition_separating_init_goal");
    init_split_method.emplace_back("identity");
    parser.add_enum_option(
        "init_split_method",
        init_split_method,
        "Choose how to initialize splits to seed diversification.",
        "init_value");
    vector<string> flaw_treatment;
    flaw_treatment.emplace_back("random_single_atom");
    flaw_treatment.emplace_back("one_split_per_atom");
    flaw_treatment.emplace_back("one_split_per_variable");
    flaw_treatment.emplace_back("max_refined_single_atom");
    parser.add_enum_option(
        "flaw_treatment",
        flaw_treatment,
        "Flaws are found in collections and can be treated in different ways. "
        "This option allows to switch between them.");
    vector<string> numeric_split_strategy;
    numeric_split_strategy.emplace_back("standard");
    numeric_split_strategy.emplace_back("exclusion");
    parser.add_enum_option(
        "numeric_split_strategy",
        numeric_split_strategy,
        "Strategy for splitting numeric variable domains: "
        "'standard' creates [lower, x) and [x, upper) with different partitions, "
        "'exclusion' creates R\\{x} (two disjoint ranges) and {x} as separate partitions.",
        "standard", {});
    vector<string> exec_entire_plan_modes;
    exec_entire_plan_modes.emplace_back("stop_at_first_flaw");
    exec_entire_plan_modes.emplace_back("execute_entire_plan");
    exec_entire_plan_modes.emplace_back("randomize");
    parser.add_enum_option(
        "exec_entire_plan",
        exec_entire_plan_modes,
        "Choose whether to execute the entire abstract plan for each iteration. "
        "Options: stop_at_first_flaw (default), execute_entire_plan, randomize.",
        "stop_at_first_flaw");
}
}

namespace options {
template <>
std::string TypeNamer<domain_abstractions::FlawTreatment>::name() {
    return "FlawTreatment";
}

template <>
std::string TypeNamer<domain_abstractions::InitSplitMethod>::name() {
    return "InitSplitMethod";
}

template <>
std::string TypeNamer<domain_abstractions::NumericSplitStrategy>::name() {
    return "NumericSplitStrategy";
}

template <>
std::string TypeNamer<domain_abstractions::ExecEntirePlanMode>::name() {
    return "ExecEntirePlanMode";
}
}
