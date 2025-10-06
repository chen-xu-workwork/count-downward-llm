#include "domain_abstraction_state_registry.h"

#include <limits>

using namespace std;

namespace domain_abstractions {

size_t DomainAbstractionStateRegistry::insert_state(const DomainAbstractionState &state) {
    state_data_pool.push_back(state);
    size_t id(state_data_pool.size() - 1);
    pair<StateIDSet::iterator, bool> result = registered_states.insert(id);
    bool is_new_entry = result.second;
    if (!is_new_entry) {
        state_data_pool.pop_back();
    }
    assert(registered_states.size() == state_data_pool.size());
    return *result.first;
}

size_t DomainAbstractionStateRegistry::get_id(const DomainAbstractionState &state) {
    // TODO: avoid the push_back + pop_back
    state_data_pool.push_back(state);
    size_t id(state_data_pool.size() - 1);
    pair<StateIDSet::iterator, bool> result = registered_states.insert(id);
    bool is_new_entry = result.second;
    if (is_new_entry) {
        // state was not generated during exploration
        registered_states.erase(id);
        state_data_pool.pop_back();
        assert(registered_states.size() == state_data_pool.size());
        return numeric_limits<size_t>::max();
    }
    state_data_pool.pop_back();
    assert(registered_states.size() == state_data_pool.size());
    return *result.first;
}

}
