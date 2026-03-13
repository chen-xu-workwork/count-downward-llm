#include "cegar.h"

#include "cegar_logger.h"
#include "domain_abstraction.h"
#include "domain_abstraction_factory.h"

#include "../axioms.h"
#include "utils.h"

#include <variant>
#include <algorithm>
#include <cmath>
#include <optional>
#include <tuple>
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

static void print_numeric_expression_tree(const TaskProxy &task_proxy,
                                          int prop_var_id,
                                          const shared_ptr<CEGARLogger> &logger);

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
    const bool use_threshold_aware_numeric_splits;
    const bool use_progress_weighted_flaw_selection;
    const int refinement_batch_size;
    const shared_ptr<utils::RandomNumberGenerator> &rng;
    const std::unordered_set<int> init_split_var_ids;
    std::unordered_set<int> blacklisted_variables;
    
    shared_ptr<CEGARLogger> logger;

    std::vector<int> abstract_domain_sizes;
    std::vector<int> real_domain_sizes;
    
    NumericDomainMappings numeric_domain_mapping;
    std::vector<int> numeric_domain_sizes;
    std::unordered_set<int> blacklisted_numeric_variables;
    
    mutable std::unordered_map<int, std::unordered_map<int, ap_float>>
        last_concrete_values_by_prop_var;
    
    std::unordered_map<int, std::unordered_set<int>> comparison_axiom_dependencies;
    std::unordered_set<int> comparison_axiom_var_ids;

    struct NumericSplitCheckKey {
        int var_id;
        ap_float value;
        bool include_in_lower;
        int version;

        bool operator==(const NumericSplitCheckKey &other) const {
            return var_id == other.var_id &&
                   value == other.value &&
                   include_in_lower == other.include_in_lower &&
                   version == other.version;
        }
    };

    struct NumericSplitCheckKeyHash {
        size_t operator()(const NumericSplitCheckKey &k) const {
            size_t h = std::hash<int>{}(k.var_id);
            h ^= std::hash<ap_float>{}(k.value) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<bool>{}(k.include_in_lower) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>{}(k.version) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    struct IncludeInLowerKey {
        int prop_var_id;
        int split_var_id;
        ap_float split_value;
        size_t dependency_state_sig;

        bool operator==(const IncludeInLowerKey &other) const {
            return prop_var_id == other.prop_var_id &&
                   split_var_id == other.split_var_id &&
                   split_value == other.split_value &&
                   dependency_state_sig == other.dependency_state_sig;
        }
    };

    struct IncludeInLowerKeyHash {
        size_t operator()(const IncludeInLowerKey &k) const {
            size_t h = std::hash<int>{}(k.prop_var_id);
            h ^= std::hash<int>{}(k.split_var_id) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<ap_float>{}(k.split_value) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<size_t>{}(k.dependency_state_sig) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    mutable std::unordered_map<NumericSplitCheckKey, bool, NumericSplitCheckKeyHash> can_split_cache;
    mutable std::unordered_map<IncludeInLowerKey, bool, IncludeInLowerKeyHash> include_in_lower_cache;
    std::vector<int> numeric_split_version;
    std::unordered_map<int, std::vector<ap_float>> numeric_split_hint_values;
    std::unordered_map<int, std::vector<ap_float>> numeric_split_step_sizes;
    std::unordered_set<int> goal_related_prop_vars;
    int current_iteration = 0;

    bool can_split_cached(int num_var_id, ap_float value, bool include_in_lower) const;
    void on_numeric_split(int num_var_id);
    void build_numeric_split_hints(const TaskProxy &task_proxy);
    void build_goal_related_prop_vars(const TaskProxy &task_proxy);
    std::vector<ap_float> get_numeric_split_candidates(int num_var_id, ap_float base_value) const;
    double score_flaw(const Flaw &flaw, int abstraction_size, const NumericDomainMappings &numeric_domain_mapping) const;
    bool fix_top_k_flaws(std::vector<Flaw> &&flaws,
                         DomainMapping &domain_mapping,
                         int abstraction_size,
                         NumericDomainMappings &numeric_domain_mapping);
    size_t compute_dependency_state_signature(
        int prop_var_id,
        int split_var_id,
        const std::vector<ap_float> &concrete_values) const;

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
                DomainMapping &domain_mapping, int abstraction_size, NumericDomainMappings &numeric_domain_mapping);
    bool fix_single_random_flaw(std::vector<Flaw> &&flaws,
                            DomainMapping &domain_mapping,
                            int abstraction_size, NumericDomainMappings &numeric_domain_mapping  );
    bool fix_single_flaw_max_refined(
        vector<Flaw> &&flaws, DomainMapping &domain_mapping,
        int abstraction_size, NumericDomainMappings &numeric_domain_mapping);
    bool fix_flaws_per_atom(std::vector<Flaw> &&flaws,
                        DomainMapping &domain_mapping,
                        int abstraction_size, NumericDomainMappings &numeric_domain_mapping);
    bool fix_flaws_per_variable(std::vector<Flaw> &&flaws,
                            DomainMapping &domain_mapping,
                            int abstraction_size, NumericDomainMappings &numeric_domain_mapping);

    bool can_refine_variable(int old_abstraction_size, int var_id);
    bool can_refine_numeric_variable(int old_abstraction_size, int numeric_var_id);

    int compute_current_abstraction_size() const;

    void add_variable_to_abstraction_if_necessary(
        int var, DomainMapping &abstraction);

    void print_statistics(const TaskProxy &task_proxy, const DomainMapping &domain_mapping);
    
    NumericDomainMappings compute_initial_numeric_domain_mapping(
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

    std::vector<Flaw> get_precondition_flaws(
        const OperatorProxy &op,
        const std::vector<int> &current_state,
        const std::vector<ap_float> &numeric_state,
        const TaskProxy &task_proxy) const;
    std::vector<Flaw> get_deviation_flaws(
        const OperatorProxy &op,
        const std::vector<ap_float> &numeric_current_state,
        const std::vector<int> &successor_state,
        const std::vector<ap_float> &numeric_successor_state,
        const std::vector<int> &abstract_successor_state,
        const std::vector<int> &abstract_numeric_successor_state,
        const DomainMapping &domain_mapping,
        const NumericDomainMappings &numeric_domain_mapping,
        const TaskProxy &task_proxy) const;
    std::vector<Flaw> get_goal_flaws(
        const TaskProxy &task_proxy,
        const std::vector<int> &current_state,
        const std::vector<ap_float> &numeric_state) const;
    enum class DependentNumericRefinement {
        NONE,
        ONE,
        ALL
    };
    bool try_refine_from_flaw(
        const Flaw &flaw,
        DomainMapping &domain_mapping,
        int abstraction_size,
        NumericDomainMappings &numeric_domain_mapping,
        DependentNumericRefinement dependent_numeric_refinement = DependentNumericRefinement::ONE,
        std::unordered_set<int> *refined_prop_vars = nullptr,
        std::unordered_set<int> *refined_numeric_vars = nullptr);

    void log_no_flaws_fixed_diagnostics(
        const std::vector<Flaw> &flaws,
        int abstraction_size,
        const NumericDomainMappings &numeric_domain_mapping) const;
    void dump_flaw(const Flaw &flaw) const;
public:
                CEGAR(int max_abstraction_size,
                    double max_time,
                    bool use_wildcard_plans,
                    bool deviation_flaws,
                        ExecEntirePlanMode exec_entire_plan,
                    FlawTreatment flaw_treatment,
                    InitSplitMethod init_split_method,
                    NumericSplitStrategy numeric_split_strategy,
                        bool use_threshold_aware_numeric_splits,
                        bool use_progress_weighted_flaw_selection,
                        int refinement_batch_size,
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
                bool use_threshold_aware_numeric_splits,
                bool use_progress_weighted_flaw_selection,
                int refinement_batch_size,
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
            use_threshold_aware_numeric_splits(use_threshold_aware_numeric_splits),
            use_progress_weighted_flaw_selection(use_progress_weighted_flaw_selection),
            refinement_batch_size(std::max(1, refinement_batch_size)),
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
    numeric_split_version.assign(task_proxy.get_numeric_variables().size(), 0);
}

bool CEGAR::can_split_cached(int num_var_id, ap_float value, bool include_in_lower) const {
    NumericSplitCheckKey key{num_var_id, value, include_in_lower, numeric_split_version[num_var_id]};
    auto it = can_split_cache.find(key);
    if (it != can_split_cache.end()) {
        return it->second;
    }
    bool result = numeric_domain_mapping[num_var_id]->can_split(value, include_in_lower);
    can_split_cache.emplace(key, result);
    return result;
}

void CEGAR::on_numeric_split(int num_var_id) {
    ++numeric_split_version[num_var_id];
}

void CEGAR::build_numeric_split_hints(const TaskProxy &task_proxy) {
    numeric_split_hint_values.clear();
    numeric_split_step_sizes.clear();

    const NumericVariablesProxy num_vars = task_proxy.get_numeric_variables();

    vector<ap_float> global_constant_values;
    global_constant_values.reserve(num_vars.size());
    for (size_t i = 0; i < num_vars.size(); ++i) {
        if (num_vars[i].get_var_type() == numType::constant) {
            global_constant_values.push_back(num_vars[i].get_initial_state_value());
        }
    }

    for (const auto &entry : comparison_axiom_dependencies) {
        for (int dep_var_id : entry.second) {
            auto &hints = numeric_split_hint_values[dep_var_id];
            hints.insert(hints.end(), global_constant_values.begin(), global_constant_values.end());
            hints.push_back(num_vars[dep_var_id].get_initial_state_value());
        }
    }

    OperatorsProxy operators = task_proxy.get_operators();
    for (OperatorProxy op : operators) {
        for (auto ass_eff_proxy : op.get_ass_effects()) {
            NumAssProxy ass_eff = ass_eff_proxy.get_assignment();
            int affected_var_id = ass_eff.get_affected_variable().get_id();
            if (affected_var_id < 0 || affected_var_id >= static_cast<int>(num_vars.size())) {
                continue;
            }
            if (num_vars[affected_var_id].get_var_type() != numType::regular) {
                continue;
            }
            NumericVariableProxy assigned_var = ass_eff.get_assigned_variable();
            if (assigned_var.get_var_type() == numType::constant) {
                ap_float magnitude = std::fabs(assigned_var.get_initial_state_value());
                if (magnitude > 0) {
                    numeric_split_step_sizes[affected_var_id].push_back(magnitude);
                }
            }
        }
    }

    for (auto &entry : numeric_split_hint_values) {
        auto &vals = entry.second;
        std::sort(vals.begin(), vals.end());
        vals.erase(std::unique(vals.begin(), vals.end()), vals.end());
    }
    for (auto &entry : numeric_split_step_sizes) {
        auto &vals = entry.second;
        std::sort(vals.begin(), vals.end());
        vals.erase(std::unique(vals.begin(), vals.end()), vals.end());
    }
}

void CEGAR::build_goal_related_prop_vars(const TaskProxy &task_proxy) {
    goal_related_prop_vars.clear();
    for (const FactProxy &goal : task_proxy.get_goals()) {
        goal_related_prop_vars.insert(goal.get_variable().get_id());
    }
    for (OperatorProxy axiom : task_proxy.get_axioms()) {
        if (axiom.get_preconditions().empty() || axiom.get_effects().size() != 1) {
            continue;
        }
        for (FactProxy pre : axiom.get_preconditions()) {
            goal_related_prop_vars.insert(pre.get_variable().get_id());
        }
    }
}

std::vector<ap_float> CEGAR::get_numeric_split_candidates(int num_var_id, ap_float base_value) const {
    std::vector<ap_float> candidates;
    candidates.push_back(base_value);

    if (!use_threshold_aware_numeric_splits) {
        return candidates;
    }

    auto hints_it = numeric_split_hint_values.find(num_var_id);
    auto steps_it = numeric_split_step_sizes.find(num_var_id);

    // Interval-aware candidates: use threshold boundaries and nearby interval centers.
    // This keeps split values aligned to semantically meaningful regions.
    vector<ap_float> boundaries;
    if (hints_it != numeric_split_hint_values.end()) {
        boundaries.insert(boundaries.end(), hints_it->second.begin(), hints_it->second.end());
        if (steps_it != numeric_split_step_sizes.end()) {
            for (ap_float t : hints_it->second) {
                for (ap_float s : steps_it->second) {
                    boundaries.push_back(t + s);
                    boundaries.push_back(t - s);
                }
            }
        }
    }

    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

    vector<pair<ap_float, ap_float>> ranked; // (distance, value)
    for (ap_float b : boundaries) {
        ranked.emplace_back(std::fabs(base_value - b), b);
    }

    // Add interval centers between adjacent boundaries to encourage coarser bucket splits.
    for (size_t i = 0; i + 1 < boundaries.size(); ++i) {
        ap_float mid = (boundaries[i] + boundaries[i + 1]) / 2.0;
        ranked.emplace_back(std::fabs(base_value - mid), mid);
    }

    // Keep the previous behavior as fallback when no threshold structure is available.
    if (ranked.empty() && hints_it != numeric_split_hint_values.end()) {
        for (ap_float t : hints_it->second) {
            ranked.emplace_back(std::fabs(base_value - t), t);
            if (steps_it != numeric_split_step_sizes.end()) {
                for (ap_float s : steps_it->second) {
                    ranked.emplace_back(std::fabs(base_value - (t + s)), t + s);
                    ranked.emplace_back(std::fabs(base_value - (t - s)), t - s);
                }
            }
        }
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        if (a.first != b.first) {
            return a.first < b.first;
        }
        return a.second < b.second;
    });

    std::unordered_set<ap_float> seen;
    seen.insert(base_value);
    const size_t max_candidates = 12;
    for (const auto &p : ranked) {
        ap_float candidate = p.second;
        if (seen.insert(candidate).second) {
            candidates.push_back(candidate);
            if (candidates.size() >= max_candidates) {
                break;
            }
        }
    }

    return candidates;
}

double CEGAR::score_flaw(const Flaw &flaw,
                        int abstraction_size,
                        const NumericDomainMappings &numeric_domain_mapping) const {
    (void)abstraction_size;
    auto residual_distance_score = [&](int num_id, ap_float value) -> double {
        auto hints_it = numeric_split_hint_values.find(num_id);
        if (hints_it == numeric_split_hint_values.end() || hints_it->second.empty()) {
            return 0.0;
        }

        ap_float min_dist = std::numeric_limits<ap_float>::infinity();
        for (ap_float t : hints_it->second) {
            min_dist = std::min(min_dist, std::fabs(value - t));
        }

        ap_float min_step = 1.0;
        auto steps_it = numeric_split_step_sizes.find(num_id);
        if (steps_it != numeric_split_step_sizes.end() && !steps_it->second.empty()) {
            min_step = std::numeric_limits<ap_float>::infinity();
            for (ap_float s : steps_it->second) {
                if (s > 0) {
                    min_step = std::min(min_step, s);
                }
            }
            if (!std::isfinite(min_step) || min_step <= 0) {
                min_step = 1.0;
            }
        }

        // Higher score when closer to a meaningful threshold (residual-driven).
        ap_float normalized = min_dist / min_step;
        return 1.0 / (1.0 + normalized);
    };

    return visit([&](auto &&f) -> double {
        using T = std::decay_t<decltype(f)>;
        if constexpr (std::is_same_v<T, PropFlaw>) {
            const Fact &fact = f.first;
            double score = 0.0;
            if (goal_related_prop_vars.count(fact.var)) {
                score += 100.0;
            }
            if (is_comparison_axiom_variable(fact.var)) {
                score += 40.0;
            }
            score += static_cast<double>(abstract_domain_sizes[fact.var]);
            for (const NumericFlaw &nf : f.second) {
                int num_id = std::get<0>(nf);
                ap_float val = std::get<1>(nf);
                bool dir = std::get<2>(nf);
                score += 60.0 * residual_distance_score(num_id, val);
                const auto candidates = get_numeric_split_candidates(num_id, val);
                bool any = false;
                for (ap_float c : candidates) {
                    if (can_split_cached(num_id, c, dir) || can_split_cached(num_id, c, !dir)) {
                        any = true;
                        break;
                    }
                }
                if (any) {
                    score += 10.0;
                }
            }
            return score;
        } else {
            int num_id = std::get<0>(f);
            ap_float val = std::get<1>(f);
            bool dir = std::get<2>(f);
            double score = 50.0 + numeric_domain_mapping[num_id]->get_num_partitions();
            score += 120.0 * residual_distance_score(num_id, val);
            const auto candidates = get_numeric_split_candidates(num_id, val);
            bool any = false;
            for (ap_float c : candidates) {
                if (can_split_cached(num_id, c, dir) || can_split_cached(num_id, c, !dir)) {
                    any = true;
                    break;
                }
            }
            if (!any) {
                score -= 1000.0;
            }
            return score;
        }
    }, flaw);
}

size_t CEGAR::compute_dependency_state_signature(
    int prop_var_id,
    int split_var_id,
    const std::vector<ap_float> &concrete_values) const {
    auto deps_it = comparison_axiom_dependencies.find(prop_var_id);
    if (deps_it == comparison_axiom_dependencies.end()) {
        return 0;
    }
    std::vector<int> dep_vars(deps_it->second.begin(), deps_it->second.end());
    std::sort(dep_vars.begin(), dep_vars.end());
    size_t h = 0x9e3779b97f4a7c15ULL;
    for (int var_id : dep_vars) {
        if (var_id == split_var_id) {
            continue;
        }
        h ^= std::hash<int>{}(var_id) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<ap_float>{}(concrete_values[var_id]) + 0x9e3779b9 + (h << 6) + (h >> 2);
    }
    return h;
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
    logger->log(Verbosity::DEBUG, "");
    if (init_split_method == InitSplitMethod::RANDOM_PARTITION
        || initialization_fits_size_limit(abstraction_size, var_id)) {
        pair<int, vector<int>> init_split;
        switch (init_split_method) {
        case InitSplitMethod::GOAL_VALUE:
            //assert(variable_specified_in_goal(var_id, task_proxy));
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

    bool is_comparison_var = is_comparison_axiom_variable(var_id);
    if (is_comparison_var) {
        logger->log(Verbosity::DEBUG, "  Variable ", var_id, " is a comparison axiom - splitting at initial value 0 (true in numeric fd) instead of actual initial value");
        init_split[0] = 1;
        return make_pair(2, move(init_split));
    } else {
        int init_value = task_proxy.get_initial_state()[var_id].get_value();
        logger->log(Verbosity::DEBUG, "init value ", init_value);
        assert(utils::in_bounds(init_value, init_split));
        init_split[init_value] = 1;
        return make_pair(2, move(init_split));
    }
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
                    assert(dep_var_id >= 0 && dep_var_id < static_cast<int>(numeric_state.size()));
                    ap_float concrete_value = numeric_state[dep_var_id];
                    bool is_lower = determine_include_in_lower(var_id, dep_var_id, concrete_value, numeric_state, task_proxy);
                    if (can_split_cached(dep_var_id, concrete_value, is_lower)) {
                        NumericFlaw numeric_flaw{dep_var_id, concrete_value, is_lower};
                        numeric_flaws.push_back(numeric_flaw);

                    } else if (can_split_cached(dep_var_id, concrete_value, !is_lower)) {
                        NumericFlaw numeric_flaw{dep_var_id, concrete_value, !is_lower};
                        numeric_flaws.push_back(numeric_flaw);
                    }
                    
                }

                flaws.emplace_back(
                    std::in_place_type<PropFlaw>,
                    Fact(var_id, pre.get_value()),
                    move(numeric_flaws)
                );
                logger->log(Verbosity::DEBUG, "Precondition Flaw");
                dump_flaw(flaws.back());
            } else {
                flaws.emplace_back(
                    std::in_place_type<PropFlaw>,
                    Fact(var_id, pre.get_value()),
                    std::vector<NumericFlaw>{}
                );
                logger->log(Verbosity::DEBUG, "Precondition Flaw");
                dump_flaw(flaws.back());
            }
        }
    }
    return flaws;
}

vector<CEGAR::Flaw> CEGAR::get_deviation_flaws(
    const OperatorProxy &op,
    const vector<ap_float> &numeric_current_state,
    const vector<int> &successor_state, const vector<ap_float> &numeric_successor_state,
    const vector<int> &abstract_successor_state, const vector<int> &abstract_numeric_successor_state,
    const DomainMapping &domain_mapping,
    const NumericDomainMappings &numeric_domain_mapping,
    const TaskProxy &task_proxy) const {
    // TODO: Blacklisting????!!!!!
    vector<Flaw> flaws;

    /*
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
            vector<NumericFlaw> numeric_flaws;
            for (int dep_var_id : it->second) {
                assert(dep_var_id >= 0 && dep_var_id < static_cast<int>(numeric_successor_state.size()));
                ap_float concrete_value = numeric_current_state[dep_var_id];
                //ap_float concrete_value = numeric_successor_state[dep_var_id];
                //bool is_lower = determine_include_in_lower(var_id, dep_var_id, concrete_value, numeric_successor_state, task_proxy);
                bool is_lower = determine_include_in_lower(var_id, dep_var_id, concrete_value, numeric_current_state, task_proxy);
                NumericFlaw numeric_flaw{dep_var_id, concrete_value, is_lower};
                numeric_flaws.push_back(numeric_flaw);
            }
            PropFlaw pf{Fact(static_cast<int>(var_id), correct_abstract_value), numeric_flaws};
            flaws.push_back(pf);
            logger->log(Verbosity::DEBUG, "Deviation Flaw");
            dump_flaw(flaws.back());
            print_numeric_expression_tree(task_proxy, var_id, logger);
        }
    }
    */

    for (size_t var_id = 0; var_id < numeric_successor_state.size(); ++var_id) {
        bool operator_modified_var = false;
        for (auto ass_eff_proxy : op.get_ass_effects()) {
            NumAssProxy effect = ass_eff_proxy.get_assignment();
            if (effect.get_affected_variable().get_id() == static_cast<int>(var_id)) {
                operator_modified_var = true;
                break;
            }
        }
        if (!operator_modified_var) {
            continue;
        }

        int abstract_value = abstract_numeric_successor_state[var_id];
        int correct_abstract_value = numeric_domain_mapping[var_id]->get_partition_index(
            numeric_successor_state[var_id]);
        if (abstract_value != correct_abstract_value) {
            ap_float concrete_next_value = numeric_successor_state[var_id];
            ap_float concrete_current_value = numeric_current_state[var_id];

            bool operator_increased_value = concrete_next_value > concrete_current_value;
            if (concrete_next_value == concrete_current_value) {
                continue;
            }
            bool is_lower = !operator_increased_value;

            if (can_split_cached(static_cast<int>(var_id), concrete_current_value, is_lower)) {
                NumericFlaw numeric_flaw{static_cast<int>(var_id), concrete_current_value, is_lower};
                flaws.push_back(numeric_flaw);

            } else if (can_split_cached(static_cast<int>(var_id), concrete_current_value, !is_lower)) {
                NumericFlaw numeric_flaw{static_cast<int>(var_id), concrete_current_value, !is_lower};
                flaws.push_back(numeric_flaw);
            }
            if (!flaws.empty()) {
                dump_flaw(flaws.back());    
            }
        }
    }
    return flaws;
}


static const char *cal_operator_to_string(cal_operator op) {
    switch (op) {
    case cal_operator::sum:
        return "+";
    case cal_operator::diff:
        return "-";
    case cal_operator::mult:
        return "*";
    case cal_operator::divi:
        return "/";
    }
    return "?";
}

static const char *comp_operator_to_string(comp_operator op) {
    switch (op) {
    case comp_operator::lt:
        return "<";
    case comp_operator::le:
        return "<=";
    case comp_operator::eq:
        return "==";
    case comp_operator::ge:
        return ">=";
    case comp_operator::gt:
        return ">";
    case comp_operator::ue:
        return "!=";
    }
    return "?";
}

[[maybe_unused]] static void print_numeric_expression_tree(const TaskProxy &task_proxy,
                                                           int prop_var_id,
                                                           const shared_ptr<CEGARLogger> &logger) {
    if (!logger) {
        return;
    }

    VariableProxy prop_var = task_proxy.get_variables()[prop_var_id];

    optional<ComparisonAxiomProxy> comparison_axiom;
    ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
    for (ComparisonAxiomProxy axiom : comparison_axioms) {
        if (axiom.get_true_fact().get_variable().get_id() == prop_var_id) {
            comparison_axiom = axiom;
            break;
        }
    }

    if (!comparison_axiom) {
        logger->log(Verbosity::DEBUG,
                    "No comparison axiom found for v", prop_var_id,
                    " (", prop_var.get_name(), ")");
        return;
    }

    unordered_map<int, AssignmentAxiomProxy> assignment_by_effect;
    AssignmentAxiomsProxy assignment_axioms = task_proxy.get_assignment_axioms();
    for (AssignmentAxiomProxy axiom : assignment_axioms) {
        int effect_var_id = axiom.get_assignment_variable().get_id();
        assignment_by_effect.emplace(effect_var_id, axiom);
    }

    auto numeric_label = [&](int num_var_id) {
        NumericVariableProxy num_var = task_proxy.get_numeric_variables()[num_var_id];
        string type_str;
        switch (num_var.get_var_type()) {
        case numType::regular:
            type_str = "regular";
            break;
        case numType::derived:
            type_str = "derived";
            break;
        case numType::constant:
            type_str = "constant";
            break;
        default:
            type_str = "unknown";
            break;
        }

        string label = "num" + to_string(num_var_id) + " (" + num_var.get_name() + ", " + type_str + ")";
        if (num_var.get_var_type() == numType::constant) {
            label += " = " + to_string(num_var.get_initial_state_value());
        }
        return label;
    };

    auto print_branch = [&](const string &prefix, bool is_last, const string &text) {
        logger->log(Verbosity::DEBUG, prefix, (is_last ? "└─ " : "├─ "), text);
    };

    unordered_set<int> recursion_stack;
    auto print_numeric_subtree = [&](int num_var_id,
                                     const string &prefix,
                                     bool is_last,
                                     auto &print_numeric_subtree_ref) -> void {
        print_branch(prefix, is_last, numeric_label(num_var_id));

        auto it = assignment_by_effect.find(num_var_id);
        if (it == assignment_by_effect.end()) {
            return;
        }

        if (recursion_stack.count(num_var_id)) {
            string child_prefix = prefix + (is_last ? "   " : "│  ");
            print_branch(child_prefix, true, "<cycle detected>");
            return;
        }

        recursion_stack.insert(num_var_id);

        AssignmentAxiomProxy defining_axiom = it->second;
        NumericVariableProxy left = defining_axiom.get_left_variable();
        NumericVariableProxy right = defining_axiom.get_right_variable();

        string child_prefix = prefix + (is_last ? "   " : "│  ");
        string op_text = string("op ") + cal_operator_to_string(defining_axiom.get_arithmetic_operator_type());
        print_branch(child_prefix, false, op_text);

        string op_child_prefix = child_prefix + "│  ";
        print_numeric_subtree_ref(left.get_id(), op_child_prefix, false, print_numeric_subtree_ref);
        print_numeric_subtree_ref(right.get_id(), op_child_prefix, true, print_numeric_subtree_ref);

        recursion_stack.erase(num_var_id);
    };

    ComparisonAxiomProxy root = *comparison_axiom;
    NumericVariableProxy left_root = root.get_left_variable();
    NumericVariableProxy right_root = root.get_right_variable();

    logger->log(Verbosity::DEBUG,
                "Numeric expression tree for v", prop_var_id,
                " (", prop_var.get_name(), ")");
    logger->log(Verbosity::DEBUG,
                "└─ comparison ",
                comp_operator_to_string(root.get_comparison_operator_type()));
    print_numeric_subtree(left_root.get_id(), "   ", false, print_numeric_subtree);
    print_numeric_subtree(right_root.get_id(), "   ", true, print_numeric_subtree);
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
                        assert(dep_var_id >= 0 && dep_var_id < static_cast<int>(numeric_state.size()));
                        ap_float concrete_value = numeric_state[dep_var_id];
                        bool is_lower = determine_include_in_lower(var_id, dep_var_id, concrete_value, numeric_state, task_proxy);
                        if (can_split_cached(dep_var_id, concrete_value, is_lower)) {
                            NumericFlaw numeric_flaw{dep_var_id, concrete_value, is_lower};
                            numeric_flaws.push_back(numeric_flaw);
                        } else if (can_split_cached(dep_var_id, concrete_value, !is_lower)) {
                            NumericFlaw numeric_flaw{dep_var_id, concrete_value, !is_lower};
                            numeric_flaws.push_back(numeric_flaw);
                        }
                        if (!numeric_flaws.empty()) {
                            dump_flaw(numeric_flaws.back());    
                        }
                    }
                    flaws.emplace_back(
                        std::in_place_type<PropFlaw>,
                        Fact(var_id, goal.get_value()),
                        move(numeric_flaws)
                    );
                    logger->log(Verbosity::DEBUG, "Comp Goal Flaw");
                    dump_flaw(flaws.back());
                    print_numeric_expression_tree(task_proxy, var_id, logger);
                    
                } else {
                    flaws.emplace_back(
                        std::in_place_type<PropFlaw>,
                        Fact(var_id, goal.get_value()),
                        move(std::vector<NumericFlaw>{})
                    );
                    logger->log(Verbosity::DEBUG, "Reg Goal Flaw");
                    dump_flaw(flaws.back());
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
                            if (can_split_cached(dep_var_id, concrete_value, is_lower)) {
                                NumericFlaw numeric_flaw{dep_var_id, concrete_value, is_lower};
                                numeric_flaws.push_back(numeric_flaw);
                            } else if (can_split_cached(dep_var_id, concrete_value, !is_lower)) {
                                NumericFlaw numeric_flaw{dep_var_id, concrete_value, !is_lower};
                                numeric_flaws.push_back(numeric_flaw);
                            }
                            if (!numeric_flaws.empty()) {
                                dump_flaw(numeric_flaws.back());    
                            }
                        }
                        flaws.emplace_back(
                            std::in_place_type<PropFlaw>,
                            Fact(var_id, pre.get_value()),
                            move(numeric_flaws)
                        );
                        logger->log(Verbosity::DEBUG, "Comp Goal Flaw");
                        dump_flaw(flaws.back());
                        print_numeric_expression_tree(task_proxy, var_id, logger);
                    } else {
                        flaws.emplace_back(
                            std::in_place_type<PropFlaw>,
                            Fact(var_id, pre.get_value()),
                            std::vector<NumericFlaw>{}
                        );

                        logger->log(Verbosity::DEBUG, "Reg Goal Flaw");
                        dump_flaw(flaws.back());
                    }
                }
            }
        }
    }
    
    return flaws;
}

