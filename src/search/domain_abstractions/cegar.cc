#include "cegar.h"

#include "cegar_logger.h"
#include "domain_abstraction.h"
#include "domain_abstraction_factory.h"

#include "../axioms.h"

#include <cmath>
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
private:
    // Structure to represent a numeric flaw
    struct NumericFlaw {
        int numeric_var_id;      // Regular numeric variable that needs refinement
        ap_float concrete_value; // Concrete value observed when flaw occurred
        int prop_var_id;         // Propositional variable (comparison axiom) that failed
        
        NumericFlaw(int var_id, ap_float value, int prop_id)
            : numeric_var_id(var_id), concrete_value(value), prop_var_id(prop_id) {}
    };
    
    // Track how many times each numeric variable has been refined
    // to prevent infinite loops
    mutable std::unordered_map<int, int> numeric_var_refinement_count;

    std::vector<int> local_to_global_regular_numeric_var_ids;
    std::vector<int> global_to_local_regular_numeric_var_ids; // NOTE: Not used yet(?)
    std::vector<std::unordered_set<ap_float>> already_split;
    mutable std::vector<std::unordered_set<ap_float>> regular_numeric_var_values;
    
    const int max_abstraction_size;
    const double max_time;
    const bool use_wildcard_plans;
    const bool exec_entire_plan;
    const FlawTreatment flaw_treatment;
    const InitSplitMethod init_split_method;
    const NumericSplitStrategy numeric_split_strategy;
    const shared_ptr<utils::RandomNumberGenerator> &rng;
    const std::unordered_set<int> init_split_var_ids;
    std::unordered_set<int> blacklisted_variables;
    
    // Shared logger for consistent output across CEGAR, factory, and helper
    shared_ptr<CEGARLogger> logger;

    std::vector<int> abstract_domain_sizes;
    std::vector<int> real_domain_sizes;
    
    // Numeric domain mapping and sizes (for refinement across iterations)
    NumericDomainMappingType numeric_domain_mapping;
    std::vector<int> numeric_domain_sizes;
    std::unordered_set<int> blacklisted_numeric_variables;
    
    // Temporary storage for numeric flaws detected in get_flaws()
    // (mutable because get_flaws is const but needs to store flaws)
    mutable std::vector<std::vector<NumericFlaw>> detected_numeric_flaws;
    
    // Track the INDICES of selected propositional flaws in the flaws vector.
    // This is used to correctly access detected_numeric_flaws (which is indexed by position).
    // Empty means no flaws were selected.
    mutable std::vector<int> last_selected_flaw_indices;
    
    // Mapping from propositional variables (derived from comparison axioms)
    // to the numeric variables they depend on.
    // This allows us to trace back from propositional flaws to numeric refinements.
    // Maps: propositional_var_id -> set of numeric variable IDs
    std::unordered_map<int, std::unordered_set<int>> comparison_axiom_dependencies;
    
    // Set of propositional variable IDs that are comparison axiom variables
    // Populated in build_abstraction before compute_initial_domain_mapping
    std::unordered_set<int> comparison_axiom_var_ids;
    
    // Store comparison axiom information including threshold for refinement
    struct ComparisonInfo {
        int left_var_id;   // ID of left numeric variable (might be constant)
        int right_var_id;  // ID of right numeric variable (might be constant)
        int comp_op;       // 0=<, 1=<=, 2=>, 3=>=, 4==, 5=!=
    };
    std::unordered_map<int, ComparisonInfo> comparison_axiom_info;
    
    // PHASE 2: Set of all numeric variables that are modified by operators
    // When ANY numeric flaw is detected, we should refine ALL these variables
    // to ensure non-zero hash effects in the abstraction
    std::unordered_set<int> operator_modified_numeric_vars;

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

    std::vector<Fact> get_flaws(const TaskProxy &task_proxy,
                                    const State &concrete_init,
                                    const DomainAbstraction &abstraction) const;
    bool fix_flaws(std::vector<Fact> &&flaws,
                   DomainMapping &domain_mapping, int abstraction_size);
    bool fix_single_random_flaw(std::vector<Fact> &&flaws,
                                DomainMapping &domain_mapping,
                                int abstraction_size);
    bool fix_single_flaw_max_refined(
            vector<Fact> &&flaws, DomainMapping &domain_mapping,
            int abstraction_size);
    bool fix_flaws_per_atom(std::vector<Fact> &&flaws,
                            DomainMapping &domain_mapping,
                            int abstraction_size);
    bool fix_flaws_per_variable(std::vector<Fact> &&flaws,
                                DomainMapping &domain_mapping,
                                int abstraction_size);

    bool can_refine_variable(int old_abstraction_size, int var_id);
    bool can_refine_numeric_variable(int old_abstraction_size, int numeric_var_id, const TaskProxy &task_proxy);

    void add_variable_to_abstraction_if_necessary(
        int var, DomainMapping &abstraction);

    void print_statistics(const TaskProxy &task_proxy, const DomainMapping &domain_mapping);
    
    NumericDomainMappingType compute_initial_numeric_domain_mapping(
        const TaskProxy &task_proxy);
    
    
    // Debug: print axiom dependency trees
    void print_cegar_axiom_trees(const TaskProxy &task_proxy,
                                 const std::vector<bool> &is_derived,
                                 const std::vector<std::vector<int>> &axiom_dependencies);
    
    
    // Check if a propositional variable is derived from a comparison axiom
    bool is_comparison_axiom_variable(int var_id) const {
        return comparison_axiom_dependencies.count(var_id) > 0;
    }
    
    // Check if a comparison axiom has any dependent numeric variable with at least
    // one observed value that hasn't been split yet.
    bool comparison_has_unsplit_numeric_values(int prop_var_id) const {
        auto it = comparison_axiom_dependencies.find(prop_var_id);
        if (it == comparison_axiom_dependencies.end()) {
            return false;  // No dependencies found
        }
        
        for (int numeric_var_id : it->second) {
            int local_idx = global_to_local_regular_numeric_var_ids[numeric_var_id];
            if (local_idx < 0 || local_idx >= static_cast<int>(regular_numeric_var_values.size())) {
                continue;
            }
            
            // Check if any observed value for this numeric var hasn't been split yet
            const auto &observed = regular_numeric_var_values[local_idx];
            const auto &split_set = (local_idx < static_cast<int>(already_split.size())) 
                                    ? already_split[local_idx] 
                                    : std::unordered_set<ap_float>();
            
            for (ap_float val : observed) {
                if (split_set.count(val) == 0) {
                    // Found an unsplit value
                    return true;
                }
            }
        }
        
        return false;  // All observed values have been split
    }
    
    // Check if a comparison axiom flaw should be added:
    // - Non-comparison variables: always add
    // - Comparison variables: always add (don't filter based on current iteration's
    //   observed values, as they vary semi-randomly between iterations)
    // 
    // We avoid early blacklisting because:
    // - Observed values are cleared each iteration and depend on the abstract plan
    // - A comparison with no splittable values NOW may have them in future iterations
    // - The numeric refinement step will naturally handle "no valid candidates"
    bool should_add_comparison_flaw(int prop_var_id) const {
        // Non-comparison axiom variables are always valid flaws
        if (!is_comparison_axiom_variable(prop_var_id)) {
            logger->log(Verbosity::DEBUG, "    should_add_comparison_flaw(var", prop_var_id, 
                       "): YES (not a comparison axiom)");
            return true;
        }
        
        // For comparison axioms, always add - don't filter based on current observed values
        // The numeric refinement step will handle the case when no valid candidates exist
        logger->log(Verbosity::DEBUG, "    should_add_comparison_flaw(var", prop_var_id, 
                   "): YES (comparison axiom - always allow, numeric refinement will filter)");
        return true;
    }
    
    // Determine split direction for numeric refinement
    bool determine_include_in_lower(
        int prop_var_id,
        int split_var_id,
        ap_float split_value,
        const std::unordered_map<int, ap_float> &concrete_values,
        const TaskProxy &task_proxy) const;
    
    bool fix_numeric_flaws(const std::vector<NumericFlaw> &numeric_flaws,
                          int abstraction_size,
                          const TaskProxy &task_proxy);
