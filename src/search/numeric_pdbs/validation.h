#ifndef NUMERIC_PDBS_VALIDATION_H
#define NUMERIC_PDBS_VALIDATION_H

#include "types.h"

namespace numeric_pdb_helper {
class NumericTaskProxy;
}

namespace numeric_pdbs {
extern void validate_and_normalize_pattern(
    const numeric_pdb_helper::NumericTaskProxy &task_proxy, Pattern &pattern);
extern void validate_and_normalize_patterns(
    const numeric_pdb_helper::NumericTaskProxy &task_proxy, PatternCollection &patterns);
}

#endif
