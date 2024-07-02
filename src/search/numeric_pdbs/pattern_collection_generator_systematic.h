#ifndef NUMERIC_PDBS_PATTERN_COLLECTION_GENERATOR_SYSTEMATIC_H
#define NUMERIC_PDBS_PATTERN_COLLECTION_GENERATOR_SYSTEMATIC_H

#include "pattern_generator.h"
#include "types.h"

#include "../utils/hash.h"

#include <cstdlib>
#include <memory>
#include <unordered_set>
#include <vector>

class TaskProxy;

namespace options {
class Options;
}

namespace numeric_pdb_helper {
class NumericTaskProxy;
}

namespace numeric_pdbs {
class CanonicalPDBsHeuristic;
class CausalGraph;

// Invariant: patterns are always sorted.
class PatternCollectionGeneratorSystematic : public PatternCollectionGenerator {
    using PatternSet = std::unordered_set<Pattern>;

    const size_t pattern_max_size;
    const bool only_interesting_patterns;
    std::shared_ptr<PatternCollection> patterns;
    PatternSet pattern_set;  // Cleared after pattern computation.

    void enqueue_pattern_if_new(const Pattern &pattern);
    static void compute_eff_pre_neighbors(const CausalGraph &cg,
                                   const Pattern &pattern,
                                   Pattern &result) ;
    static void compute_connection_points(const CausalGraph &cg,
                                   const Pattern &pattern,
                                   Pattern &result) ;

    void build_sga_patterns(TaskProxy task_proxy,
                            numeric_pdb_helper::NumericTaskProxy &num_task_proxy,
                            const CausalGraph &cg);
    void build_patterns(TaskProxy task_proxy, numeric_pdb_helper::NumericTaskProxy &num_task_proxy);
    void build_patterns_naive(TaskProxy task_proxy, numeric_pdb_helper::NumericTaskProxy &num_task_proxy);
public:
    explicit PatternCollectionGeneratorSystematic(const options::Options &opts);
    ~PatternCollectionGeneratorSystematic() = default;

    virtual PatternCollectionInformation generate(
        std::shared_ptr<AbstractTask> task) override;
};
}

#endif
