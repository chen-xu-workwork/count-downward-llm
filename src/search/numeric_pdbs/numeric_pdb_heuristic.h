#ifndef NUMERIC_PDBS_NUMERIC_PDB_HEURISTIC_H
#define NUMERIC_PDBS_NUMERIC_PDB_HEURISTIC_H

#include "numeric_pattern_database.h"

#include "../heuristic.h"

class GlobalState;
class State;

namespace options {
class Options;
}

namespace numeric_pdbs {
// Implements a heuristic for a single PDB.
class NumericPDBHeuristic : public Heuristic {
    PatternDatabase pdb;
protected:
    virtual ap_float compute_heuristic(const GlobalState &global_state) override;
    /* TODO: we want to get rid of compute_heuristic(const GlobalState &state)
       and change the interface to only use State objects. While we are doing
       this, the following method already allows to get the heuristic value
       for a State object. */
    ap_float compute_heuristic(const State &state) const;
public:
    /*
      Important: It is assumed that the pattern (passed via Options) is
      sorted, contains no duplicates and is small enough so that the
      number of abstract states is below numeric_limits<int>::max()
      Parameters:
       operator_costs: Can specify individual operator costs for each
       operator. This is useful for action cost partitioning. If left
       empty, default operator costs are used.
    */
    NumericPDBHeuristic(const options::Options &opts);
    virtual ~NumericPDBHeuristic() override = default;
};
}

#endif
