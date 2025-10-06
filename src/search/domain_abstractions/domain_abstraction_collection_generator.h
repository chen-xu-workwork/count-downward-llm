#ifndef DOMAIN_ABSTRACTIONS_DOMAIN_ABSTRACTION_COLLECTION_GENERATOR_H
#define DOMAIN_ABSTRACTIONS_DOMAIN_ABSTRACTION_COLLECTION_GENERATOR_H

#include "types.h"
#include "../options/options.h"

#include "../utils/logging.h"
#include <memory>

class TaskProxy;

namespace utils {
class RandomNumberGenerator;
}

namespace domain_abstractions {
class DomainAbstractionCollectionGenerator {
protected:
    std::shared_ptr<utils::RandomNumberGenerator> rng;

    virtual std::string name() const = 0;
    virtual DomainAbstractionCollection compute_abstractions(
        const TaskProxy &task_proxy) = 0;
public:
    explicit DomainAbstractionCollectionGenerator(const options::Options &opts);
    virtual ~DomainAbstractionCollectionGenerator() = default;

    DomainAbstractionCollection generate(const TaskProxy &task_proxy);
};

extern void add_domain_abstraction_collection_generator_options_to_parser(
    options::OptionParser &parser);
}

#endif
