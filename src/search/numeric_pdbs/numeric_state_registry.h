#ifndef NUMERIC_PDBS_NUMERIC_STATE_REGISTRY
#define NUMERIC_PDBS_NUMERIC_STATE_REGISTRY

#include "types.h"

#include "../globals.h"
#include "../segmented_vector.h"
#include "../task_proxy.h"

#include <unordered_set>

namespace numeric_pdbs {

struct NumericState {
    std::size_t prop_hash;
    std::vector<ap_float> num_state;

    NumericState(std::size_t prop_hash,
                 std::vector <ap_float> num_state) : prop_hash(prop_hash),
                                                num_state(std::move(num_state)) {}

    std::string get_name(const TaskProxy &proxy, const Pattern &pattern) const;

    bool operator==(const NumericState &other) const {
        return prop_hash == other.prop_hash && num_state == other.num_state;
    }
};

struct NumericStateHash {
    std::size_t operator()(const NumericState &s) const {
        std::hash <ap_float> hasher;
        std::size_t seed = s.prop_hash;
        for (ap_float i: s.num_state) {
            seed ^= hasher(i) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        }
        return seed;
    }
};

class NumericStateRegistry {
    struct StateIDSemanticHash {
        const SegmentedVector <NumericState> &state_data_pool;

        explicit StateIDSemanticHash(const SegmentedVector <NumericState> &state_data_pool_)
                : state_data_pool(state_data_pool_) {
        }

        std::size_t operator()(std::size_t id) const {
            return NumericStateHash{}(state_data_pool[id]);
        }
    };

    struct StateIDSemanticEqual {
        const SegmentedVector <NumericState> &state_data_pool;

        explicit StateIDSemanticEqual(const SegmentedVector <NumericState> &state_data_pool_)
                : state_data_pool(state_data_pool_) {
        }

        bool operator()(std::size_t lhs, std::size_t rhs) const {
            const NumericState &lhs_data = state_data_pool[lhs];
            const NumericState &rhs_data = state_data_pool[rhs];
            return lhs_data == rhs_data;
        }
    };

    typedef std::unordered_set<std::size_t,
            StateIDSemanticHash,
            StateIDSemanticEqual> StateIDSet;
    SegmentedVector <NumericState> state_data_pool;
    StateIDSet registered_states;

public:
    NumericStateRegistry() : registered_states(0,
                                               StateIDSemanticHash(state_data_pool),
                                               StateIDSemanticEqual(state_data_pool)) {}

    std::size_t insert_state(const NumericState &state);

    std::size_t get_id(const NumericState &state);

    const NumericState &lookup_state(std::size_t state_id) const {
        assert(state_id < state_data_pool.size());
        return state_data_pool[state_id];
    }

    std::size_t size() const {
        return state_data_pool.size();
    }
};
}
#endif