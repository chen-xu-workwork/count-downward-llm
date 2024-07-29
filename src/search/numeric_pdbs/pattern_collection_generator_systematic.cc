#include "pattern_collection_generator_systematic.h"

#include "causal_graph.h"
#include "numeric_condition.h"
#include "numeric_helper.h"
#include "validation.h"

#include "../option_parser.h"
#include "../plugin.h"
#include "../task_proxy.h"

#include "../utils/logging.h"
#include "../utils/markup.h"

#include <algorithm>
#include <cassert>
#include <iostream>

using namespace std;

namespace numeric_pdbs {
static bool patterns_are_disjoint(
    const Pattern &pattern1, const Pattern &pattern2) {
    size_t i = 0;
    size_t j = 0;
    for (;;) {
        if (i == pattern1.regular.size() || j == pattern2.regular.size())
            break;
        int val1 = pattern1.regular[i];
        int val2 = pattern2.regular[j];
        if (val1 == val2)
            return false;
        else if (val1 < val2)
            ++i;
        else
            ++j;
    }
    i = 0;
    j = 0;
    for (;;) {
        if (i == pattern1.numeric.size() || j == pattern2.numeric.size())
            return true;
        int val1 = pattern1.numeric[i];
        int val2 = pattern2.numeric[j];
        if (val1 == val2)
            return false;
        else if (val1 < val2)
            ++i;
        else
            ++j;
    }
}

static void compute_union_pattern(
    const Pattern &pattern1, const Pattern &pattern2, Pattern &result) {
    result.regular.clear();
    result.regular.reserve(pattern1.regular.size() + pattern2.regular.size());
    set_union(pattern1.regular.begin(), pattern1.regular.end(),
              pattern2.regular.begin(), pattern2.regular.end(),
              back_inserter(result.regular));
    result.numeric.clear();
    result.numeric.reserve(pattern1.numeric.size() + pattern2.numeric.size());
    set_union(pattern1.numeric.begin(), pattern1.numeric.end(),
              pattern2.numeric.begin(), pattern2.numeric.end(),
              back_inserter(result.numeric));
}


PatternCollectionGeneratorSystematic::PatternCollectionGeneratorSystematic(
    const Options &opts)
    : PatternCollectionGenerator(opts.get<int>("max_number_pdb_states")),
      pattern_max_size(opts.get<int>("pattern_max_size")),
      only_interesting_patterns(opts.get<bool>("only_interesting_patterns")) {
}

void PatternCollectionGeneratorSystematic::compute_eff_pre_neighbors(
    const CausalGraph &cg, const Pattern &pattern, Pattern &result) {
    /*
      Compute all variables that are reachable from pattern by an
      (eff, pre) arc and are not already contained in the pattern.
    */
    unordered_set<int> prop_candidates;
    unordered_set<int> num_candidates;

    // Compute neighbors.
    for (int var : pattern.regular) {
        vector<int> prop_neighbors(cg.get_prop_eff_to_prop_pre(var));
        prop_candidates.insert(prop_neighbors.begin(), prop_neighbors.end());
        vector<int> num_neighbors(cg.get_prop_eff_to_num_pre(var));
        num_candidates.insert(num_neighbors.begin(), num_neighbors.end());
    }
    for (int var : pattern.numeric) {
        vector<int> prop_neighbors(cg.get_num_eff_to_prop_pre(var));
        prop_candidates.insert(prop_neighbors.begin(), prop_neighbors.end());
        vector<int> num_neighbors(cg.get_num_eff_to_num_pre(var));
        num_candidates.insert(num_neighbors.begin(), num_neighbors.end());
    }

    // Remove elements of pattern.
    for (int var : pattern.regular) {
        prop_candidates.erase(var);
    }
    for (int var : pattern.numeric) {
        num_candidates.erase(var);
    }

    result.regular.assign(prop_candidates.begin(), prop_candidates.end());
    result.numeric.assign(num_candidates.begin(), num_candidates.end());
}

void PatternCollectionGeneratorSystematic::compute_connection_points(
    const CausalGraph &cg, const Pattern &pattern, Pattern &result) {
    /*
      The "connection points" of a pattern are those variables of which
      one must be contained in an SGA pattern that can be attached to this
      pattern to form a larger interesting pattern. (Interesting patterns
      are disjoint unions of SGA patterns.)

      A variable is a connection point if it satisfies the following criteria:
      1. We can get from the pattern to the connection point via
         an (eff, pre) or (eff, eff) arc in the causal graph.
      2. It is not part of pattern.
      3. We *cannot* get from the pattern to the connection point
         via an (eff, pre) arc.

      Condition 1. is the important one. The other conditions are
      optimizations that help reduce the number of candidates to
      consider.
    */
    unordered_set<int> prop_candidates;
    unordered_set<int> num_candidates;

    // Handle rule 1.
    for (int var : pattern.regular) {
        const vector<int> &prop_pred = cg.get_prop_predecessors_of_prop_var(var);
        prop_candidates.insert(prop_pred.begin(), prop_pred.end());
        const vector<int> &num_pred = cg.get_num_predecessors_of_prop_var(var);
        num_candidates.insert(num_pred.begin(), num_pred.end());
    }
    for (int var : pattern.numeric) {
        const vector<int> &num_pred = cg.get_num_predecessors_of_num_var(var);
        num_candidates.insert(num_pred.begin(), num_pred.end());
        const vector<int> &prop_pred = cg.get_prop_predecessors_of_num_var(var);
        prop_candidates.insert(prop_pred.begin(), prop_pred.end());
    }

    // Handle rules 2 and 3.
    for (int var : pattern.regular) {
        // Rule 2:
        prop_candidates.erase(var);
        // Rule 3:
        const vector<int> &prop_eff_pre = cg.get_prop_eff_to_prop_pre(var);
        for (int prop_pre_var : prop_eff_pre)
            prop_candidates.erase(prop_pre_var);
        const vector<int> &num_eff_pre = cg.get_prop_eff_to_num_pre(var);
        for (int num_pre_var : num_eff_pre)
            num_candidates.erase(num_pre_var);
    }
    for (int var : pattern.numeric) {
        // Rule 2:
        num_candidates.erase(var);
        // Rule 3:
        const vector<int> &prop_eff_pre = cg.get_num_eff_to_prop_pre(var);
        for (int prop_pre_var : prop_eff_pre)
            prop_candidates.erase(prop_pre_var);
        const vector<int> &num_eff_pre = cg.get_num_eff_to_num_pre(var);
        for (int num_pre_var : num_eff_pre)
            num_candidates.erase(num_pre_var);
    }

    result.regular.assign(prop_candidates.begin(), prop_candidates.end());
    result.numeric.assign(num_candidates.begin(), num_candidates.end());
}

void PatternCollectionGeneratorSystematic::enqueue_pattern_if_new(
    const Pattern &pattern) {
    if (pattern_set.insert(pattern).second)
        patterns->push_back(pattern);
}

void PatternCollectionGeneratorSystematic::build_sga_patterns(
        TaskProxy task_proxy,
        numeric_pdb_helper::NumericTaskProxy &num_task_proxy,
        const CausalGraph &cg) {
    assert(pattern_max_size >= 1);
    assert(pattern_set.empty());
    assert(patterns && patterns->empty());

    /*
      SGA patterns are "single-goal ancestor" patterns, i.e., those
      patterns which can be generated by following eff/pre arcs from a
      single goal variable.

      This method must generate all SGA patterns up to size
      "max_pattern_size". They must be generated in order of
      increasing size, and they must be placed in "patterns".

      The overall structure of this is a similar processing queue as
      in the main pattern generation method below, and we reuse
      "patterns" and "pattern_set" between the two methods.
    */

    // Build goal patterns.
    for (FactProxy goal : num_task_proxy.get_propositional_goals()){
        int var_id = goal.get_variable().get_id();
        Pattern goal_pattern;
        goal_pattern.regular.push_back(var_id);
        enqueue_pattern_if_new(goal_pattern);
    }
    for (const auto &num_goal : num_task_proxy.get_numeric_goals()) {
        int var_id = num_goal.get_var_id();
        Pattern goal_pattern;
        goal_pattern.numeric.push_back(var_id);
        enqueue_pattern_if_new(goal_pattern);
    }

    /*
      Grow SGA patterns until all patterns are processed. Note that
      the patterns vectors grows during the computation.
    */
    for (size_t pattern_no = 0; pattern_no < patterns->size(); ++pattern_no) {
        // We must copy the pattern because references to patterns can be invalidated.
        Pattern pattern = (*patterns)[pattern_no];
        if (pattern.regular.size() + pattern.numeric.size() == pattern_max_size)
            break;

        Pattern neighbors;
        compute_eff_pre_neighbors(cg, pattern, neighbors);

        for (int neighbor_var_id : neighbors.regular) {
            Pattern new_pattern(pattern);
            new_pattern.regular.push_back(neighbor_var_id);
            sort(new_pattern.regular.begin(), new_pattern.regular.end());

            enqueue_pattern_if_new(new_pattern);
        }
        for (int neighbor_var_id : neighbors.numeric) {
            Pattern new_pattern(pattern);
            new_pattern.numeric.push_back(neighbor_var_id);
            sort(new_pattern.numeric.begin(), new_pattern.numeric.end());

            enqueue_pattern_if_new(new_pattern);
        }
    }

    pattern_set.clear();
}

void PatternCollectionGeneratorSystematic::build_patterns(
    TaskProxy task_proxy,
    numeric_pdb_helper::NumericTaskProxy &num_task_proxy) {
    const CausalGraph &cg = task_proxy.get_numeric_causal_graph();

    // Generate SGA (single-goal-ancestor) patterns.
    // They are generated into the patterns variable,
    // so we swap them from there.
    build_sga_patterns(task_proxy, num_task_proxy, cg);
    PatternCollection sga_patterns;
    patterns->swap(sga_patterns);

    /* Index the SGA patterns by variable.

       Important: sga_patterns_by_prop_var[var] must be sorted by size.
       This is guaranteed because build_sga_patterns generates
       patterns ordered by size.
    */
    vector<vector<const Pattern *>> sga_patterns_by_prop_var(task_proxy.get_variables().size());
    for (const Pattern &pattern : sga_patterns) {
        for (int var : pattern.regular) {
            assert(!task_proxy.is_derived_variable(task_proxy.get_variables()[var]));
            assert(!num_task_proxy.is_derived_numeric_variable(task_proxy.get_variables()[var]));
            sga_patterns_by_prop_var[var].push_back(&pattern);
        }
    }
    vector<vector<const Pattern *>> sga_patterns_by_num_var(task_proxy.get_numeric_variables().size());
    for (const Pattern &pattern : sga_patterns) {
        for (int var : pattern.numeric) {
            assert(task_proxy.get_numeric_variables()[var].get_var_type() == numType::regular);
            sga_patterns_by_num_var[var].push_back(&pattern);
        }
    }

    // Enqueue the SGA patterns.
    for (const Pattern &pattern : sga_patterns)
        enqueue_pattern_if_new(pattern);


    cout << "Found " << sga_patterns.size() << " SGA patterns." << endl;

    /*
      Combine patterns in the queue with SGA patterns until all
      patterns are processed. Note that the patterns vectors grows
      during the computation.
    */
    for (size_t pattern_no = 0; pattern_no < patterns->size(); ++pattern_no) {
        // We must copy the pattern because references to patterns can be invalidated.
        Pattern pattern1 = (*patterns)[pattern_no];

        Pattern neighbors;
        compute_connection_points(cg, pattern1, neighbors);

        for (int neighbor_var : neighbors.regular) {
            const auto &candidates = sga_patterns_by_prop_var[neighbor_var];
            for (const Pattern *p_pattern2 : candidates) {
                const Pattern &pattern2 = *p_pattern2;
                if (pattern1.regular.size() + pattern1.numeric.size() +
                    pattern2.numeric.size() + pattern2.numeric.size() > pattern_max_size)
                    break;  // All remaining candidates are too large.
                if (patterns_are_disjoint(pattern1, pattern2)) {
                    Pattern new_pattern;
                    compute_union_pattern(pattern1, pattern2, new_pattern);
                    enqueue_pattern_if_new(new_pattern);
                }
            }
        }
        for (int neighbor_var : neighbors.numeric) {
            const auto &candidates = sga_patterns_by_num_var[neighbor_var];
            for (const Pattern *p_pattern2 : candidates) {
                const Pattern &pattern2 = *p_pattern2;
                if (pattern1.regular.size() + pattern1.numeric.size() +
                    pattern2.numeric.size() + pattern2.numeric.size() > pattern_max_size)
                    break;  // All remaining candidates are too large.
                if (patterns_are_disjoint(pattern1, pattern2)) {
                    Pattern new_pattern;
                    compute_union_pattern(pattern1, pattern2, new_pattern);
                    enqueue_pattern_if_new(new_pattern);
                }
            }
        }
    }

    pattern_set.clear();
    cout << "Found " << patterns->size() << " interesting patterns." << endl;
}

void PatternCollectionGeneratorSystematic::build_patterns_naive(
    TaskProxy /*task_proxy*/,
    numeric_pdb_helper::NumericTaskProxy &/*num_task_proxy*/) {
    // TODO implement this
    cerr << "not implemented: build_patterns_naive in numeric_pdbs/pattern_collection_generator_systematic.cc" << endl;
    utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
//    int num_variables = task_proxy.get_variables().size();
//    PatternCollection current_patterns(1);
//    PatternCollection next_patterns;
//    for (size_t i = 0; i < pattern_max_size; ++i) {
//        for (const Pattern &current_pattern : current_patterns) {
//            int max_var = -1;
//            if (i > 0)
//                max_var = current_pattern.back();
//            for (int var = max_var + 1; var < num_variables; ++var) {
//                Pattern pattern = current_pattern;
//                pattern.push_back(var);
//                next_patterns.push_back(pattern);
//                patterns->push_back(pattern);
//            }
//        }
//        next_patterns.swap(current_patterns);
//        next_patterns.clear();
//    }
//
//    cout << "Found " << patterns->size() << " patterns." << endl;
}

PatternCollectionInformation PatternCollectionGeneratorSystematic::generate(
    shared_ptr<AbstractTask> task) {
    TaskProxy task_proxy(*task);
    numeric_pdb_helper::NumericTaskProxy num_task_proxy(task_proxy);
    patterns = make_shared<PatternCollection>();
    pattern_set.clear();
    if (only_interesting_patterns) {
        build_patterns(task_proxy, num_task_proxy);
    } else {
        build_patterns_naive(task_proxy, num_task_proxy);
    }
    return {task, patterns, max_number_pdb_states};
}

static shared_ptr<PatternCollectionGenerator> _parse(OptionParser &parser) {
    parser.document_synopsis(
            "Systematically generated patterns",
            "Generates all (interesting) patterns with up to pattern_max_size "
            "variables. "
            "For details, see" + utils::format_paper_reference(
                    {"Florian Pommerening", "Gabriele Roeger", "Malte Helmert"},
                    "Getting the Most Out of Pattern Databases for Classical Planning",
                    "http://ijcai.org/papers13/Papers/IJCAI13-347.pdf",
                    "Proceedings of the Twenty-Third International Joint"
                    " Conference on Artificial Intelligence (IJCAI 2013)",
                    "2357-2364",
                    "2013"));

    parser.add_option<int>(
            "pattern_max_size",
            "max number of variables per pattern",
            "1",
            Bounds("1", "infinity"));

    parser.add_option<int>(
            "max_number_pdb_states",
            "max number of abstract states per pattern",
            "1000000",
            Bounds("1", "infinity"));

    parser.add_option<bool>(
            "only_interesting_patterns",
            "Only consider the union of two disjoint patterns if the union has "
            "more information than the individual patterns.",
            "true");

    Options opts = parser.parse();
    if (parser.dry_run())
        return nullptr;

    return make_shared<PatternCollectionGeneratorSystematic>(opts);
}

static PluginShared<PatternCollectionGenerator> _plugin("numeric_systematic", _parse);
}
