#ifndef NUMERIC_PDBS_CANONICAL_PDBS_H
#define NUMERIC_PDBS_CANONICAL_PDBS_H

#include "types.h"

#include "../globals.h"

#include <memory>

class State;

namespace numeric_pdbs {
class CanonicalPDBs {
    friend class CanonicalPDBsHeuristic;

    std::shared_ptr<MaxAdditivePDBSubsets> max_additive_subsets;

public:
    CanonicalPDBs(std::shared_ptr<PDBCollection> pattern_databases,
                  std::shared_ptr<MaxAdditivePDBSubsets> max_additive_subsets,
                  bool dominance_pruning);
    ~CanonicalPDBs() = default;

    ap_float get_value(const State &state) const;
};
}

#endif