void CEGAR::dump_flaw(const Flaw &flaw) const {
    if (!logger) {
        return;
    }

    auto dump_numeric_flaw = [&](const NumericFlaw &numeric_flaw,
                                 const string &prefix) {
        logger->log(Verbosity::DEBUG,
                    prefix,
                    "num", std::get<0>(numeric_flaw),
                    "=", std::get<1>(numeric_flaw),
                    ", is_lower=", (std::get<2>(numeric_flaw) ? "true" : "false"));
    };

    visit([&](auto &&f) {
        using T = std::decay_t<decltype(f)>;
        if constexpr (std::is_same_v<T, PropFlaw>) {
            const Fact &fact = f.first;
            const vector<NumericFlaw> &numeric_flaws = f.second;
            logger->log(Verbosity::DEBUG,
                        "Flaw(type=PropFlaw, v", fact.var,
                        "=", fact.value,
                        ", is_comparison_axiom=",
                        (is_comparison_axiom_variable(fact.var) ? "true" : "false"),
                        ", num_numeric_flaws=", numeric_flaws.size(), ")");
            for (size_t i = 0; i < numeric_flaws.size(); ++i) {
                dump_numeric_flaw(numeric_flaws[i],
                                  "  dependent_numeric_flaw[" +
                                      to_string(i) + "]: ");
            }
        } else if constexpr (std::is_same_v<T, NumericFlaw>) {
            logger->log(Verbosity::DEBUG, "Flaw(type=NumericFlaw)");
            dump_numeric_flaw(f, "  ");
        }
    }, flaw);
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
        const NumericVariablesProxy numeric_vars = task_proxy.get_numeric_variables();
        for (size_t i = 0; i < numeric_vars.size(); ++i) {
            if (numeric_vars[i].get_var_type() != numType::regular) {
                continue;
            }
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
    const NumericDomainMappings &ndm = abstraction.get_numeric_domain_mapping();
    
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
    const NumericDomainMappings &numeric_domain_mapping = abstraction.get_numeric_domain_mapping();

    int step_num = 1;
    for (vector<int> &equivalent_ops : wildcard_plan) {
        assert(flaws.empty()); 
        for (int op_id : equivalent_ops) {
            // Be defensive: some abstract plans may contain invalid operator IDs
            // (e.g., -1 as a sentinel). Avoid undefined behavior from indexing
            // with a negative or out-of-range id.
            if (op_id < 0 || op_id >= static_cast<int>(task_proxy.get_operators().size())) {
                logger->log(Verbosity::NONE,
                            "CEGAR warning: abstract plan contains invalid operator id ",
                            op_id, " (num_operators=", task_proxy.get_operators().size(), "). Skipping.");
                continue;
            }
            OperatorProxy op = task_proxy.get_operators()[op_id];
            string op_name = op.get_name();

            string decoded_state = decode_abstract_state_compact(current_prop_state, current_numeric_state);
            logger->log(Verbosity::DEBUG, "[PLAN] ", decoded_state, ", ", op_name);

            vector<Flaw> operator_flaws =
                get_precondition_flaws(op, current_prop_state, current_numeric_state, task_proxy);
            if (operator_flaws.empty()) {
                flaws.clear();
                vector<ap_float> numeric_state_before_op = current_numeric_state;
                apply_op_to_state(current_prop_state, op);
                apply_numeric_effects(current_numeric_state, op);
                g_axiom_evaluator->evaluate_arithmetic_axioms(current_numeric_state);
                g_axiom_evaluator->evaluate(current_prop_state, current_numeric_state);

                assert(step_num < abstract_prop_states.size());
                assert(step_num < abstract_numeric_states.size());

                const vector<int> &abstract_state = abstract_prop_states[step_num];
                const vector<int> &abstract_numeric_state = abstract_numeric_states[step_num];
                decoded_state = decode_abstract_state_compact(current_prop_state, current_numeric_state);

                logger->log(Verbosity::DEBUG, "[SUCC] ", decoded_state);


                if (this->deviation_flaws) {
                    vector<Flaw> deviation_flaw_list = get_deviation_flaws(
                        op,
                        numeric_state_before_op,
                        current_prop_state, current_numeric_state,
                        abstract_state, abstract_numeric_state,
                        domain_mapping, numeric_domain_mapping, task_proxy);
                    if (deviation_flaw_list.empty()) {
                        break;
                    }
                    logger->log(Verbosity::DEBUG, "Found ", deviation_flaw_list.size(), " deviation flaws after applying operator ", op_name);
                    flaws.insert(flaws.end(), deviation_flaw_list.begin(), deviation_flaw_list.end());
                } else {
                    break;
                }
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

    logger->log(Verbosity::DEBUG, "Plan is valid in the concrete state, checking goal conditions...");

    string decoded_state = decode_abstract_state_compact(current_prop_state, current_numeric_state);
    logger->log(Verbosity::DEBUG, "[GOAL] ", decoded_state);

    assert(flaws.empty());

    vector<Flaw> goal_flaws =
        get_goal_flaws(task_proxy, current_prop_state, current_numeric_state);
    flaws.insert(flaws.end(), goal_flaws.begin(), goal_flaws.end());
    return flaws;
}

bool CEGAR::fix_flaws(
    vector<Flaw> &&flaws, DomainMapping &domain_mapping,
    int abstraction_size, NumericDomainMappings &numeric_domain_mapping) {
    if (refinement_batch_size > 1) {
        return fix_top_k_flaws(move(flaws), domain_mapping, abstraction_size, numeric_domain_mapping);
    }

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

bool CEGAR::fix_top_k_flaws(
    vector<Flaw> &&flaws,
    DomainMapping &domain_mapping,
    int abstraction_size,
    NumericDomainMappings &numeric_domain_mapping) {
    if (flaws.empty()) {
        return false;
    }

    vector<int> order;
    order.reserve(flaws.size());
    for (int i = 0; i < static_cast<int>(flaws.size()); ++i) {
        order.push_back(i);
    }

    if (use_progress_weighted_flaw_selection) {
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return score_flaw(flaws[a], abstraction_size, numeric_domain_mapping) >
                   score_flaw(flaws[b], abstraction_size, numeric_domain_mapping);
        });
    } else {
        for (int i = static_cast<int>(order.size()) - 1; i > 0; --i) {
            int j = rng->random(i + 1);
            std::swap(order[i], order[j]);
        }
    }

    bool changed = false;
    int current_size = abstraction_size;
    int applied = 0;
    std::unordered_set<int> refined_prop_vars;
    std::unordered_set<int> refined_numeric_vars;

    for (int idx : order) {
        if (applied >= refinement_batch_size) {
            break;
        }
        bool local_changed = try_refine_from_flaw(
            flaws[idx], domain_mapping, current_size, numeric_domain_mapping,
            DependentNumericRefinement::ONE,
            &refined_prop_vars, &refined_numeric_vars);
        if (local_changed) {
            changed = true;
            ++applied;
            current_size = compute_current_abstraction_size();
        }
    }

    return changed;
}

bool CEGAR::fix_single_random_flaw(
    vector<Flaw> &&flaws, DomainMapping &domain_mapping,
    int abstraction_size, NumericDomainMappings &numeric_domain_mapping) {
    assert(!flaws.empty());

    vector<int> order;
    order.reserve(flaws.size());
    for (int i = 0; i < static_cast<int>(flaws.size()); ++i) {
        order.push_back(i);
    }

    if (use_progress_weighted_flaw_selection) {
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return score_flaw(flaws[a], abstraction_size, numeric_domain_mapping) >
                   score_flaw(flaws[b], abstraction_size, numeric_domain_mapping);
        });
    } else {
        for (int i = static_cast<int>(order.size()) - 1; i > 0; --i) {
            int j = rng->random(i + 1);
            std::swap(order[i], order[j]);
        }
    }

    for (int idx : order) {
        const Flaw &chosen = flaws[idx];
        if (try_refine_from_flaw(chosen, domain_mapping, abstraction_size, numeric_domain_mapping)) {
            return true;
        }
    }
    return false;
}

int CEGAR::compute_current_abstraction_size() const {
    long long size = 1;
    for (int d : abstract_domain_sizes) {
        if (d <= 0) {
            return 0;
        }
        size *= d;
        if (size > std::numeric_limits<int>::max()) {
            return std::numeric_limits<int>::max();
        }
    }
    for (int p : numeric_domain_sizes) {
        if (p <= 0) {
            return 0;
        }
        size *= p;
        if (size > std::numeric_limits<int>::max()) {
            return std::numeric_limits<int>::max();
        }
    }
    return static_cast<int>(size);
}

bool CEGAR::try_refine_from_flaw(
    const Flaw &flaw,
    DomainMapping &domain_mapping,
    int abstraction_size,
    NumericDomainMappings &numeric_domain_mapping,
    DependentNumericRefinement dependent_numeric_refinement,
    std::unordered_set<int> *refined_prop_vars,
    std::unordered_set<int> *refined_numeric_vars) {

    // IMPORTANT: a single "refine from flaw" can apply multiple refinements
    // (e.g., split a comparison-axiom propositional var AND split one or more
    // dependent numeric vars). We must check the size limit against the
    // cumulative abstraction size.
    int current_abstraction_size = abstraction_size;

    return visit([&](auto &&f) -> bool {
        using T = std::decay_t<decltype(f)>;

        if constexpr (std::is_same_v<T, PropFlaw>) {
            const Fact &fact = f.first;
            if (refined_prop_vars && refined_prop_vars->count(fact.var)) {
                return false;
            }
            if (!can_refine_variable(current_abstraction_size, fact.var)) {
                return false;
            }

            add_variable_to_abstraction_if_necessary(fact.var, domain_mapping);

            if (!is_comparison_axiom_variable(fact.var)) {
                int old_domain_size = abstract_domain_sizes[fact.var];
                domain_mapping[fact.var][fact.value] = abstract_domain_sizes[fact.var];
                abstract_domain_sizes[fact.var] += 1;
                // Update running abstraction size (exact, since size is a product).
                if (old_domain_size > 0) {
                    long long new_size = (static_cast<long long>(current_abstraction_size) / old_domain_size) * (old_domain_size + 1);
                    current_abstraction_size = new_size > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : static_cast<int>(new_size);
                }
                if (refined_prop_vars) {
                    refined_prop_vars->insert(fact.var);
                }
                return true;
            }

            bool prop_changed = false;
            int old_domain_size = abstract_domain_sizes[fact.var];
            if (abstract_domain_sizes[fact.var] < 2) {
                domain_mapping[fact.var][0] = 1;
                abstract_domain_sizes[fact.var] = 2;
                prop_changed = true;
            } else {
                domain_mapping[fact.var][0] = 1;
                abstract_domain_sizes[fact.var] = 2;
            }

            // Update running abstraction size if this was the first refinement of the comparison axiom variable.
            if (prop_changed && old_domain_size > 0 && old_domain_size != 2) {
                long long new_size = (static_cast<long long>(current_abstraction_size) / old_domain_size) * 2;
                current_abstraction_size = new_size > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : static_cast<int>(new_size);
            }
            if (refined_prop_vars) {
                refined_prop_vars->insert(fact.var);
            }

            const std::vector<NumericFlaw> &dependent_numeric_flaws = f.second;
            if (dependent_numeric_refinement == DependentNumericRefinement::NONE || dependent_numeric_flaws.empty()) {
                return prop_changed;
            }

            auto try_refine_numeric = [&](const NumericFlaw &nf) -> bool {
                int num_id = std::get<0>(nf);
                const ap_float &val = std::get<1>(nf);
                bool flag = std::get<2>(nf);

                if (refined_numeric_vars && refined_numeric_vars->count(num_id)) {
                    return false;
                }
                // Use the *current* abstraction size, since we may already have refined
                // the associated comparison-axiom propositional variable.
                if (!can_refine_numeric_variable(current_abstraction_size, num_id)) {
                    return false;
                }

                int old_partitions = numeric_domain_mapping[num_id]->get_num_partitions();

                const vector<ap_float> candidates = get_numeric_split_candidates(num_id, val);
                std::optional<std::pair<ap_float, bool>> chosen;
                for (ap_float candidate : candidates) {
                    if (can_split_cached(num_id, candidate, flag)) {
                        chosen = {candidate, flag};
                        break;
                    }
                    if (can_split_cached(num_id, candidate, !flag)) {
                        chosen = {candidate, !flag};
                        break;
                    }
                }
                if (!chosen) {
                    return false;
                }

                numeric_domain_mapping[num_id]->split_at(chosen->first, chosen->second);
                on_numeric_split(num_id);
                numeric_domain_sizes[num_id] = numeric_domain_mapping[num_id]->get_num_partitions();

                // Update running abstraction size.
                int new_partitions = numeric_domain_sizes[num_id];
                if (old_partitions > 0 && new_partitions > 0) {
                    long long new_size = (static_cast<long long>(current_abstraction_size) / old_partitions) * new_partitions;
                    current_abstraction_size = new_size > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : static_cast<int>(new_size);
                }
                if (refined_numeric_vars) {
                    refined_numeric_vars->insert(num_id);
                }
                return true;
            };

            if (dependent_numeric_refinement == DependentNumericRefinement::ALL) {
                bool any_numeric_changed = false;
                for (const NumericFlaw &nf : dependent_numeric_flaws) {
                    any_numeric_changed = try_refine_numeric(nf) || any_numeric_changed;
                }
                return any_numeric_changed || prop_changed;
            }

            // ONE: try all dependent numeric flaws in random order until one split succeeds.
            vector<int> order;
            order.reserve(dependent_numeric_flaws.size());
            for (int i = 0; i < static_cast<int>(dependent_numeric_flaws.size()); ++i) {
                order.push_back(i);
            }
            for (int i = static_cast<int>(order.size()) - 1; i > 0; --i) {
                int j = rng->random(i + 1);
                std::swap(order[i], order[j]);
            }
            for (int idx : order) {
                if (try_refine_numeric(dependent_numeric_flaws[idx])) {
                    return true;
                }
            }

            return prop_changed;
        } else {
            int num_id = std::get<0>(f);
            const ap_float &val = std::get<1>(f);
            bool flag = std::get<2>(f);

            if (refined_numeric_vars && refined_numeric_vars->count(num_id)) {
                return false;
            }

            if (!can_refine_numeric_variable(current_abstraction_size, num_id)) {
                return false;
            }

            int old_partitions = numeric_domain_mapping[num_id]->get_num_partitions();

            const vector<ap_float> candidates = get_numeric_split_candidates(num_id, val);
            std::optional<std::pair<ap_float, bool>> chosen;
            for (ap_float candidate : candidates) {
                if (can_split_cached(num_id, candidate, flag)) {
                    chosen = {candidate, flag};
                    break;
                }
                if (can_split_cached(num_id, candidate, !flag)) {
                    chosen = {candidate, !flag};
                    break;
                }
            }

            if (!chosen) {
                return false;
            }

            numeric_domain_mapping[num_id]->split_at(chosen->first, chosen->second);
            on_numeric_split(num_id);
            numeric_domain_sizes[num_id] = numeric_domain_mapping[num_id]->get_num_partitions();

            int new_partitions = numeric_domain_sizes[num_id];
            if (old_partitions > 0 && new_partitions > 0) {
                long long new_size = (static_cast<long long>(current_abstraction_size) / old_partitions) * new_partitions;
                current_abstraction_size = new_size > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : static_cast<int>(new_size);
            }
            if (refined_numeric_vars) {
                refined_numeric_vars->insert(num_id);
            }
            return true;
        }
    }, flaw);
}

bool CEGAR::fix_single_flaw_max_refined(
    vector<Flaw> &&flaws, DomainMapping &domain_mapping,
    int abstraction_size, NumericDomainMappings &numeric_domain_mapping) {
    // MAX_REFINED_SINGLE_ATOM:
    // Prefer fixing flaws whose associated variable(s) are already the most refined,
    // but do not get stuck if the current maximum tier is unrefinable.
    struct Candidate {
        int flaw_index;
        int score;
        vector<int> dependent_numeric_indices;  // only used for comparison-axiom PropFlaws
    };

    vector<Candidate> candidates;
    candidates.reserve(flaws.size());

    for (int i = 0; i < static_cast<int>(flaws.size()); ++i) {
        int score = 0;
        vector<int> dep_candidates;
        visit([&](auto &&f) {
            using T = std::decay_t<decltype(f)>;
            if constexpr (std::is_same_v<T, PropFlaw>) {
                const Fact &fact = f.first;
                score = abstract_domain_sizes[fact.var];
                if (is_comparison_axiom_variable(fact.var)) {
                    const std::vector<NumericFlaw> &numeric_flaws = f.second;
                    int max_numeric_domain_size = 0;
                    for (int j = 0; j < static_cast<int>(numeric_flaws.size()); ++j) {
                        int id = std::get<0>(numeric_flaws[j]);
                        int num_partitions = numeric_domain_mapping[id]->get_num_partitions();
                        if (num_partitions > max_numeric_domain_size) {
                            dep_candidates.clear();
                            max_numeric_domain_size = num_partitions;
                            dep_candidates.push_back(j);
                        } else if (num_partitions == max_numeric_domain_size) {
                            dep_candidates.push_back(j);
                        }
                    }
                    score += max_numeric_domain_size;
                }
            } else {
                int id = std::get<0>(f);
                score = numeric_domain_mapping[id]->get_num_partitions();
            }
        }, flaws[i]);
        candidates.push_back(Candidate{i, score, std::move(dep_candidates)});
    }

    sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        return a.score > b.score;
    });

    // Iterate candidates by decreasing score; within each tier, randomize the order.
    size_t pos = 0;
    while (pos < candidates.size()) {
        const int tier_score = candidates[pos].score;
        size_t end = pos;
        while (end < candidates.size() && candidates[end].score == tier_score) {
            ++end;
        }

        vector<size_t> order;
        order.reserve(end - pos);
        for (size_t k = pos; k < end; ++k) {
            order.push_back(k);
        }
        for (int i = static_cast<int>(order.size()) - 1; i > 0; --i) {
            int j = rng->random(i + 1);
            std::swap(order[i], order[j]);
        }

        for (size_t k : order) {
            const Candidate &cand = candidates[k];
            Flaw chosen = flaws[cand.flaw_index];

            // If we picked a comparison-axiom PropFlaw, restrict its dependent numeric flaws
            // to those associated with the max-refined numeric variable(s), but allow trying
            // all of them (in random order) to avoid picking an unsplittable one.
            if (holds_alternative<PropFlaw>(chosen)) {
                PropFlaw pf = get<PropFlaw>(chosen);
                const Fact &fact = pf.first;
                if (is_comparison_axiom_variable(fact.var) && !cand.dependent_numeric_indices.empty()) {
                    vector<NumericFlaw> restricted;
                    restricted.reserve(cand.dependent_numeric_indices.size());
                    for (int idx : cand.dependent_numeric_indices) {
                        restricted.push_back(pf.second[idx]);
                    }
                    pf.second = std::move(restricted);
                    chosen = pf;
                }
            }

            if (try_refine_from_flaw(chosen, domain_mapping, abstraction_size, numeric_domain_mapping)) {
                return true;
            }
        }

        pos = end;
    }

    return false;
}

