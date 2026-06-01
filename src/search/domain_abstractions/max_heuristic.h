#ifndef DOMAIN_ABSTRACTIONS_MAX_HEURISTIC_H
#define DOMAIN_ABSTRACTIONS_MAX_HEURISTIC_H

#include "domain_abstraction_collection_generator.h"

#include "../heuristic.h"

namespace domain_abstractions {
class MaxHeuristic : public Heuristic {
private:
    DomainAbstractionCollection abstractions;

protected:
    virtual ap_float compute_heuristic(const GlobalState &global_state) override;

public:
    explicit MaxHeuristic(const options::Options &opts);
    virtual ~MaxHeuristic() = default;
};
}

#endif
