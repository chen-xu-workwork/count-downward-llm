#ifndef NUMERIC_PDBS_TYPES_H
#define NUMERIC_PDBS_TYPES_H

#include <memory>
#include <vector>

namespace numeric_pdbs {
class PatternDatabase;
using Pattern = std::vector<int>;
using PatternCollection = std::vector<Pattern>;
using PDBCollection = std::vector<std::shared_ptr<PatternDatabase>>;
using MaxAdditivePDBSubsets = std::vector<PDBCollection>;
}

#endif
