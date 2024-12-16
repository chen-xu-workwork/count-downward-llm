#include "incremental_canonical_pdbs.h"

#include "canonical_pdbs.h"
#include "pattern_database.h"

#include "../utils/timer.h"

#include <iostream>
#include <limits>

using namespace std;
using numeric_pdb_helper::NumericTaskProxy;

namespace numeric_pdbs {
IncrementalCanonicalPDBs::IncrementalCanonicalPDBs(
    shared_ptr<AbstractTask> task,
    shared_ptr<NumericTaskProxy> task_proxy,
    const PatternCollection &intitial_patterns,
    size_t max_number_pdb_states)
    : task(std::move(task)),
      task_proxy(std::move(task_proxy)),
      patterns(make_shared<PatternCollection>(intitial_patterns.begin(),
                                              intitial_patterns.end())),
      pattern_databases(make_shared<PDBCollection>()),
      max_additive_subsets(nullptr),
      size(0),
      max_number_pdb_states(max_number_pdb_states) {
    utils::Timer timer;
    pattern_databases->reserve(patterns->size());
    for (const Pattern &pattern : *patterns)
        add_pdb_for_pattern(pattern);
    are_additive = compute_additive_vars(*this->task_proxy);
    recompute_max_additive_subsets();
    cout << "PDB collection construction time: " << timer << endl;
}

void IncrementalCanonicalPDBs::add_pdb_for_pattern(const Pattern &pattern) {
    pattern_databases->emplace_back(new PatternDatabase(task_proxy, pattern, max_number_pdb_states));
    size += pattern_databases->back()->get_size();
}

void IncrementalCanonicalPDBs::add_pattern(const Pattern &pattern) {
    patterns->push_back(pattern);
    add_pdb_for_pattern(pattern);
    recompute_max_additive_subsets();
}

void IncrementalCanonicalPDBs::recompute_max_additive_subsets() {
    max_additive_subsets = compute_max_additive_subsets(*pattern_databases,
                                                        are_additive);
}

MaxAdditivePDBSubsets IncrementalCanonicalPDBs::get_max_additive_subsets(
    const Pattern &new_pattern) {
    return numeric_pdbs::compute_max_additive_subsets_with_pattern(
        *max_additive_subsets, new_pattern, are_additive);
}

ap_float IncrementalCanonicalPDBs::get_value(const State &state) const {
    CanonicalPDBs canonical_pdbs(pattern_databases, max_additive_subsets, false);
    return canonical_pdbs.get_value(state);
}

bool IncrementalCanonicalPDBs::is_dead_end(const State &state) const {
    for (const shared_ptr<PatternDatabase> &pdb : *pattern_databases)
        if (pdb->get_value(state).second == numeric_limits<ap_float>::max())
            return true;
    return false;
}

PatternCollectionInformation
IncrementalCanonicalPDBs::get_pattern_collection_information() const {
    PatternCollectionInformation result(task_proxy, patterns, max_number_pdb_states);
    result.set_pdbs(pattern_databases);
    result.set_max_additive_subsets(max_additive_subsets);
    return result;
}
}
