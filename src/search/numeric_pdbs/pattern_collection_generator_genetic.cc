#include "pattern_collection_generator_genetic.h"

#include "validation.h"
#include "zero_one_pdbs.h"

#include "causal_graph.h"
#include "../globals.h"
#include "../option_parser.h"
#include "../plugin.h"
#include "../task_proxy.h"

#include "../utils/markup.h"
#include "../utils/math.h"
#include "../utils/rng.h"
#include "../utils/timer.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <unordered_set>
#include <vector>

#include "numeric_helper.h"
#include "pattern_database.h"

using namespace std;

namespace numeric_pdbs {
PatternCollectionGeneratorGenetic::PatternCollectionGeneratorGenetic(
    const Options &opts)
    : PatternCollectionGenerator(opts.get<int>("max_number_pdb_states")),
      num_collections(opts.get<int>("num_collections")),
      num_episodes(opts.get<int>("num_episodes")),
      mutation_probability(opts.get<double>("mutation_probability")),
      disjoint_patterns(opts.get<bool>("disjoint")) ,
      extend_abstract_state_space(opts.get<bool>("extend_abstract_state_space")),
      f_layer_offset_ratio(opts.get<double>("f_layer_offset_ratio")),
      keep_parent_pointers(opts.get<bool>("keep_parent_pointers")),
      need_goal(opts.get<bool>("need_goal")),
      exploration_h(InnerHeuristic(opts.get_enum("exploration_heuristic"))),
      frontier_h(InnerHeuristic(opts.get_enum("frontier_heuristic"))),
      failed_lookup_h(InnerHeuristic(opts.get_enum("failed_lookup_heuristic"))) {
}

void PatternCollectionGeneratorGenetic::select(
    const vector<double> &fitness_values) {
    vector<double> cumulative_fitness;
    cumulative_fitness.reserve(fitness_values.size());
    double total_so_far = 0;
    for (double fitness_value : fitness_values) {
        total_so_far += fitness_value;
        cumulative_fitness.push_back(total_so_far);
    }
    // total_so_far is now sum over all fitness values.

    vector<vector<vector<bool>>> new_pattern_collections;
    new_pattern_collections.reserve(num_collections);
    for (int i = 0; i < num_collections; ++i) {
        int selected;
        if (total_so_far == 0) {
            // All fitness values are 0 => choose uniformly.
            selected = (*g_rng())(fitness_values.size());
        } else {
            // [0..total_so_far)
            double random = (*g_rng())() * total_so_far;
            // Find first entry which is strictly greater than random.
            selected = upper_bound(cumulative_fitness.begin(),
                                   cumulative_fitness.end(), random) -
                       cumulative_fitness.begin();
        }
        new_pattern_collections.push_back(pattern_collections[selected]);
    }
    pattern_collections.swap(new_pattern_collections);
}

void PatternCollectionGeneratorGenetic::mutate() {
    for (auto &collection : pattern_collections) {
        for (vector<bool> &pattern : collection) {
            for (size_t k = 0; k < pattern.size(); ++k) {
                double random = (*g_rng())(); // [0..1)
                if (random < mutation_probability) {
                    pattern[k].flip();
                }
            }
        }
    }
}

Pattern PatternCollectionGeneratorGenetic::transform_to_pattern_normal_form(
    const vector<bool> &bitvector, numeric_pdb_helper::NumericTaskProxy &task_proxy) const {
    //NOTE: Current implementation has both numeric and prop vars in the same vector.
    Pattern pattern;
    for (size_t i = 0; i < bitvector.size(); ++i) {
        if (bitvector[i]) {
            if (i < task_proxy.get_num_variables()) {
                // Propositional variable.
                pattern.regular.push_back(i);
            } else {
                // Numeric variable.
                assert(i >= task_proxy.get_num_variables());
                cout << "numeric var ID - DEBUG - " << i - task_proxy.get_num_variables();
                pattern.numeric.push_back(i - task_proxy.get_num_variables());
            }
        }
    }
    return pattern;
}

void PatternCollectionGeneratorGenetic::remove_irrelevant_variables(
    Pattern &pattern,
    numeric_pdb_helper::NumericTaskProxy &task_proxy) const {

    unordered_set<int> in_original_pattern(pattern.regular.begin(), pattern.regular.end());
    in_original_pattern.insert(pattern.numeric.begin(), pattern.numeric.end());
    unordered_set<int> in_pruned_pattern;


    vector<int> vars_to_check;
    for (FactProxy goal : task_proxy.get_propositional_goals()) {
        int var_id = goal.get_variable().get_id();
        if (in_original_pattern.count(var_id)) {
            // Goals are causally relevant.
            vars_to_check.push_back(var_id);
            in_pruned_pattern.insert(var_id);
        }
    }
    for (const auto &num_goal : task_proxy.get_numeric_goals()) {
        int var_id = num_goal.get_var_id();
        if (in_original_pattern.count(var_id)) {
            // Numeric goals are causally relevant.
            vars_to_check.push_back(var_id);
            in_pruned_pattern.insert(var_id);
        }
    }


    while (!vars_to_check.empty()) {
        int var = vars_to_check.back();
        vars_to_check.pop_back();
        /*
          A variable is relevant to the pattern if it is a goal variable or if
          there is a pre->eff arc from the variable to a relevant variable.
          Note that there is no point in considering eff->eff arcs here.
        */
        // TODO: Figure out why causal graph is constructed here? Isn't that slow?
        const CausalGraph &cg = task_proxy.get_numeric_causal_graph();

        // TODO: Do all of that for all kinds of "get" functions.
        // TODO: Figure out if num var IDs always larger than var IDs. 
        const vector<int> &rel = cg.get_prop_eff_to_prop_pre(var);
        for (size_t i = 0; i < rel.size(); ++i) {
            int var_no = rel[i];
            if (in_original_pattern.count(var_no) &&
                !in_pruned_pattern.count(var_no)) {
                // Parents of relevant variables are causally relevant.
                vars_to_check.push_back(var_no);
                in_pruned_pattern.insert(var_no);
            }
        }

        const vector<int> &rel2 = cg.get_prop_eff_to_num_pre(var);
        for (size_t i = 0; i < rel2.size(); ++i) {
            int var_no = rel2[i];
            if (in_original_pattern.count(var_no) &&
                !in_pruned_pattern.count(var_no)) {
                // Parents of relevant variables are causally relevant.
                vars_to_check.push_back(var_no);
                in_pruned_pattern.insert(var_no);
            }
        }

        const vector<int> &rel3 = cg.get_num_eff_to_prop_pre(var);
        for (size_t i = 0; i < rel3.size(); ++i) {
            int var_no = rel3[i];
            if (in_original_pattern.count(var_no) &&
                !in_pruned_pattern.count(var_no)) {
                // Parents of relevant variables are causally relevant.
                vars_to_check.push_back(var_no);
                in_pruned_pattern.insert(var_no);
            }
        }

        const vector<int> &rel4 = cg.get_num_eff_to_num_pre(var);
        for (size_t i = 0; i < rel4.size(); ++i) {
            int var_no = rel4[i];
            if (in_original_pattern.count(var_no) &&
                !in_pruned_pattern.count(var_no)) {
                // Parents of relevant variables are causally relevant.
                vars_to_check.push_back(var_no);
                in_pruned_pattern.insert(var_no);
            }
        }
    }

    pattern.regular.assign(in_pruned_pattern.begin(), in_pruned_pattern.end());
    sort(pattern.regular.begin(), pattern.regular.end());
}

bool PatternCollectionGeneratorGenetic::is_pattern_too_large(
    const Pattern &pattern, numeric_pdb_helper::NumericTaskProxy &task_proxy) const {
    //NOTE: Wait a sec. Isn't that redundant  because we check this durnig pattern construction already. Or is this function called elsewhere?
    // Test if the pattern respects the memory limit.

    VariablesProxy variables = task_proxy.get_variables();
    numeric_pdb_helper::ResNumericVariablesProxy numeric_variables = task_proxy.get_numeric_variables();
    int mem = 1;
    for (size_t i = 0; i < pattern.regular.size(); ++i) {
        VariableProxy var = variables[pattern.regular[i]];
        int domain_size = var.get_domain_size();
        if (!utils::is_product_within_limit(mem, domain_size, max_number_pdb_states))
            return true;
        mem *= domain_size;
    }
    for (size_t i = 0; i < pattern.numeric.size(); ++i) {
        //What is a ResNumericVariableProxy? How is it different from NumericVariableProxy?
        int domain_size = task_proxy.get_approximate_domain_size(numeric_variables[pattern.numeric[i]]);
        if (!utils::is_product_within_limit(mem, domain_size, max_number_pdb_states))
            return true;
        mem *= domain_size;
    }


    //TODO: Add bound check for numeric variables.
    return false;
}

bool PatternCollectionGeneratorGenetic::mark_used_variables(
    //NOTE: Function name is misleading. It does not mark variables, it checks whether pattern contains duplicates.
    const Pattern &pattern, vector<bool> &variables_used) const {
    for (size_t i = 0; i < pattern.regular.size(); ++i) {
        int var_id = pattern.regular[i];
        if (variables_used[var_id])
            return true;
        variables_used[var_id] = true;
    }
    for (size_t i = 0; i < pattern.numeric.size(); ++i) {
        int var_id = pattern.numeric[i];
        if (variables_used[var_id])
            return true;
        variables_used[var_id] = true;
    }
    return false;
}

void PatternCollectionGeneratorGenetic::evaluate(numeric_pdb_helper::NumericTaskProxy &task_proxy, vector<double> &fitness_values) {

    for (const auto &collection : pattern_collections) {
        //cout << "evaluate pattern collection " << (i + 1) << " of "
        //     << pattern_collections.size() << endl;
        double fitness = 0;
        bool pattern_valid = true;
        vector<bool> variables_used(task_proxy.get_variables().size() + task_proxy.get_numeric_variables().size(), false);
        shared_ptr<PatternCollection> pattern_collection = make_shared<PatternCollection>();
        pattern_collection->reserve(collection.size());
        for (const vector<bool> &bitvector : collection) {
            Pattern pattern = transform_to_pattern_normal_form(bitvector, task_proxy);

            if (is_pattern_too_large(pattern, task_proxy)) {
                cout << "pattern exceeds the memory limit!" << endl;
                pattern_valid = false;
                break;
            }

            if (disjoint_patterns) {
                if (mark_used_variables(pattern, variables_used)) {
                    cout << "patterns are not disjoint anymore!" << endl;
                    pattern_valid = false;
                    break;
                }
            }

            //TODO: Fix the task proxy code. It looks horrible.
            remove_irrelevant_variables(pattern, task_proxy);
            pattern_collection->push_back(pattern);
        }
        if (!pattern_valid) {
            /* Set fitness to a very small value to cover cases in which all
               patterns are invalid. */
            fitness = 0.001;
        } else {
            /* Generate the pattern collection heuristic and get its fitness
               value. */
            ZeroOnePDBs zero_one_pdbs(
                make_shared<numeric_pdb_helper::NumericTaskProxy>(task_proxy), 
                *pattern_collection,
                max_number_pdb_states,
                extend_abstract_state_space,
                need_goal,
                f_layer_offset_ratio,
                keep_parent_pointers,
                exploration_h,
                frontier_h,
                failed_lookup_h
            );
            fitness = zero_one_pdbs.compute_approx_mean_finite_h();
            // Update the best heuristic found so far.
            if (fitness > best_fitness) {
                best_fitness = fitness;
                cout << "best_fitness = " << best_fitness << endl;
                best_patterns = pattern_collection;
            }
        }
        fitness_values.push_back(fitness);
    }
}

void PatternCollectionGeneratorGenetic::bin_packing(numeric_pdb_helper::NumericTaskProxy &task_proxy) {
    VariablesProxy variables = task_proxy.get_variables();
    numeric_pdb_helper::ResNumericVariablesProxy numeric_variables = task_proxy.get_numeric_variables();

    vector<int> variable_ids;
    variable_ids.reserve(variables.size() + numeric_variables.size());
    //TODO: check if reserve changes size(). If so, we could end up into issues

    //NOTE: Assume variable IDs are ordered and numeric variables always have larger IDs.
    for (size_t i = 0; i < variables.size(); ++i) {
        variable_ids.push_back(i);
        cout << "variable " << i << " is propositional variable with ID " << variables[i].get_id() << endl;
    }
    for (size_t i = 0; i < numeric_variables.size(); ++i) {
        size_t numeric_var_id = i + variables.size();
        assert(numeric_var_id >= variables.size());
        // get var type
        numType var_type = numeric_variables[i].get_var_type();
        if (numType::regular != var_type) {
            continue; // Skip non-regular numeric variables.
        }
        variable_ids.push_back(numeric_var_id);
        cout << "variable " << (numeric_var_id) << " is numeric variable with ID " << numeric_variables[i].get_id() << endl;
    }

    for (int i = 0; i < num_collections; ++i) {
        // Use random variable ordering for all pattern collections.
        g_rng()->shuffle(variable_ids);
        vector<vector<bool>> pattern_collection;
        //TOOD: Potential waste of memory here
        vector<bool> pattern(variables.size() + numeric_variables.size(), false);

        int current_size = 1;
        for (size_t j = 0; j < variable_ids.size(); ++j) {
            int var_id = variable_ids[j];
            int next_var_size = 0;
            if (var_id < variables.size()) {
                //NOTE: var_id is a propositional variable.
                next_var_size = variables[var_id].get_domain_size();
                if (next_var_size > max_number_pdb_states)
                    // var never fits into a bin.
                    continue;
            } else {
                size_t numeric_var_id = var_id - variables.size();
                //NOTE: var_id is a numeric variable.
                //TODO: Make sure that aux variables are treated correctly.
                cout << "Numeric Variables size: " << numeric_variables.size() << "\t";;
                cout << "Numeric variable ID: " << numeric_var_id << endl;
                next_var_size = task_proxy.get_approximate_domain_size(numeric_variables[numeric_var_id]);
                if (next_var_size > max_number_pdb_states)
                    // var never fits into a bin.
                    continue;
            }

            cout << "current size: " << current_size << ", " << next_var_size << ", " << max_number_pdb_states << endl;
            
            if (!utils::is_product_within_limit(current_size, next_var_size,
                                                max_number_pdb_states)) {
                // Open a new bin for var.
                pattern_collection.push_back(pattern);
                pattern.clear();
                pattern.resize(variables.size(), false);
                current_size = 1;
            }
            current_size *= next_var_size;
            pattern[var_id] = true;
        }
        /*
          The last bin has not bin inserted into pattern_collection, do so now.
          We test current_size against 1 because this is cheaper than
          testing if pattern is an all-zero bitvector. current_size
          can only be 1 if *all* variables have a domain larger than
          pdb_max_size.
        */
        if (current_size > 1) {
            pattern_collection.push_back(pattern);
        }
        pattern_collections.push_back(pattern_collection);
    }
}

void PatternCollectionGeneratorGenetic::genetic_algorithm(
    numeric_pdb_helper::NumericTaskProxy &task_proxy) {
    best_fitness = -1;
    best_patterns = nullptr;
    bin_packing(task_proxy);
    vector<double> initial_fitness_values;
    evaluate(task_proxy, initial_fitness_values);
    for (int i = 0; i < num_episodes; ++i) {
        cout << endl;
        cout << "--------- episode no " << (i + 1) << " ---------" << endl;
        mutate();
        vector<double> fitness_values;
        evaluate(task_proxy, fitness_values);
        // We allow to select invalid pattern collections.
        select(fitness_values);
    }
}

PatternCollectionInformation PatternCollectionGeneratorGenetic::generate(
    shared_ptr<AbstractTask> task) {
    auto task_proxy = make_shared<numeric_pdb_helper::NumericTaskProxy>(task);
    utils::Timer timer;
    genetic_algorithm(*task_proxy);
    cout << "Pattern generation (Edelkamp) time: " << timer << endl;
    assert(best_patterns);

    cout << "pattern size: " << best_patterns->size() << endl;
    for (Pattern p : *best_patterns) {
        cout << "regular IDs: ";
        for (auto var_id : p.regular) {
            cout << var_id << " ";
        }
        cout << endl;
        cout << "numeric IDs: ";
        for (auto var_id : p.regular) {
            cout << var_id << " ";
        }
        cout << endl;
    }

    return {task_proxy,
            best_patterns,
            max_number_pdb_states,
            extend_abstract_state_space,
            f_layer_offset_ratio,
            need_goal,
            keep_parent_pointers,
            exploration_h,
            frontier_h,
            failed_lookup_h};
}

static shared_ptr<PatternCollectionGenerator> _parse(OptionParser &parser) {
    parser.document_synopsis(
        "Genetic Algorithm Patterns",
        "The following paper describes the automated creation of pattern "
        "databases with a genetic algorithm. Pattern collections are initially "
        "created with a bin-packing algorithm. The genetic algorithm is used "
        "to optimize the pattern collections with an objective function that "
        "estimates the mean heuristic value of the the pattern collections. "
        "Pattern collections with higher mean heuristic estimates are more "
        "likely selected for the next generation." + utils::format_paper_reference(
            {"Stefan Edelkamp"},
            "Automated Creation of Pattern Database Search Heuristics",
            "http://www.springerlink.com/content/20613345434608x1/",
            "Proceedings of the 4th Workshop on Model Checking and Artificial"
            " Intelligence (!MoChArt 2006)",
            "35-50",
            "2007"));
    parser.document_language_support("action costs", "supported");
    parser.document_language_support("conditional effects", "not supported");
    parser.document_language_support("axioms", "not supported");
    parser.document_note(
        "Note",
        "This pattern generation method uses the "
        "zero/one pattern database heuristic.");
    parser.document_note(
        "Implementation Notes",
        "The standard genetic algorithm procedure as described in the paper is "
        "implemented in Fast Downward. The implementation is close to the "
        "paper.\n\n"
        "+ Initialization<<BR>>"
        "In Fast Downward bin-packing with the next-fit strategy is used. A "
        "bin corresponds to a pattern which contains variables up to "
        "``pdb_max_size``. With this method each variable occurs exactly in "
        "one pattern of a collection. There are ``num_collections`` "
        "collections created.\n"
        "+ Mutation<<BR>>"
        "With probability ``mutation_probability`` a bit is flipped meaning "
        "that either a variable is added to a pattern or deleted from a "
        "pattern.\n"
        "+ Recombination<<BR>>"
        "Recombination isn't implemented in Fast Downward. In the paper "
        "recombination is described but not used.\n"
        "+ Evaluation<<BR>>"
        "For each pattern collection the mean heuristic value is computed. For "
        "a single pattern database the mean heuristic value is the sum of all "
        "pattern database entries divided through the number of entries. "
        "Entries with infinite heuristic values are ignored in this "
        "calculation. The sum of these individual mean heuristic values yield "
        "the mean heuristic value of the collection.\n"
        "+ Selection<<BR>>"
        "The higher the mean heuristic value of a pattern collection is, the "
        "more likely this pattern collection should be selected for the next "
        "generation. Therefore the mean heuristic values are normalized and "
        "converted into probabilities and Roulette Wheel Selection is used.\n"
        "+\n\n", true);

    parser.add_option<int>(
        "max_number_pdb_states",
        "maximal number of states per pattern database ",
        "50000",
        Bounds("1", "infinity"));
    parser.add_option<int>(
        "num_collections",
        "number of pattern collections to maintain in the genetic "
        "algorithm (population size)",
        "5",
        Bounds("1", "infinity"));
    parser.add_option<int>(
        "num_episodes",
        "number of episodes for the genetic algorithm",
        "30",
        Bounds("0", "infinity"));
    parser.add_option<double>(
        "mutation_probability",
        "probability for flipping a bit in the genetic algorithm",
        "0.01",
        Bounds("0.0", "1.0"));
    parser.add_option<bool>(
        "disjoint",
        "consider a pattern collection invalid (giving it very low "
        "fitness) if its patterns are not disjoint",
        "false");

    PatternDatabase::add_pdb_options(parser);

    Options opts = parser.parse();
    if (parser.dry_run())
        return 0;

    return make_shared<PatternCollectionGeneratorGenetic>(opts);
}

static PluginShared<PatternCollectionGenerator> _plugin("numeric_genetic", _parse);
}