bool CEGAR::fix_flaws_per_atom(
    vector<Flaw> &&flaws, DomainMapping &domain_mapping,
    int abstraction_size, NumericDomainMappings &numeric_domain_mapping) {
    auto atom_key = [](const Flaw &flaw) {
        return visit([&](auto &&f) {
            using T = std::decay_t<decltype(f)>;
            if constexpr (std::is_same_v<T, PropFlaw>) {
                return std::make_tuple(0, f.first.var, f.first.value, ap_float(0), false);
            } else {
                return std::make_tuple(1, std::get<0>(f), 0, std::get<1>(f), std::get<2>(f));
            }
        }, flaw);
    };

    sort(flaws.begin(), flaws.end(), [&](const Flaw &a, const Flaw &b) {
        return atom_key(a) < atom_key(b);
    });

    bool changed = false;
    int current_size = abstraction_size;
    bool has_last = false;
    decltype(atom_key(flaws.front())) last_key;

    for (const Flaw &flaw : flaws) {
        auto key = atom_key(flaw);
        if (has_last && key == last_key) {
            continue;
        }
        has_last = true;
        last_key = key;
        bool local_changed = try_refine_from_flaw(
            flaw, domain_mapping, current_size, numeric_domain_mapping,
            DependentNumericRefinement::ALL);
        changed = changed || local_changed;
        if (local_changed) {
            current_size = compute_current_abstraction_size();
        }
    }

    return changed;
}

