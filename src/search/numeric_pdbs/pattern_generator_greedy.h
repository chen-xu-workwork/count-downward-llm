#ifndef NUMERIC_PDBS_PATTERN_GENERATOR_GREEDY_H
#define NUMERIC_PDBS_PATTERN_GENERATOR_GREEDY_H

#include "pattern_generator.h"
#include "variable_order_finder.h"

namespace options {
class Options;
}

namespace utils {
class RandomNumberGenerator;
}

namespace numeric_pdbs {
class PatternGeneratorGreedy : public PatternGenerator {
    int max_states;
    bool numeric_variables_first;
    VariableOrderType var_order_type;
    std::shared_ptr<utils::RandomNumberGenerator> rng;
public:
    explicit PatternGeneratorGreedy(const options::Options &opts);
    explicit PatternGeneratorGreedy(int max_states,
                                    bool numeric_variables_first,
                                    VariableOrderType var_order_type,
                                    std::shared_ptr<utils::RandomNumberGenerator> rng);
    virtual ~PatternGeneratorGreedy() = default;

    Pattern generate(std::shared_ptr<AbstractTask> task) override;
};
}

#endif
