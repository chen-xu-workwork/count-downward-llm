#include "cegar.h"

#include "domain_abstraction.h"
#include "domain_abstraction_factory.h"

#include "../axioms.h"
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
    
    const int max_abstraction_size;
    const double max_time;
    const bool use_wildcard_plans;
    const FlawTreatment flaw_treatment;
    const InitSplitMethod init_split_method;
    const NumericSplitStrategy numeric_split_strategy;
    const shared_ptr<utils::RandomNumberGenerator> &rng;
    const std::unordered_set<int> init_split_var_ids;
    std::unordered_set<int> blacklisted_variables;

    std::vector<int> abstract_domain_sizes;
    std::vector<int> real_domain_sizes;
    
    // Numeric domain mapping and sizes (for refinement across iterations)
    NumericDomainMappingType numeric_domain_mapping;
    std::vector<int> numeric_domain_sizes;
    std::unordered_set<int> blacklisted_numeric_variables;
    
    // Temporary storage for numeric flaws detected in get_flaws()
    // (mutable because get_flaws is const but needs to store flaws)
    mutable std::vector<NumericFlaw> detected_numeric_flaws;
    
    // Mapping from propositional variables (derived from comparison axioms)
    // to the numeric variables they depend on.
    // This allows us to trace back from propositional flaws to numeric refinements.
    // Maps: propositional_var_id -> set of numeric variable IDs
    std::unordered_map<int, std::unordered_set<int>> comparison_axiom_dependencies;
    
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
    
    void build_comparison_axiom_mapping(const TaskProxy &task_proxy);
    
    // Debug: print axiom dependency trees
    void print_cegar_axiom_trees(const TaskProxy &task_proxy,
                                 const std::vector<bool> &is_derived,
                                 const std::vector<std::vector<int>> &axiom_dependencies);
    
    // Numeric variable refinement
    ap_float extract_threshold_from_comparison(int prop_var_id, 
                                               const TaskProxy &task_proxy) const;
    bool fix_numeric_flaws(const std::vector<NumericFlaw> &numeric_flaws,
                          int abstraction_size,
                          const TaskProxy &task_proxy);
public:
    CEGAR(int max_abstraction_size,
          double max_time,
          bool use_wildcard_plans,
          FlawTreatment flaw_treatment,
          InitSplitMethod init_split_method,
          NumericSplitStrategy numeric_split_strategy,
          const shared_ptr<utils::RandomNumberGenerator> &rng,
          unordered_set<int> &&init_split_var_ids,
          unordered_set<int> &&blacklisted_variables);

    DomainAbstraction build_abstraction(const TaskProxy &task_proxy);
};

