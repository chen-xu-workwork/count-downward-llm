#ifndef NUMERIC_PDBS_CANONICAL_PDBS_H
#define NUMERIC_PDBS_CANONICAL_PDBS_H

#include "types.h"

#include "../globals.h"

#include <memory>

class State;

namespace numeric_pdbs {
class CanonicalPDBs {
    std::shared_ptr<MaxAdditivePDBSubsets> max_additive_subsets;
    mutable size_t number_lookup_misses; // for statistics only

public:
    CanonicalPDBs(std::shared_ptr<PDBCollection> pattern_databases,
                  std::shared_ptr<MaxAdditivePDBSubsets> max_additive_subsets,
                  bool dominance_pruning);
    ~CanonicalPDBs() = default;

    ap_float get_value(const State &state) const;

    size_t get_number_lookup_misses() const {
        return number_lookup_misses;
    }
};
}

#endif
