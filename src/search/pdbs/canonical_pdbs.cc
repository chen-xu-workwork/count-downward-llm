#include "canonical_pdbs.h"

#include "dominance_pruning.h"
#include "pattern_database.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <limits>

using namespace std;

namespace pdbs {
CanonicalPDBs::CanonicalPDBs(
    shared_ptr<PDBCollection> pattern_databases,
    shared_ptr<MaxAdditivePDBSubsets> max_additive_subsets_,
    bool dominance_pruning)
    : max_additive_subsets(max_additive_subsets_) {
    assert(max_additive_subsets);
    if (dominance_pruning) {
        max_additive_subsets = prune_dominated_subsets(
            *pattern_databases, *max_additive_subsets);
    }
}

ap_float CanonicalPDBs::get_value(const State &state) const {
    // If we have an empty collection, then max_additive_subsets = { \emptyset }.
    assert(!max_additive_subsets->empty());
    ap_float max_h = 0;
    for (const auto &subset : *max_additive_subsets) {
        ap_float subset_h = 0;
        for (const shared_ptr<PatternDatabase> &pdb : subset) {
            /* Experiments showed that it is faster to recompute the
               h values than to cache them in an unordered_map. */
            ap_float h = pdb->get_value(state);
            if (h == numeric_limits<ap_float>::max())
                return numeric_limits<ap_float>::max();
            subset_h += h;
        }
        max_h = max(max_h, subset_h);
    }
    return max_h;
}
}
