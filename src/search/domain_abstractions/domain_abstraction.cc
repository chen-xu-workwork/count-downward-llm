#include "domain_abstraction.h"

#include "../utils/logging.h"
#include "../utils/system.h"
#include "../numeric_pdbs/numeric_helper.h"

using namespace std;
using namespace numeric_pdbs;

namespace domain_abstractions {

int DomainAbstraction::hash_index(const vector<int> &state) const {
    int index = 0;
    for (size_t i = 0; i < state.size(); ++i) {
        if (!domain_mapping[i].empty()) {
            index += hash_multipliers[i] * domain_mapping[i][state[i]];
        }
    }
    return index;
}

int DomainAbstraction::get_value(const State &state) const {
    if (!has_numeric_variables) {
        // Purely propositional case - use direct hash indexing
        // Build the abstract state from the concrete state (only propositional variables)
        vector<int> abstract_state(state.size());
        for (size_t i = 0; i < state.size(); ++i) {
            abstract_state[i] = state[i].get_value();
        }
        
        // Compute the hash index for the abstract state
        int index = hash_index(abstract_state);
        
        // Return the distance value from the distances vector
        return distances[index];
    } else {
        // Mixed propositional and numeric case - use state registry
        
        // Compute combined hash for both propositional and numeric variables
        size_t state_hash = 0;
        
        // 1. Add propositional variables to hash
        // domain_mapping[i].empty() means variable i is not in the abstraction
        for (size_t i = 0; i < domain_mapping.size(); ++i) {
            if (!domain_mapping[i].empty()) {
                int val = state[i].get_value();
                int abstract_val = domain_mapping[i][val];
                state_hash += hash_multipliers[i] * abstract_val;
            }
        }
        
        // 2. Add numeric variables to hash
        // numeric_domain_mapping[i] corresponds to numeric variable i
        for (size_t i = 0; i < numeric_domain_mapping.size(); ++i) {
            ap_float value = state.nval(i);
            int partition = numeric_domain_mapping[i].get_partition_index(value);
            // Add partition to hash (using a simple multiplication like propositional vars)
            // Note: We need appropriate hash multipliers for numeric variables too
            state_hash += hash_multipliers[domain_mapping.size() + i] * partition;
        }
        
        // 3. Create DomainAbstractionState and look it up in state registry
        DomainAbstractionState abs_state(state_hash);
        size_t state_id = state_registry->get_id(abs_state);
        
        // 4. Return the distance if the state was found
        if (state_id == numeric_limits<size_t>::max()) {
            // State not found in registry - this shouldn't happen if the abstraction
            // was properly built, but we'll return infinity as a safe fallback
            return numeric_limits<int>::max();
        }
        
        return distances[state_id];
    }
}

void DomainAbstraction::dump(utils::LogProxy &log) const {
    return;
    //cout << "DomainAbstraction dump not implemented." << std::endl;
}

}