bool CEGAR::fix_flaws_per_variable(
    vector<Flaw> &&flaws, DomainMapping &domain_mapping,
    int abstraction_size, NumericDomainMappings &numeric_domain_mapping) {
    auto variable_key = [](const Flaw &flaw) {
        return visit([&](auto &&f) {
            using T = std::decay_t<decltype(f)>;
            if constexpr (std::is_same_v<T, PropFlaw>) {
                return std::make_pair(0, f.first.var);
            } else {
                return std::make_pair(1, std::get<0>(f));
            }
        }, flaw);
    };

    sort(flaws.begin(), flaws.end(), [&](const Flaw &a, const Flaw &b) {
        return variable_key(a) < variable_key(b);
    });

    bool changed = false;
    int current_size = abstraction_size;
    std::unordered_set<int> refined_prop_vars;
    std::unordered_set<int> refined_numeric_vars;
    bool has_last = false;
    decltype(variable_key(flaws.front())) last_key;

    for (const Flaw &flaw : flaws) {
        auto key = variable_key(flaw);
        if (has_last && key == last_key) {
            continue;
        }
        has_last = true;
        last_key = key;
        bool local_changed = try_refine_from_flaw(
            flaw, domain_mapping, current_size, numeric_domain_mapping,
            DependentNumericRefinement::ONE,
            &refined_prop_vars, &refined_numeric_vars);
        changed = changed || local_changed;
        if (local_changed) {
            current_size = compute_current_abstraction_size();
        }
    }

    return changed;
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
        logger->log(Verbosity::INFO, "=== CEGAR Statistics ===");
        logger->log(Verbosity::INFO, "Final abstraction size: ", abstraction_size);
        logger->log(Verbosity::INFO, "Propositional variables:");
        logger->log(Verbosity::INFO, "  Total: ", num_variables);
        logger->log(Verbosity::INFO, "  Trivial (size 1): ", num_trivial_variables);
        logger->log(Verbosity::INFO, "  Complete (not abstracted): ", num_complete_variables);
        logger->log(Verbosity::INFO, "  Average domain size ratio: ", avg_domain_size);
        
        // Print details of non-trivial propositional variables
        logger->log(Verbosity::INFO, "  Non-trivial propositional variables:");
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
        
        logger->log(Verbosity::INFO, "Numeric variables:");
        logger->log(Verbosity::INFO, "  Total: ", num_numeric_variables);
        logger->log(Verbosity::INFO, "  Trivial (1 partition): ", num_trivial_numeric_vars);
        logger->log(Verbosity::INFO, "  Refined (>1 partition): ", num_refined_numeric_vars);
        logger->log(Verbosity::INFO, "  Total partitions: ", total_numeric_partitions);
        logger->log(Verbosity::INFO, "  Average partitions per variable: ", avg_numeric_partitions);
        
        // Print details of refined numeric variables
        logger->log(Verbosity::INFO, "  Refined numeric variables:");
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
        
        logger->log(Verbosity::INFO, "========================");
    }
}

