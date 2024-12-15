#ifndef NUMERIC_PDBS_CANONICAL_PDBS_HEURISTIC_H
#define NUMERIC_PDBS_CANONICAL_PDBS_HEURISTIC_H

#include "canonical_pdbs.h"

#include "../heuristic.h"

namespace numeric_pdbs {
// Implements the canonical heuristic function.
class CanonicalPDBsHeuristic : public Heuristic {
    CanonicalPDBs canonical_pdbs;

protected:
    ap_float compute_heuristic(const GlobalState &state) override;
    /* TODO: we want to get rid of compute_heuristic(const GlobalState &state)
       and change the interface to only use State objects. While we are doing
       this, the following method already allows to get the heuristic value
       for a State object. */
    ap_float compute_heuristic(const State &state) const;

public:
    explicit CanonicalPDBsHeuristic(const options::Options &opts);
    virtual ~CanonicalPDBsHeuristic() = default;

    void print_statistics() const override;
};
}

#endif
