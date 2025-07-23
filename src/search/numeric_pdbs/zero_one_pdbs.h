#ifndef NUMERIC_PDBS_ZERO_ONE_PDBS_H
#define NUMERIC_PDBS_ZERO_ONE_PDBS_H

#include "types.h"
#include "numeric_helper.h"

class State;
class TaskProxy;

namespace numeric_pdbs {
class ZeroOnePDBs {
    PDBCollection pattern_databases;

    bool extend_abstract_state_space;
    double f_layer_offset_ratio;
    bool keep_parent_pointers;
    int need_goal;
    double max_h_factor;

    InnerHeuristic exploration_h;
    InnerHeuristic frontier_h;
    InnerHeuristic failed_lookup_h;
public:
    ZeroOnePDBs(
      const std::shared_ptr<numeric_pdb_helper::NumericTaskProxy> &task_proxy, 
      const PatternCollection &patterns,
      std::size_t max_number_states,
      bool extend_abstract_state_space,
      bool need_goal,
      double f_layer_offset_ratio,
      bool keep_parent_pointers,
      double max_h_factor,
      InnerHeuristic exploration_h,
      InnerHeuristic frontier_h,
      InnerHeuristic failed_lookup_h
    );

    ~ZeroOnePDBs() = default;

    ap_float get_value(const State &state) const;
    /*
      Returns the sum of all mean finite h-values of every PDB.
      This is an approximation of the real mean finite h-value of the Heuristic,
      because dead-ends are ignored for the computation of the mean finite
      h-values for a PDB. As a consequence, if different PDBs have different
      states which are dead-end, we do not calculate the real mean h-value for
      these states.
    */
    ap_float compute_approx_mean_finite_h() const;

    void dump() const;
};
}

#endif