public:
    CEGAR(int max_abstraction_size,
          double max_time,
          bool use_wildcard_plans,
          bool exec_entire_plan,
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
        bool exec_entire_plan,
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

static pair<vector<Fact>, vector<vector<pair<int, ap_float>>>> get_precondition_flaws(
    const OperatorProxy &op, const vector<int> &current_state,
    const unordered_set<int> &blacklisted_variables, std::unordered_map<int, std::unordered_set<int>> deps) {
    vector<Fact> flaws;
    vector<vector<pair<int, ap_float>>> regular_numeric_flaws;
    for (FactProxy pre : op.get_preconditions()) {
        int var_id = pre.get_variable().get_id();
        if (blacklisted_variables.count(var_id) == 0
            && current_state[var_id] != pre.get_value()) {
            flaws.emplace_back(var_id, pre.get_value());
            regular_numeric_flaws.emplace_back();
            regular_numeric_flaws.back().reserve(deps[var_id].size());
            for (int dep_var_id : deps[var_id]) {
                // Use NaN as placeholder - actual split value is determined later in get_flaws
                regular_numeric_flaws.back().emplace_back(dep_var_id, std::numeric_limits<ap_float>::quiet_NaN()); 
            }

        }
    }

    return make_pair(flaws, regular_numeric_flaws);
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

static pair<vector<Fact>, vector<vector<pair<int, ap_float>>>> get_goal_flaws(
    const TaskProxy &task_proxy, const vector<int> &current_state,
    const unordered_set<int> &blacklisted_variables,
    std::unordered_map<int, std::unordered_set<int>> deps) {
    vector<Fact> flaws;
    vector<vector<pair<int, ap_float>>> regular_numeric_flaws;
    
    // First, collect non-derived goals directly
    for (const FactProxy &goal : task_proxy.get_goals()) {
        int var_id = goal.get_variable().get_id();
        if (!is_derived_variable(task_proxy, var_id)) {
            if (blacklisted_variables.count(var_id) == 0
                && current_state[var_id] != goal.get_value()) {
                flaws.emplace_back(var_id, goal.get_value());
                regular_numeric_flaws.emplace_back();
                regular_numeric_flaws.back().reserve(deps[var_id].size());
                for (int dep_var_id : deps[var_id]) {
                    // Use NaN as placeholder - actual split value is determined later in get_flaws
                    regular_numeric_flaws.back().emplace_back(dep_var_id, std::numeric_limits<ap_float>::quiet_NaN());
                }
            }
        }
    }
    
    // Reconstruct goals from goal axioms (numeric goals are compiled into axioms)
    // There should be at most two axioms: one dummy axiom (no preconditions),
    // and one optional goal axiom that encodes numeric/propositional goals
    assert(task_proxy.get_axioms().size() <= 2);
    
    for (OperatorProxy axiom : task_proxy.get_axioms()) {
        // Goal axioms have preconditions and exactly one effect
        if (!axiom.get_preconditions().empty() && axiom.get_effects().size() == 1) {
            // The preconditions of this goal axiom are the actual goals
            for (FactProxy pre : axiom.get_preconditions()) {
                int var_id = pre.get_variable().get_id();
                if (blacklisted_variables.count(var_id) == 0
                    && current_state[var_id] != pre.get_value()) {
                    flaws.emplace_back(var_id, pre.get_value());
                    regular_numeric_flaws.emplace_back();
                    regular_numeric_flaws.back().reserve(deps[var_id].size());
                    for (int dep_var_id : deps[var_id]) {
                        // Use NaN as placeholder - actual split value is determined later in get_flaws
                        regular_numeric_flaws.back().emplace_back(dep_var_id, std::numeric_limits<ap_float>::quiet_NaN());
                    }
                }
            }
        }
    }
    
    return make_pair(flaws, regular_numeric_flaws);
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


vector<Fact> CEGAR::get_flaws(
    const TaskProxy &task_proxy, const State &concrete_init,
    const DomainAbstraction &abstraction) const {

    // Clear any previously detected numeric flaws
    detected_numeric_flaws.clear();

    // Initialize propositional state
    vector<int> current_state;
    current_state.reserve(concrete_init.size());
    for (int i = 0; i < concrete_init.size(); ++i) {
        current_state.push_back(concrete_init[i].get_value());
    }
    
    // Initialize numeric state
    vector<ap_float> numeric_state = g_root_task()->get_initial_state_numeric_values();
    
    g_axiom_evaluator->evaluate_arithmetic_axioms(numeric_state);
    g_axiom_evaluator->evaluate(current_state, numeric_state);

    vector<vector<int>> wildcard_plan = abstraction.get_plan();
    vector<Fact> flaws;

    // Helper: decode current abstract state (propositional + numeric partitions)
    auto decode_abstract_state_compact = [&](const vector<int> &prop_state,
                                             const vector<ap_float> &num_state) -> string {
        const DomainMapping &dm = abstraction.get_domain_mapping();
        const NumericDomainMappingType &ndm = abstraction.get_numeric_domain_mapping();
        stringstream ss;
        ss << "[";
        // Propositional variables in abstraction
        bool first = true;
        for (size_t i = 0; i < dm.size(); ++i) {
            if (!dm[i].empty()) {
                if (!first) ss << ", ";
                first = false;
                int concrete_val = prop_state[i];
                ss << "v" << i << "=" << concrete_val << " (" << dm[i][concrete_val] << ")";
            }
        }
        // Numeric partitions
        for (size_t i = 0; i < ndm.size(); ++i) {
            if (ndm[i]->get_num_partitions() == 1) continue; // Skip trivial numeric variables
            if (!first) ss << ", ";
            first = false;
            ss << "num" << i << "=" << num_state[i];
        }
        ss << "]";
        return ss.str();
    };

    
    regular_numeric_var_values.clear();
    regular_numeric_var_values.reserve(local_to_global_regular_numeric_var_ids.size());

    for (size_t i = 0; i < local_to_global_regular_numeric_var_ids.size(); ++i) {
        int var_id = local_to_global_regular_numeric_var_ids[i];
        NumericVariableProxy num_var = task_proxy.get_numeric_variables()[var_id];
        if (num_var.get_var_type() == numType::regular || num_var.get_var_type() == numType::constant) { //TODO: Why constants?
            std::unordered_set<ap_float> values;
            if (i < already_split.size() && 
                already_split[i].count(numeric_state[var_id]) == 0) {
                values.insert(numeric_state[var_id]);
            }
            regular_numeric_var_values.push_back(std::move(values));
        }
    }

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
    logger->log(Verbosity::DEBUG, "PLAN: State 0 (start): ", decode_abstract_state_compact(current_state, numeric_state));
    
    int step_num = 0;
    for (vector<int> &equivalent_ops : wildcard_plan) {
        //assert(flaws.empty()); RANDOM FLAW

        
        
        for (int op_id : equivalent_ops) {
            OperatorProxy op = task_proxy.get_operators()[op_id];
            string op_name = op.get_name();

            
            
            // Check propositional preconditions
            pair<vector<Fact>, vector<vector<pair<int, ap_float>>>> flaw_data =
                get_precondition_flaws(
                    op, current_state, blacklisted_variables, comparison_axiom_dependencies);

            vector<Fact> operator_flaws = flaw_data.first;
            vector<vector<pair<int, ap_float>>> regular_numeric_flaws = flaw_data.second;
            if (operator_flaws.empty()) {
                // Propositional preconditions satisfied - apply operator
                // In standard CEGAR (exec_entire_plan=false), clear any previously accumulated flaws
                // since we successfully applied an operator after them.
                if (!exec_entire_plan) {
                    flaws.clear();
                    detected_numeric_flaws.clear();
                }
                
                string decoded_state = decode_abstract_state_compact(current_state, numeric_state);
                logger->log(Verbosity::DEBUG, "[PLAN] ", decoded_state, ", ", op_name);
                apply_op_to_state(current_state, op);
                apply_numeric_effects(numeric_state, op);
                g_axiom_evaluator->evaluate_arithmetic_axioms(numeric_state);
                g_axiom_evaluator->evaluate(current_state, numeric_state);

                for (size_t i = 0; i < local_to_global_regular_numeric_var_ids.size(); ++i) {
                    int var_id = local_to_global_regular_numeric_var_ids[i];
                    // Use index i (not var_id) to access already_split since it's indexed by position in regular_numeric_var_ids
                    ap_float val = numeric_state[var_id];
                    if (i < already_split.size() && 
                        already_split[i].count(val) == 0 &&
                        regular_numeric_var_values[i].count(val) == 0) {
                        regular_numeric_var_values[i].insert(val);
                    }
                }

                break;
            } else {
                // We have precondition flaws
                // Check if any precondition flaw is on a comparison axiom variable
                NumericVariablesProxy num_vars = task_proxy.get_numeric_variables();
                for (size_t i = 0; i < operator_flaws.size(); ++i) {
                    Fact &flaw = operator_flaws[i];
                    vector<pair<int, ap_float>> &reg_numeric_flaws = regular_numeric_flaws[i];
                    
                    // Skip comparison flaws that are already refined and have no new split values
                    if (!should_add_comparison_flaw(flaw.var)) {
                        logger->log(Verbosity::DEBUG, "  Skipping comparison flaw var=", flaw.var,
                                    " (already refined, no new observed values)");
                        continue;
                    }
                    
                    flaws.push_back(flaw);

                    // Build inner vector of numeric flaws for this propositional flaw
                    vector<NumericFlaw> numeric_flaws_for_this_prop_flaw;
                    
                    for (pair<int, ap_float> &reg_flaw : reg_numeric_flaws) {
                        int numeric_var_id = reg_flaw.first;
                        ap_float concrete_value = reg_flaw.second;
                        logger->log(Verbosity::DEBUG, "  [DEBUG FLAW] Comparison axiom flaw: var=", flaw.var,
                                        " value=", flaw.value,
                                        " op=", op_name);
                       
                    }
                    
                    logger->log(Verbosity::DEBUG, "[DEBUG  Already_split]");
                    for (pair<int, ap_float> &reg_flaw : reg_numeric_flaws) {
                        int id = reg_flaw.first;
                        logger->log(Verbosity::DEBUG, "   num_", id, " : ", num_vars[id].get_var_type());
                    }
                    for (pair<int, ap_float> &reg_flaw : reg_numeric_flaws) {
                        int numeric_var_id = reg_flaw.first;
                        assert(num_vars[numeric_var_id].get_var_type() != numType::derived);

                        ap_float concrete_value = numeric_state[numeric_var_id];
                        logger->log(Verbosity::DEBUG, "    Detected numeric flaw on num_", numeric_var_id,
                                         " with concrete value ", concrete_value);

                        int local_numeric_var_index = global_to_local_regular_numeric_var_ids[numeric_var_id];
                        assert(local_numeric_var_index != -1);

                        ap_float split_value = concrete_value;
                        if (!regular_numeric_var_values[local_numeric_var_index].empty()) {
                            split_value = *regular_numeric_var_values[local_numeric_var_index].begin();
                            logger->log(Verbosity::DEBUG, "   LAST regular_numeric_var_values[", numeric_var_id, "] = ",
                                           split_value);
                        }
                        numeric_flaws_for_this_prop_flaw.emplace_back(
                            numeric_var_id, split_value, flaw.var);
                      
                    }
                    // Add the inner vector to the 2D structure
                    detected_numeric_flaws.push_back(numeric_flaws_for_this_prop_flaw);
                }

                // When exec_entire_plan=true, continue executing the plan even after flaws
                // to accumulate all flaws for experimental analysis.
                if (exec_entire_plan) {
                    apply_op_to_state(current_state, op);
                    apply_numeric_effects(numeric_state, op);
                    g_axiom_evaluator->evaluate_arithmetic_axioms(numeric_state);
                    g_axiom_evaluator->evaluate(current_state, numeric_state);
                }
            }
        }

        

        
        if (!flaws.empty()) {
            string decoded_state = decode_abstract_state_compact(current_state, numeric_state);
            logger->log(Verbosity::DEBUG, "[PLAN] ", decoded_state, " -- FLAW at step ", step_num);
            logger->log_no_endl(Verbosity::DEBUG, "  Propositional flaws: ");
            for (const Fact &flaw : flaws) {
                logger->log_no_endl(Verbosity::DEBUG, "fdr_", flaw.var, "=", flaw.value, " ");
            }
            logger->log(Verbosity::DEBUG, "");
            logger->log_no_endl(Verbosity::DEBUG, "  Numeric flaws: ");
            for (const std::vector<NumericFlaw> &num_flaw_vec : detected_numeric_flaws) {
                for (const NumericFlaw &num_flaw : num_flaw_vec) {
                    logger->log_no_endl(Verbosity::DEBUG, "num_", num_flaw.numeric_var_id,
                                           " (concrete value: ", num_flaw.concrete_value, ") ");
                }
            }
            logger->log(Verbosity::DEBUG, "");

            // DEBUG regular_numeric_var_values
            for (size_t i = 0; i < regular_numeric_var_values.size(); ++i) {
                logger->log_no_endl(Verbosity::DEBUG, "Numeric variable num_", local_to_global_regular_numeric_var_ids[i],
                                        " observed values during plan execution: ");
                for (const ap_float &val : regular_numeric_var_values[i]) {
                    logger->log_no_endl(Verbosity::DEBUG, val, " ");
                }
                logger->log(Verbosity::DEBUG, "");
            }
            // DEBUG already_split
            for (size_t i = 0; i < already_split.size(); ++i) {
                logger->log_no_endl(Verbosity::DEBUG, "Numeric variable num_", local_to_global_regular_numeric_var_ids[i],
                                        " already split values: ");
                for (const ap_float &val : already_split[i]) {
                    logger->log_no_endl(Verbosity::DEBUG, val, " ");
                }
                logger->log(Verbosity::DEBUG, "");
            }
           
            // In standard CEGAR (exec_entire_plan=false), return flaws immediately 
            // when detected to refine and restart.
            if (!exec_entire_plan) {
                return flaws;
            }
        }
        logger->log(Verbosity::DEBUG, "");
        step_num++;
    }

    string decoded_state = decode_abstract_state_compact(current_state, numeric_state);
    logger->log(Verbosity::DEBUG, "[PLAN] ", decoded_state);

    for (size_t i = 0; i < local_to_global_regular_numeric_var_ids.size(); ++i) {
        int var_id = local_to_global_regular_numeric_var_ids[i];
        // Use index i (not var_id) to access already_split since it's indexed by position in regular_numeric_var_ids
        ap_float val = numeric_state[var_id];
        if (i < already_split.size() && 
            already_split[i].count(val) == 0 &&
            regular_numeric_var_values[i].count(val) == 0) {
            regular_numeric_var_values[i].insert(val);
        }
    }

 

    // Check goal flaws
    // In standard CEGAR (exec_entire_plan=false), we should have no plan execution flaws
    // since we return early. In exec_entire_plan mode, flaws may have accumulated.
    assert(exec_entire_plan || flaws.empty());
    
    pair<vector<Fact>, vector<vector<pair<int, ap_float>>>> goal_flaw_data =
        get_goal_flaws(task_proxy, current_state, blacklisted_variables,
                       comparison_axiom_dependencies);
    
    vector<Fact> goal_flaws = goal_flaw_data.first;
    vector<vector<pair<int, ap_float>>> goal_numeric_flaws = goal_flaw_data.second;
    
    // Process goal flaws similar to precondition flaws
    NumericVariablesProxy num_vars = task_proxy.get_numeric_variables();
    for (size_t i = 0; i < goal_flaws.size(); ++i) {
        Fact &flaw = goal_flaws[i];
        vector<pair<int, ap_float>> &reg_numeric_flaws = goal_numeric_flaws[i];
        
        // Skip comparison flaws that are already refined and have no new split values
        if (!should_add_comparison_flaw(flaw.var)) { //TODO: I think that is deprecated....
            logger->log(Verbosity::DEBUG, "  Skipping comparison goal flaw var=", flaw.var,
                        " (already refined, no new observed values)");
            continue;
        }
        
        flaws.push_back(flaw);
        
        // Build inner vector of numeric flaws for this propositional flaw
        vector<NumericFlaw> numeric_flaws_for_this_prop_flaw;
        
        for (pair<int, ap_float> &reg_flaw : reg_numeric_flaws) {
            int numeric_var_id = reg_flaw.first;
            assert(num_vars[numeric_var_id].get_var_type() != numType::derived);
            
            ap_float concrete_value = numeric_state[numeric_var_id];
            logger->log(Verbosity::DEBUG, "    Detected numeric goal flaw on num_", numeric_var_id,
                            " with concrete value ", concrete_value);
            
            int local_numeric_var_index = global_to_local_regular_numeric_var_ids[numeric_var_id];
            assert(local_numeric_var_index != -1);
            
            ap_float split_value = concrete_value;
            if (!regular_numeric_var_values[local_numeric_var_index].empty()) {
                split_value = *regular_numeric_var_values[local_numeric_var_index].begin();
                logger->log(Verbosity::DEBUG, "   LAST regular_numeric_var_values[", numeric_var_id, "] = ",
                                split_value);
            }
            numeric_flaws_for_this_prop_flaw.emplace_back(
                numeric_var_id, split_value, flaw.var);
        }
        
        // Add the inner vector to the 2D structure
        detected_numeric_flaws.push_back(numeric_flaws_for_this_prop_flaw);
    }

    // DEBUG: Print goal flaw summary with already_split and observed values
    if (!goal_flaws.empty()) {
        logger->log(Verbosity::DEBUG, "\n=== GOAL FLAW DEBUG ===");
        logger->log(Verbosity::DEBUG, "  Number of goal flaws: ", goal_flaws.size());
        for (size_t i = 0; i < goal_flaws.size(); ++i) {
            const Fact &flaw = goal_flaws[i];
            logger->log(Verbosity::DEBUG, "  Goal flaw [", i, "]: var", flaw.var, 
                       " = ", flaw.value);
            
            // Check if this is a comparison axiom
            if (is_comparison_axiom_variable(flaw.var)) {
                logger->log(Verbosity::DEBUG, "    (comparison axiom variable)");
            }
            
            // Show associated numeric flaws  
            if (i < detected_numeric_flaws.size()) {
                logger->log(Verbosity::DEBUG, "    numeric flaws: ", detected_numeric_flaws[i].size());
                for (const NumericFlaw &nf : detected_numeric_flaws[i]) {
                    logger->log(Verbosity::DEBUG, "      num_", nf.numeric_var_id, 
                               " concrete_value=", nf.concrete_value,
                               " prop_var_id=", nf.prop_var_id);
                    
                    // Check already_split status for the numeric variable
                    int local_idx = global_to_local_regular_numeric_var_ids[nf.numeric_var_id];
                    if (local_idx >= 0 && local_idx < static_cast<int>(already_split.size())) {
                        logger->log_no_endl(Verbosity::DEBUG, "        already_split values: {");
                        bool first = true;
                        for (ap_float v : already_split[local_idx]) {
                            if (!first) logger->log_no_endl(Verbosity::DEBUG, ", ");
                            first = false;
                            logger->log_no_endl(Verbosity::DEBUG, v);
                        }
                        logger->log(Verbosity::DEBUG, "}");
                    }
                    
                    // Check observed values
                    if (local_idx >= 0 && local_idx < static_cast<int>(regular_numeric_var_values.size())) {
                        logger->log_no_endl(Verbosity::DEBUG, "        observed values: {");
                        bool first = true;
                        for (ap_float v : regular_numeric_var_values[local_idx]) {
                            if (!first) logger->log_no_endl(Verbosity::DEBUG, ", ");
                            first = false;
                            logger->log_no_endl(Verbosity::DEBUG, v);
                        }
                        logger->log(Verbosity::DEBUG, "}");
                    }
                }
            }
        }
        logger->log(Verbosity::DEBUG, "=== END GOAL FLAW DEBUG ===\n");
    }

    return flaws;
}

bool CEGAR::fix_flaws(
    vector<Fact> &&flaws, DomainMapping &domain_mapping,
    int abstraction_size) {
    switch (flaw_treatment) {
        case FlawTreatment::RANDOM_SINGLE_ATOM:
            return fix_single_random_flaw(move(flaws), domain_mapping, abstraction_size);
        case FlawTreatment::ONE_SPLIT_PER_ATOM:
            return fix_flaws_per_atom(move(flaws), domain_mapping, abstraction_size);
        case FlawTreatment::ONE_SPLIT_PER_VARIABLE:
            return fix_flaws_per_variable(move(flaws), domain_mapping, abstraction_size);
        case FlawTreatment::MAX_REFINED_SINGLE_ATOM:
            return fix_single_flaw_max_refined(move(flaws), domain_mapping, abstraction_size);
    }
    assert(false);
    return false;
}

bool CEGAR::fix_single_random_flaw(
    vector<Fact> &&flaws, DomainMapping &domain_mapping,
    int abstraction_size) {
    // TODO: Number of repetitions set to log(|flaws|) + 1 is somewhat arbitrary...
    int repetitions = ceil(1 + std::log(flaws.size()));
    for (int i = 0; i < repetitions; ++i) {
        // Choose a random index to preserve the mapping to detected_numeric_flaws
        int chosen_idx = rng->random(flaws.size());
        Fact fact = flaws[chosen_idx];
        
        if (can_refine_variable(abstraction_size, fact.var)) {
            add_variable_to_abstraction_if_necessary(fact.var, domain_mapping);
            
            if (is_comparison_axiom_variable(fact.var)) {
                // Comparison axiom variables have domain {0=true, 1=false, 2=unevaluated}
                // Flaws always occur with value 0 (true), and refinement splits true from {false, unevaluated}
                assert(fact.value == 0);
                
                // If already refined propositionally (size >= 2), only do numeric refinement
                if (abstract_domain_sizes[fact.var] >= 2) {
                    logger->log(Verbosity::INFO, "Comparison axiom var ", fact.var,
                               " already refined propositionally, selecting for numeric refinement");
                } else {
                    // Do the propositional refinement
                    domain_mapping[fact.var][0] = 1;
                    abstract_domain_sizes[fact.var] = 2;
                    logger->log(Verbosity::INFO, "Refined propositional var ", fact.var,
                               " (comparison axiom) at value ", fact.value);
                }
            } else {
                int old_size = abstract_domain_sizes[fact.var];
                domain_mapping[fact.var][fact.value] = abstract_domain_sizes[fact.var];
                abstract_domain_sizes[fact.var] += 1;
                logger->log(Verbosity::INFO, "Refined propositional var ", fact.var,
                           " at value ", fact.value,
                           " (abstract domain size: ", old_size, " -> ", abstract_domain_sizes[fact.var], ")");
            }
            // Record the chosen flaw INDEX (not var ID) for numeric flaw lookup
            last_selected_flaw_indices.clear();
            last_selected_flaw_indices.push_back(chosen_idx);

            return true;
        } else {
            logger->log(Verbosity::DEBUG, "Variable ", fact.var,
                           " cannot be refined (domain size exceeds limit or blacklisted).");
        }
    }
    return false;
}

/* Chooses a flaw for that the increase in abstraction size is the smallest among all given ones
 * -> Leads to the smallest possible increase in abstraction size in every iteration 
 * 
 * For numeric planning, we need to consider pairs of (comparison var, numeric var) together.
 * The ranking score is the SUM of abstract domain sizes:
 * - For non-comparison vars: score = abstract_domain_sizes[var]
 * - For comparison vars: score = abstract_domain_sizes[prop_var] + numeric_partitions[num_var]
 *   (if comparison already refined, prop contributes 2 to score but 0 to growth)
 * Higher score = less growth when refined = preferred candidate.
 */
bool CEGAR::fix_single_flaw_max_refined(
        vector<Fact> &&flaws, DomainMapping &domain_mapping,
        int abstraction_size) {
    
    // Unified candidate structure for both propositional-only and comparison+numeric pairs
    struct UnifiedCandidate {
        int flaw_idx;           // Index in flaws vector
        int prop_var_id;        // Propositional variable ID
        int prop_value;         // Propositional value (for non-comparison refinement)
        bool is_comparison;     // Whether this is a comparison axiom variable
        // For comparison flaws, also store the numeric candidate info
        int numeric_var_id;     // -1 if non-comparison
        ap_float split_value;   // Only valid if is_comparison
        int local_numeric_id;   // Local ID for already_split tracking
        int score;              // Higher = less growth = preferred
    };
    
    vector<UnifiedCandidate> all_candidates;
    int num_flaws = static_cast<int>(flaws.size());
    
    for (int i = 0; i < num_flaws; ++i) {
        const Fact &flaw = flaws[i];
        int prop_var_id = flaw.var;
        
        // Skip if propositional variable can't be refined
        if (!can_refine_variable(abstraction_size, prop_var_id)) {
            continue;
        }
        
        if (is_comparison_axiom_variable(prop_var_id)) {
            // For comparison axioms, we need valid numeric candidates
            // Check detected_numeric_flaws for this flaw index
            if (i >= static_cast<int>(detected_numeric_flaws.size())) {
                continue;  // No numeric flaws recorded for this flaw
            }
            
            const vector<NumericFlaw> &numeric_flaws = detected_numeric_flaws[i];
            int prop_score = abstract_domain_sizes[prop_var_id];
            
            // Build candidates for each valid numeric flaw
            for (const NumericFlaw &nf : numeric_flaws) {
                int num_var_id = nf.numeric_var_id;
                ap_float split_val = nf.concrete_value;
                
                // Check if numeric variable can be refined (includes size limit check)
                if (!can_refine_numeric_variable(abstraction_size, num_var_id, 
                                                  TaskProxy(*g_root_task()))) {
                    continue;
                }
                
                int local_id = global_to_local_regular_numeric_var_ids[num_var_id];
                if (local_id < 0 || local_id >= static_cast<int>(already_split.size())) {
                    continue;
                }
                
                // Check if already split at this value
                if (already_split[local_id].count(split_val)) {
                    continue;
                }
                
                // Compute combined score: prop_score + numeric_partitions
                int num_partitions = numeric_domain_mapping[num_var_id]->get_num_partitions();
                int combined_score = prop_score + num_partitions;
                
                UnifiedCandidate cand;
                cand.flaw_idx = i;
                cand.prop_var_id = prop_var_id;
                cand.prop_value = flaw.value;
                cand.is_comparison = true;
                cand.numeric_var_id = num_var_id;
                cand.split_value = split_val;
                cand.local_numeric_id = local_id;
                cand.score = combined_score;
                
                all_candidates.push_back(cand);
            }
            // If no valid numeric candidates for this comparison, skip it entirely
        } else {
            // Non-comparison propositional variable: straightforward
            int score = abstract_domain_sizes[prop_var_id];
            
            UnifiedCandidate cand;
            cand.flaw_idx = i;
            cand.prop_var_id = prop_var_id;
            cand.prop_value = flaw.value;
            cand.is_comparison = false;
            cand.numeric_var_id = -1;
            cand.split_value = 0;
            cand.local_numeric_id = -1;
            cand.score = score;
            
            all_candidates.push_back(cand);
        }
    }
    
    // If no candidates at all, we can't fix any flaws
    if (all_candidates.empty()) {
        logger->log(Verbosity::DEBUG, "fix_single_flaw_max_refined: No valid candidates");
        return false;
    }
    
    // Find maximum score
    int max_score = 0;
    for (const auto &c : all_candidates) {
        if (c.score > max_score) {
            max_score = c.score;
        }
    }
    
    // Collect all candidates with max score
    vector<int> best_candidate_indices;
    for (size_t i = 0; i < all_candidates.size(); ++i) {
        if (all_candidates[i].score == max_score) {
            best_candidate_indices.push_back(static_cast<int>(i));
        }
    }
    
    // Choose one randomly
    int chosen_cand_idx = *rng->choose(best_candidate_indices);
    const UnifiedCandidate &chosen = all_candidates[chosen_cand_idx];
    
    // Apply the refinement
    add_variable_to_abstraction_if_necessary(chosen.prop_var_id, domain_mapping);
    
    if (chosen.is_comparison) {
        // Comparison axiom: refine propositionally if not already, then refine numeric
        assert(chosen.prop_value == 0);
        
        bool prop_refined = false;
        if (abstract_domain_sizes[chosen.prop_var_id] < 2) {
            domain_mapping[chosen.prop_var_id][0] = 1;
            abstract_domain_sizes[chosen.prop_var_id] = 2;
            prop_refined = true;
            logger->log(Verbosity::INFO, "Refined comparison var ", chosen.prop_var_id,
                       " (max_refined mode)");
        }
        
        // Now refine the numeric variable
        // Build concrete_values map for determine_include_in_lower
        std::unordered_map<int, ap_float> concrete_values;
        // Gather all numeric values for this prop_var from detected_numeric_flaws
        if (chosen.flaw_idx < static_cast<int>(detected_numeric_flaws.size())) {
            for (const NumericFlaw &nf : detected_numeric_flaws[chosen.flaw_idx]) {
                concrete_values[nf.numeric_var_id] = nf.concrete_value;
            }
        }
        
        bool include_in_lower = determine_include_in_lower(
            chosen.prop_var_id, chosen.numeric_var_id, chosen.split_value,
            concrete_values, TaskProxy(*g_root_task()));
        
        int old_partitions = numeric_domain_mapping[chosen.numeric_var_id]->get_num_partitions();
        int new_partitions = numeric_domain_mapping[chosen.numeric_var_id]->split_at(
            chosen.split_value, include_in_lower);
        already_split[chosen.local_numeric_id].insert(chosen.split_value);
        
        if (new_partitions > old_partitions) {
            numeric_domain_sizes[chosen.numeric_var_id] = new_partitions;
            numeric_var_refinement_count[chosen.numeric_var_id]++;
            logger->log(Verbosity::INFO, "Refined num_", chosen.numeric_var_id,
                       " at ", chosen.split_value,
                       " (partitions: ", old_partitions, " -> ", new_partitions, ")",
                       " via max_refined mode");
        }
        
        // Clear flaw indices - we've already handled the numeric refinement here
        // so fix_numeric_flaws should NOT be called with these flaws
        last_selected_flaw_indices.clear();
        
        return prop_refined || (new_partitions > old_partitions);
    } else {
        // Non-comparison: standard propositional refinement
        domain_mapping[chosen.prop_var_id][chosen.prop_value] = abstract_domain_sizes[chosen.prop_var_id];
        abstract_domain_sizes[chosen.prop_var_id] += 1;
        
        logger->log(Verbosity::INFO, "Refined propositional var ", chosen.prop_var_id,
                   " at value ", chosen.prop_value,
                   " (abstract domain size: ", abstract_domain_sizes[chosen.prop_var_id] - 1,
                   " -> ", abstract_domain_sizes[chosen.prop_var_id], ") via max_refined mode");
        
        // Record the flaw index for potential numeric flaw lookup (though non-comparison won't have any)
        last_selected_flaw_indices.clear();
        last_selected_flaw_indices.push_back(chosen.flaw_idx);
        return true;
    }
}

bool CEGAR::fix_flaws_per_atom(
    vector<Fact> &&flaws, DomainMapping &domain_mapping,
    int abstraction_size) {
    // FIXME: Bias for variables with low index.
    // NOTE: Sorting breaks the correspondence with detected_numeric_flaws!
    // We need to track original indices before sorting.
    vector<pair<Fact, int>> flaws_with_indices;
    for (size_t i = 0; i < flaws.size(); ++i) {
        flaws_with_indices.push_back({flaws[i], static_cast<int>(i)});
    }
    sort(flaws_with_indices.begin(), flaws_with_indices.end(),
         [](const pair<Fact, int> &a, const pair<Fact, int> &b) {
             return a.first < b.first;
         });
    
    Fact last_flaw(-1, -1);
    for (const auto &[flaw, orig_idx] : flaws_with_indices) {
        if (flaw == last_flaw) {
            // duplicate
            continue;
        }
        if (can_refine_variable(abstraction_size, flaw.var)) {
            add_variable_to_abstraction_if_necessary(flaw.var, domain_mapping);
            if (is_comparison_axiom_variable(flaw.var)) {
                // Comparison axiom variables have domain {0=true, 1=false, 2=unevaluated}
                // Flaws always occur with value 0 (true), and refinement splits true from {false, unevaluated}
                assert(flaw.value == 0);
                if (abstract_domain_sizes[flaw.var] < 2) {
                    domain_mapping[flaw.var][0] = 1;
                    abstract_domain_sizes[flaw.var] = 2;
                }
            } else {
                domain_mapping[flaw.var][flaw.value] =
                    abstract_domain_sizes[flaw.var];
                abstract_domain_sizes[flaw.var] += 1;
            }
            // Track all flaw indices we refine
            last_selected_flaw_indices.push_back(orig_idx);
            last_flaw = flaw;
        }
    }
    return last_flaw != Fact(-1, -1);
}

bool CEGAR::fix_flaws_per_variable(
    vector<Fact> &&flaws, DomainMapping &domain_mapping,
    int abstraction_size) {
    // FIXME: Bias for variables with low index.
    // NOTE: Sorting breaks the correspondence with detected_numeric_flaws!
    // We need to track original indices before sorting.
    vector<pair<Fact, int>> flaws_with_indices;
    for (size_t i = 0; i < flaws.size(); ++i) {
        flaws_with_indices.push_back({flaws[i], static_cast<int>(i)});
    }
    sort(flaws_with_indices.begin(), flaws_with_indices.end(),
         [](const pair<Fact, int> &a, const pair<Fact, int> &b) {
             return a.first < b.first;
         });
    
    Fact last_flaw(-1, -1);
    for (const auto &[flaw, orig_idx] : flaws_with_indices) {
        if (flaw.var > last_flaw.var
            && can_refine_variable(abstraction_size, flaw.var)) {
            /* Introduce new abstract value only for every new variable,
               opposed to for every atom as in *fix_flaws_per_atom* above. */
            add_variable_to_abstraction_if_necessary(flaw.var, domain_mapping);
            if (is_comparison_axiom_variable(flaw.var)) {
                // Comparison axiom variables have domain {0=true, 1=false, 2=unevaluated}
                // Flaws always occur with value 0 (true), and refinement splits true from {false, unevaluated}
                assert(flaw.value == 0);
                if (abstract_domain_sizes[flaw.var] < 2) {
                    domain_mapping[flaw.var][0] = 1;
                    abstract_domain_sizes[flaw.var] = 2;
                }
            } else {
                abstract_domain_sizes[flaw.var] += 1;
            }
            // Track each flaw index we refine
            last_selected_flaw_indices.push_back(orig_idx);
        } else if (flaw.var != last_flaw.var || flaw.value == last_flaw.value) {
            // Duplicate or does not fit size limit.
            continue;
        }
        if (!is_comparison_axiom_variable(flaw.var)) {
            domain_mapping[flaw.var][flaw.value] =
                abstract_domain_sizes[flaw.var] - 1;
        }
        last_flaw = flaw;
    }
    return last_flaw != Fact(-1, -1);
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
            // Constants should have a single partition at their exact value
            // Use ConstantMapping which prevents splitting
            ap_float const_value = num_var.get_initial_state_value();
            //std::cout << "  num_" << i << " (" << num_var.get_name() 
            //         << ") is CONSTANT with value " << const_value 
            //         << " - creating ConstantMapping" << std::endl;
            
            numeric_domain_mapping.push_back(std::make_unique<ConstantMapping>(const_value));
        } else if (var_type == numType::derived) {
            //td::cout << "  num_" << i << " (" << num_var.get_name() 
            //        << ") is DERIVED - skipping explicit mapping (implicitly abstracted)" << std::endl;
            //TODO: Can we get rid of this?
            numeric_domain_mapping.push_back(std::make_unique<ConstantMapping>(0));
        } else if (var_type == numType::regular) {
            //std::cout << "  num_" << i << " (" << num_var.get_name() 
            //         << ") is REGULAR - creating refinable mapping" << std::endl;
            
            if (numeric_split_strategy == NumericSplitStrategy::EXCLUSION) {
                numeric_domain_mapping.push_back(std::make_unique<ExclusionSplitMapping>());
            } else {
                // Default: StandardSplitMapping
                numeric_domain_mapping.push_back(std::make_unique<StandardSplitMapping>());
            }
        } else {
            //std::cout << "  num_" << i << " (" << num_var.get_name() 
            //         << ") is OTHER/UNKNOWN (type=" << static_cast<int>(var_type)
            //         << ") - creating refinable mapping" << std::endl;
            
            if (numeric_split_strategy == NumericSplitStrategy::EXCLUSION) {
                numeric_domain_mapping.push_back(std::make_unique<ExclusionSplitMapping>());
            } else {
                numeric_domain_mapping.push_back(std::make_unique<StandardSplitMapping>());
            }
        }
    }
    
    // Apply initial splits for numeric variables that are in init_split_var_ids
    // Numeric var IDs are encoded as num_prop_vars + numeric_var_id
    int num_prop_vars = task_proxy.get_variables().size();
    
    for (int encoded_var_id : init_split_var_ids) {
        if (encoded_var_id < num_prop_vars) {
            // This is a propositional variable, skip (handled in compute_initial_domain_mapping)
            continue;
        }
        
        int numeric_var_id = encoded_var_id - num_prop_vars;
        if (numeric_var_id < 0 || numeric_var_id >= num_numeric_variables) {
            continue;
        }
        
        // Check if blacklisted
        if (blacklisted_numeric_variables.count(numeric_var_id) > 0) {
            continue;
        }
        
        NumericVariableProxy num_var = num_vars[numeric_var_id];
        numType var_type = num_var.get_var_type();
        
        // Only apply init split to regular numeric variables
        if (var_type != numType::regular) {
            continue;
        }
        
        // For IDENTITY and GOAL_VALUE_OR_RANDOM_IF_NON_GOAL:
        // Split at initial value with random boundary inclusion
        // (Numeric variables are never goals, so both methods behave the same)
        if (init_split_method == InitSplitMethod::IDENTITY ||
            init_split_method == InitSplitMethod::GOAL_VALUE_OR_RANDOM_IF_NON_GOAL) {
            
            ap_float init_value = num_var.get_initial_state_value();
            bool include_in_lower = rng->random(2) == 0;  // Random flip
            
            logger->log(Verbosity::DEBUG, "Initial split for numeric variable num_", numeric_var_id,
                           " (", num_var.get_name(), ") at init value ", init_value,
                           ", include_in_lower=", include_in_lower);
            
            numeric_domain_mapping[numeric_var_id]->split_at(init_value, include_in_lower);
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
    //cout << "DEBUG AXIOM MAP: Building assignment axiom dependency graph" << endl;
    //cout << "DEBUG AXIOM MAP: Total numeric variables: " << num_numeric_vars << endl;
    //cout << "DEBUG AXIOM MAP: Assignment axioms: " << assignment_axioms.size() << endl;

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
        //get var type
        numType num_type = num_var.get_var_type();
        if (num_type == numType::regular) {
            is_regular[i] = true;
            //cout << "  num_" << var_id << " (" << num_var.get_name() << ") is REGULAR" << endl;
        }
    }

    
    // Helper function to recursively find all regular (non-derived) variables
    // that a given variable depends on
    auto find_regular_dependencies = [&](int var_id, auto& find_regular_dependencies_ref) -> unordered_set<int> {
        unordered_set<int> regular_vars;
        
        assert(var_id >= 0 && var_id < num_numeric_vars);
        
        if (is_regular[var_id]) {
            // This is a regular variable - add it
            regular_vars.insert(var_id);
        } else {
            // This is a derived variable - recurse on its dependencies
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

    // Now build the comparison axiom mapping
    ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
    logger->log(Verbosity::DEBUG, "DEBUG: Building comparison axiom mapping, total axioms: ", comparison_axioms.size());
    for (ComparisonAxiomProxy axiom : comparison_axioms) {
        // Get the propositional variable created by this comparison axiom
        FactProxy true_fact = axiom.get_true_fact();
        FactProxy false_fact = axiom.get_false_fact();
        assert(true_fact.get_variable().get_id() == false_fact.get_variable().get_id());
        
        int prop_var_id = true_fact.get_variable().get_id();
        
        logger->log(Verbosity::DEBUG, "DEBUG: Processing comparison axiom for fdr_", prop_var_id,
                        " (", true_fact.get_variable().get_name(), ")");
        
        // Get the numeric variables used in the comparison (may be derived!)
        int left_var_id = axiom.get_left_variable().get_id();
        int right_var_id = axiom.get_right_variable().get_id();
        assert(left_var_id >= 0 && left_var_id < num_numeric_vars);
        assert(right_var_id >= 0 && right_var_id < num_numeric_vars);
        
        // Trace through to find regular variables
        unordered_set<int> regular_vars;
        
        unordered_set<int> left_deps = find_regular_dependencies(left_var_id, find_regular_dependencies);
        regular_vars.insert(left_deps.begin(), left_deps.end());
        
        unordered_set<int> right_deps = find_regular_dependencies(right_var_id, find_regular_dependencies);
        regular_vars.insert(right_deps.begin(), right_deps.end());
        
        // Store the mapping
        comparison_axiom_dependencies[prop_var_id] = regular_vars;
        
        // Store comparison axiom info for determining split direction
        comp_operator comp_op = axiom.get_comparison_operator_type();
        comparison_axiom_info[prop_var_id] = ComparisonInfo{left_var_id, right_var_id, static_cast<int>(comp_op)};
        
        // Debug output for ALL comparison axioms
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
    
    // Print full comparison_axiom_dependencies mapping
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
    
    // PHASE 2: Collect all numeric variables modified by operators
    // This ensures we refine ALL operator-modified variables when numeric flaws occur
    operator_modified_numeric_vars.clear();
    
    logger->log(Verbosity::DEBUG, "DEBUG PHASE2: Collecting operator-modified numeric variables...");
    OperatorsProxy operators = task_proxy.get_operators();
    for (OperatorProxy op : operators) {
        // Check additive effects (NumAss effects)
        for (auto ass_eff_proxy : op.get_ass_effects()) {
            NumAssProxy ass_eff = ass_eff_proxy.get_assignment();
            int affected_var_id = ass_eff.get_affected_variable().get_id();
            
            if (affected_var_id >= 0 && affected_var_id < num_numeric_vars) {
                // Only add if this is a REGULAR variable (not derived)
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
    
    // Print comprehensive axiom dependency tree for CEGAR
    //print_cegar_axiom_trees(task_proxy, is_derived, axiom_dependencies);
}

DomainAbstraction CEGAR::build_abstraction(
    const TaskProxy &task_proxy) {
    logger->log(Verbosity::INFO, "Building domain abstraction...");
    utils::reserve_extra_memory_padding(memory_padding_in_mb);
    utils::CountdownTimer timer(max_time);

    // Blacklist logic axiom variables (derived variables that are NOT comparison axioms)
    // Logic axioms are typically used for goal compilation and should not be refined
    // Only comparison axioms should be refinable
    // MUST be done BEFORE compute_initial_domain_mapping!
    
    // First, collect all comparison axiom variable IDs (into member variable)
    comparison_axiom_var_ids.clear();
    ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
    for (ComparisonAxiomProxy axiom : comparison_axioms) {
        int var_id = axiom.get_true_fact().get_variable().get_id();
        comparison_axiom_var_ids.insert(var_id);
    }
    
    // NOTE: There are always 2 logic axioms that we ignore
    // Now blacklist all axiom variables that are NOT comparison axioms
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
    
    // Debug: Check which variables are derived/axiom variables
    logger->log(Verbosity::DEBUG, "Variable analysis:");
    for (int var_id = 0; var_id < task_proxy.get_variables().size(); ++var_id) {
        VariableProxy var = task_proxy.get_variables()[var_id];
        bool is_comparison = (comparison_axiom_var_ids.count(var_id) > 0);
        bool is_blacklisted = (blacklisted_variables.count(var_id) > 0);
        bool has_mapping = !domain_mapping[var_id].empty();
        logger->log(Verbosity::DEBUG, "  Variable ", var_id, " (", var.get_name(), "): ",
                        "comparison=", (is_comparison ? "yes" : "no"), ", ",
                        "blacklisted=", (is_blacklisted ? "yes" : "no"), ", ",
                        "has_mapping=", (has_mapping ? "yes" : "no"), ", ",
                        "domain_size=", var.get_domain_size());
    }
    
    // Debug: Show logic axioms
    logger->log(Verbosity::DEBUG, "Logic axioms in task: ", task_proxy.get_axioms().size());
    for (size_t i = 0; i < task_proxy.get_axioms().size(); ++i) {
        OperatorProxy axiom = task_proxy.get_axioms()[i];
        logger->log(Verbosity::DEBUG, "  Logic axiom ", i, ": ", axiom.get_name());
        for (EffectProxy eff : axiom.get_effects()) {
            logger->log(Verbosity::DEBUG, "    affects variable ", eff.get_fact().get_variable().get_id());
        }
    }
    
    // Debug: Show comparison axioms
    logger->log(Verbosity::DEBUG, "Comparison axioms in task: ", g_comp_axioms.size());
    for (size_t i = 0; i < g_comp_axioms.size(); ++i) {
        const ComparisonAxiom &ax = g_comp_axioms[i];
        logger->log(Verbosity::DEBUG, "  Comparison axiom ", i, ": affects variable ", ax.affected_variable);
    }
    
    // Initialize numeric domain mapping with full range (-inf, inf) for all numeric variables
    numeric_domain_mapping = compute_initial_numeric_domain_mapping(task_proxy);
    numeric_domain_sizes.resize(numeric_domain_mapping.size(), 1);
    

    
    // DEBUG: Print all comparison axiom mappings
    logger->log(Verbosity::DEBUG, "DEBUG: Comparison axiom mappings:");
    for (const auto &entry : comparison_axiom_dependencies) {
        int prop_var_id = entry.first;
        const unordered_set<int> &numeric_var_ids = entry.second;
        logger->log_no_endl(Verbosity::DEBUG, "  Propositional var ", prop_var_id, " depends on numeric vars: ");
        for (int nvar : numeric_var_ids) {
            logger->log_no_endl(Verbosity::DEBUG, nvar, " ");
        }
        logger->log(Verbosity::DEBUG, "");
    }
    
    DomainAbstractionFactory factory(
        task_proxy, domain_mapping, abstract_domain_sizes,
        numeric_domain_mapping, numeric_domain_sizes,
        true, rng, use_wildcard_plans, logger);
    DomainAbstraction abstraction = factory.generate();

    int iteration = 1;
    State concrete_init = task_proxy.get_initial_state();
    //concrete_init.unpack();
    while (!termination_criterion_satisfied(timer)) {
        
        logger->log(Verbosity::INFO, "iteration #", iteration);

        vector<Fact> flaws =
            get_flaws(task_proxy, concrete_init, abstraction);
        // Reset the selected flaw indices for this refinement step
        last_selected_flaw_indices.clear();
        
        // FILTERING: Remove comparison axiom flaws that have no refinable underlying numeric flaws.
        // This prevents wasted iterations where a comparison is selected but has nothing to refine.
        // We keep the flaws and detected_numeric_flaws vectors synchronized (same indexing).
        vector<Fact> filtered_flaws;
        vector<vector<NumericFlaw>> filtered_detected_numeric_flaws;
        
        for (size_t i = 0; i < flaws.size(); ++i) {
            const Fact &flaw = flaws[i];
            
            // Non-comparison flaws: always keep
            if (!is_comparison_axiom_variable(flaw.var)) {
                filtered_flaws.push_back(flaw);
                if (i < detected_numeric_flaws.size()) {
                    filtered_detected_numeric_flaws.push_back(detected_numeric_flaws[i]);
                } else {
                    filtered_detected_numeric_flaws.push_back({});
                }
                continue;
            }
            
            // Comparison flaw: check if any underlying numeric flaw can be refined
            bool has_refinable_numeric = false;
            if (i < detected_numeric_flaws.size()) {
                for (const NumericFlaw &nf : detected_numeric_flaws[i]) {
                    int local_idx = global_to_local_regular_numeric_var_ids[nf.numeric_var_id];
                    if (local_idx >= 0 && static_cast<size_t>(local_idx) < already_split.size() &&
                        already_split[local_idx].count(nf.concrete_value) == 0) {
                        has_refinable_numeric = true;
                        break;
                    }
                }
            }
            
            if (has_refinable_numeric) {
                filtered_flaws.push_back(flaw);
                filtered_detected_numeric_flaws.push_back(detected_numeric_flaws[i]);
            } else {
                logger->log(Verbosity::DEBUG, "Filtering out comparison flaw var=", flaw.var,
                           " (no refinable underlying numeric flaws)");
            }
        }
        
        // Replace with filtered versions
        flaws = std::move(filtered_flaws);
        detected_numeric_flaws = std::move(filtered_detected_numeric_flaws);

        // SUMMARY: Final flaws and dependencies for this iteration
        if (!flaws.empty() || !detected_numeric_flaws.empty()) {
            logger->log(Verbosity::DEBUG, "SUMMARY: Flaws after plan validation");
            if (!flaws.empty()) {
                logger->log(Verbosity::DEBUG, "  Propositional flaws:");
                // Access variable and numeric proxies for names
                VariablesProxy vars = task_proxy.get_variables();
                NumericVariablesProxy num_vars = task_proxy.get_numeric_variables();
                for (const Fact &f : flaws) {
                    bool is_comp = (comparison_axiom_dependencies.find(f.var) != comparison_axiom_dependencies.end());
                    // Print propositional variable with its human-readable name
                    string prop_name = vars[f.var].get_name();
                    logger->log(Verbosity::DEBUG, "    fdr_", f.var, " (", prop_name, ")=", f.value,
                                    (is_comp ? " (comparison)" : ""));
                    if (is_comp) {
                        const auto &deps = comparison_axiom_dependencies.at(f.var);
                        logger->log_no_endl(Verbosity::DEBUG, "      depends on numeric: ");
                        bool first = true;
                        for (int nv : deps) {
                            if (!first) logger->log_no_endl(Verbosity::DEBUG, ", ");
                            // Include numeric variable name
                            string num_name = num_vars[nv].get_name();
                            logger->log_no_endl(Verbosity::DEBUG, "num_", nv, " (", num_name, ")");
                            first = false;
                        }
                        logger->log(Verbosity::DEBUG, "");
                    }
                }
            } else {
                logger->log(Verbosity::DEBUG, "  Propositional flaws: none");
            }
            if (!detected_numeric_flaws.empty()) {
                logger->log(Verbosity::DEBUG, "  Numeric flaws:");
                // Access proxies (reuse if already declared above not available in this scope)
                VariablesProxy vars = task_proxy.get_variables();
                NumericVariablesProxy num_vars = task_proxy.get_numeric_variables();
                for (const vector<NumericFlaw> &num_flaw_vec : detected_numeric_flaws) {
                    for (const NumericFlaw &nf : num_flaw_vec) {
                        string num_name = num_vars[nf.numeric_var_id].get_name();
                        string prop_name = vars[nf.prop_var_id].get_name();
                        logger->log(Verbosity::DEBUG, "    num_", nf.numeric_var_id, " (", num_name, ")",
                                        " at value ", nf.concrete_value,
                                        " (from axiom fdr_", nf.prop_var_id, " (", prop_name, "))");
                    }
                }
            } else {
                logger->log(Verbosity::DEBUG, "  Numeric flaws: none");
            }
        }

        if (flaws.empty() && detected_numeric_flaws.empty()) {
            logger->log(Verbosity::DEBUG, "No more flaws found, terminating CEGAR refinement.");
            break;
        }

        // First try to fix propositional flaws (if any)
        bool flaws_fixed = true;
        if (!flaws.empty()) {
            flaws_fixed = fix_flaws(move(flaws), domain_mapping, abstraction.size());
            if (!flaws_fixed) {
                logger->log(Verbosity::INFO, "Could not fix any propositional flaws (all at size limit or blacklisted)");
            }
        } else {
            logger->log(Verbosity::INFO, "No propositional flaws to fix");
        }
        
        // Then try to fix numeric flaws (if any)
        bool numeric_flaws_fixed = true;

        // Get numeric flaws from the selected propositional flaws (by indices)
        std::vector<NumericFlaw> selected_numeric_flaws;
        for (int flaw_idx : last_selected_flaw_indices) {
            if (flaw_idx >= 0 && 
                static_cast<size_t>(flaw_idx) < detected_numeric_flaws.size()) {
                const vector<NumericFlaw> &nflaws = detected_numeric_flaws[flaw_idx];
                selected_numeric_flaws.insert(selected_numeric_flaws.end(),
                                              nflaws.begin(), nflaws.end());
            }
        }
        logger->log(Verbosity::DEBUG, "Collected ", selected_numeric_flaws.size(), 
                   " numeric flaws from ", last_selected_flaw_indices.size(), " flaw indices");

        if (selected_numeric_flaws.empty()) {
            logger->log(Verbosity::INFO, "No numeric flaws to fix (selected list is empty)");
        } else {
            logger->log(Verbosity::INFO, "Attempting to fix ", selected_numeric_flaws.size(), " numeric flaws");
        }
        numeric_flaws_fixed = fix_numeric_flaws(selected_numeric_flaws, abstraction.size(), task_proxy);
        
        if (!flaws_fixed || !numeric_flaws_fixed) {
            assert(max_abstraction_size != numeric_limits<int>::max());
            logger->log(Verbosity::INFO, "Terminating CEGAR loop because fixing flaws ",
                           "surpasses abstraction size limit of ", max_abstraction_size, " states. ",
                           "Generated ", abstraction.size(), " abstract states.");
            break;
        }


     
        
        // Validate that numeric_domain_sizes matches the actual partitions
      
        bool all_valid = true;
        for (size_t i = 0; i < numeric_domain_mapping.size(); ++i) {
            int actual_partitions = numeric_domain_mapping[i]->get_num_partitions();
            int expected_partitions = numeric_domain_sizes[i];
            if (actual_partitions != expected_partitions) {
                logger->log(Verbosity::DEBUG, "ERROR: num_", i, " has ", actual_partitions,
                                " partitions but expected ", expected_partitions);
                all_valid = false;
            }
            if (!numeric_domain_mapping[i]->is_valid()) {
                logger->log(Verbosity::DEBUG, "ERROR: num_", i, " has invalid mapping");
                all_valid = false;
            }
        }
        if (!all_valid) {
            logger->log(Verbosity::DEBUG, "CRITICAL ERROR: Numeric domain mapping validation failed!");
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        }

        logger->log(Verbosity::DEBUG, "  Domain mapping (propositional):");
        for (size_t i = 0; i < domain_mapping.size(); ++i) {
            if (!domain_mapping[i].empty()) {
                logger->log_no_endl(Verbosity::DEBUG, "    var ", i, ": ");
                for (size_t j = 0; j < domain_mapping[i].size(); ++j) {
                    if (domain_mapping[i][j] == 0) continue;
                    logger->log_no_endl(Verbosity::DEBUG, j, " -> ", domain_mapping[i][j], ", ");
                }
                logger->log(Verbosity::DEBUG, "");
            }
        }
        logger->log(Verbosity::DEBUG, "  Domain mapping (numeric):");
        for (size_t i = 0; i < numeric_domain_mapping.size(); ++i) {
            if (numeric_domain_mapping[i]->get_num_partitions() > 1) {
                logger->log_no_endl(Verbosity::DEBUG, "    num var ", i, ": { ");
                // Iterate through actual ranges and show partition assignment
                const auto &ranges = numeric_domain_mapping[i]->get_ranges();
                for (size_t range_idx = 0; range_idx < ranges.size(); ++range_idx) {
                    const NumericRange &range = ranges[range_idx];
                    logger->log_no_endl(Verbosity::DEBUG, "partition ", range.partition_index, ": ", range.to_string(), " ");
                }   
                logger->log(Verbosity::DEBUG, "}");
            }
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
    // If already refined (size >= 2), still return true to allow numeric refinement.
    // We don't check for unsplit values here because:
    // - Observed values vary semi-randomly between iterations
    // - A comparison with no splittable values NOW may have them later
    // - The fix_single_random_flaw will skip the no-op propositional refinement
    // - The numeric refinement step will naturally handle "no valid candidates"
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
    const std::unordered_map<int, ap_float> &concrete_values,
    const TaskProxy &task_proxy) const {
    
    // Get the set of regular variables this comparison depends on
    auto deps_it = comparison_axiom_dependencies.find(prop_var_id);
    if (deps_it == comparison_axiom_dependencies.end()) {
        logger->log(Verbosity::DEBUG, "determine_include_in_lower: No dependencies for prop_var_id ",
                   prop_var_id, ", defaulting to false");
        return false;
    }
    
    const std::unordered_set<int> &dependent_vars = deps_it->second;
    
    // Build base ranges: singleton [v, v] for all variables except split_var
    std::unordered_map<int, NumericRange> base_ranges_lower;  // For include_in_lower=true
    std::unordered_map<int, NumericRange> base_ranges_upper;  // For include_in_lower=false
    
    for (int var_id : dependent_vars) {
        if (var_id == split_var_id) {
            base_ranges_lower[var_id] = NumericRange(
                -std::numeric_limits<ap_float>::infinity(), split_value, 
                false, true);  // (-inf, split_value]
            
            base_ranges_upper[var_id] = NumericRange(
                split_value, std::numeric_limits<ap_float>::infinity(),
                true, false);  // [split_value, inf)
        } else {
            auto val_it = concrete_values.find(var_id);
            if (val_it != concrete_values.end()) {
                NumericRange singleton(val_it->second, val_it->second, true, true);
                base_ranges_lower[var_id] = singleton;
                base_ranges_upper[var_id] = singleton;
            } else {
                logger->log(Verbosity::DEBUG, "determine_include_in_lower: Missing concrete value for var ",
                           var_id, ", using full range");
                NumericRange full(-std::numeric_limits<ap_float>::infinity(),
                                 std::numeric_limits<ap_float>::infinity(),
                                 false, false);
                base_ranges_lower[var_id] = full;
                base_ranges_upper[var_id] = full;
            }
        }
    }
    
    // Compute all ranges (including derived variables) for both cases
    std::unordered_map<int, NumericRange> all_ranges_lower = 
        compute_all_numeric_ranges(base_ranges_lower, task_proxy);
    std::unordered_map<int, NumericRange> all_ranges_upper = 
        compute_all_numeric_ranges(base_ranges_upper, task_proxy);
    
    // Evaluate the comparison for both cases
    // Returns: 0 = definitely true, 1 = definitely false, 2 = unknown
    int eval_lower = evaluate_comparison_with_ranges(prop_var_id, all_ranges_lower, task_proxy);
    int eval_upper = evaluate_comparison_with_ranges(prop_var_id, all_ranges_upper, task_proxy);
    
    logger->log(Verbosity::DEBUG, "determine_include_in_lower for prop_var_id=", prop_var_id,
               ", split_var=", split_var_id, ", split_value=", split_value);
    logger->log(Verbosity::DEBUG, "  eval with (-inf, ", split_value, "]: ", 
               (eval_lower == 0 ? "TRUE" : (eval_lower == 1 ? "FALSE" : "UNKNOWN")));
    logger->log(Verbosity::DEBUG, "  eval with [", split_value, ", inf): ",
               (eval_upper == 0 ? "TRUE" : (eval_upper == 1 ? "FALSE" : "UNKNOWN")));
    
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
        return false;
    } else if (eval_lower == 2 && eval_upper == 2) {
        // Both give UNKNOWN - this shouldn't happen in theory, but default to false
        logger->log(Verbosity::DEBUG, "  -> Both give UNKNOWN, defaulting to include_in_lower=false");
        return false;
    } else if (eval_lower == 2) {
        // Only lower gives UNKNOWN (upper gives TRUE) - prefer UNKNOWN over TRUE
        logger->log(Verbosity::DEBUG, "  -> Choosing include_in_lower=true (gives UNKNOWN over TRUE)");
        return true;
    } else if (eval_upper == 2) {
        // Only upper gives UNKNOWN (lower gives TRUE) - prefer UNKNOWN over TRUE
        logger->log(Verbosity::DEBUG, "  -> Choosing include_in_lower=false (gives UNKNOWN over TRUE)");
        return false;
    } else {
        // Both give TRUE - doesn't matter, default to false
        logger->log(Verbosity::DEBUG, "  -> Both give TRUE, defaulting to include_in_lower=false");
        return false;
    }
}

bool CEGAR::fix_numeric_flaws(
    const vector<NumericFlaw> &numeric_flaws, int abstraction_size, const TaskProxy &task_proxy) {
    
    if (numeric_flaws.empty()) {
        return true;
    }
    
    // Build a map of concrete values for each numeric variable and prop_var_id
    // This is needed for determine_include_in_lower
    std::unordered_map<int, std::unordered_map<int, ap_float>> concrete_values_by_prop_var;
    for (const NumericFlaw &flaw : numeric_flaws) {
        concrete_values_by_prop_var[flaw.prop_var_id][flaw.numeric_var_id] = flaw.concrete_value;
    }
    
    // Build list of valid candidates: non-blacklisted numeric vars with split values not already split
    struct Candidate {
        int numeric_var_id;
        ap_float split_value;
        int local_id;
        int prop_var_id;  // Added to track which comparison axiom this flaw is from
    };
    vector<Candidate> valid_candidates;
    
    for (const NumericFlaw &flaw : numeric_flaws) {
        int numeric_var_id = flaw.numeric_var_id;
        ap_float split_value = flaw.concrete_value;
        
        // Check if we can refine this numeric variable
        if (!can_refine_numeric_variable(abstraction_size, numeric_var_id, task_proxy)) {
            logger->log(Verbosity::DEBUG, "DEBUG: Cannot refine num_", numeric_var_id,
                           " (blacklisted or size limit)");
            continue;
        }
        
        // Bounds check
        if (numeric_var_id < 0 || numeric_var_id >= (int)numeric_domain_mapping.size()) {
            logger->log(Verbosity::INFO, "ERROR: numeric_var_id ", numeric_var_id,
                           " is out of bounds! numeric_domain_mapping.size()=",
                           numeric_domain_mapping.size());
            continue;
        }
        
        int local_id = global_to_local_regular_numeric_var_ids[numeric_var_id];
        assert(already_split.size() > static_cast<size_t>(local_id));
        
        // Check if this split value has already been used
        if (already_split[local_id].count(split_value)) {
            logger->log(Verbosity::INFO, "WARNING: Split value ", split_value,
                           " for num_", numeric_var_id, " already in already_split - flaw detection produced duplicate!");
            // This should not happen - if we already split at this value, the flaw should have been resolved
            // Continue for now but log at INFO level to help diagnose
            continue;
        }
        
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
    int prop_var_id = selected.prop_var_id;
    
    // Assert that split_value is not NaN (it should have been replaced in get_flaws)
    assert(!std::isnan(split_value));
    
    // Determine split direction to ensure the comparison evaluates to FALSE
    // in the abstract state containing the concrete flaw state
    const std::unordered_map<int, ap_float> &concrete_values = concrete_values_by_prop_var[prop_var_id];
    bool include_in_lower = determine_include_in_lower(
        prop_var_id, numeric_var_id, split_value, concrete_values, task_proxy);
    
    logger->log(Verbosity::DEBUG, "Split direction for num_", numeric_var_id,
               " at ", split_value, ": include_in_lower=", include_in_lower);
    
    int old_num_partitions = numeric_domain_mapping[numeric_var_id]->get_num_partitions();
    
    int after_concrete_split = numeric_domain_mapping[numeric_var_id]->split_at(split_value, include_in_lower);
    already_split[local_id].insert(split_value);
    int new_num_partitions = after_concrete_split;
    
    if (new_num_partitions > old_num_partitions) {
        // Successfully split - created at least one new partition
        numeric_domain_sizes[numeric_var_id] = new_num_partitions;
        
        // Increment refinement counter
        numeric_var_refinement_count[numeric_var_id]++;
        
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
        FlawTreatment flaw_treatment,
        InitSplitMethod init_split_method,
        NumericSplitStrategy numeric_split_strategy,
        bool exec_entire_plan,
        const shared_ptr<utils::RandomNumberGenerator> &rng,
        const TaskProxy &task_proxy,
        unordered_set<int> &&init_split_var_ids,
        unordered_set<int> &&blacklisted_variables,
        unordered_set<int> &&blacklisted_numeric_variables) {
    CEGAR cegar(
        max_abstraction_size,
        max_time,
        use_wildcard_plans,
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
    parser.add_option<bool>(
        "exec_entire_plan",
        "When false (default), CEGAR returns immediately upon detecting the first flaw "
        "and refines the abstraction. When true, CEGAR executes the entire abstract plan "
        "and accumulates all flaws before returning. This is an experimental option for "
        "research purposes.",
        "false");
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
}
