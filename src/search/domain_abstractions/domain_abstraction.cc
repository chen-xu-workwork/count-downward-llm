#include "domain_abstraction.h"

#include "utils.h"
#include "../utils/logging.h"
#include "../utils/system.h"
#include "../numeric_pdbs/numeric_helper.h"

#include <algorithm>
#include <limits>

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
        assert(state_registry);
        
        // Compute the abstract state hash using the utility function that includes
        // full cascade evaluation of derived numeric variables and comparison axioms
        size_t state_hash = compute_abstract_state_hash(
            state, task_proxy, domain_mapping, 
            numeric_domain_mapping, hash_multipliers);

        //DEBUG state and numeric state
        cout << "DEBUG DomainAbstraction::get_value: state_hash=" << state_hash << "\n";
        
        
        // Create DomainAbstractionState and look it up in state registry
        DomainAbstractionState abs_state(state_hash);
        size_t state_id = state_registry->get_id(abs_state);
        
        // Return the distance if the state was found
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