CEGAR::CEGAR(
        int max_abstraction_size,
        double max_time,
        bool use_wildcard_plans,
        FlawTreatment flaw_treatment,
        InitSplitMethod init_split_method,
        NumericSplitStrategy numeric_split_strategy,
        const shared_ptr<utils::RandomNumberGenerator> &rng,
        unordered_set<int> &&init_split_var_ids,
        unordered_set<int> &&blacklisted_variables)
    : max_abstraction_size(max_abstraction_size),
      max_time(max_time),
      use_wildcard_plans(use_wildcard_plans),
      flaw_treatment(flaw_treatment),
      init_split_method(init_split_method),
      numeric_split_strategy(numeric_split_strategy),
      rng(rng),
      init_split_var_ids(move(init_split_var_ids)),
      blacklisted_variables(move(blacklisted_variables)) {
    /* TODO: Should we check somewhere that *init_split_var_ids* does not
        contain elements that are blacklisted? */
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
        vector<int> split_vars(
            init_split_var_ids.begin(), init_split_var_ids.end());
        rng->shuffle(split_vars);
        int abstraction_size = 1;
        for (const int var_id : split_vars) {
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
    cout << "Initial split for variable " << var_id << ": " << "with initialization method "; 
    switch (init_split_method) {
    case InitSplitMethod::GOAL_VALUE:
        cout << "GOAL_VALUE" << endl;
        assert(variable_specified_in_goal(var_id, task_proxy));
        break;
    case InitSplitMethod::GOAL_VALUE_OR_RANDOM_IF_NON_GOAL:
        cout << "GOAL_VALUE_OR_RANDOM_IF_NON_GOAL" << endl;
        break;
    case InitSplitMethod::INIT_VALUE:
        cout << "INIT_VALUE" << endl;
        break;
    case InitSplitMethod::RANDOM_VALUE: 
        cout << "RANDOM_VALUE" << endl;
        break;
    case InitSplitMethod::RANDOM_PARTITION:
        cout << "RANDOM_PARTITION" << endl;
        break;
    case InitSplitMethod::RANDOM_BINARY_PARTITION_SEPARATING_INIT_GOAL:
        cout << "RANDOM_BINARY_PARTITION_SEPARATING_INIT_GOAL" << endl;
        break;
    case InitSplitMethod::IDENTITY:
        cout << "IDENTITY" << endl;
        break;
    }
    cout << endl;
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
    cout << "init value " << init_value << endl;
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
            cout << "Initial split for variable " << var_id
                 << " exceeds size limit." << endl;
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

static vector<Fact> get_precondition_flaws(
    const OperatorProxy &op, const vector<int> &current_state,
    const unordered_set<int> &blacklisted_variables) {
    vector<Fact> flaws;
    for (FactProxy pre : op.get_preconditions()) {
        int var_id = pre.get_variable().get_id();
        if (blacklisted_variables.count(var_id) == 0
            && current_state[var_id] != pre.get_value()) {
            flaws.emplace_back(var_id, pre.get_value());
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

static vector<Fact> get_goal_flaws(
    const TaskProxy &task_proxy, const vector<int> &current_state,
    const unordered_set<int> &blacklisted_variables) {
    vector<Fact> flaws;
    
    // First, collect non-derived goals directly
    for (const FactProxy &goal : task_proxy.get_goals()) {
        int var_id = goal.get_variable().get_id();
        if (!is_derived_variable(task_proxy, var_id)) {
            if (blacklisted_variables.count(var_id) == 0
                && current_state[var_id] != goal.get_value()) {
                flaws.emplace_back(var_id, goal.get_value());
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


vector<Fact> CEGAR::get_flaws(
    const TaskProxy &task_proxy, const State &concrete_init,
    const DomainAbstraction &abstraction) const {

    // Clear any previously detected numeric flaws
    detected_numeric_flaws.clear();

    // Initialize propositional state
    vector<int> current_state;
    for (int i = 0; i < concrete_init.size(); ++i) {
        current_state.push_back(concrete_init[i].get_value());
    }
    
    // Initialize numeric state
    vector<ap_float> numeric_state = g_root_task()->get_initial_state_numeric_values();
    
    // CRITICAL: Evaluate axioms on initial state!
    // Without this, derived variables (comparison axioms) are uninitialized,
    // causing empty plans to incorrectly appear valid even when goals aren't satisfied.
    g_axiom_evaluator->evaluate_arithmetic_axioms(numeric_state);
    g_axiom_evaluator->evaluate(current_state, numeric_state);

    vector<vector<int>> wildcard_plan = abstraction.get_plan();
    vector<Fact> flaws;

    cout << "DEBUG: Validating wildcard plan with " << wildcard_plan.size() << " steps" << endl;
    int step_num = 0;
    for (vector<int> &equivalent_ops : wildcard_plan) {
        assert(flaws.empty());
        cout << "DEBUG: Step " << step_num << " - " << equivalent_ops.size() << " equivalent operators" << endl;
        
        for (int op_id : equivalent_ops) {
            OperatorProxy op = task_proxy.get_operators()[op_id];
            
            // Check propositional preconditions
            vector<Fact> operator_flaws =
                get_precondition_flaws(
                    op, current_state, blacklisted_variables);

            if (operator_flaws.empty()) {
                // Propositional preconditions satisfied - apply operator
                flaws.clear();
                detected_numeric_flaws.clear();
                
                // DEBUG: Print operator being executed
                cout << "  Executing operator: " << op.get_name() << endl;
                
                // DEBUG: Print propositional effects
                cout << "    Propositional effects:" << endl;
                for (EffectProxy effect : op.get_effects()) {
                    FactProxy effect_fact = effect.get_fact();
                    int var_id = effect_fact.get_variable().get_id();
                    int old_value = current_state[var_id];
                    int new_value = effect_fact.get_value();
                    cout << "      var" << var_id << ": " << old_value << " -> " << new_value << endl;
                }
                
                // DEBUG: Print numeric effects
                if (op.get_ass_effects().size() > 0) {
                    cout << "    Numeric effects:" << endl;
                    for (auto ass_eff_proxy : op.get_ass_effects()) {
                        NumAssProxy effect = ass_eff_proxy.get_assignment();
                        int affected_var_id = effect.get_affected_variable().get_id();
                        ap_float old_value = numeric_state[affected_var_id];
                        
                        NumericVariableProxy assigned_var = effect.get_assigned_variable();
                        ap_float operand = numeric_state[assigned_var.get_id()];
                        
                        f_operator op_type = effect.get_assigment_operator_type();
                        string op_str;
                        ap_float new_value = old_value;
                        switch (op_type) {
                            case assign: op_str = "="; new_value = operand; break;
                            case increase: op_str = "+="; new_value = old_value + operand; break;
                            case decrease: op_str = "-="; new_value = old_value - operand; break;
                            case scale_up: op_str = "*="; new_value = old_value * operand; break;
                            case scale_down: 
                                op_str = "/="; 
                                new_value = (operand != 0) ? old_value / operand : old_value; 
                                break;
                        }
                        
                        cout << "      num_var" << affected_var_id << " " << op_str << " num_var" 
                             << assigned_var.get_id() << " (value=" << operand << "): " 
                             << old_value << " -> " << new_value << endl;
                    }
                }
                
                apply_op_to_state(current_state, op);
                apply_numeric_effects(numeric_state, op);
                g_axiom_evaluator->evaluate_arithmetic_axioms(numeric_state);
                g_axiom_evaluator->evaluate(current_state, numeric_state);
                break;
            } else {
                // We have precondition flaws
                // KEY PRINCIPLE: Only add numeric flaws for comparison axioms that appear as precondition flaws
                
                // Check if any precondition flaw is on a comparison axiom variable
                for (Fact &flaw : operator_flaws) {
                    auto it = comparison_axiom_dependencies.find(flaw.var);
                    if (it != comparison_axiom_dependencies.end()) {
                        // This is a comparison axiom variable - trace to regular numeric variables it depends on
                        const unordered_set<int> &dep_vars = it->second;
                        for (int numeric_var_id : dep_vars) {
                            // Split at current concrete value
                            ap_float concrete_value = numeric_state[numeric_var_id];
                            detected_numeric_flaws.emplace_back(
                                numeric_var_id, concrete_value, flaw.var);
                        }
                        // Also add the comparison axiom itself as a propositional flaw
                        flaws.emplace_back(flaw.var, flaw.value);
                    } else {
                        // Regular propositional flaw
                        flaws.emplace_back(flaw.var, flaw.value);
                    }
                }
            }
        }
        
        if (!flaws.empty() || !detected_numeric_flaws.empty()) {
            cout << "DEBUG: Flaw found at step " << step_num << endl;
            cout << "DEBUG: Propositional flaws: " << flaws.size() 
                 << ", Numeric flaws: " << detected_numeric_flaws.size() << endl;
            return flaws;
        }
        step_num++;
    }

    // Check goal flaws
    assert(flaws.empty());
    cout << "DEBUG: Plan executed successfully, checking goals..." << endl;
    cout << "DEBUG: Current propositional state after plan execution:" << endl;
    for (size_t i = 0; i < current_state.size(); ++i) {
        if (i < 30) {  // Only print first 30 variables
            cout << "  fdr_" << i << "=" << current_state[i] << endl;
        }
    }
    
    cout << "DEBUG: Current numeric state after plan execution:" << endl;
    for (size_t i = 0; i < numeric_state.size(); ++i) {
        if (i < 20) {  // Only print first 20 numeric variables
            cout << "  num_" << i << "=" << numeric_state[i] << endl;
        }
    }
    
    cout << "DEBUG: Specific variables of interest:" << endl;
    if (13 < current_state.size()) cout << "  fdr_13=" << current_state[13] << endl;
    if (33 < current_state.size()) cout << "  fdr_33=" << current_state[33] << endl;
    if (66 < current_state.size()) cout << "  fdr_66=" << current_state[66] << endl;
    
    flaws = get_goal_flaws(task_proxy, current_state,
                           blacklisted_variables);
    
    cout << "DEBUG: Goal flaws detected: " << flaws.size() << endl;
    
    // Separate comparison axiom flaws from regular propositional flaws
    // KEY PRINCIPLE: Only add numeric flaws for comparison axioms that appear as propositional flaws
    vector<Fact> filtered_flaws;
    for (const Fact &flaw : flaws) {
        cout << "  Goal flaw: ID: fdr_" << flaw.var << "=" << flaw.value << endl;
        
        // Check if this goal flaw is on a comparison axiom variable
        auto it = comparison_axiom_dependencies.find(flaw.var);
        if (it != comparison_axiom_dependencies.end()) {
            cout << "    -> This is a comparison axiom variable (numeric goal)" << endl;
            cout << "    -> Flaw is on fdr_" << flaw.var << endl;
            
            // Numeric goal flaw - trace to regular numeric variables that this comparison depends on
            const unordered_set<int> &dep_vars = it->second;
            
            cout << "    -> Will add numeric flaws for the following regular variables:" << endl;
            for (int numeric_var_id : dep_vars) {
                cout << "       num_" << numeric_var_id << endl;
            }
            
            bool added_any_numeric_flaw = false;
            for (int numeric_var_id : dep_vars) {
                // Get current concrete value
                ap_float concrete_value = numeric_state[numeric_var_id];
                cout << "       Adding numeric flaw: num_" << numeric_var_id 
                     << " with concrete value " << concrete_value << endl;
                
                // Add this as a numeric flaw to refine
                detected_numeric_flaws.emplace_back(
                    numeric_var_id, concrete_value, flaw.var);
                added_any_numeric_flaw = true;
            }
            
            // ALWAYS refine the comparison axiom variable itself (propositional)
            // This is important even if we can't refine the numeric variables
            cout << "    -> Adding comparison axiom fdr_" << flaw.var 
                 << " to propositional flaws for refinement" << endl;
            filtered_flaws.push_back(flaw);
        } else {
            // Regular propositional flaw - keep it
            filtered_flaws.push_back(flaw);
        }
    }
    
    cout << "DEBUG: Total flaws: " << filtered_flaws.size() << " propositional, "
         << detected_numeric_flaws.size() << " numeric" << endl;
    cout << "DEBUG: Numeric flaws only added for comparison axioms that appear in propositional flaws" << endl;
    return filtered_flaws;
}

bool CEGAR::fix_flaws(
    vector<Fact> &&flaws, DomainMapping &domain_mapping,
    int abstraction_size) {
    switch(flaw_treatment) {
        case FlawTreatment::RANDOM_SINGLE_ATOM:
            return fix_single_random_flaw(
                move(flaws), domain_mapping, abstraction_size);
        case FlawTreatment::ONE_SPLIT_PER_ATOM:
            return fix_flaws_per_atom(
                move(flaws), domain_mapping, abstraction_size);
        case FlawTreatment::ONE_SPLIT_PER_VARIABLE:
            return fix_flaws_per_variable(
                move(flaws), domain_mapping, abstraction_size);
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
    cout << "DEBUG fix_single_random_flaw: Processing " << flaws.size() << " flaws, "
         << repetitions << " repetitions" << endl;
    for (int i = 0; i < repetitions; ++i) {
        Fact fact(*rng->choose(flaws));
        cout << "  Attempt " << (i+1) << ": chosen flaw fdr_" << fact.var << "=" << fact.value << endl;
        cout << "    Current abstract_domain_size[" << fact.var << "] = " 
             << abstract_domain_sizes[fact.var] << endl;
        cout << "    Real domain size = " << real_domain_sizes[fact.var] << endl;
        
        if (can_refine_variable(abstraction_size, fact.var)) {
            cout << "    Can refine - adding to abstraction" << endl;
            add_variable_to_abstraction_if_necessary(fact.var, domain_mapping);
            
            // Show domain mapping before modification
            cout << "    Domain mapping before: [";
            for (size_t j = 0; j < domain_mapping[fact.var].size(); ++j) {
                if (j > 0) cout << ", ";
                cout << domain_mapping[fact.var][j];
            }
            cout << "]" << endl;
            
            cout << "    Setting domain_mapping[" << fact.var << "][" << fact.value 
                 << "] = " << abstract_domain_sizes[fact.var] << endl;
            domain_mapping[fact.var][fact.value] =
                abstract_domain_sizes[fact.var];
            
            // Show domain mapping after modification
            cout << "    Domain mapping after: [";
            for (size_t j = 0; j < domain_mapping[fact.var].size(); ++j) {
                if (j > 0) cout << ", ";
                cout << domain_mapping[fact.var][j];
            }
            cout << "]" << endl;
            
            abstract_domain_sizes[fact.var] += 1;
            cout << "    New abstract_domain_size[" << fact.var << "] = " 
                 << abstract_domain_sizes[fact.var] << endl;
            return true;
        } else {
            cout << "    Cannot refine (blacklisted or size limit)" << endl;
        }
    }
    return false;
}

/* Chooses a flaw for that the increase in abstraction size is the smallest among all given ones
 * -> Leads to the smallest possible increase in abstraction size in every iteration */
bool CEGAR::fix_single_flaw_max_refined(
        vector<Fact> &&flaws, DomainMapping &domain_mapping,
        int abstraction_size) {
    // determine domain sizes of flaws, select the ones with max refinement
    int current_max_domain_size = 0;
    vector<int> current_flaw_candidates;
    int num_flaws = (int) flaws.size();

    for (int i = 0; i < num_flaws; ++i) {
        // determine domain size
        int domain_size = abstract_domain_sizes[flaws[i].var];
        // check how domain size of flaw.var ranks
        if (domain_size > current_max_domain_size) {
            current_flaw_candidates.clear();
            current_flaw_candidates.emplace_back(i);
            current_max_domain_size = domain_size;
        } else if (domain_size == current_max_domain_size) {
            current_flaw_candidates.emplace_back(i);
        }
    }
    /* We do not repeat the selection of flaws since with this Method the abstraction size increase can not get lower
     * with another choice -> If the first choice does not work, none will! */
    Fact fact (flaws[*rng->choose(current_flaw_candidates)]);
    if (can_refine_variable(abstraction_size, fact.var)) {
        add_variable_to_abstraction_if_necessary(fact.var, domain_mapping);
        domain_mapping[fact.var][fact.value] = abstract_domain_sizes[fact.var];
        abstract_domain_sizes[fact.var] += 1;
        return true;
    }
    return false;
}

bool CEGAR::fix_flaws_per_atom(
    vector<Fact> &&flaws, DomainMapping &domain_mapping,
    int abstraction_size) {
    // FIXME: Bias for variables with low index.
    sort(flaws.begin(), flaws.end());
    Fact last_flaw(-1, -1);
    for (const Fact &flaw : flaws) {
        if (flaw == last_flaw) {
            // duplicate
            continue;
        }
        if (can_refine_variable(abstraction_size, flaw.var)) {
            add_variable_to_abstraction_if_necessary(flaw.var, domain_mapping);
            domain_mapping[flaw.var][flaw.value] =
                abstract_domain_sizes[flaw.var];
            abstract_domain_sizes[flaw.var] += 1;
            last_flaw = flaw;
        }
    }
    return last_flaw != Fact(-1, -1);
}

bool CEGAR::fix_flaws_per_variable(
    vector<Fact> &&flaws, DomainMapping &domain_mapping,
    int abstraction_size) {
    // FIXME: Bias for variables with low index.
    sort(flaws.begin(), flaws.end());
    Fact last_flaw(-1, -1);
    for (const Fact &flaw : flaws) {
        if (flaw.var > last_flaw.var
            && can_refine_variable(abstraction_size, flaw.var)) {
            /* Introduce new abstract value only for every new variable,
               opposed to for every atom as in *fix_flaws_per_atom* above. */
            add_variable_to_abstraction_if_necessary(flaw.var, domain_mapping);
            abstract_domain_sizes[flaw.var] += 1;
        } else if (flaw.var != last_flaw.var || flaw.value == last_flaw.value) {
            // Duplicate or does not fit size limit.
            continue;
        }
        domain_mapping[flaw.var][flaw.value] =
            abstract_domain_sizes[flaw.var] - 1;
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

    cout << "\n=== CEGAR Statistics ===" << endl;
    cout << "Final abstraction size: " << abstraction_size << endl;
    
    cout << "\nPropositional variables:" << endl;
    cout << "  Total: " << num_variables << endl;
    cout << "  Trivial (size 1): " << num_trivial_variables << endl;
    cout << "  Complete (not abstracted): " << num_complete_variables << endl;
    cout << "  Average domain size ratio: " << avg_domain_size << endl;
    
    // Print details of non-trivial propositional variables
    cout << "\n  Non-trivial propositional variables:" << endl;
    for (int i = 0; i < num_variables; ++i) {
        if (abstract_domain_sizes[i] > 1) {
            VariableProxy var = task_proxy.get_variables()[i];
            int original_size = var.get_domain_size();
            cout << "    var" << i << " (" << var.get_name() << "): "
                 << "abstract_size=" << abstract_domain_sizes[i] 
                 << ", original_size=" << original_size << endl;
            
            // Print the domain mapping if it's not too large
            if (abstract_domain_sizes[i] <= 10 && original_size <= 20) {
                cout << "      mapping: [";
                for (int val = 0; val < original_size; ++val) {
                    if (val > 0) cout << ", ";
                    cout << val << "->" << domain_mapping[i][val];
                }
                cout << "]" << endl;
            }
        }
    }
    
    cout << "\nNumeric variables:" << endl;
    cout << "  Total: " << num_numeric_variables << endl;
    cout << "  Trivial (1 partition): " << num_trivial_numeric_vars << endl;
    cout << "  Refined (>1 partition): " << num_refined_numeric_vars << endl;
    cout << "  Total partitions: " << total_numeric_partitions << endl;
    cout << "  Average partitions per variable: " << avg_numeric_partitions << endl;
    
    // Print details of refined numeric variables
    cout << "\n  Refined numeric variables:" << endl;
    for (int i = 0; i < num_numeric_variables; ++i) {
        if (numeric_domain_sizes[i] > 1) {
            NumericVariableProxy num_var = task_proxy.get_numeric_variables()[i];
            cout << "    var" << i << " (" << num_var.get_name() << "): "
                 << numeric_domain_sizes[i] << " partitions" << endl;
            
            // Print the ranges for this variable
            const vector<NumericRange> &ranges = numeric_domain_mapping[i]->get_ranges();
            for (size_t j = 0; j < ranges.size(); ++j) {
                cout << "      partition " << ranges[j].partition_index << ": ";
                // Print lower bound with correct bracket
                cout << (ranges[j].lower_inclusive ? "[" : "(");
                if (ranges[j].lower == -numeric_limits<ap_float>::infinity()) {
                    cout << "-inf";
                } else {
                    cout << ranges[j].lower;
                }
                cout << ", ";
                // Print upper bound
                if (ranges[j].upper == numeric_limits<ap_float>::infinity()) {
                    cout << "inf";
                } else {
                    cout << ranges[j].upper;
                }
                // Print upper bracket
                cout << (ranges[j].upper_inclusive ? "]" : ")") << endl;
            }
        }
    }
    
    cout << "========================\n" << endl;
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
    std::cout << "DEBUG: NumericSplitStrategy = " 
              << (numeric_split_strategy == NumericSplitStrategy::EXCLUSION ? "EXCLUSION" : "STANDARD") 
              << std::endl;
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
            std::cout << "  num_" << i << " (" << num_var.get_name() 
                     << ") is CONSTANT with value " << const_value 
                     << " - creating ConstantMapping" << std::endl;
            
            numeric_domain_mapping.push_back(std::make_unique<ConstantMapping>(const_value));
        } else if (var_type == numType::derived) {
            // Derived variables are computed from other variables via assignment axioms
            // They should be implicitly abstracted based on their source variables
            // Don't create a refinable mapping - use ConstantMapping as placeholder
            // (the actual partitioning happens implicitly during axiom evaluation)
            std::cout << "  num_" << i << " (" << num_var.get_name() 
                     << ") is DERIVED - skipping explicit mapping (implicitly abstracted)" << std::endl;
            
            // Use a placeholder ConstantMapping with value 0
            // The actual value doesn't matter since derived variables are computed
            numeric_domain_mapping.push_back(std::make_unique<ConstantMapping>(0));
        } else if (var_type == numType::regular) {
            // Regular variables: create refinable mapping
            std::cout << "  num_" << i << " (" << num_var.get_name() 
                     << ") is REGULAR - creating refinable mapping" << std::endl;
            
            if (numeric_split_strategy == NumericSplitStrategy::EXCLUSION) {
                numeric_domain_mapping.push_back(std::make_unique<ExclusionSplitMapping>());
            } else {
                // Default: StandardSplitMapping
                numeric_domain_mapping.push_back(std::make_unique<StandardSplitMapping>());
            }
        } else {
            // Unknown or instrumentation type - treat as refinable for now
            std::cout << "  num_" << i << " (" << num_var.get_name() 
                     << ") is OTHER/UNKNOWN (type=" << static_cast<int>(var_type)
                     << ") - creating refinable mapping" << std::endl;
            
            if (numeric_split_strategy == NumericSplitStrategy::EXCLUSION) {
                numeric_domain_mapping.push_back(std::make_unique<ExclusionSplitMapping>());
            } else {
                numeric_domain_mapping.push_back(std::make_unique<StandardSplitMapping>());
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
    vector<bool> is_derived(num_numeric_vars, false);
    
    // Build dependency graph: derived_var -> [source_vars]
    vector<vector<int>> axiom_dependencies(num_numeric_vars);
    
    AssignmentAxiomsProxy assignment_axioms = task_proxy.get_assignment_axioms();
    cout << "DEBUG AXIOM MAP: Building assignment axiom dependency graph" << endl;
    cout << "DEBUG AXIOM MAP: Total numeric variables: " << num_numeric_vars << endl;
    cout << "DEBUG AXIOM MAP: Assignment axioms: " << assignment_axioms.size() << endl;
    
    for (AssignmentAxiomProxy axiom : assignment_axioms) {
        int derived_id = axiom.get_assignment_variable().get_id();
        int left_id = axiom.get_left_variable().get_id();
        int right_id = axiom.get_right_variable().get_id();
        
        if (derived_id >= 0 && derived_id < num_numeric_vars) {
            is_derived[derived_id] = true;
            
            if (left_id >= 0) {
                axiom_dependencies[derived_id].push_back(left_id);
            }
            if (right_id >= 0 && right_id != left_id) {
                axiom_dependencies[derived_id].push_back(right_id);
            }
            
            if (derived_id == 70 || derived_id == 21 || derived_id == 37 || derived_id == 66) {
                cout << "DEBUG AXIOM MAP:   Axiom: num_" << derived_id << " := num_" 
                     << left_id << " op num_" << right_id << endl;
            }
        }
    }
    
    // Helper function to recursively find all regular (non-derived) variables
    // that a given variable depends on
    auto find_regular_dependencies = [&](int var_id, auto& find_regular_dependencies_ref) -> unordered_set<int> {
        unordered_set<int> regular_vars;
        
        if (var_id < 0 || var_id >= num_numeric_vars) {
            return regular_vars;
        }
        
        if (!is_derived[var_id]) {
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
            cout << "DEBUG:   Goal: ";
            cout << goal_fact.get_variable().get_name()
                    << "=" << goal_fact.get_value() << endl;
        }

    // Now build the comparison axiom mapping
    ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
    cout << "DEBUG: Building comparison axiom mapping, total axioms: " << comparison_axioms.size() << endl;
    for (ComparisonAxiomProxy axiom : comparison_axioms) {
        // Get the propositional variable created by this comparison axiom
        FactProxy true_fact = axiom.get_true_fact();
        FactProxy false_fact = axiom.get_false_fact();

        
        assert(true_fact.get_variable().get_id() == false_fact.get_variable().get_id());
        int prop_var_id = true_fact.get_variable().get_id();
        
        cout << "DEBUG: Processing comparison axiom for fdr_" << prop_var_id 
             << " (" << true_fact.get_variable().get_name() << ")" << endl;
        
        // Get the numeric variables used in the comparison (may be derived!)
        int left_var_id = axiom.get_left_variable().get_id();
        int right_var_id = axiom.get_right_variable().get_id();
        
        // Trace through to find regular variables
        unordered_set<int> regular_vars;
        
        if (left_var_id >= 0) {
            unordered_set<int> left_deps = find_regular_dependencies(left_var_id, find_regular_dependencies);
            regular_vars.insert(left_deps.begin(), left_deps.end());
        }
        
        if (right_var_id >= 0) {
            unordered_set<int> right_deps = find_regular_dependencies(right_var_id, find_regular_dependencies);
            regular_vars.insert(right_deps.begin(), right_deps.end());
        }
        
        // Store the mapping
        comparison_axiom_dependencies[prop_var_id] = regular_vars;
        
        // Debug output for ALL comparison axioms
        cout << "  fdr_" << prop_var_id << " depends on:" << endl;
        cout << "    left_var=num_" << left_var_id 
             << (left_var_id >= 0 && is_derived[left_var_id] ? " (DERIVED)" : " (regular)") << endl;
        cout << "    right_var=num_" << right_var_id 
             << (right_var_id >= 0 && is_derived[right_var_id] ? " (DERIVED)" : " (regular)") << endl;
        cout << "    Regular dependencies: {";
        for (int reg_var : regular_vars) {
            cout << "num_" << reg_var << " ";
        }
        cout << "}" << endl;
        
        // Store comparison info (operator and variable IDs for threshold extraction)
        ComparisonInfo info;
        info.left_var_id = left_var_id;
        info.right_var_id = right_var_id;
        info.comp_op = static_cast<int>(axiom.get_comparison_operator_type());
        comparison_axiom_info[prop_var_id] = info;
        
        cout << "DEBUG: Stored mapping for fdr_" << prop_var_id << " -> {";
        for (int reg_var : regular_vars) {
            cout << "num_" << reg_var << " ";
        }
        cout << "} with operator=" << info.comp_op << endl;
    }
    
    cout << "DEBUG: Total comparison axiom dependencies stored: " 
         << comparison_axiom_dependencies.size() << endl;
    
    // PHASE 2: Collect all numeric variables modified by operators
    // This ensures we refine ALL operator-modified variables when numeric flaws occur
    operator_modified_numeric_vars.clear();
    
    cout << "DEBUG PHASE2: Collecting operator-modified numeric variables..." << endl;
    OperatorsProxy operators = task_proxy.get_operators();
    for (OperatorProxy op : operators) {
        // Check additive effects (NumAss effects)
        for (auto ass_eff_proxy : op.get_ass_effects()) {
            NumAssProxy ass_eff = ass_eff_proxy.get_assignment();
            int affected_var_id = ass_eff.get_affected_variable().get_id();
            
            if (affected_var_id >= 0 && affected_var_id < num_numeric_vars) {
                // Only add if this is a REGULAR variable (not derived)
                if (!is_derived[affected_var_id]) {
                    operator_modified_numeric_vars.insert(affected_var_id);
                }
            }
        }
    }
    
    cout << "DEBUG PHASE2: Operator-modified numeric variables: ";
    vector<int> sorted_op_vars(operator_modified_numeric_vars.begin(), 
                                operator_modified_numeric_vars.end());
    sort(sorted_op_vars.begin(), sorted_op_vars.end());
    for (int var_id : sorted_op_vars) {
        cout << "var" << var_id << " ";
    }
    cout << endl;
    cout << "DEBUG PHASE2: Total: " << operator_modified_numeric_vars.size() 
         << " variables" << endl;
    
    // Print comprehensive axiom dependency tree for CEGAR
    print_cegar_axiom_trees(task_proxy, is_derived, axiom_dependencies);
}

void CEGAR::print_cegar_axiom_trees(
    const TaskProxy &task_proxy,
    const vector<bool> &is_derived,
    const vector<vector<int>> &axiom_dependencies) {
    
    cout << "\n========== CEGAR: Axiom Dependency Trees ==========" << endl;
    
    int num_numeric_vars = task_proxy.get_numeric_variables().size();
    
    // Print assignment axioms with derived status
    cout << "\n--- Assignment Axioms (from CEGAR perspective) ---" << endl;
    AssignmentAxiomsProxy assignment_axioms = task_proxy.get_assignment_axioms();
    cout << "Total assignment axioms: " << assignment_axioms.size() << endl;
    
    for (AssignmentAxiomProxy axiom : assignment_axioms) {
        int derived_id = axiom.get_assignment_variable().get_id();
        int left_id = axiom.get_left_variable().get_id();
        int right_id = axiom.get_right_variable().get_id();
        cal_operator op = axiom.get_arithmetic_operator_type();
        
        string op_str;
        switch (op) {
            case cal_operator::sum: op_str = "+"; break;
            case cal_operator::diff: op_str = "-"; break;
            case cal_operator::mult: op_str = "*"; break;
            case cal_operator::divi: op_str = "/"; break;
            default: op_str = "?"; break;
        }
        
        NumericVariableProxy derived_var = axiom.get_assignment_variable();
        NumericVariableProxy left_var = axiom.get_left_variable();
        NumericVariableProxy right_var = axiom.get_right_variable();
        
        cout << "  Axiom: var" << derived_id << " := var" << left_id << " " << op_str << " var" << right_id;
        if (derived_id == 66 || derived_id == 67 || derived_id == 68 || derived_id == 70 || 
            derived_id == 21 || derived_id == 22 || derived_id == 26 || derived_id == 28) {
            cout << " *** KEY VAR ***";
        }
        cout << endl;
        cout << "    Names: " << derived_var.get_name() << " := " 
             << left_var.get_name() << " " << op_str << " " << right_var.get_name() << endl;
        cout << "    Derived: " << (is_derived[derived_id] ? "YES" : "NO")
             << ", Left: " << (left_id >= 0 && is_derived[left_id] ? "YES" : "NO")
             << ", Right: " << (right_id >= 0 && is_derived[right_id] ? "YES" : "NO") << endl;
    }
    
    // Print comparison axioms and their regular dependencies
    cout << "\n--- Comparison Axioms -> Regular Variable Mappings ---" << endl;
    cout << "Format: prop_varX := (numvar_left op numvar_right) -> {regular dependencies}" << endl;
    
    for (const auto &entry : comparison_axiom_dependencies) {
        int prop_var_id = entry.first;
        const unordered_set<int> &regular_vars = entry.second;
        
        cout << "  Propositional var" << prop_var_id << " depends on numeric vars: {";
        for (int reg_var : regular_vars) {
            cout << reg_var << " ";
        }
        cout << "}" << endl;
    }
    
    // Print full dependency chains for key variables
    cout << "\n--- Full Dependency Chains for Key Variables ---" << endl;
    vector<int> key_vars = {66, 67, 68, 70, 21, 22, 26, 28};
    
    for (int var_id : key_vars) {
        if (var_id >= num_numeric_vars) continue;
        
        cout << "\nvar" << var_id << ": ";
        if (is_derived[var_id]) {
            cout << "DERIVED, depends on: [";
            for (size_t i = 0; i < axiom_dependencies[var_id].size(); ++i) {
                cout << "var" << axiom_dependencies[var_id][i];
                if (i < axiom_dependencies[var_id].size() - 1) cout << ", ";
            }
            cout << "]" << endl;
            
            // Print full transitive closure to regular variables
            function<unordered_set<int>(int)> find_all_regular;
            find_all_regular = [&](int v) -> unordered_set<int> {
                unordered_set<int> result;
                if (v < 0 || v >= num_numeric_vars) return result;
                
                if (!is_derived[v]) {
                    result.insert(v);
                } else {
                    for (int dep : axiom_dependencies[v]) {
                        unordered_set<int> deps = find_all_regular(dep);
                        result.insert(deps.begin(), deps.end());
                    }
                }
                return result;
            };
            
            unordered_set<int> regular_deps = find_all_regular(var_id);
            cout << "  Traces to REGULAR variables: {";
            for (int reg : regular_deps) {
                cout << "var" << reg << " ";
            }
            cout << "}" << endl;
        } else {
            cout << "REGULAR (base variable)" << endl;
        }
    }
    
    cout << "\n===================================================\n" << endl;
}

DomainAbstraction CEGAR::build_abstraction(
    const TaskProxy &task_proxy) {
    cout << "Building domain abstraction..." << endl;
    utils::reserve_extra_memory_padding(memory_padding_in_mb);
    utils::CountdownTimer timer(max_time);

    // Blacklist logic axiom variables (derived variables that are NOT comparison axioms)
    // Logic axioms are typically used for goal compilation and should not be refined
    // Only comparison axioms should be refinable
    // MUST be done BEFORE compute_initial_domain_mapping!
    
    // First, collect all comparison axiom variable IDs
    unordered_set<int> comparison_axiom_var_ids;
    ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
    for (ComparisonAxiomProxy axiom : comparison_axioms) {
        int var_id = axiom.get_true_fact().get_variable().get_id();
        comparison_axiom_var_ids.insert(var_id);
    }
    
    // Now blacklist all axiom variables that are NOT comparison axioms
    cout << "Blacklisting logic axiom variables:" << endl;
    for (OperatorProxy axiom : task_proxy.get_axioms()) {
        for (EffectProxy eff : axiom.get_effects()) {
            int var_id = eff.get_fact().get_variable().get_id();
            // Only blacklist if this is NOT a comparison axiom
            if (comparison_axiom_var_ids.count(var_id) == 0) {
                blacklisted_variables.insert(var_id);
                cout << "  Blacklisted logic axiom variable " << var_id 
                     << " (" << eff.get_fact().get_variable().get_name() << ")" << endl;
            }
        }
    }

    DomainMapping domain_mapping =
        compute_initial_domain_mapping(task_proxy);
    cout << "Initial domain mapping: " << domain_mapping << endl;
    
    // Debug: Check which variables are derived/axiom variables
    cout << "Variable analysis:" << endl;
    for (int var_id = 0; var_id < task_proxy.get_variables().size(); ++var_id) {
        VariableProxy var = task_proxy.get_variables()[var_id];
        bool is_comparison = (comparison_axiom_var_ids.count(var_id) > 0);
        bool is_blacklisted = (blacklisted_variables.count(var_id) > 0);
        bool has_mapping = !domain_mapping[var_id].empty();
        cout << "  Variable " << var_id << " (" << var.get_name() << "): "
             << "comparison=" << (is_comparison ? "yes" : "no") << ", "
             << "blacklisted=" << (is_blacklisted ? "yes" : "no") << ", "
             << "has_mapping=" << (has_mapping ? "yes" : "no") << ", "
             << "domain_size=" << var.get_domain_size() << endl;
    }
    
    // Debug: Show logic axioms
    cout << "Logic axioms in task: " << task_proxy.get_axioms().size() << endl;
    for (size_t i = 0; i < task_proxy.get_axioms().size(); ++i) {
        OperatorProxy axiom = task_proxy.get_axioms()[i];
        cout << "  Logic axiom " << i << ": " << axiom.get_name() << endl;
        for (EffectProxy eff : axiom.get_effects()) {
            cout << "    affects variable " << eff.get_fact().get_variable().get_id() << endl;
        }
    }
    
    // Debug: Show comparison axioms
    cout << "Comparison axioms in task: " << g_comp_axioms.size() << endl;
    for (size_t i = 0; i < g_comp_axioms.size(); ++i) {
        const ComparisonAxiom &ax = g_comp_axioms[i];
        cout << "  Comparison axiom " << i << ": affects variable " << ax.affected_variable << endl;
    }
    
    // Initialize numeric domain mapping with full range (-inf, inf) for all numeric variables
    numeric_domain_mapping = compute_initial_numeric_domain_mapping(task_proxy);
    numeric_domain_sizes.resize(numeric_domain_mapping.size(), 1);
    
    // Build mapping from comparison axiom propositional variables to numeric variables
    build_comparison_axiom_mapping(task_proxy);
    
    // DEBUG: Print all comparison axiom mappings
    cout << "DEBUG: Comparison axiom mappings:" << endl;
    for (const auto &entry : comparison_axiom_dependencies) {
        int prop_var_id = entry.first;
        const unordered_set<int> &numeric_var_ids = entry.second;
        cout << "  Propositional var " << prop_var_id << " depends on numeric vars: ";
        for (int nvar : numeric_var_ids) {
            cout << nvar << " ";
        }
        cout << endl;
    }
    
    DomainAbstractionFactory factory(
        task_proxy, domain_mapping, abstract_domain_sizes,
        numeric_domain_mapping, numeric_domain_sizes,
        true, rng, use_wildcard_plans);
    DomainAbstraction abstraction = factory.generate();

    int iteration = 1;
    State concrete_init = task_proxy.get_initial_state();
    //concrete_init.unpack();
    while (!termination_criterion_satisfied(timer)) {
        
        //abstraction.dump(log);
        cout << "iteration #" << iteration << endl;

        vector<Fact> flaws =
            get_flaws(task_proxy, concrete_init, abstraction);

        if (flaws.empty() && detected_numeric_flaws.empty()) {
            
            cout << "No more flaws found, terminating CEGAR refinement."
                << endl;
           
            break;
        }

        // First try to fix propositional flaws (if any)
        bool flaws_fixed = true;
        if (!flaws.empty()) {
            flaws_fixed = fix_flaws(move(flaws), domain_mapping, abstraction.size());
        }
        
        // Then try to fix numeric flaws (if any)
        bool numeric_flaws_fixed = true;
        if (!detected_numeric_flaws.empty()) {
            numeric_flaws_fixed = fix_numeric_flaws(detected_numeric_flaws, abstraction.size(), task_proxy);
        }
        
        if (!flaws_fixed || !numeric_flaws_fixed) {
            assert(max_abstraction_size != numeric_limits<int>::max());
            cout << "Terminating CEGAR loop because fixing flaws "
                 << "surpasses abstraction size limit of "
                 << max_abstraction_size << " states." << endl;
            break;
        }


     
        
        // Validate that numeric_domain_sizes matches the actual partitions
      
        bool all_valid = true;
        for (size_t i = 0; i < numeric_domain_mapping.size(); ++i) {
            int actual_partitions = numeric_domain_mapping[i]->get_num_partitions();
            int expected_partitions = numeric_domain_sizes[i];
            if (actual_partitions != expected_partitions) {
                cout << "ERROR: num_" << i << " has " << actual_partitions 
                     << " partitions but expected " << expected_partitions << endl;
                all_valid = false;
            }
            if (!numeric_domain_mapping[i]->is_valid()) {
                cout << "ERROR: num_" << i << " has invalid mapping" << endl;
                all_valid = false;
            }
        }
        if (!all_valid) {
            cout << "CRITICAL ERROR: Numeric domain mapping validation failed!" << endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        }

        // DEBUG: Print what we're passing to the factory
        cout << "DEBUG CEGAR: Creating factory for iteration " << iteration << " with numeric_domain_sizes: ";
        for (size_t i = 0; i < min(numeric_domain_sizes.size(), size_t(10)); ++i) {
            cout << "v" << i << "=" << numeric_domain_sizes[i] << " ";
        }
        cout << endl;
        cout << "DEBUG CEGAR: Specifically, v66=" << numeric_domain_sizes[66] << ", v17=" << numeric_domain_sizes[17] 
             << ", v2=" << numeric_domain_sizes[2] << endl;
        
        DomainAbstractionFactory new_factory(
            task_proxy, domain_mapping, abstract_domain_sizes,
            numeric_domain_mapping, numeric_domain_sizes,
            true, rng, true);
        
        abstraction = new_factory.generate();
        ++iteration;
        
        // DEBUG: Print state 126 after iteration 2
        if (iteration == 3) {  // iteration was just incremented, so iteration 2 just finished
            // Check if state 126 exists in the abstraction
            int total_states = abstraction.size();
          
        }
    }

    if (utils::extra_memory_padding_is_reserved()) {
        utils::release_extra_memory_padding();
    }


    print_statistics(task_proxy, domain_mapping);
    cout << "Number of CEGAR iterations: " << iteration << endl;
    //abstraction.dump(log);

    return abstraction;
}

bool CEGAR::termination_criterion_satisfied(
    utils::CountdownTimer &timer) {
    if (timer.is_expired()) {
        cout << "Terminating CEGAR; time limit reached." << endl;
        return true;
    }
    if (!utils::extra_memory_padding_is_reserved()) {
        cout << "Terminating CEGAR; memory limit reached." << endl;
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
    
    int domain_size = abstract_domain_sizes[var_id];
    cout << "Domain size of var" << var_id << " is " << domain_size << endl;
    cout << "Old abstraction size: " << old_abstraction_size << endl;
    int abs_size_without_var = old_abstraction_size / domain_size;
    if (utils::is_product_within_limit(abs_size_without_var, domain_size + 1,
                                       max_abstraction_size)) {
        return true;
    }
    cout << "Cannot refine var" << var_id << " (size limit); blacklisting" << endl;
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
    if (numeric_var_id >= 0 && numeric_var_id < num_vars.size()) {
        NumericVariableProxy num_var = num_vars[numeric_var_id];
        numType var_type = num_var.get_var_type();
        
        cout << "DEBUG can_refine: num_" << numeric_var_id 
             << " has type=" << static_cast<int>(var_type) 
             << " (1=constant, 2=derived, 4=regular)" << endl;
        
        if (var_type == numType::constant) {
            cout << "Cannot refine num_" << numeric_var_id << " (CONSTANT); ignoring" << endl;
            return false;
        }
        
        if (var_type == numType::derived) {
            cout << "Cannot refine num_" << numeric_var_id << " (DERIVED); ignoring" << endl;
            return false;
        }
    }
    
    // Check if this numeric variable is in the abstraction
    if (numeric_var_id < 0 || numeric_var_id >= static_cast<int>(numeric_domain_mapping.size())) {
        return false;
    }
    
    int current_partitions = numeric_domain_mapping[numeric_var_id]->get_num_partitions();
    int abs_size_without_var = old_abstraction_size / current_partitions;

    cout << "Numeric variable " << numeric_var_id 
         << " has " << current_partitions << " partitions." << endl;
    cout << "Old abstraction size: " << old_abstraction_size << endl;
    cout << "Abstraction size without this variable: " << abs_size_without_var << endl;
    cout << "Max abstraction size: " << max_abstraction_size << endl;
    
    // Splitting will create one more partition
    if (utils::is_product_within_limit(abs_size_without_var, current_partitions + 1,
                                       max_abstraction_size)) {
        return true;
    }
    
    cout << "Cannot refine numeric variable " << numeric_var_id << "; blacklisting" << endl;
    blacklisted_numeric_variables.insert(numeric_var_id);
    return false;
}

ap_float CEGAR::extract_threshold_from_comparison(
    int prop_var_id, const TaskProxy &task_proxy) const {
    
    // Look up the comparison info for this propositional variable
    auto it = comparison_axiom_info.find(prop_var_id);
    if (it == comparison_axiom_info.end()) {
        // No comparison info found - shouldn't happen
        cerr << "WARNING: No comparison info for prop_var_id " << prop_var_id << endl;
        return 0.0;
    }
    
    const ComparisonInfo &info = it->second;
    int left_var_id = info.left_var_id;
    int right_var_id = info.right_var_id;
    
    // Get the numeric variables
    NumericVariablesProxy numeric_vars = task_proxy.get_numeric_variables();
    
    // One of the variables should be a constant (the threshold)
    // The other is the actual numeric variable being compared
    ap_float threshold = 0.0;
    bool found_threshold = false;
    
    if (left_var_id >= 0 && left_var_id < (int)numeric_vars.size()) {
        NumericVariableProxy left_var = numeric_vars[left_var_id];
        if (left_var.get_var_type() == numType::constant) {
            threshold = left_var.get_initial_state_value();
            found_threshold = true;
        }
    }
    
    if (right_var_id >= 0 && right_var_id < (int)numeric_vars.size()) {
        NumericVariableProxy right_var = numeric_vars[right_var_id];
        if (right_var.get_var_type() == numType::constant) {
            threshold = right_var.get_initial_state_value();
            found_threshold = true;
        }
    }
    
    if (!found_threshold) {
        // Neither side is a constant - this is a variable-to-variable comparison
        // In this case, there's no fixed threshold to split at
        cerr << "WARNING: Comparison axiom " << prop_var_id 
             << " has no constant threshold (both sides are variables)" << endl;
    }
    
    return threshold;
}

bool CEGAR::fix_numeric_flaws(
    const vector<NumericFlaw> &numeric_flaws, int abstraction_size, const TaskProxy &task_proxy) {
    
    if (numeric_flaws.empty()) {
        return true;
    }
    
    bool refined_any = false;
    
    for (const NumericFlaw &flaw : numeric_flaws) {
        int numeric_var_id = flaw.numeric_var_id;
        ap_float concrete_value = flaw.concrete_value;
        int prop_var_id = flaw.prop_var_id;
        
        // Check if we've attempted to refine this variable too many times
        int attempt_count = numeric_var_refinement_count[numeric_var_id];
        
        if (attempt_count >= 10) {
            cout << "DEBUG: Skipping flaw for num_" << numeric_var_id 
                 << " at value " << concrete_value
                 << " (variable refined " << attempt_count << " times already - may be stuck)"
                 << endl;
            continue;  // Skip this flaw to avoid infinite loop
        }
        
        cout << "DEBUG: Processing numeric flaw for num_" << numeric_var_id 
             << " at value " << concrete_value
             << " (variable refinement count: " << attempt_count << ")"
             << endl;
        
        // Check if we can refine this variable
        if (can_refine_numeric_variable(abstraction_size, numeric_var_id, task_proxy)) {
            // Bounds check
            if (numeric_var_id < 0 || numeric_var_id >= (int)numeric_domain_mapping.size()) {
                cout << "ERROR: numeric_var_id " << numeric_var_id 
                     << " is out of bounds! numeric_domain_mapping.size()=" 
                     << numeric_domain_mapping.size() << endl;
                continue;
            }
            
            // OPTION A: Split at both current value AND goal threshold
            // This prevents duplicate flaws while promoting progress toward the goal
            int old_num_partitions = numeric_domain_mapping[numeric_var_id]->get_num_partitions();
            
            // First split: at the concrete value
            int after_concrete_split = numeric_domain_mapping[numeric_var_id]->split_at(concrete_value);
            bool concrete_split_created_partition = (after_concrete_split > old_num_partitions);
            
            // Second split: at the goal threshold (if this is a comparison axiom flaw)
            ap_float threshold = extract_threshold_from_comparison(prop_var_id, task_proxy);
            int after_threshold_split = after_concrete_split;
            bool threshold_split_created_partition = false;
            
            // Only split at threshold if it's different from concrete_value
            // (to avoid splitting at the same point twice)
            if (threshold != concrete_value) {
                after_threshold_split = numeric_domain_mapping[numeric_var_id]->split_at(threshold);
                threshold_split_created_partition = (after_threshold_split > after_concrete_split);
                
                if (threshold_split_created_partition) {
                    cout << "DEBUG: Also split num_" << numeric_var_id 
                         << " at goal threshold " << threshold << endl;
                }
            }
            
            int new_num_partitions = after_threshold_split;
            
            if (new_num_partitions > old_num_partitions) {
                // Successfully split - created at least one new partition
                numeric_domain_sizes[numeric_var_id] = new_num_partitions;
                refined_any = true;
                
                // Increment refinement counter for this variable
                numeric_var_refinement_count[numeric_var_id]++;
                
                cout << "Refined num_" << numeric_var_id 
                     << " at value " << concrete_value;
                if (threshold != concrete_value) {
                    cout << " and threshold " << threshold;
                }
                cout << " (partitions: " << old_num_partitions << " -> " << new_num_partitions << ")"
                     << endl;
            } else {
                // No new partitions created - splits already exist
                cout << "DEBUG: Flaw for num_" << numeric_var_id 
                     << " at value " << concrete_value 
                     << " (threshold=" << threshold << ")"
                     << " - splits already exist (no refinement needed)" << endl;
            } 
        } else {
            cout << "DEBUG: Cannot refine num_" << numeric_var_id 
                 << " (blacklisted or size limit)" << endl;
        }
    }
    
    if (!refined_any) {
        cout << "WARNING: fix_numeric_flaws() called but no numeric variables were refined!" << endl;
        cout << "  This usually means all flaws are at partition boundaries," << endl;
        cout << "  which indicates the abstraction is already distinguishing these values." << endl;
    }
    
    return refined_any;
}

DomainAbstraction generate_domain_abstraction_with_cegar(
        int max_abstraction_size,
        double max_time,
        bool use_wildcard_plans,
        FlawTreatment flaw_treatment,
        InitSplitMethod init_split_method,
        NumericSplitStrategy numeric_split_strategy,
        const shared_ptr<utils::RandomNumberGenerator> &rng,
        const TaskProxy &task_proxy,
        unordered_set<int> &&init_split_var_ids,
        unordered_set<int> &&blacklisted_variables) {
    CEGAR cegar(
        max_abstraction_size,
        max_time,
        use_wildcard_plans,
        flaw_treatment,
        init_split_method,
        numeric_split_strategy,
        rng,
        move(init_split_var_ids),
        move(blacklisted_variables));
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
    std::cout << "DEBUG PARSER: Registering numeric_split_strategy option with values: standard, exclusion, default=standard" << std::endl;
    parser.add_enum_option(
        "numeric_split_strategy",
        numeric_split_strategy,
        "Strategy for splitting numeric variable domains: "
        "'standard' creates [lower, x) and [x, upper) with different partitions, "
        "'exclusion' creates R\\{x} (two disjoint ranges) and {x} as separate partitions.",
        "standard", {});
    std::cout << "DEBUG PARSER: numeric_split_strategy option registered successfully" << std::endl;
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
