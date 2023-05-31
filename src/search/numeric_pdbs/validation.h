#ifndef NUMERIC_PDBS_VALIDATION_H
#define NUMERIC_PDBS_VALIDATION_H

#include "types.h"

class TaskProxy;

namespace numeric_pdbs {
extern void validate_and_normalize_pattern(
    const TaskProxy &task_proxy, Pattern &pattern);
extern void validate_and_normalize_patterns(
    const TaskProxy &task_proxy, PatternCollection &patterns);
}

#endif
