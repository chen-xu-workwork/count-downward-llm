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

        if (false) {
                // DEBUG: Print non-trivial propositional state values
            cout << "DEBUG DomainAbstraction::get_value:\n";
            cout << "  Propositional state (non-trivial only):\n";
            VariablesProxy vars = task_proxy.get_variables();
            for (size_t i = 0; i < state.size(); ++i) {
                // Only print if this variable is part of the abstraction (non-trivial)
                if (!domain_mapping[i].empty()) {
                    int concrete_val = state[i].get_value();
                    int abstract_val = domain_mapping[i][concrete_val];
                    cout << "    fdr_" << i << " (" << vars[i].get_name() << ") = " 
                        << concrete_val << " (abstract: " << abstract_val << ")\n";
                }
            }
        }
        
        
        // DEBUG: Print non-trivial numeric state values
        
        if (false) {
            NumericVariablesProxy num_vars = task_proxy.get_numeric_variables();
            cout << "  Numeric state (non-trivial only):\n";
            for (size_t i = 0; i < num_vars.size(); ++i) {
                // Only print if this numeric variable is part of the abstraction (non-trivial)
                // A variable is non-trivial if it has a mapping and has been split (> 1 partition)
                if (numeric_domain_mapping[i] && 
                    numeric_domain_mapping[i]->get_num_partitions() > 1) {
                    ap_float concrete_val = state.nval(i);
                    int partition_idx = numeric_domain_mapping[i]->get_partition_index(concrete_val);
                    
                    // Find the range that contains this value
                    const auto &ranges = numeric_domain_mapping[i]->get_ranges();
                    for (const auto &range : ranges) {
                        if (range.contains(concrete_val)) {
                            cout << "    num_" << i << " (" << num_vars[i].get_name() << ") = " 
                                << concrete_val << " (abstract: partition " << partition_idx 
                                << ", range: " 
                                << (range.lower_inclusive ? "[" : "(") << range.lower 
                                << ", " << range.upper << (range.upper_inclusive ? "]" : ")") 
                                << ")\n";
                            break;
                        }
                    }
                }
            }
            cout << "  State hash: " << state_hash << "\n";
        }
        
        
        // Create DomainAbstractionState and look it up in state registry
        DomainAbstractionState abs_state(state_hash);
        size_t state_id = state_registry->get_id(abs_state);
        
        // Return the distance if the state was found
        if (state_id == numeric_limits<size_t>::max()) {
            // State not found in registry - this shouldn't happen if the abstraction
            // was properly built, but we'll return infinity as a safe fallback
            cout << "  State NOT FOUND in registry! Returning infinity.\n";
            return numeric_limits<int>::max();
        }
        
        int distance = distances[state_id];
        
        return distance;
    }
}

void DomainAbstraction::dump(utils::LogProxy &log) const {
    return;
    //cout << "DomainAbstraction dump not implemented." << std::endl;
}

}

