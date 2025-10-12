#ifndef DOMAIN_ABSTRACTIONS_DOMAIN_ABSTRACTION_COLLECTION_GENERATOR_MULTIPLE_CEGAR_H
#define DOMAIN_ABSTRACTIONS_DOMAIN_ABSTRACTION_COLLECTION_GENERATOR_MULTIPLE_CEGAR_H

#include "domain_abstraction_collection_generator_multiple.h"

namespace domain_abstractions {
enum class FlawTreatment;
enum class InitSplitCandidates;
enum class InitSplitQuantity;
enum class InitSplitMethod;

class DomainAbstractionCollectionGeneratorMultipleCegar : public DomainAbstractionCollectionGeneratorMultiple {
    const bool use_wildcard_plans;
    const FlawTreatment flaw_treatment;
    const InitSplitMethod init_split_method;

    virtual std::string id() const override;
    virtual void initialize(const TaskProxy &) override {}
    virtual DomainAbstraction compute_abstraction(
        int max_abstraction_size,
        double max_time,
        const std::shared_ptr<utils::RandomNumberGenerator> &rng,
        const TaskProxy &task_proxy,
        const Fact &goal,
        std::unordered_set<int> &&init_split_var_ids,
        std::unordered_set<int> &&blacklisted_variables) override;
public:
    explicit DomainAbstractionCollectionGeneratorMultipleCegar(options::Options &opts);
};
}

#endif
