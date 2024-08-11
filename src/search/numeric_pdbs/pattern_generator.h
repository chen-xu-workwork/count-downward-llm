#ifndef NUMERIC_PDBS_PATTERN_GENERATOR_H
#define NUMERIC_PDBS_PATTERN_GENERATOR_H

#include "pattern_collection_information.h"
#include "types.h"

#include <memory>

class AbstractTask;

namespace numeric_pdbs {
class PatternCollectionGenerator {
protected:
    // approximate upper bound on the number of abstract states per PDB possibly reachable within the pattern
    const size_t max_number_pdb_states;
public:
    explicit PatternCollectionGenerator(size_t max_number_pdb_states)
            : max_number_pdb_states(max_number_pdb_states) {
    }

    virtual PatternCollectionInformation generate(std::shared_ptr<AbstractTask> task) = 0;

    size_t get_max_number_pdb_states() const {
        return max_number_pdb_states;
    }
};

class PatternGenerator {
protected:
    // approximate upper bound on the number of abstract states possibly reachable within the pattern
    const size_t max_number_pdb_states;
public:
    explicit PatternGenerator(size_t max_number_pdb_states)
            : max_number_pdb_states(max_number_pdb_states) {
    }

    virtual Pattern generate(std::shared_ptr<AbstractTask> task,
                             std::shared_ptr<numeric_pdb_helper::NumericTaskProxy> task_proxy) = 0;

    size_t get_max_number_pdb_states() const {
        return max_number_pdb_states;
    }
};
}

#endif
