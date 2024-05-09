#ifndef NUMERIC_PDBS_VARIABLE_ORDER_FINDER_H
#define NUMERIC_PDBS_VARIABLE_ORDER_FINDER_H

#include "../task_proxy.h"
#include "numeric_helper.h"

#include <memory>
#include <vector>

namespace numeric_pdbs {
enum VariableOrderType {
    CG_GOAL_LEVEL,
    CG_GOAL_RANDOM,
    GOAL_CG_LEVEL,
    RANDOM,
    LEVEL,
    REVERSE_LEVEL
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
    bool is_pdb;

    void select_next(int position, int var_no, bool is_numeric);

public:
    VariableOrderFinder(const std::shared_ptr<AbstractTask> task,
                        std::shared_ptr<numeric_pdb_helper::NumericTaskProxy> num_proxy,
                        VariableOrderType variable_order_type, bool is_pdb = false);

    bool done() const;

    std::pair<int, bool> next();

    void dump() const;
};
}
#endif
