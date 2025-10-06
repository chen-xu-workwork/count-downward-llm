#ifndef DOMAIN_ABSTRACTION_DOMAIN_ABSTRACTION_H
#define DOMAIN_ABSTRACTION_DOMAIN_ABSTRACTION_H

#include "types.h"
#include "domain_abstraction_state_registry.h"
#include "../utils/logging.h"
#include "../task_proxy.h"
#include <memory>

namespace utils {
class LogProxy;
}

#include <vector>

namespace domain_abstractions {
class DomainAbstraction {
    // Domain mapping for propositional variables
    DomainMapping domain_mapping;
    
    // Domain mapping for numeric variables (one per numeric variable in the abstraction)
    NumericDomainMappingType numeric_domain_mapping;
    
    std::vector<int> hash_multipliers;
    std::vector<int> distances;
    // TODO: get rid of this here and return it from the factory optionally.
    std::vector<std::vector<int>> wildcard_plan;
    
    // State registry for handling discretized numeric variables
    std::unique_ptr<DomainAbstractionStateRegistry> state_registry;
    
    bool has_numeric_variables;

    int hash_index(const std::vector<int> &state) const;

public:
    DomainAbstraction(DomainMapping &&domain_mapping,
                      NumericDomainMappingType &&numeric_domain_mapping, 
                      std::vector<int> &&hash_multipliers,
                      std::vector<int> &&distances,
                      std::vector<std::vector<int>> &&wildcard_plan,
                      std::unique_ptr<DomainAbstractionStateRegistry> &&state_registry = nullptr
                      )
        : domain_mapping(std::move(domain_mapping)),
          numeric_domain_mapping(std::move(numeric_domain_mapping)),
          hash_multipliers(std::move(hash_multipliers)),
          distances(std::move(distances)),
          wildcard_plan(std::move(wildcard_plan)),
          state_registry(std::move(state_registry)),
          has_numeric_variables(!this->numeric_domain_mapping.empty()) {
    }

    const DomainMapping &get_domain_mapping() const {
        return domain_mapping;
    }

    const DomainMapping &&extract_domain_mapping() {
        return std::move(domain_mapping);
    }
    
    const NumericDomainMappingType &get_numeric_domain_mapping() const {
        return numeric_domain_mapping;
    }
    
    NumericDomainMappingType &get_numeric_domain_mapping() {
        return numeric_domain_mapping;
    }

    int get_value(const State &state) const;

    std::vector<std::vector<int>> get_plan() const {
        return wildcard_plan;
    }

    int size() const {
        return distances.size();
    }

    void dump(utils::LogProxy &log) const;
};
}

#endif
