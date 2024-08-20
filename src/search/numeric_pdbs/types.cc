#include "types.h"

#include "../utils/hash.h"
#include "../utils/logging.h"

namespace std {

size_t hash<numeric_pdbs::Pattern>::operator()(const numeric_pdbs::Pattern& pattern) const {
    size_t h1 = std::hash<std::vector<int>>()(pattern.regular);
    size_t h2 = std::hash<std::vector<int>>()(pattern.numeric);
    return h1 ^ h2;
}

ostream &operator<<(ostream &stream, const numeric_pdbs::Pattern &pattern) {
    stream << pattern.regular << pattern.numeric;
    return stream;
}
}