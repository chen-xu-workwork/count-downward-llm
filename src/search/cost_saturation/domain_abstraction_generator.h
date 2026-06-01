#ifndef COST_SATURATION_DOMAIN_ABSTRACTION_GENERATOR_H
#define COST_SATURATION_DOMAIN_ABSTRACTION_GENERATOR_H

#include "abstraction_generator.h"

namespace options {
class Options;
}

namespace domain_abstractions {
class DomainAbstractionCollectionGenerator;
}

namespace cost_saturation {
class DomainAbstractionGenerator : public AbstractionGenerator {
    const std::shared_ptr<domain_abstractions::DomainAbstractionCollectionGenerator> generator;
    const bool combine_labels;

public:
    explicit DomainAbstractionGenerator(const options::Options &opts);

    Abstractions generate_abstractions(
        const std::shared_ptr<AbstractTask> &task,
        DeadEnds *dead_ends);
};
}

#endif
