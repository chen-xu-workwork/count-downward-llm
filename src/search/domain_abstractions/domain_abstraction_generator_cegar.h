#ifndef DOMAIN_ABSTRACTIONS_DOMAIN_ABSTRACTION_GENERATOR_CEGAR_H
#define DOMAIN_ABSTRACTIONS_DOMAIN_ABSTRACTION_GENERATOR_CEGAR_H

#include "domain_abstraction_generator.h"
#include <unordered_set>
#include "../option_parser.h"

namespace utils {
class RandomNumberGenerator;
}

namespace domain_abstractions {
enum class FlawTreatment;
enum class InitSplitMethod;
enum class NumericSplitStrategy;
enum class ExecEntirePlanMode;

enum class InitSplitOptions {
    NONE,
    RANDOM_GOAL,
    RANDOM_NON_GOAL,
    RANDOM_ANY,
    ALL_GOALS,
    ALL_NON_GOALS,
    ALL,
};

class DomainAbstractionGeneratorCEGAR : public DomainAbstractionGenerator {
private:
    const int max_abstraction_size;
    const double max_time;
    const bool use_wildcard_plans;
    const bool deviation_flaws;
    const FlawTreatment flaw_treatment;
    const InitSplitMethod init_split_method;
    const NumericSplitStrategy numeric_split_strategy;
    const ExecEntirePlanMode exec_entire_plan;
    const InitSplitOptions init_split_option;

    std::unordered_set<int> get_init_split_var_ids(const TaskProxy &task_proxy);
public:
    explicit DomainAbstractionGeneratorCEGAR(const options::Options &opts);

    DomainAbstraction build_abstraction(const TaskProxy &task_proxy) override;
};
}

#endif
