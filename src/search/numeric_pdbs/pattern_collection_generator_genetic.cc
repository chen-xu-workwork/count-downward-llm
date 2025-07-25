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
using namespace numeric_pdb_helper;

namespace numeric_pdbs {
PatternCollectionGeneratorGenetic::PatternCollectionGeneratorGenetic(
        const Options &opts)
        : PatternCollectionGenerator(opts.get<int>("max_number_pdb_states")),
          num_collections(opts.get<int>("num_collections")),
          num_episodes(opts.get<int>("num_episodes")),
          mutation_probability(opts.get<double>("mutation_probability")),
          pdb_params(PatternDatabase::parse_static_pdb_parameters(opts)),
          disjoint_patterns(opts.get<bool>("disjoint")) {
}

void PatternCollectionGeneratorGenetic::select(
        const vector<double> &fitness_values) {
    vector<double> cumulative_fitness;
    cumulative_fitness.reserve(fitness_values.size());
    double total_so_far = 0;
    for (double fitness_value: fitness_values) {
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

void PatternCollectionGeneratorGenetic::transform_to_pattern_bitvector_form(
        vector<bool> &bitvector,
        const Pattern &pattern,
        const NumericTaskProxy &task_proxy) const {
    int num_variables = task_proxy.get_num_variables();
    bitvector.assign(num_variables, false);
    for (size_t i = 0; i < pattern.regular.size(); ++i) {
        bitvector[pattern.regular[i]] = true;
    }
    for (size_t i = 0; i < pattern.numeric.size(); ++i) {
        bitvector[pattern.numeric[i] + g_variable_name.size()] = true;
    }
}

void PatternCollectionGeneratorGenetic::mutate2(const NumericTaskProxy &task_proxy) {
    Pattern trans_pattern;
    int num_vars = task_proxy.get_num_variables();
    for (size_t i = 0; i < pattern_collections.size(); ++i) {
        for (size_t j = 0; j < pattern_collections[i].size(); ++j) {
            vector<bool> &pattern = pattern_collections[i][j];
            vector<bool> orig_pattern = pattern;
            for (size_t k = 0; k < pattern.size(); ++k) {
                double random = (*g_rng())(); // [0..1)
                if (random < mutation_probability) {
                    if (k >= num_vars &&
                        task_proxy.get_numeric_var_type(k - num_vars) != numType::regular) {
                        continue; // skip numeric derived variables
                    }
                    pattern[k].flip();

                    // Check if new pattern has any irrelevant irrelevant variables
                    trans_pattern = transform_to_pattern_normal_form(pattern, task_proxy);
                    remove_irrelevant_variables(trans_pattern, task_proxy);
                    transform_to_pattern_bitvector_form(pattern, trans_pattern, task_proxy);
                }
            }
            // In some domains with lots of variables with large problems, e.g. airport, we end up generating too many large patterns
            // so testing to remove vars at random till pattern is not oversize
            // cout<<"time:,"<<utils::g_timer<<",pattern too large:"<<trans_pattern<<endl                                                                    ;
            if (is_pattern_too_large(trans_pattern, task_proxy)) {
                vector<int> pattern_vars;
                for (size_t l = 0; l < trans_pattern.regular.size(); ++l) {
                    pattern_vars.push_back(trans_pattern.regular[l]);
                }
                for (size_t l = 0; l < trans_pattern.numeric.size(); ++l) {
                    pattern_vars.push_back(trans_pattern.numeric[l] + num_vars);
                }
                g_rng()->shuffle(pattern_vars);

                while (is_pattern_too_large(trans_pattern, task_proxy)) {
                    for (size_t k = 0; k < pattern_vars.size(); k++) {
                        if (pattern[pattern_vars[k]]) { // so var is on, lets turn it off

                            pattern[pattern_vars[k]] = false;
                            trans_pattern = transform_to_pattern_normal_form(pattern, task_proxy);
                            remove_irrelevant_variables(trans_pattern, task_proxy);
                            transform_to_pattern_bitvector_form(pattern, trans_pattern, task_proxy);
                            break;
                        }
                    }
                    // need to refresh list of variables
                    pattern_vars.clear();
                    for (size_t l = 0; l < trans_pattern.regular.size(); ++l) {
                        pattern_vars.push_back(trans_pattern.regular[l]);
                    }
                    for (size_t l = 0; l < trans_pattern.numeric.size(); ++l) {
                        pattern_vars.push_back(trans_pattern.numeric[l] + num_vars);
                    }
                    g_rng()->shuffle(pattern_vars);
                }
            }
        }
    }
}

void PatternCollectionGeneratorGenetic::mutate(const NumericTaskProxy &task_proxy) {
    auto num_variables = task_proxy.get_num_variables();
    for (auto &collection: pattern_collections) {
        for (vector<bool> &pattern: collection) {
            for (size_t k = 0; k < pattern.size(); ++k) {
                // NOTE: skip if numeric derived variable has been encountered.
                if (k >= task_proxy.get_num_variables() &&
                    task_proxy.get_numeric_var_type(k - num_variables) != numType::regular) {
                    continue;
                }
                double random = (*g_rng())(); // [0..1)
                if (random < mutation_probability) {
                    pattern[k].flip();
                }
            }
        }
    }
}

Pattern PatternCollectionGeneratorGenetic::transform_to_pattern_normal_form(
        const vector<bool> &bitvector, const NumericTaskProxy &task_proxy) const {
    // NOTE: Current implementation has both numeric and prop vars in the same vector.
    Pattern pattern;
    for (size_t i = 0; i < bitvector.size(); ++i) {
        if (bitvector[i]) {
            if (i < task_proxy.get_num_variables()) {
                // Propositional variable.
                pattern.regular.push_back(i);
            } else {
                // Numeric variable.
                auto numeric_var_id = i - task_proxy.get_num_variables();
                assert(numeric_var_id >= 0);
                assert(task_proxy.get_numeric_var_type(numeric_var_id) == numType::regular);
                assert(i >= task_proxy.get_num_variables());
                pattern.numeric.push_back(numeric_var_id);
            }
        }
    }
    return pattern;
}

void PatternCollectionGeneratorGenetic::remove_irrelevant_variables(
        Pattern &pattern,
        const NumericTaskProxy &task_proxy) const {

    unordered_set<int> in_original_pattern(pattern.regular.begin(), pattern.regular.end());

    unordered_set<int> in_original_numeric_pattern(pattern.numeric.begin(), pattern.numeric.end());

    unordered_set<int> in_pruned_regular_pattern;
    unordered_set<int> in_pruned_numeric_pattern;

    vector<int> vars_to_check;
    vector<int> numeric_vars_to_check;
    for (FactProxy goal: task_proxy.get_propositional_goals()) {
        int var_id = goal.get_variable().get_id();
        if (in_original_pattern.count(var_id)) {
            // Goals are causally relevant.
            vars_to_check.push_back(var_id);
            in_pruned_regular_pattern.insert(var_id);
        }
    }
    for (const auto &num_goal: task_proxy.get_numeric_goals()) {
        int var_id = num_goal.get_var_id();
        if (in_original_numeric_pattern.count(var_id)) {
            // Numeric goals are causally relevant.
            numeric_vars_to_check.push_back(var_id);
            in_pruned_numeric_pattern.insert(var_id);
        }
    }

    const CausalGraph &cg = task_proxy.get_numeric_causal_graph();

    while (!vars_to_check.empty() || !numeric_vars_to_check.empty()) {
        if (!vars_to_check.empty()) {
            int var = vars_to_check.back();
            vars_to_check.pop_back();
            /*
            A variable is relevant to the pattern if it is a goal variable or if
            there is a pre->eff arc from the variable to a relevant variable.
            Note that there is no point in considering eff->eff arcs here.
            */

            const vector<int> &rel = cg.get_prop_eff_to_prop_pre(var);
            for (size_t i = 0; i < rel.size(); ++i) {
                int var_no = rel[i];
                if (in_original_pattern.count(var_no) &&
                    !in_pruned_regular_pattern.count(var_no)) {
                    // Parents of relevant variables are causally relevant.
                    vars_to_check.push_back(var_no);
                    in_pruned_regular_pattern.insert(var_no);
                }
            }

            const vector<int> &rel2 = cg.get_prop_eff_to_num_pre(var);
            for (size_t i = 0; i < rel2.size(); ++i) {
                int numeric_var_no = rel2[i];
                if (in_original_numeric_pattern.count(numeric_var_no) &&
                    !in_pruned_numeric_pattern.count(numeric_var_no)) {
                    // Parents of relevant variables are causally relevant.
                    numeric_vars_to_check.push_back(numeric_var_no);
                    in_pruned_numeric_pattern.insert(numeric_var_no);
                }
            }
        }

        if (!numeric_vars_to_check.empty()) {
            int numeric_var = numeric_vars_to_check.back();
            numeric_vars_to_check.pop_back();
            const vector<int> &rel3 = cg.get_num_eff_to_prop_pre(numeric_var);
            for (size_t i = 0; i < rel3.size(); ++i) {
                int var_no = rel3[i];
                if (in_original_pattern.count(var_no) &&
                    !in_pruned_regular_pattern.count(var_no)) {
                    // Parents of relevant variables are causally relevant.
                    vars_to_check.push_back(var_no);
                    in_pruned_regular_pattern.insert(var_no);
                }
            }

            const vector<int> &rel4 = cg.get_num_eff_to_num_pre(numeric_var);
            for (size_t i = 0; i < rel4.size(); ++i) {
                int numeric_var_no = rel4[i];
                if (in_original_numeric_pattern.count(numeric_var_no) &&
                    !in_pruned_numeric_pattern.count(numeric_var_no)) {
                    // Parents of relevant variables are causally relevant.
                    numeric_vars_to_check.push_back(numeric_var_no);
                    in_pruned_numeric_pattern.insert(numeric_var_no);
                }
            }
        }
    }

    pattern.regular.assign(in_pruned_regular_pattern.begin(), in_pruned_regular_pattern.end());
    pattern.numeric.assign(in_pruned_numeric_pattern.begin(), in_pruned_numeric_pattern.end());
    sort(pattern.regular.begin(), pattern.regular.end());
    sort(pattern.numeric.begin(), pattern.numeric.end());
}

bool PatternCollectionGeneratorGenetic::is_pattern_too_large(
        const Pattern &pattern, const NumericTaskProxy &task_proxy) const {
    // NOTE: Wait a sec. Isn't that redundant  because we check this durnig pattern construction already. Or is this function called elsewhere?
    //  Test if the pattern respects the memory limit.

    VariablesProxy variables = task_proxy.get_variables();
    ResNumericVariablesProxy numeric_variables = task_proxy.get_numeric_variables();
    int mem = 1;
    for (size_t i = 0; i < pattern.regular.size(); ++i) {
        VariableProxy var = variables[pattern.regular[i]];
        int domain_size = var.get_domain_size();
        if (!utils::is_product_within_limit(mem, domain_size, max_number_pdb_states))
            return true;
        mem *= domain_size;
    }
    for (size_t i = 0; i < pattern.numeric.size(); ++i) {
        // What is a ResNumericVariableProxy? How is it different from NumericVariableProxy?
        int domain_size = task_proxy.get_approximate_domain_size(numeric_variables[pattern.numeric[i]]);
        if (!utils::is_product_within_limit(mem, domain_size, max_number_pdb_states))
            return true;
        mem *= domain_size;
    }
    return false;
}

bool PatternCollectionGeneratorGenetic::mark_used_variables(
        // NOTE: Function name is misleading. It does not mark variables, it checks whether pattern contains duplicates.
        const NumericTaskProxy &task_proxy, const Pattern &pattern, vector<bool> &variables_used) const {
    for (size_t i = 0; i < pattern.regular.size(); ++i) {
        int var_id = pattern.regular[i];
        if (variables_used[var_id])
            return true;
        variables_used[var_id] = true;
    }
    for (size_t i = 0; i < pattern.numeric.size(); ++i) {
        int var_id = pattern.numeric[i] + task_proxy.get_variables().size();
        if (variables_used[var_id])
            return true;
        variables_used[var_id] = true;
    }
    return false;
}

void PatternCollectionGeneratorGenetic::evaluate(
        const NumericTaskProxy &task_proxy,
        vector<double> &fitness_values) {

    for (const auto &collection: pattern_collections) {
        // cout << "evaluate pattern collection " << (i + 1) << " of "
        //      << pattern_collections.size() << endl                                                                                                    ;
        double fitness = 0;
        bool pattern_valid = true;
        vector<bool> variables_used(task_proxy.get_variables().size() + task_proxy.get_numeric_variables().size(),
                                    false);
        shared_ptr<PatternCollection> pattern_collection = make_shared<PatternCollection>();
        pattern_collection->reserve(collection.size());
        for (const vector<bool> &bitvector: collection) {
            Pattern pattern = transform_to_pattern_normal_form(bitvector, task_proxy);

            if (is_pattern_too_large(pattern, task_proxy)) {
                cout << "pattern exceeds the memory limit!" << endl;
                pattern_valid = false;
                break;
            }

            if (disjoint_patterns) {
                if (mark_used_variables(task_proxy, pattern, variables_used)) {
                    cout << "patterns are not disjoint anymore!" << endl;
                    pattern_valid = false;
                    break;
                }
            }

            // TODO: Fix the task proxy code. It looks horrible.
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
                    make_shared<NumericTaskProxy>(task_proxy),
                    *pattern_collection,
                    pdb_params);
            fitness = zero_one_pdbs.compute_approx_mean_finite_h();
            cout << "fitness = " << fitness << endl;
            cout << "best_fitness = " << best_fitness << endl;
            // print all patterns:
            for (const auto &pattern: *pattern_collection) {
                cout << "Pattern: " << pattern << endl;
            }
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

void PatternCollectionGeneratorGenetic::bin_packing2(const NumericTaskProxy &task_proxy) {
    bin_packing_reg_count++;

    //TODO: The next block is currently ignored because there are 2x pdb_max size parameters. 
    // TODO: Make this a parameter
    bool use_norm_dist = true;

    int temp = 0;
    if (use_norm_dist) {
        std::normal_distribution<double> distribution((max_target_size + min_target_size) / 2,
                                                      (max_target_size - min_target_size) / 2);
        temp = distribution(generator);
    } else {
        temp = rand() % (max_target_size - min_target_size + 1);
        temp += min_target_size;
    }
    // Limited to between min_size and max_size
    pdb_max_size = 9 * pow(10, temp);
    pdb_max_size = min(pdb_max_size, pow(10, initial_max_target_size));
    pdb_max_size = max(pdb_max_size, pow(10, min_target_size));
    //TODO: Hack, because there are now too ways to set the pdb. The code block on top of that is currently ignored.
    pdb_max_size = max_number_pdb_states;

    VariablesProxy variables = task_proxy.get_variables();
    ResNumericVariablesProxy numeric_variables = task_proxy.get_numeric_variables();
    const CausalGraph &causal_graph = task_proxy.get_numeric_causal_graph();

    for (int i = 0; i < num_collections; ++i) {
        //NOTE: state computing (goal) vars with sufficiently small domain size

        //pattern_collections.clear();
        //NOTE: remaining_vars contains all variables with domain size <= pdb_max_size
        set<int> remaining_vars;
        for (size_t j = 0; j < variables.size(); ++j) {
            double next_var_size = variables[j].get_domain_size();
            if (next_var_size <= pdb_max_size) {
                remaining_vars.insert(j);
            }
        }
        for (size_t j = 0; j < numeric_variables.size(); ++j) {
            if (numeric_variables[j].get_var_type() != numType::regular) {
                continue; // skip derived numeric variables
            }
            double next_var_size = task_proxy.get_approximate_domain_size(numeric_variables[j]);
            if (next_var_size <= pdb_max_size) {
                remaining_vars.insert(j + variables.size());
            }
        }

        //NOTE: remaining_goal_vars contains all goal vars with domain <= pdb_max_size
        set<int> remaining_goal_vars;
        for (FactProxy goal: task_proxy.get_propositional_goals()) {
            double next_var_size = goal.get_variable().get_domain_size();
            if (next_var_size <= pdb_max_size) {
                remaining_goal_vars.insert(goal.get_variable().get_id());
            }
        }
        for (const auto &num_goal: task_proxy.get_numeric_goals()) {
            int var_id = num_goal.get_var_id();
            if (task_proxy.get_numeric_var_type(var_id) != numType::regular) {
                continue; // skip derived numeric variables
            }
            double next_var_size = task_proxy.get_approximate_domain_size(numeric_variables[var_id]);
            if (next_var_size <= pdb_max_size) {
                remaining_goal_vars.insert(var_id + variables.size());
            }
        }

        cout << "Remaining vars: ";
        for (auto v : remaining_vars) {
            cout << v << ", ";
        }
        cout << endl << "Remaining goal vars: ";
        for (auto v : remaining_goal_vars) {
            cout << v << ", ";
        }
        cout << endl;


        vector<vector<bool>> pattern_collection;
        vector<bool> pattern(variables.size() + numeric_variables.size(), false);
        double current_size = 1;

        vector<int> pattern_int; //NOTE: int = interesting?
        vector<int> candidate_pattern;
        int var_id;
        while (!remaining_vars.empty()) {
            if (!pattern_int.empty()) {
                candidate_pattern = pattern_int;
                sort(candidate_pattern.begin(), candidate_pattern.end());
                set<int> rel_vars_set;
                vector<int> relevant_vars;
                vector<int> relevant_vars_in_remaining;
                for (auto var: pattern_int) {
                    cout << "Var in int pattern: " << var << endl;
                    if (var < variables.size()) {
                        const vector<int> &rel_vars = causal_graph.get_prop_eff_to_prop_pre(var);
                        for (auto var2: rel_vars) {
                            rel_vars_set.insert(var2);
                        }
                        for (auto var2: causal_graph.get_prop_eff_to_num_pre(var)) {
                            if (task_proxy.get_numeric_var_type(var2) == numType::regular) {
                                rel_vars_set.insert(var2 + variables.size());
                            }
                        }
                    } else {
                        const vector<int> &rel_vars = causal_graph.get_num_eff_to_prop_pre(var - variables.size());
                        for (auto var2: rel_vars) {
                            rel_vars_set.insert(var2);
                        }
                        for (auto var2: causal_graph.get_num_eff_to_num_pre(var - variables.size())) {
                            if (task_proxy.get_numeric_var_type(var2) == numType::regular) {
                                rel_vars_set.insert(var2 + variables.size());
                            }
                        }
                    }
                }
                cout << "before back insert: ";
                for (auto v : rel_vars_set) {
                    cout << v << ", ";
                }
                cout << endl;
                set_difference(rel_vars_set.begin(), rel_vars_set.end(),
                               candidate_pattern.begin(), candidate_pattern.end(),
                               back_inserter(relevant_vars));
                            
                cout<<"relevant vars to current_pattern:";for (auto item : relevant_vars) cout<<item<<",";cout<<endl;
                set_intersection(relevant_vars.begin(), relevant_vars.end(),
                                 remaining_vars.begin(), remaining_vars.end(),
                                 back_inserter(relevant_vars_in_remaining));
                cout<<"relevant vars in remaining:";for (auto item : relevant_vars_in_remaining) cout<<item<<",";cout<<flush<<endl;
                //NOTE: relevant_vars_in_remaining contains all relevant vars not in the pattern
                
                //NOTE: Add a single random variable to pattern. 
                g_rng()->shuffle(relevant_vars_in_remaining); //TODO: In original code, relevant_vars was shuffled. Does not make sense. 
                while (!relevant_vars_in_remaining.empty()) {
                    var_id = relevant_vars_in_remaining.back();
                    relevant_vars_in_remaining.pop_back();
                    if (var_id < variables.size()) {
                        double next_var_size = variables[var_id].get_domain_size();
                        if (utils::is_product_within_limit(current_size, next_var_size, pdb_max_size)) {
                            candidate_pattern.push_back(var_id);
                            current_size *= next_var_size;
                            pattern[var_id] = true;
                            remaining_vars.erase(var_id);
                            remaining_goal_vars.erase(var_id);
                            break;
                        }
                    } else {
                        if (task_proxy.get_numeric_var_type(var_id - variables.size()) != numType::regular) {
                            remaining_vars.erase(var_id);
                            remaining_goal_vars.erase(var_id);
                            continue; // skip derived numeric variables
                        }
                        double next_var_size = task_proxy.get_approximate_domain_size(
                                numeric_variables[var_id - variables.size()]);
                        if (utils::is_product_within_limit(current_size, next_var_size, pdb_max_size)) {
                            candidate_pattern.push_back(var_id);
                            current_size *= next_var_size;
                            pattern[var_id] = true;
                            remaining_vars.erase(var_id);
                            remaining_goal_vars.erase(var_id);
                            break;
                        }
                    }
                }
                if (candidate_pattern != pattern_int) {
                    pattern_int = candidate_pattern;
                } else { // no var is small enough to be added, or none left
                    // cout<<"no more relevant vars can be added"<<flush<<endl;
                    if (!pattern_int.empty()) {
                        pattern_collection.push_back(pattern);
                        Pattern trans_pattern = transform_to_pattern_normal_form(pattern_collection.back(), task_proxy);
                        // cout<<"added pattern["<<pattern_collection.size()-1<<"]:"<<trans_pattern<<",size:"<<get_pattern_size(trans_pattern)<<flush<<endl;
                        pattern_int.clear();
                        pattern.clear();
                        pattern.resize(variables.size() + numeric_variables.size(), false);
                        current_size = 1;
                        // TRYING ONLY ONE PATTERN
                        if (single_pattern_only) {
                            break;
                        }
                    }
                }
            } else { // choose a remaining var at random, nothing selected yet for this pattern
                //right now, empty pattern. Start adding a single (goal) var to pattern_int and pattern
                if (!use_first_goal_vars) {
                    auto temp_it = remaining_vars.begin();
                    advance(temp_it, rand() % remaining_vars.size());
                    var_id = *temp_it;
                } else { // using goal valrs first
                    auto temp_it = remaining_goal_vars.begin();
                    if (remaining_goal_vars.empty()) {
                        // no more goal vars, so no more patterns, as we can not start it with a goal variable
                        break;
                    }
                    temp_it = remaining_goal_vars.begin();
                    advance(temp_it, rand() % remaining_goal_vars.size());
                    var_id = *temp_it;
                    remaining_goal_vars.erase(temp_it);
                }
                remaining_vars.erase(var_id);

                // cout<<"\t\tfirst var for pattern:"<<var_id<<",remaining_goal_vars:"<<flush;
                // for(auto id : remaining_goal_vars) cout<<","<<id;
                // cout<<",remaining_vars:";
                // for(auto id : remaining_vars) cout<<","<<id;
                // cout<<endl;

                pattern[var_id] = true;
                pattern_int.push_back(var_id);
                double next_var_size;
                if (var_id < variables.size()) {
                    next_var_size = variables[var_id].get_domain_size();
                } else {
                    assert(task_proxy.get_numeric_var_type(var_id - variables.size()) == numType::regular);
                    next_var_size = task_proxy.get_approximate_domain_size(
                            numeric_variables[var_id - variables.size()]);
                }
                current_size *= next_var_size;
            }
        }
        // Add the last pattern!
        if (!pattern_int.empty()) {
            pattern_collection.push_back(pattern);
            Pattern trans_pattern = transform_to_pattern_normal_form(pattern_collection.back(), task_proxy);
            // cout<<"added last added pattern["<<pattern_collection.size()-1<<"]:"<<trans_pattern<<",size:"<<get_pattern_size(trans_pattern)<<endl;
        }
        // Sort patterns by size, so zero_one cost partition benefits larger patterns over shorter ones
        sort(pattern_collection.begin(), pattern_collection.end(), compare_pattern_length);

        pattern_collections.push_back(pattern_collection);
    }
    //NOTE: Uncomment if you want to debug
    for (auto col : pattern_collections) {
        cout << "Collection" << endl;
        for (auto p : col) {
            cout << "pattern: ";
            for (int i = 0; i < p.size(); i++) {
                if (p[i]) {
                    cout << i << ", ";
                }
            } 
            cout << endl;
        }
    }
    //exit(0);
}

void PatternCollectionGeneratorGenetic::bin_packing(const NumericTaskProxy &task_proxy) {
    VariablesProxy variables = task_proxy.get_variables();
    ResNumericVariablesProxy numeric_variables = task_proxy.get_numeric_variables();

    vector<int> variable_ids;
    variable_ids.reserve(variables.size() + numeric_variables.size());

    // NOTE: Assume variable IDs are ordered and numeric variables always have larger IDs.
    for (size_t i = 0; i < variables.size(); ++i) {
        variable_ids.push_back(i);
    }
    for (size_t i = 0; i < numeric_variables.size(); ++i) {
        size_t numeric_var_id = i + variables.size();
        assert(numeric_var_id >= variables.size());
        // get var type
        numType var_type = numeric_variables[i].get_var_type();
        if (numType::regular != var_type) {
            continue; // Skip non-regular numeric variables.
        }
        variable_ids.push_back(static_cast<int>(numeric_var_id));
    }

    for (int i = 0; i < num_collections; ++i) {
        // Use random variable ordering for all pattern collections.
        g_rng()->shuffle(variable_ids);
        vector<vector<bool>> pattern_collection;
        // TOOD: Potential waste of memory here
        vector<bool> pattern(variables.size() + numeric_variables.size(), false);

        int current_size = 1;
        for (size_t j = 0; j < variable_ids.size(); ++j) {
            int var_id = variable_ids[j];
            int next_var_size = 0;
            if (var_id < variables.size()) {
                // NOTE: var_id is a propositional variable.
                next_var_size = variables[var_id].get_domain_size();
            } else {
                size_t numeric_var_id = var_id - variables.size();
                // NOTE: var_id is a numeric variable.
                next_var_size = task_proxy.get_approximate_domain_size(numeric_variables[numeric_var_id]);
            }
            assert(next_var_size > 0);
            if (next_var_size > max_number_pdb_states) {
                // var never fits into a bin.
                continue;
            }

            if (!utils::is_product_within_limit(current_size, next_var_size,
                                                max_number_pdb_states)) {
                // Open a new bin for var.
                pattern_collection.push_back(pattern);
                pattern.clear();
                pattern.resize(variables.size() + numeric_variables.size(), false);
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
        cout << "Pattern collection " << (i + 1) << " of " << num_collections
             << " has " << pattern_collection.size() << " patterns." << endl;
        for (const auto &p: pattern_collection) {
            for (size_t var_id = 0; var_id < p.size(); ++var_id) {
                if (p[var_id]) {
                    cout << var_id << " ";
                }
            }
            cout << endl;
        }
        pattern_collections.push_back(pattern_collection);
    }
}

void PatternCollectionGeneratorGenetic::genetic_algorithm(const NumericTaskProxy &task_proxy) {
    best_fitness = -1;
    best_patterns = nullptr;
    bin_packing2(task_proxy);
    vector<double> initial_fitness_values;
    evaluate(task_proxy, initial_fitness_values);
    //exit(0);
    cout << "Initial fitness values: ";
    for (double fitness: initial_fitness_values) {
        cout << fitness << " ";
    }
    cout << endl;
    for (int i = 0; i < num_episodes; ++i) {
        cout << endl;
        cout << "--------- episode no " << (i + 1) << " ---------" << endl;
        mutate2(task_proxy);
        vector<double> fitness_values;
        evaluate(task_proxy, fitness_values);
        // We allow to select invalid pattern collections.
        select(fitness_values);
    }
}

PatternCollectionInformation PatternCollectionGeneratorGenetic::generate(shared_ptr <AbstractTask> task) {
    auto task_proxy = make_shared<NumericTaskProxy>(task);
    utils::Timer timer;
    genetic_algorithm(*task_proxy);
    cout << "Pattern generation (Edelkamp) time: " << timer << endl;
    assert(best_patterns);

    // remove empty patterns:
    best_patterns->erase(remove_if(best_patterns->begin(), best_patterns->end(),
                                   [](const Pattern &p) {
                                       return p.regular.empty() && p.numeric.empty();
                                   }),
                         best_patterns->end());

    cout << "Number patterns in collection: " << best_patterns->size() << endl;
    for (const Pattern &p: *best_patterns) {
        cout << "Pattern: " << p << endl;
    }

    return {task_proxy,
            best_patterns,
            pdb_params};
}

static shared_ptr <PatternCollectionGenerator> _parse(OptionParser &parser) {
    parser.document_synopsis(
            "Genetic Algorithm Patterns",
            "The following paper describes the automated creation of pattern "
            "databases with a genetic algorithm. Pattern collections are initially "
            "created with a bin-packing algorithm. The genetic algorithm is used "
            "to optimize the pattern collections with an objective function that "
            "estimates the mean heuristic value of the the pattern collections. "
            "Pattern collections with higher mean heuristic estimates are more "
            "likely selected for the next generation." +
            utils::format_paper_reference(
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
            "+\n\n",
            true);

    parser.add_option<int>(
            "max_number_pdb_states",
            "maximal number of states per pattern database ",
            "50000",
            Bounds("1", "infinity"));
    parser.add_option<int>(
            "max_pdb_size",
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
