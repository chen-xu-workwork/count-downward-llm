#include "domain_abstraction.h"

#include "../utils/logging.h"

using namespace std;

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

int DomainAbstraction::get_value(const vector<int> &state) const {
    return distances[hash_index(state)];
}

}
