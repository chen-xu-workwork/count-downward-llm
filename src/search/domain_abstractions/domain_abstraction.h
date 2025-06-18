#ifndef DOMAIN_ABSTRACTION_DOMAIN_ABSTRACTION_H
#define DOMAIN_ABSTRACTION_DOMAIN_ABSTRACTION_H

#include "types.h"


#include <vector>

namespace domain_abstractions {
class DomainAbstraction {
    DomainMapping domain_mapping;
    std::vector<int> hash_multipliers;
    std::vector<int> distances;
    // TODO: get rid of this here and return it from the factory optionally.
    std::vector<std::vector<int>> wildcard_plan;

    int hash_index(const std::vector<int> &state) const;

public:
    DomainAbstraction(DomainMapping &&domain_mapping,
                      std::vector<int> &&hash_multipliers,
                      std::vector<int> &&distances,
                      std::vector<std::vector<int>> &&wildcard_plan)
        : domain_mapping(std::move(domain_mapping)),
          hash_multipliers(std::move(hash_multipliers)),
          distances(std::move(distances)),
          wildcard_plan(std::move(wildcard_plan)) {
    }

    const DomainMapping &get_domain_mapping() const {
        return domain_mapping;
    }

    const DomainMapping &&extract_domain_mapping() {
        return std::move(domain_mapping);
    }

    int get_value(const std::vector<int> &state) const;

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
