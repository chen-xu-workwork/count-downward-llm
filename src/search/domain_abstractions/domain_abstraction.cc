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
    // Build the abstract state from the concrete state
    vector<int> abstract_state(state.size());
    for (size_t i = 0; i < state.size(); ++i) {
        abstract_state[i] = state[i].get_value();
    }
    
    // Compute the hash index for the abstract state
    int index = hash_index(abstract_state);
    
    if (!has_numeric_variables) {
        // Purely propositional case - use direct hash indexing
        // Return the distance value from the distances vector
        return distances[index];
    } else {
        // Mixed propositional and numeric case - use state registry
        // TODO: For numeric planning support, we need to:
        // 1. Extract numeric variable values from the state
        // 2. Create a NumericState with prop_hash and numeric values
        // 3. Look it up in state_registry
        // 4. Return distances[state_id]
        
        // For now, numeric variables are not yet supported in domain abstractions
        // This is a placeholder for future implementation
        cerr << "Numeric variables in domain abstractions not yet implemented" << endl;
        utils::exit_with(utils::ExitCode::UNSUPPORTED);
    }
}

void DomainAbstraction::dump(utils::LogProxy &log) const {
    return;
    //cout << "DomainAbstraction dump not implemented." << std::endl;
}

}

