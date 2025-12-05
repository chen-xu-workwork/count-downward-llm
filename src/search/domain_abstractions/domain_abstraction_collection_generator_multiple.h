#ifndef DOMAIN_ABSTRACTIONS_DOMAIN_ABSTRACTION_COLLECTION_GENERATOR_MULTIPLE_H
#define DOMAIN_ABSTRACTIONS_DOMAIN_ABSTRACTION_COLLECTION_GENERATOR_MULTIPLE_H

#include "domain_abstraction_collection_generator.h"

#include "types.h"
#include "../task_proxy.h"

#include <set>
#include <unordered_set>


namespace options {
class OptionParser;
}

namespace utils {
class CountdownTimer;
class RandomNumberGenerator;
}

namespace domain_abstractions {
enum class VariableSubset {
    GOALS,
    NON_GOALS,
    ALL,
};
// TODO: allow percentage of facts between "ALL" and "SINGLE"?
enum class InitSplitQuantity {
    NONE,
    SINGLE,
    ALL,
};

/*
  This pattern collection generator is a general framework for computing a
  pattern collection for a given planning task. It is an abstract base class
  which must be subclasses to provide a method for computing a single pattern
  for the given task and a single goal of the task.

  The algorithm works as follows. It first stores the goals of the task in
  random order. Then, it repeatedly iterates over all goals and for each goal,
  it uses the given method for computing a single pattern. If the pattern is
  new (duplicate detection), it is kept for the final collection.

  The algorithm runs until reaching a given time limit. Another parameter allows
  exiting early if no new patterns are found for a certain time ("stagnation").
  Further parameters allow enabling blacklisting for the given pattern computation
  method after a certain time to force some diversification or to enable said
  blacklisting when stagnating.
*/
class DomainAbstractionCollectionGeneratorMultiple : public DomainAbstractionCollectionGenerator {
    const int max_abstraction_size;
    const double abstraction_generation_max_time;
    const double total_max_time;
    const double stagnation_limit;
    const double blacklisting_start_time;
    const bool enable_blacklist_on_stagnation;
    const VariableSubset blacklist_variables;
protected:
    const VariableSubset init_split_variables;
private:
    const InitSplitQuantity init_split_quantity;
    std::shared_ptr<utils::RandomNumberGenerator> rng;
    const int random_seed;

    // Variables used in the main loop.
    int remaining_collection_size;
    bool blacklisting;
    double time_point_of_last_new_pattern;

    void check_blacklist_trigger_timer(const utils::CountdownTimer &timer);
    std::unordered_set<int> get_blacklisted_variables(
        std::vector<int> &blacklist_candidates);
    std::unordered_set<int> get_init_split_variables(
        std::vector<int> &init_split_candidates, int iteration);
    void handle_generated_abstraction(
        DomainAbstraction &&abstraction,
        std::set<DomainMapping> &generated_mappings,
        DomainAbstractionCollection &generated_abstractions,
        const utils::CountdownTimer &timer);
    bool collection_size_limit_reached() const;
    bool time_limit_reached(const utils::CountdownTimer &timer) const;
    bool check_for_stagnation(const utils::CountdownTimer &timer);
    virtual std::string id() const = 0;
    virtual void initialize(const TaskProxy &task_proxy) = 0;
    virtual DomainAbstraction compute_abstraction(
        int max_abstraction_size,
        double max_time,
        const std::shared_ptr<utils::RandomNumberGenerator> &rng,
        const TaskProxy &task_proxy,
        const Fact &goal,
        std::unordered_set<int> &&init_split_var_ids,
        std::unordered_set<int> &&blacklisted_variables) = 0;
    virtual std::string name() const override;
    virtual DomainAbstractionCollection compute_abstractions(
        const TaskProxy &task_proxy) override;

    std::vector<int> get_candidates(const TaskProxy &task_proxy,
                                    VariableSubset option,
                                    bool include_numeric);
public:
    explicit DomainAbstractionCollectionGeneratorMultiple(options::Options &opts);
    virtual ~DomainAbstractionCollectionGeneratorMultiple() override = default;
};

extern void add_multiple_algorithm_implementation_notes_to_parser(
    options::OptionParser &parser);
extern void add_multiple_options_to_parser(options::OptionParser &parser);
}

#endif
