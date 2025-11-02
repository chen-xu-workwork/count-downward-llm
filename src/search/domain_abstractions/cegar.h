#ifndef DOMAIN_ABSTRACTIONS_CEGAR_H
#define DOMAIN_ABSTRACTIONS_CEGAR_H

#include <memory>
#include <unordered_set>
#include <vector>
#include "../options/options.h"
#include "../option_parser.h"

class Fact;
class TaskProxy;

namespace options {
class OptionParser;
}

namespace utils {
class LogProxy;
class RandomNumberGenerator;
}

namespace domain_abstractions {
class DomainAbstraction;

enum class FlawTreatment {
    RANDOM_SINGLE_ATOM,
    ONE_SPLIT_PER_ATOM,
    ONE_SPLIT_PER_VARIABLE,
    MAX_REFINED_SINGLE_ATOM,
};
enum class InitSplitMethod{
    GOAL_VALUE,
    GOAL_VALUE_OR_RANDOM_IF_NON_GOAL,
    INIT_VALUE,
    RANDOM_VALUE,
    RANDOM_PARTITION,
    RANDOM_BINARY_PARTITION_SEPARATING_INIT_GOAL,
    IDENTITY,
};

enum class NumericSplitStrategy {
    STANDARD,   // Standard split: [lower, x) and [x, upper) with different partitions
    EXCLUSION,  // Exclusion split: R\{x} and {x} as separate partitions
};

/*
  This function implements the CEGAR algorithm for computing a domain
  abstraction.

  TODO
*/
extern DomainAbstraction generate_domain_abstraction_with_cegar(
            int max_abstraction_size,
            double max_time,
            bool use_wildcard_plans,
            FlawTreatment flaw_treatment,
            InitSplitMethod init_split_method,
            NumericSplitStrategy numeric_split_strategy,
            const std::shared_ptr <utils::RandomNumberGenerator> &rng,
            const TaskProxy &task_proxy,
            std::unordered_set<int> &&init_split_var_ids = std::unordered_set<int>(),
            std::unordered_set<int> &&blacklisted_variables = std::unordered_set<int>());


extern void add_cegar_implementation_notes_to_parser(
    options::OptionParser &parser);
extern void add_domain_abstraction_cegar_options_to_parser(
    options::OptionParser &parser);
}

#endif
