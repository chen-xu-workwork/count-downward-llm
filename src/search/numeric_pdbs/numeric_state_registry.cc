#include "numeric_state_registry.h"

#include "numeric_helper.h"

#include <sstream>

using namespace std;
using numeric_pdb_helper::NumericTaskProxy;

namespace numeric_pdbs {
string NumericState::get_name(const NumericTaskProxy &proxy, const Pattern &pattern) const {
    stringstream ss;
    ss << "NumericState {" << endl;
    ss << "\tprop: " << prop_hash << endl;
    ss << "\tnum: ";
    int i = 0;
    for (int var: pattern.numeric) {
        ss << proxy.get_numeric_variables()[var].get_name() << " = " << num_state[i++] << "; ";
    }
    ss << endl << "}" << endl;
    return ss.str();
}

size_t NumericStateRegistry::insert_state(const NumericState &state) {
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

size_t NumericStateRegistry::get_id(const NumericState &state) {
    // TODO avoid the push_back + pop_back
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