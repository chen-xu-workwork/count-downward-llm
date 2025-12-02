#include "domain_abstraction.h"

#include "utils.h"
#include "../utils/logging.h"
#include "../utils/system.h"
#include "../numeric_pdbs/numeric_helper.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <iomanip>

using namespace std;
using namespace numeric_pdbs;

namespace domain_abstractions {

// Helper: compute propositional domain sizes from DomainMapping
static std::vector<int> get_domain_sizes_from_mapping(const DomainMapping &domain_mapping) {
    std::vector<int> domain_sizes;
    domain_sizes.reserve(domain_mapping.size());
    for (const auto &map_vec : domain_mapping) {
        if (map_vec.empty()) {
            domain_sizes.push_back(1);
        } else {
            int max_val = 0;
            for (int v : map_vec) {
                if (v > max_val) max_val = v;
            }
            domain_sizes.push_back(max_val + 1);
        }
    }
    return domain_sizes;
}

// Decode an abstract state index into a human-readable string representation.
// This mirrors the implementation in DomainAbstractionFactory::decode_abstract_state
// Includes variable names from TaskProxy for more informative debug output
static std::string decode_abstract_state(int state_index, const std::vector<int> &domain_sizes,
                                         const NumericDomainMappingType &numeric_domain_mapping,
                                         const std::vector<int> &hash_multipliers,
                                         const TaskProxy &task_proxy) {
    std::stringstream ss;
    ss << "State " << state_index << ": [";
    
    VariablesProxy vars = task_proxy.get_variables();
    NumericVariablesProxy num_vars = task_proxy.get_numeric_variables();
    
    bool first = true;
    int remaining = state_index;
    
    // Propositional variables
    for (size_t var_id = 0; var_id < domain_sizes.size(); ++var_id) {
        if (domain_sizes[var_id] <= 1) continue;
        int multiplier = hash_multipliers[var_id];
        int value = (remaining / multiplier) % domain_sizes[var_id];
        
        if (!first) ss << ", ";
        first = false;
        
        // Get variable name if available
        std::string var_name = (var_id < vars.size()) ? vars[var_id].get_name() : "?";
        ss << var_name << "(v" << var_id << ")=" << value;
    }
    
    // Numeric partitions
    for (size_t num_var_id = 0; num_var_id < numeric_domain_mapping.size(); ++num_var_id) {
        if (numeric_domain_mapping[num_var_id]->get_num_partitions() <= 1) continue;
        int multiplier_idx = domain_sizes.size() + num_var_id;
        int multiplier = hash_multipliers[multiplier_idx];
        int num_partitions = numeric_domain_mapping[num_var_id]->get_num_partitions();
        int partition = (remaining / multiplier) % num_partitions;
        
        if (!first) ss << ", ";
        first = false;
        
        // Get numeric variable name if available
        std::string num_var_name = (num_var_id < num_vars.size()) ? num_vars[num_var_id].get_name() : "?";
        
        const NumericRange *rng = numeric_domain_mapping[num_var_id]->get_range_for_partition(partition);
        if (rng) {
            ap_float lower = rng->lower;
            ap_float upper = rng->upper;
            bool lower_incl = rng->lower_inclusive;
            bool upper_incl = rng->upper_inclusive;
            std::string lower_str = lower_incl ? "[" : "(";
            std::string upper_str = upper_incl ? "]" : ")";
            ss << num_var_name << "(num" << num_var_id << ")=" << lower_str << lower << "," << upper << upper_str;
        } else {
            ss << num_var_name << "(num" << num_var_id << ")=INVALID";
        }
    }
    ss << "]";
    return ss.str();
}


int DomainAbstraction::hash_index(const vector<int> &state) const {
    int index = 0;
    for (size_t i = 0; i < state.size(); ++i) {
        if (!domain_mapping[i].empty()) {
            index += hash_multipliers[i] * domain_mapping[i][state[i]];
        }
    }
    return index;
}

ap_float DomainAbstraction::get_value(const State &state) const {
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
        
        assert(state_hash < distances.size() && state_hash >= 0);
        ap_float distance = distances[state_hash];

        if (false) {
            for (size_t i = 0; i < distances.size(); ++i) {
                string dec = decode_abstract_state(i, 
                                            get_domain_sizes_from_mapping(domain_mapping), 
                                            numeric_domain_mapping, 
                                            hash_multipliers,
                                            task_proxy);

                int sum = 0;



                cout << "distance[" << i << "] = " << distances[i] << " -> " << dec << endl;
            }
        }

        bool is_concrete_goal = true;
        for (FactProxy goal : task_proxy.get_goals()) {
            int var_id = goal.get_variable().get_id();
            if (state[var_id].get_value() != goal.get_value()) {
                is_concrete_goal = false;
                break;
            }
        }

        if (!is_concrete_goal) {
            distance = std::max(distance, min_operator_cost);
        }

        return distance;
    }
}

void DomainAbstraction::dump(utils::LogProxy &log) const {
    return;
    //cout << "DomainAbstraction dump not implemented." << std::endl;
}

}

