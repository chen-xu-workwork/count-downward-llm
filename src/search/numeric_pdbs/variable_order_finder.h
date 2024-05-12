#ifndef NUMERIC_PDBS_VARIABLE_ORDER_FINDER_H
#define NUMERIC_PDBS_VARIABLE_ORDER_FINDER_H

#include "../abstract_task.h"

#include <memory>
#include <vector>

namespace numeric_pdb_helper {
class NumericTaskProxy;
}

namespace numeric_pdbs {
enum VariableOrderType {
    CG_GOAL_LEVEL,
    CG_GOAL_RANDOM,
    GOAL_CG_LEVEL,
//    RANDOM,
//    LEVEL,
//    REVERSE_LEVEL
};

class VariableOrderFinder {
    const std::shared_ptr<AbstractTask> task;

    const VariableOrderType variable_order_type;

    std::vector<int> selected_vars;
    // first is variable id, second is true if variable is numeric
    std::vector<std::pair<int, bool>> remaining_vars;
    std::vector<bool> is_goal_variable;
    std::vector<bool> is_numeric_goal_variable;
    std::vector<bool> is_causal_predecessor;

    void select_next(size_t position, int var_no, bool is_numeric);

public:
    VariableOrderFinder(std::shared_ptr<AbstractTask> task,
                        const std::shared_ptr<numeric_pdb_helper::NumericTaskProxy> &num_proxy,
                        VariableOrderType variable_order_type,
                        bool numeric_variables_first,
                        const std::shared_ptr<utils::RandomNumberGenerator> &rng);

    bool done() const;

    std::pair<int, bool> next();

    void dump() const;
};
}
#endif