void CEGAR::add_variable_to_abstraction_if_necessary(
    int var, DomainMapping &abstraction) {
    if (abstraction[var].empty()) {
        int real_domain_size = real_domain_sizes[var];
        abstraction[var].resize(real_domain_size, 0);
    }
}

NumericDomainMappings CEGAR::compute_initial_numeric_domain_mapping(
    const TaskProxy &task_proxy) {
    // Get number of numeric variables
    int num_numeric_variables = task_proxy.get_numeric_variables().size();
    
    // Initialize numeric domain mapping with full range (-inf, inf) for regular/derived variables
    // Constants should have a single partition at their exact value
    // Choose strategy based on configuration
    logger->log(Verbosity::DEBUG, "DEBUG: NumericSplitStrategy = ", 
                   (numeric_split_strategy == NumericSplitStrategy::EXCLUSION ? "EXCLUSION" : "STANDARD"));
    NumericDomainMappings numeric_domain_mapping;
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
    
    logger->log(Verbosity::DEBUG, "=== COMPLETE comparison_axiom_dependencies mapping ===");
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
    logger->log(Verbosity::DEBUG, "======================================================");
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

    build_numeric_split_hints(task_proxy);
    build_goal_related_prop_vars(task_proxy);

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
        current_iteration = iteration;
        can_split_cache.clear();
        include_in_lower_cache.clear();
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
            logger->log(Verbosity::NONE, "CEGAR stopping: no more flaws found.");
            break;
        }

        logger->log(Verbosity::DEBUG, "Flaws found in this iteration: ", flaws.size());

        // Keep a copy for diagnostics in case we cannot fix any flaw.
        vector<Flaw> flaws_for_fix = flaws;
        bool flaws_fixed = fix_flaws(move(flaws_for_fix), domain_mapping, abstraction.size(), numeric_domain_mapping);
        if (!flaws_fixed) {
            log_no_flaws_fixed_diagnostics(flaws, abstraction.size(), numeric_domain_mapping);
            logger->log(Verbosity::INFO,
                        "CEGAR stopping: no flaws could be fixed (abstraction size=",
                        abstraction.size(), ", max_abstraction_size=", max_abstraction_size, ").");
            break;
        }
        
        logger->log(Verbosity::DEBUG, "Generating new abstraction for next iteration (", iteration, ")...");
        DomainAbstractionFactory new_factory(
            task_proxy, domain_mapping, abstract_domain_sizes,
            numeric_domain_mapping, numeric_domain_sizes,
            true, rng, use_wildcard_plans, logger);
        
        abstraction = new_factory.generate();
        ++iteration;
        
        // (trimmed legacy debug)
    }

    logger->log(Verbosity::INFO,
                "CEGAR terminated. Final abstraction size: ",
                abstraction.size());

    if (utils::extra_memory_padding_is_reserved()) {
        utils::release_extra_memory_padding();
    }


    print_statistics(task_proxy, domain_mapping);
    logger->log(Verbosity::INFO, "Number of CEGAR iterations: ", iteration);

    return abstraction;
}

