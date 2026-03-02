#include "domain_abstraction_collection_generator_multiple.h"

#include "domain_abstraction.h"
#include "domain_abstraction_factory.h"
#include "utils.h"

#include "../option_parser.h"
#include "../plugin.h"

#include "../utils/countdown_timer.h"
#include "../utils/logging.h"
#include "../utils/markup.h"
#include "../utils/rng.h"
#include "../utils/rng_options.h"

#include <vector>

using namespace std;

namespace domain_abstractions {

// Fingerprint for a NumericDomainMappings: captures range boundaries and inclusivity.
// Two numeric mappings are considered equal if they have identical ranges.
// Each range is encoded as: (lower, upper, lower_inclusive, upper_inclusive)
// where inclusivity flags are encoded as 0.0 or 1.0 for comparison purposes.
using NumericMappingFingerprint = vector<vector<ap_float>>;

static NumericMappingFingerprint get_numeric_fingerprint(
    const NumericDomainMappings &numeric_mapping) {
    NumericMappingFingerprint fingerprint;
    fingerprint.reserve(numeric_mapping.size());
    for (const auto &mapping : numeric_mapping) {
        vector<ap_float> boundaries;
        const auto &ranges = mapping->get_ranges();
        // Store range boundaries AND inclusivity flags to fully identify partitioning
        // Format per range: lower, upper, lower_inclusive (0/1), upper_inclusive (0/1)
        boundaries.reserve(ranges.size() * 4);
        for (const auto &range : ranges) {
            boundaries.push_back(range.lower);
            boundaries.push_back(range.upper);
            boundaries.push_back(range.lower_inclusive ? 1.0 : 0.0);
            boundaries.push_back(range.upper_inclusive ? 1.0 : 0.0);
        }
        fingerprint.push_back(move(boundaries));
    }
    return fingerprint;
}

// Combined key for duplicate detection: propositional mapping + numeric fingerprint
struct AbstractionKey {
    DomainMapping domain_mapping;
    NumericMappingFingerprint numeric_fingerprint;
    
