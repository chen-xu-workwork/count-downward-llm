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
    DomainMapping domain_mapping;
    NumericDomainMappingType numeric_domain_mapping;
    
    std::vector<int> hash_multipliers;
    std::vector<ap_float> distances;
    // TODO: get rid of this here and return it from the factory optionally.
    std::vector<std::vector<int>> wildcard_plan;
    std::vector<std::vector<int>> abstract_prop_states; //used for deviation flaws
    std::vector<std::vector<int>> abstract_numeric_states; //used for deviation flaws
    
    // State registry for handling discretized numeric variables
    
    // Task proxy for evaluating comparison axioms
    TaskProxy task_proxy;
    
    bool has_numeric_variables;
    ap_float min_operator_cost;

    int hash_index(const std::vector<int> &state) const;

public:
    DomainAbstraction(DomainMapping &&domain_mapping,
                      NumericDomainMappingType &&numeric_domain_mapping, 
                      std::vector<int> &&hash_multipliers,
                      std::vector<ap_float> &&distances,
                      std::vector<std::vector<int>> &&wildcard_plan,
                      std::vector<std::vector<int>> &&abstract_prop_states,
                      std::vector<std::vector<int>> &&abstract_numeric_states,
                      std::unique_ptr<DomainAbstractionStateRegistry> &&state_registry,
                      const TaskProxy &task_proxy
                      )
        : domain_mapping(std::move(domain_mapping)),
          numeric_domain_mapping(std::move(numeric_domain_mapping)),
          hash_multipliers(std::move(hash_multipliers)),
          distances(std::move(distances)),
          wildcard_plan(std::move(wildcard_plan)),
          abstract_prop_states(std::move(abstract_prop_states)),
          abstract_numeric_states(std::move(abstract_numeric_states)),
          task_proxy(task_proxy),
          has_numeric_variables(false),
          min_operator_cost(std::numeric_limits<ap_float>::max()) {
        // Check if any numeric variable has non-trivial partitioning
        for (const auto &num_mapping : this->numeric_domain_mapping) {
            if (num_mapping->get_ranges().size() > 1) {
                has_numeric_variables = true;
                break;
            }
        }
        // Compute minimum operator cost
        for (OperatorProxy op : task_proxy.get_operators()) {
            min_operator_cost = std::min(min_operator_cost, op.get_cost());
        }
    }

    const DomainMapping &get_domain_mapping() const {
        return domain_mapping;
    }

    DomainMapping extract_domain_mapping() {
        return std::move(domain_mapping);
    }

    NumericDomainMappingType extract_numeric_domain_mapping() {
        return std::move(numeric_domain_mapping);
    }
    
    const NumericDomainMappingType &get_numeric_domain_mapping() const {
        return numeric_domain_mapping;
    }
    
    NumericDomainMappingType &get_numeric_domain_mapping() {
        return numeric_domain_mapping;
    }

    ap_float get_value(const State &state) const;

    std::vector<std::vector<int>> get_plan() const {
        return wildcard_plan;
    }

    std::vector<std::vector<int>> get_abstract_prop_states() const {
        return abstract_prop_states;
    }

    std::vector<std::vector<int>> get_abstract_numeric_states() const {
        return abstract_numeric_states;
    }


    int size() const {
        return distances.size();
    }
    
    ap_float get_distance_by_index(int index) const {
        if (index >= 0 && index < static_cast<int>(distances.size())) {
            return distances[index];
        }
        return std::numeric_limits<ap_float>::max();
    }
    
    ap_float get_min_operator_cost() const {
        return min_operator_cost;
    }

    void dump(utils::LogProxy &log) const;
};
}

#endif
