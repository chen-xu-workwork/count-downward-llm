#ifndef NUMERIC_PDBS_PATTERN_GENERATOR_GREEDY_H
#define NUMERIC_PDBS_PATTERN_GENERATOR_GREEDY_H

#include "pattern_generator.h"

namespace options {
class Options;
}

namespace numeric_pdbs {
class PatternGeneratorGreedy : public PatternGenerator {
    int max_states;
public:
    explicit PatternGeneratorGreedy(const options::Options &opts);
    explicit PatternGeneratorGreedy(int max_states);
    virtual ~PatternGeneratorGreedy() = default;

    virtual Pattern generate(std::shared_ptr<AbstractTask> task) override;
};
}

#endif
