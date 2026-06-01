#ifndef DOMAIN_ABSTRACTIONS_DOMAIN_ABSTRACTION_COLLECTION_GENERATOR_MULTIPLE_CEGAR_H
#define DOMAIN_ABSTRACTIONS_DOMAIN_ABSTRACTION_COLLECTION_GENERATOR_MULTIPLE_CEGAR_H

#include "domain_abstraction_collection_generator_multiple.h"

namespace domain_abstractions {
enum class FlawTreatment;
enum class InitSplitCandidates;
enum class InitSplitQuantity;
enum class InitSplitMethod;
enum class NumericSplitStrategy;
enum class ExecEntirePlanMode;

class DomainAbstractionCollectionGeneratorMultipleCegar : public DomainAbstractionCollectionGeneratorMultiple {
    const bool use_wildcard_plans;
    const bool deviation_flaws;
    const FlawTreatment flaw_treatment;
    const InitSplitMethod init_split_method;
    const NumericSplitStrategy numeric_split_strategy;
    const ExecEntirePlanMode exec_entire_plan;
    const bool use_threshold_aware_numeric_splits;
    const bool use_progress_weighted_flaw_selection;
    const int refinement_batch_size;

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