void CEGAR::log_no_flaws_fixed_diagnostics(
    const std::vector<Flaw> &flaws,
    int abstraction_size,
    const NumericDomainMappings &numeric_domain_mapping) const {
    // This is printed at Verbosity::NONE because it's intended for automated parsing
    // in smoke tests and for debugging domains where CEGAR stops unexpectedly.
    // Keep it compact and single-line.
    int prop_total = 0;
    int prop_blacklisted = 0;
    int prop_size_blocked = 0;
    int prop_other_unrefinable = 0;
    int prop_potentially_refinable = 0;

    int num_total = 0;
    int num_blacklisted = 0;
    int num_size_blocked = 0;
    int num_unsplittable = 0;
    int num_potentially_refinable = 0;

    for (const Flaw &flaw : flaws) {
        visit([&](auto &&f) {
            using T = std::decay_t<decltype(f)>;
            if constexpr (std::is_same_v<T, PropFlaw>) {
                ++prop_total;
                const Fact &fact = f.first;

                if (blacklisted_variables.count(fact.var)) {
                    ++prop_blacklisted;
                    return;
                }

                // Approximate whether refining this variable would violate the size bound.
                int domain_size = abstract_domain_sizes[fact.var];
                if (domain_size <= 0) {
                    ++prop_other_unrefinable;
                    return;
                }
                int abs_without_var = abstraction_size / domain_size;
                if (!utils::is_product_within_limit(abs_without_var, domain_size + 1, max_abstraction_size)) {
                    ++prop_size_blocked;
                    return;
                }

                // If it's a comparison axiom variable, we may only be able to refine its dependent numeric vars.
                if (is_comparison_axiom_variable(fact.var)) {
                    bool any_dep_refinable = false;
                    for (const NumericFlaw &nf : f.second) {
                        int num_id = std::get<0>(nf);
                        const ap_float &val = std::get<1>(nf);
                        bool flag = std::get<2>(nf);

                        if (blacklisted_numeric_variables.count(num_id)) {
                            continue;
                        }
                        if (num_id < 0 || num_id >= static_cast<int>(numeric_domain_mapping.size())) {
                            continue;
                        }

                        int partitions = numeric_domain_mapping[num_id]->get_num_partitions();
                        if (partitions <= 0) {
                            continue;
                        }
                        int abs_without_num = abstraction_size / partitions;
                        if (!utils::is_product_within_limit(abs_without_num, partitions + 1, max_abstraction_size)) {
                            continue;
                        }
                        if (!(can_split_cached(num_id, val, flag) ||
                            can_split_cached(num_id, val, !flag))) {
                            continue;
                        }
                        any_dep_refinable = true;
                        break;
                    }
                    if (any_dep_refinable) {
                        ++prop_potentially_refinable;
                    } else {
                        ++prop_other_unrefinable;
                    }
                    return;
                }

                ++prop_potentially_refinable;
            } else {
                ++num_total;
                int num_id = std::get<0>(f);
                const ap_float &val = std::get<1>(f);
                bool flag = std::get<2>(f);

                if (blacklisted_numeric_variables.count(num_id)) {
                    ++num_blacklisted;
                    return;
                }
                if (num_id < 0 || num_id >= static_cast<int>(numeric_domain_mapping.size())) {
                    ++num_unsplittable;
                    return;
                }

                int partitions = numeric_domain_mapping[num_id]->get_num_partitions();
                if (partitions <= 0) {
                    ++num_unsplittable;
                    return;
                }
                int abs_without_num = abstraction_size / partitions;
                if (!utils::is_product_within_limit(abs_without_num, partitions + 1, max_abstraction_size)) {
                    ++num_size_blocked;
                    return;
                }
                    if (!(can_split_cached(num_id, val, flag) ||
                        can_split_cached(num_id, val, !flag))) {
                    ++num_unsplittable;
                    return;
                }
                ++num_potentially_refinable;
            }
        }, flaw);
    }

    logger->log(
        Verbosity::INFO,
        "CEGAR no_flaws_fixed diagnostics: abs=", abstraction_size,
        " max=", max_abstraction_size,
        " flaws=", flaws.size(),
        " prop(total=", prop_total,
        ", blacklisted=", prop_blacklisted,
        ", size_blocked=", prop_size_blocked,
        ", other_unrefinable=", prop_other_unrefinable,
        ", potentially_refinable=", prop_potentially_refinable,
        ") num(total=", num_total,
        ", blacklisted=", num_blacklisted,
        ", size_blocked=", num_size_blocked,
        ", unsplittable_or_invalid=", num_unsplittable,
        ", potentially_refinable=", num_potentially_refinable,
        ")");
}

