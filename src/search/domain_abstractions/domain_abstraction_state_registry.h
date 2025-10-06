#ifndef DOMAIN_ABSTRACTIONS_STATE_REGISTRY_H
#define DOMAIN_ABSTRACTIONS_STATE_REGISTRY_H

#include "types.h"
#include "../segmented_vector.h"

#include <unordered_set>

namespace domain_abstractions {

// State registry for domain abstractions with discretized numeric variables
class DomainAbstractionStateRegistry {
    struct StateIDSemanticHash {
        const SegmentedVector<DomainAbstractionState> &state_data_pool;

        explicit StateIDSemanticHash(const SegmentedVector<DomainAbstractionState> &state_data_pool_)
                : state_data_pool(state_data_pool_) {
        }

        std::size_t operator()(std::size_t id) const {
            return DomainAbstractionStateHash{}(state_data_pool[id]);
        }
    };

    struct StateIDSemanticEqual {
        const SegmentedVector<DomainAbstractionState> &state_data_pool;

        explicit StateIDSemanticEqual(const SegmentedVector<DomainAbstractionState> &state_data_pool_)
                : state_data_pool(state_data_pool_) {
        }

        bool operator()(std::size_t lhs, std::size_t rhs) const {
            const DomainAbstractionState &lhs_data = state_data_pool[lhs];
            const DomainAbstractionState &rhs_data = state_data_pool[rhs];
            return lhs_data == rhs_data;
        }
    };

    typedef std::unordered_set<std::size_t,
            StateIDSemanticHash,
            StateIDSemanticEqual> StateIDSet;
    SegmentedVector<DomainAbstractionState> state_data_pool;
    StateIDSet registered_states;

public:
    DomainAbstractionStateRegistry() : registered_states(0,
                                               StateIDSemanticHash(state_data_pool),
                                               StateIDSemanticEqual(state_data_pool)) {}

    // Insert a state and return its ID (creates new ID if not exists)
    std::size_t insert_state(const DomainAbstractionState &state);

    // Get the ID of a state (returns max size_t if not found)
    std::size_t get_id(const DomainAbstractionState &state);

    // Look up a state by ID
    const DomainAbstractionState &lookup_state(std::size_t state_id) const {
        assert(state_id < state_data_pool.size());
        return state_data_pool[state_id];
    }

    // Get the number of registered states
    std::size_t size() const {
        return state_data_pool.size();
    }
};

}

#endif