    bool operator<(const AbstractionKey &other) const {
        if (domain_mapping != other.domain_mapping) {
            return domain_mapping < other.domain_mapping;
        }
        return numeric_fingerprint < other.numeric_fingerprint;
    }
};

DomainAbstractionCollectionGeneratorMultiple::DomainAbstractionCollectionGeneratorMultiple(
    options::Options &opts)
    : DomainAbstractionCollectionGenerator(opts),
      max_abstraction_size(opts.get<int>("max_abstraction_size")),
      abstraction_generation_max_time(opts.get<double>("abstraction_generation_max_time")),
      total_max_time(opts.get<double>("total_max_time")),
      stagnation_limit(opts.get<double>("stagnation_limit")),
      blacklisting_start_time(total_max_time * opts.get<double>("blacklist_trigger_percentage")),
      enable_blacklist_on_stagnation(opts.get<bool>("enable_blacklist_on_stagnation")),
      blacklist_variables(VariableSubset(opts.get_enum("blacklist_option"))),
      init_split_variables(VariableSubset(opts.get_enum("init_split_candidates"))),
      init_split_quantity(InitSplitQuantity(opts.get_enum("init_split_quantity"))),
      rng(utils::parse_rng_from_options(opts)),
      random_seed(opts.get<int>("random_seed")),
      remaining_collection_size(opts.get<int>("max_collection_size")),
      blacklisting(false),
      time_point_of_last_new_pattern(0.0) {
}

void DomainAbstractionCollectionGeneratorMultiple::check_blacklist_trigger_timer(
    const utils::CountdownTimer &timer) {

    if (!blacklisting && timer.get_elapsed_time() > blacklisting_start_time) {
        blacklisting = true;
        /*
          Also treat this time point as having seen a new pattern to avoid
          stopping due to stagnation right after enabling blacklisting.
        */
        time_point_of_last_new_pattern = timer.get_elapsed_time();
    }
}

unordered_set<int> DomainAbstractionCollectionGeneratorMultiple::get_blacklisted_variables(
    vector<int> &blacklist_candidates) {
    unordered_set<int> blacklisted_variables;
    if (blacklisting && !blacklist_candidates.empty()) {
        /*
          Randomize the number of blacklist variables. We want to choose
          at least 1 blacklist candidate, so we pick a random value in
          the range [1, |blacklist_candidates|].
        */
        int blacklist_size = rng->random(blacklist_candidates.size());
        ++blacklist_size;
        rng->shuffle(blacklist_candidates);
        blacklisted_variables.insert(
            blacklist_candidates.begin(), blacklist_candidates.begin() + blacklist_size);
    }

   


    return blacklisted_variables;
}

unordered_set<int> DomainAbstractionCollectionGeneratorMultiple::get_init_split_variables(
    vector<int> &init_split_candidates, int iteration) {
    unordered_set<int> var_ids;
    switch (init_split_quantity) {
    case InitSplitQuantity::NONE:
        break;
    case InitSplitQuantity::SINGLE: {
        assert(!init_split_candidates.empty());
        int index = iteration % init_split_candidates.size();
        //assert(utils::in_bounds(index, init_split_candidates));
        var_ids.insert(init_split_candidates[index]);
        break;
    }
    case InitSplitQuantity::ALL:
        assert(!init_split_candidates.empty());
        var_ids.insert(init_split_candidates.begin(),
                       init_split_candidates.end());
        break;
    }
    return var_ids;
}

void DomainAbstractionCollectionGeneratorMultiple::handle_generated_abstraction(
    DomainAbstraction &&abstraction,
    set<AbstractionKey> &generated_keys,
    DomainAbstractionCollection &generated_abstractions,
    const utils::CountdownTimer &timer) {
    
    // Create a combined key from both propositional and numeric mappings
    AbstractionKey key;
    key.domain_mapping = abstraction.get_domain_mapping();
    key.numeric_fingerprint = get_numeric_fingerprint(abstraction.get_numeric_domain_mapping());
    
    // Debug: print key
    // cout << "Key domain mapping: [";
    // for (size_t i = 0; i < key.domain_mapping.size(); ++i) {
    //     cout << "[";
    //     for (size_t j = 0; j < key.domain_mapping[i].size(); ++j) {
    //         cout << key.domain_mapping[i][j];
    //         if (j + 1 < key.domain_mapping[i].size()) cout << ", ";
    //     }
    //     cout << "]";
    //     if (i + 1 < key.domain_mapping.size()) cout << ", ";
    // }
    // cout << "]\nNumeric fingerprint: [";
    // for (size_t i = 0; i < key.numeric_fingerprint.size(); ++i) {
    //     // Skip trivial [-inf, inf, 0, 0] entries (unsplit numeric variables)
    //     const auto &fp = key.numeric_fingerprint[i];
    //     if (fp.size() == 4 && 
    //         fp[0] == -std::numeric_limits<ap_float>::infinity() &&
    //         fp[1] == std::numeric_limits<ap_float>::infinity() &&
    //         fp[2] == 0.0 && fp[3] == 0.0) {
    //         continue;
    //     }
    //     cout << "num" << i << ":[";
    //     for (size_t j = 0; j < fp.size(); ++j) {
    //         cout << fp[j];
    //         if (j + 1 < fp.size()) cout << ", ";
    //     }
    //     cout << "] ";
    // }
    // cout << "]" << endl;
    
    if (generated_keys.insert(key).second) {
        /*
          compute_pattern generated a new pattern. Create/retrieve corresponding
          PDB, update collection size and reset time_point_of_last_new_pattern.
        */
        time_point_of_last_new_pattern = timer.get_elapsed_time();
        //cout << "abstraction size: " << abstraction.size()
        //     << ", remaining collection size: " << remaining_collection_size - abstraction.size()
        //     << ", time elapsed: " << timer.get_elapsed_time() << "s"
        //     << ", time remaining: " << timer.get_remaining_time() << "s"
        //     << endl;
        remaining_collection_size -= abstraction.size();
        generated_abstractions.push_back(move(abstraction));
    }
}

bool DomainAbstractionCollectionGeneratorMultiple::collection_size_limit_reached() const {
    if (remaining_collection_size <= 0) {
        /*
          This value can become negative if the given size limits for
          pdb or collection size are so low that compute_pattern already
          violates the limit, possibly even with only using a single goal
          variable.
        */
        return true;
    }
    return false;
}

bool DomainAbstractionCollectionGeneratorMultiple::time_limit_reached(
    const utils::CountdownTimer &timer) const {
    if (timer.is_expired()) {
        return true;
    }
    return false;
}

bool DomainAbstractionCollectionGeneratorMultiple::check_for_stagnation(
    const utils::CountdownTimer &timer) {
    // Test if no new pattern was generated for longer than stagnation_limit.
    if (timer.get_elapsed_time() - time_point_of_last_new_pattern > stagnation_limit) {
        if (enable_blacklist_on_stagnation) {
            if (blacklisting) {
                return true;
            } else {
                blacklisting = true;
                time_point_of_last_new_pattern = timer.get_elapsed_time();
            }
        } else {
            return true;
        }
    }
    return false;
}

string DomainAbstractionCollectionGeneratorMultiple::name() const {
    return "multiple " + id() + " domain abstraction collection generator";
}

DomainAbstractionCollection DomainAbstractionCollectionGeneratorMultiple::compute_abstractions(
    const TaskProxy &task_proxy) {

    utils::CountdownTimer timer(total_max_time);

    vector<Fact> goals = get_goals_in_random_order(task_proxy, *rng);
    for (const Fact &goal : goals) {
        cout << task_proxy.get_variables()[goal.var].get_name() << "=" << goal.value << " ";
    }
    cout << endl;

    vector<int> blacklist_candidates =
        get_candidates(task_proxy, blacklist_variables, true);

    vector<int> init_split_candidates =
        get_candidates(task_proxy, init_split_variables, true);


    initialize(task_proxy);

    set<AbstractionKey> generated_keys;
    DomainAbstractionCollection generated_abstractions;

    shared_ptr<utils::RandomNumberGenerator> pattern_computation_rng =
        make_shared<utils::RandomNumberGenerator>(random_seed);
    int num_iterations = 1;
    int goal_index = 0;

    while (true) {
        check_blacklist_trigger_timer(timer);

        unordered_set<int> blacklisted_variables =
            get_blacklisted_variables(blacklist_candidates);
        unordered_set<int> init_split_var_ids =
            get_init_split_variables(init_split_candidates, num_iterations);

        int remaining_pdb_size = min(remaining_collection_size, max_abstraction_size);
        double remaining_time =
            min(static_cast<double>(timer.get_remaining_time()), abstraction_generation_max_time);

        DomainAbstraction abstraction = compute_abstraction(
            remaining_pdb_size,
            remaining_time,
            pattern_computation_rng,
            task_proxy,
            goals[goal_index],
            move(init_split_var_ids),
            move(blacklisted_variables));
        handle_generated_abstraction(
            move(abstraction),
            generated_keys,
            generated_abstractions,
            timer);

        if (collection_size_limit_reached() ||
            time_limit_reached(timer) ||
            check_for_stagnation(timer)) {
                //cout << "collection sizle limit reached?" << collection_size_limit_reached() << " "
                //     << "time limit reached? " << time_limit_reached(timer) << " "
                //     << "stagnation? " << check_for_stagnation(timer) << endl; 
            break;
        }

        ++num_iterations;
        ++goal_index;
        goal_index = goal_index % goals.size();
        //assert(utils::in_bounds(goal_index, goals));
    }

    return generated_abstractions;
}

vector<int> DomainAbstractionCollectionGeneratorMultiple::get_candidates(
    const TaskProxy &task_proxy, VariableSubset option, bool include_numeric) {
    vector<int> candidates;
    
    int num_prop_vars = task_proxy.get_variables().size();
    
    unordered_set<int> logic_axiom_effect_vars;
    for (OperatorProxy axiom : task_proxy.get_axioms()) {
        for (EffectProxy eff : axiom.get_effects()) {
            logic_axiom_effect_vars.insert(eff.get_fact().get_variable().get_id());
        }
    }
    
    unordered_set<int> comparison_axiom_vars;
    for (ComparisonAxiomProxy axiom : task_proxy.get_comparison_axioms()) {
        comparison_axiom_vars.insert(axiom.get_true_fact().get_variable().get_id());
    }
    
    unordered_map<int, int> goal_axiom_map;  // effect_var_id -> axiom_index
    int axiom_idx = 0;
    for (OperatorProxy axiom : task_proxy.get_axioms()) {
        if (!axiom.get_preconditions().empty() && axiom.get_effects().size() == 1) {
            int effect_var_id = axiom.get_effects()[0].get_fact().get_variable().get_id();
            goal_axiom_map[effect_var_id] = axiom_idx;
        }
        axiom_idx++;
    }
    
    unordered_set<int> goal_var_ids;
    for (FactProxy goal : task_proxy.get_goals()) {
        int var_id = goal.get_variable().get_id();
        auto it = goal_axiom_map.find(var_id);
        if (it != goal_axiom_map.end()) {
            // This is a "fake" goal (axiom effect) - extract its preconditions as actual goals
            OperatorProxy goal_axiom = task_proxy.get_axioms()[it->second];
            for (FactProxy pre : goal_axiom.get_preconditions()) {
                int pre_var_id = pre.get_variable().get_id();
                // These preconditions are always comparison axiom variables
                // assert(comparison_axiom_vars.count(pre_var_id) > 0);
                goal_var_ids.insert(pre_var_id);
            }
        } else {
            // Regular goal
            goal_var_ids.insert(var_id);
        }
    }
    
    switch (option) {
    case VariableSubset::GOALS: {
        // Add goal variables (which include the comparison axiom goal preconditions)
        for (int var_id : goal_var_ids) {
            // Exclude logic axiom effects (instrumentation)
            if (logic_axiom_effect_vars.count(var_id) == 0) {
                candidates.push_back(var_id);
            }
        }
        // Numeric variables are never goals, so nothing to add
        break;
    }
    case VariableSubset::NON_GOALS: {
        // Add non-goal propositional variables:
        // - Exclude logic axiom effects (instrumentation)
        // - Exclude goal variables (including goal comparison axiom vars)
        // - Exclude non-goal comparison axiom variables (never want to blacklist them)
        for (int var_id = 0; var_id < num_prop_vars; ++var_id) {
            if (goal_var_ids.count(var_id) == 0 
                && logic_axiom_effect_vars.count(var_id) == 0
                && comparison_axiom_vars.count(var_id) == 0) {
                candidates.push_back(var_id);
            }
        }
        // Add numeric variables if requested (they are all non-goals)
        // Encoded as: num_prop_vars + numeric_var_id
        if (include_numeric) {
            int num_numeric_vars = task_proxy.get_numeric_variables().size();
            for (int num_var_id = 0; num_var_id < num_numeric_vars; ++num_var_id) {
                // Only add regular numeric variables (not constants or derived)
                NumericVariableProxy num_var = task_proxy.get_numeric_variables()[num_var_id];
                if (num_var.get_var_type() == numType::regular) {
                    candidates.push_back(num_prop_vars + num_var_id);
                }
            }
        }
        break;
    }
    case VariableSubset::ALL: {
        // Add all propositional variables:
        // - Exclude logic axiom effects (instrumentation)
        // - Exclude non-goal comparison axiom variables (never want to blacklist them)
        // - But INCLUDE goal comparison axiom variables
        for (int var_id = 0; var_id < num_prop_vars; ++var_id) {
            if (logic_axiom_effect_vars.count(var_id) == 0) {
                // Include if it's a goal comparison var, exclude if non-goal comparison var
                if (comparison_axiom_vars.count(var_id) == 0 || goal_var_ids.count(var_id) > 0) {
                    candidates.push_back(var_id);
                }
            }
        }
        // Add numeric variables if requested
        // Encoded as: num_prop_vars + numeric_var_id
        if (include_numeric) {
            int num_numeric_vars = task_proxy.get_numeric_variables().size();
            for (int num_var_id = 0; num_var_id < num_numeric_vars; ++num_var_id) {
                NumericVariableProxy num_var = task_proxy.get_numeric_variables()[num_var_id];
                if (num_var.get_var_type() == numType::regular) {
                    candidates.push_back(num_prop_vars + num_var_id);
                }
            }
        }
        break;
    }
    }
    return candidates;
}

void add_multiple_algorithm_implementation_notes_to_parser(
    options::OptionParser &parser) {
    parser.document_note(
        "Short description of the 'multiple algorithm framework'",
        "This algorithm is a general framework for computing a pattern collection "
        "for a given planning task. It requires as input a method for computing a "
        "single pattern for the given task and a single goal of the task. The "
        "algorithm works as follows. It first stores the goals of the task in "
        "random order. Then, it repeatedly iterates over all goals and for each "
        "goal, it uses the given method for computing a single pattern. If the "
        "pattern is new (duplicate detection), it is kept for the final collection.\n"
        "The algorithm runs until reaching a given time limit. Another parameter allows "
        "exiting early if no new patterns are found for a certain time ('stagnation'). "
        "Further parameters allow enabling blacklisting for the given pattern computation "
        "method after a certain time to force some diversification or to enable said "
        "blacklisting when stagnating.",
        true);
    parser.document_note(
        "Implementation note about the 'multiple algorithm framework'",
        "A difference compared to the original implementation used in the "
        "paper is that the original implementation of stagnation in "
        "the multiple CEGAR/RCG algorithms started counting the time towards "
        "stagnation only after having generated a duplicate pattern. Now, "
        "time towards stagnation starts counting from the start and is reset "
        "to the current time only when having found a new pattern or when "
        "enabling blacklisting.",
        true);
}

void add_multiple_options_to_parser(options::OptionParser &parser) {
    parser.add_option<int>(
        "max_abstraction_size",
        "maximum number of states for each pattern database, computed "
        "by compute_abstraction (possibly ignored by singleton patterns consisting "
        "of a goal variable)",
        "1000000",
        Bounds("1", "infinity"));
    parser.add_option<int>(
        "max_collection_size",
        "maximum number of states in all pattern databases of the "
        "collection (possibly ignored, see max_abstraction_size)",
        "10000000",
        Bounds("1", "infinity"));
    parser.add_option<double>(
        "abstraction_generation_max_time",
        "maximum time in seconds for each call to the algorithm for "
        "computing a single pattern",
        "infinity",
        Bounds("0.0", "infinity"));
    parser.add_option<double>(
        "total_max_time",
        "maximum time in seconds for this pattern collection generator. "
        "It will always execute at least one iteration, i.e., call the "
        "algorithm for computing a single pattern at least once.",
        "10.0",
        Bounds("0.0", "infinity"));
    parser.add_option<double>(
        "stagnation_limit",
        "maximum time in seconds this pattern generator is allowed to run "
        "without generating a new pattern. It terminates prematurely if this "
        "limit is hit unless enable_blacklist_on_stagnation is enabled.",
        "20.0",
        Bounds("1.0", "infinity"));
    parser.add_option<double>(
        "blacklist_trigger_percentage",
        "percentage of total_max_time after which blacklisting is enabled",
        "0.75",
        Bounds("0.0", "1.0"));
    parser.add_option<bool>(
        "enable_blacklist_on_stagnation",
        "if true, blacklisting is enabled when stagnation_limit is hit "
        "for the first time (unless it was already enabled due to "
        "blacklist_trigger_percentage) and pattern generation is terminated "
        "when stagnation_limit is hit for the second time. If false, pattern "
        "generation is terminated already the first time stagnation_limit is "
        "hit.",
        "true");
    vector<string> candidate_options; // TODO: rename
    candidate_options.push_back("goals");
    candidate_options.push_back("non_goals");
    candidate_options.push_back("all");
    parser.add_enum_option(
        "blacklist_option",
        candidate_options,
        "Specify which variables should be considered when blacklisting: "
        "*goals* only, *non-goals* only, or *any* variables.",
        "all");
    parser.add_enum_option(
        "init_split_candidates",
        candidate_options,
        "Choose candidate variables for initial split",
        "all");
    vector<string> init_split_quantity;
    init_split_quantity.emplace_back("none");
    init_split_quantity.emplace_back("single");
    init_split_quantity.emplace_back("all");
    parser.add_enum_option(
        "init_split_quantity", init_split_quantity,
        "Choose how many facts to split for seeding diversification.",
        "single");
    add_domain_abstraction_collection_generator_options_to_parser(parser);
}
}

namespace options {
template <>
std::string TypeNamer<domain_abstractions::VariableSubset>::name() {
    return "VariableSubset";
}

template <>
std::string TypeNamer<domain_abstractions::InitSplitQuantity>::name() {
    return "InitSplitQuantity";
}
}
