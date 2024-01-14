#ifndef NUMERIC_PDBS_TYPES_H
#define NUMERIC_PDBS_TYPES_H

#include <memory>
#include <vector>
#include "../task_proxy.h" // required for the implementation of hash<std::vector<int>>

namespace numeric_pdbs {
class PatternDatabase;

struct Pattern {
    std::vector<int> regular;
    std::vector<int> numeric;

    bool operator==(const Pattern &other) const {
        return regular == other.regular && numeric == other.numeric;
    }

    bool operator<(const Pattern &other) const {
        return regular < other.regular || (regular == other.regular && numeric < other.numeric);
    }

    bool operator!=(const Pattern &other) const {
        return !(*this == other);
    }

    bool operator>(const Pattern &other) const {
        return other < *this;
    }

    bool operator<=(const Pattern &other) const {
        return !(other < *this);
    }

    bool operator>=(const Pattern &other) const {
        return !(*this < other);
    }
};

using PatternCollection = std::vector<Pattern>;
using PDBCollection = std::vector<std::shared_ptr<PatternDatabase>>;
using MaxAdditivePDBSubsets = std::vector<PDBCollection>;
}

namespace std {
    template <>
    struct hash<numeric_pdbs::Pattern> {
        size_t operator()(const numeric_pdbs::Pattern& pattern) const {
            size_t h1 = std::hash<std::vector<int>>()(pattern.regular);
            size_t h2 = std::hash<std::vector<int>>()(pattern.numeric);
            return h1 ^ h2;
        }
    };
}

#endif
