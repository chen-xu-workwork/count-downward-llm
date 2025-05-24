#include "canonical_pdbs.h"

#include "dominance_pruning.h"
#include "pattern_database.h"

#include <algorithm>
#include <cassert>
#include <limits>

using namespace std;

namespace numeric_pdbs {
std::unique_ptr<lm_cut_numeric_heuristic::LandmarkCutNumericHeuristic> CanonicalPDBs::lmc;
std::unique_ptr<rmax_heuristic::RMaxHeuristic> CanonicalPDBs::hrmax;

CanonicalPDBs::CanonicalPDBs(
        const shared_ptr<AbstractTask> &task,
        shared_ptr<PDBCollection> pattern_databases,
        shared_ptr<MaxAdditivePDBSubsets> max_additive_subsets_,
        bool dominance_pruning,
        InnerHeuristic global_failed_lookup_h)
        : max_additive_subsets(max_additive_subsets_),
          number_lookup_misses(0) {

    assert(max_additive_subsets);
    if (dominance_pruning) {
        max_additive_subsets = prune_dominated_subsets(
            *pattern_databases, *max_additive_subsets);
    }

    switch (global_failed_lookup_h) {
        case InnerHeuristic::LMCUT:
            if (!lmc) {
                lmc = unique_ptr<lm_cut_numeric_heuristic::LandmarkCutNumericHeuristic>(
                        new lm_cut_numeric_heuristic::LandmarkCutNumericHeuristic(task));
                lmc->initialize();
            }
            break;
        case InnerHeuristic::HRMAX:
            if (!hrmax) {
                hrmax = unique_ptr<rmax_heuristic::RMaxHeuristic>(new rmax_heuristic::RMaxHeuristic(task));
                hrmax->initialize();
            }
            break;
        case InnerHeuristic::BLIND:
            break;
        default:
            cerr << "ERROR: only hrmax and lmcut are supported as global_failed_lookup_heuristic." << endl;
            utils::exit_with(utils::ExitCode::INPUT_ERROR);
    }
}

ap_float CanonicalPDBs::get_value(const State &state) const {
    // If we have an empty collection, then max_additive_subsets = { \emptyset }.
    assert(!max_additive_subsets->empty());
    ap_float max_h = 0;
    int found_state = 0;
    int num_pdbs = 0;
    for (const auto &subset : *max_additive_subsets) {
        ap_float subset_h = 0;
        for (const shared_ptr<PatternDatabase> &pdb : subset) {
            /* Experiments showed that it is faster to recompute the
               h values than to cache them in an unordered_map. */
            ++num_pdbs;
            auto [found_state_pdb, h] = pdb->get_value(state);
            if (found_state_pdb){
                found_state++;
            }
            if (h == numeric_limits<ap_float>::max())
                return numeric_limits<ap_float>::max();
            subset_h += h;
        }
        max_h = max(max_h, subset_h);
    }
    if (found_state < 0.5 * float(num_pdbs)){
        number_lookup_misses++;
        if (lmc){
            return lmc->compute_heuristic(state);
        }
        if (hrmax){
            return hrmax->compute_heuristic(state);
        }
    }
    return max_h;
}
}