bool CEGAR::termination_criterion_satisfied(
    utils::CountdownTimer &timer) {
    if (timer.is_expired()) {
        logger->log(Verbosity::NONE, "CEGAR stopping: time limit reached.");
        return true;
    }
    if (!utils::extra_memory_padding_is_reserved()) {
        logger->log(Verbosity::NONE, "CEGAR stopping: memory limit reached.");
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
    int old_abstraction_size, int numeric_var_id) {
    if (blacklisted_numeric_variables.count(numeric_var_id)) {
        return false;
    }
    
    assert(numeric_var_id >= 0 && numeric_var_id < static_cast<int>(numeric_domain_mapping.size()));
    
    int current_partitions = numeric_domain_mapping[numeric_var_id]->get_num_partitions();
    int abs_size_without_var = old_abstraction_size / current_partitions;


    
    if (utils::is_product_within_limit(abs_size_without_var, current_partitions + 1,
                                       max_abstraction_size)) {
                                            logger->log(Verbosity::DEBUG, "num", numeric_var_id,
        " has ", current_partitions, " partitions.");
        logger->log(Verbosity::DEBUG, "Old abstraction size: ", old_abstraction_size);
        logger->log(Verbosity::DEBUG, "Abstraction size without this variable: ", abs_size_without_var);
        return true;
    }
    
    logger->log(Verbosity::DEBUG, "Cannot refine num", numeric_var_id, "; blacklisting");
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
    
    struct AssignmentEval {
        int derived_id;
        int left_id;
        int right_id;
        cal_operator op;
    };

    AssignmentAxiomsProxy assignment_axioms = task_proxy.get_assignment_axioms();
    std::vector<AssignmentEval> eval_axioms;
    eval_axioms.reserve(assignment_axioms.size());
    std::unordered_map<int, int> derived_to_idx;
    derived_to_idx.reserve(assignment_axioms.size());

    for (AssignmentAxiomProxy axiom : assignment_axioms) {
        AssignmentEval eval{
            axiom.get_assignment_variable().get_id(),
            axiom.get_left_variable().get_id(),
            axiom.get_right_variable().get_id(),
            axiom.get_arithmetic_operator_type()};
        int idx = static_cast<int>(eval_axioms.size());
        eval_axioms.push_back(eval);
        derived_to_idx[eval.derived_id] = idx;
    }

    auto apply_axiom = [&](const AssignmentEval &ax) -> bool {
        auto left_it = ranges.find(ax.left_id);
        auto right_it = ranges.find(ax.right_id);
        if (left_it == ranges.end() || right_it == ranges.end()) {
            return false;
        }

        const NumericRange &l_range = left_it->second;
        const NumericRange &r_range = right_it->second;
        NumericRange res = NumericDomainMapping::apply_range_operation(l_range, r_range, ax.op);

        auto it = ranges.find(ax.derived_id);
        if (it == ranges.end() ||
            it->second.lower != res.lower || it->second.upper != res.upper ||
            it->second.lower_inclusive != res.lower_inclusive ||
            it->second.upper_inclusive != res.upper_inclusive) {
            ranges[ax.derived_id] = res;
            return true;
        }
        return false;
    };

    std::vector<std::vector<int>> edges(eval_axioms.size());
    std::vector<int> indegree(eval_axioms.size(), 0);
    for (size_t i = 0; i < eval_axioms.size(); ++i) {
        const AssignmentEval &ax = eval_axioms[i];
        int left_dep_idx = -1;
        auto left_it = derived_to_idx.find(ax.left_id);
        if (left_it != derived_to_idx.end()) {
            left_dep_idx = left_it->second;
            edges[left_it->second].push_back(static_cast<int>(i));
            ++indegree[i];
        }
        auto right_it = derived_to_idx.find(ax.right_id);
        if (right_it != derived_to_idx.end() && right_it->second != left_dep_idx) {
            edges[right_it->second].push_back(static_cast<int>(i));
            ++indegree[i];
        }
    }

    std::vector<int> topo_order;
    topo_order.reserve(eval_axioms.size());
    std::vector<int> queue;
    queue.reserve(eval_axioms.size());
    for (size_t i = 0; i < indegree.size(); ++i) {
        if (indegree[i] == 0) {
            queue.push_back(static_cast<int>(i));
        }
    }
    for (size_t q = 0; q < queue.size(); ++q) {
        int u = queue[q];
        topo_order.push_back(u);
        for (int v : edges[u]) {
            if (--indegree[v] == 0) {
                queue.push_back(v);
            }
        }
    }

    if (topo_order.size() == eval_axioms.size()) {
        for (int idx : topo_order) {
            apply_axiom(eval_axioms[idx]);
        }
    } else {
        bool changed = true;
        while (changed) {
            changed = false;
            for (const AssignmentEval &ax : eval_axioms) {
                changed = apply_axiom(ax) || changed;
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

    IncludeInLowerKey cache_key{
        prop_var_id,
        split_var_id,
        split_value,
        compute_dependency_state_signature(prop_var_id, split_var_id, concrete_values)
    };
    auto cache_it = include_in_lower_cache.find(cache_key);
    if (cache_it != include_in_lower_cache.end()) {
        return cache_it->second;
    }
    
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
    
    logger->log(Verbosity::VERBOSE, "determine_include_in_lower for prop_var_id=v", prop_var_id,
               ", split_var=num", split_var_id, ", split_value=", split_value);
    logger->log(Verbosity::VERBOSE, "  eval with (-inf, ", split_value, "]: ", 
               (eval_lower == 0 ? "TRUE" : (eval_lower == 1 ? "FALSE" : "UNKNOWN")));
    logger->log(Verbosity::VERBOSE, "  eval with [", split_value, ", inf): ",
               (eval_upper == 0 ? "TRUE" : (eval_upper == 1 ? "FALSE" : "UNKNOWN")));
    
    // TODO: add assertion that true should never be possible
    // TODO: If todo asumption does not hold, return enum result
    // Prefer FALSE (=1) over UNKNOWN (=2) over TRUE (=0)
    // We want the comparison to be FALSE in the abstract state
    if (eval_lower == 1 && eval_upper != 1) {
        // Only include_in_lower=true gives FALSE
        logger->log(Verbosity::VERBOSE, "  -> Choosing include_in_lower=true (gives FALSE)");
        include_in_lower_cache.emplace(cache_key, true);
        return true;
    } else if (eval_upper == 1 && eval_lower != 1) {
        // Only include_in_lower=false gives FALSE
        logger->log(Verbosity::VERBOSE, "  -> Choosing include_in_lower=false (gives FALSE)");
        include_in_lower_cache.emplace(cache_key, false);
        return false;
    } else if (eval_lower == 1 && eval_upper == 1) {
        // Both give FALSE - either works, default to false
        logger->log(Verbosity::INFO, "WARNING: determine_include_in_lower: both branches evaluate to FALSE; defaulting to include_in_lower=false");
        include_in_lower_cache.emplace(cache_key, false);
        return false;
    } else if (eval_lower == 2 && eval_upper == 2) {
        // Both give UNKNOWN - this shouldn't happen in theory, but default to false
        logger->log(Verbosity::INFO, "WARNING: determine_include_in_lower: both branches evaluate to UNKNOWN; defaulting to include_in_lower=false");
        include_in_lower_cache.emplace(cache_key, false);
        return false;
    } else if (eval_lower == 2) {
        // Only lower gives UNKNOWN (upper gives TRUE) - prefer UNKNOWN over TRUE
        logger->log(Verbosity::VERBOSE, "  -> Choosing include_in_lower=true (gives UNKNOWN over TRUE)");
        //utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        include_in_lower_cache.emplace(cache_key, true);
        return true;
    } else if (eval_upper == 2) {
        // Only upper gives UNKNOWN (lower gives TRUE) - prefer UNKNOWN over TRUE
        logger->log(Verbosity::VERBOSE, "  -> Choosing include_in_lower=false (gives UNKNOWN over TRUE)");
        //utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        include_in_lower_cache.emplace(cache_key, false);
        return false;
    } else {
        // Both give TRUE - doesn't matter, default to false
        logger->log(Verbosity::VERBOSE, "  -> Both give TRUE, defaulting to include_in_lower=false");
        include_in_lower_cache.emplace(cache_key, false);
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
    bool use_threshold_aware_numeric_splits,
    bool use_progress_weighted_flaw_selection,
    int refinement_batch_size,
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
        use_threshold_aware_numeric_splits,
        use_progress_weighted_flaw_selection,
        refinement_batch_size,
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
        "false");
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

    parser.add_option<bool>(
        "use_threshold_aware_numeric_splits",
        "If true, try additional threshold/step-based split candidates around each numeric flaw value.",
        "false");
    parser.add_option<bool>(
        "use_interval_numeric_splits",
        "If true, use interval-aware numeric split candidates (threshold boundaries and interval centers). "
        "Alias of/useful replacement for use_threshold_aware_numeric_splits.",
        "false");
    parser.add_option<bool>(
        "use_progress_weighted_flaw_selection",
        "If true, prioritize flaws with a simple goal/progress-aware scoring before refinement.",
        "false");
    parser.add_option<bool>(
        "use_residual_distance_flaw_selection",
        "If true, prioritize flaws by residual distance to meaningful numeric thresholds. "
        "Alias of/useful replacement for use_progress_weighted_flaw_selection.",
        "false");
    parser.add_option<int>(
        "refinement_batch_size",
        "Number of flaw refinements attempted per CEGAR iteration (>=1). Values >1 enable top-k batch refinement.",
        "1");
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
