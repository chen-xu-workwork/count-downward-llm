#ifndef COST_SATURATION_MATCH_TREE_H
#define COST_SATURATION_MATCH_TREE_H

#include "../task_proxy.h"
#include "../abstract_task.h"

#include <vector>

namespace cost_saturation {

class MatchTree {
    TaskProxy task_proxy;
    struct Node;
    std::vector<int> pattern;
    std::vector<int> hash_multipliers;
    Node *root;

    void insert_recursive(int op_id,
                          const std::vector<Fact> &preconditions,
                          int pre_index,
                          Node **edge_from_parent);
    void get_applicable_operator_ids_recursive(
        Node *node, int state_index,
        std::vector<int> &applicable_operators) const;

public:
    MatchTree(const TaskProxy &task_proxy,
              const std::vector<int> &pattern,
              const std::vector<int> &hash_multipliers);
    ~MatchTree();

    void insert(int op_id, const std::vector<Fact> &preconditions);

    void get_applicable_operator_ids(
        int state_index,
        std::vector<int> &applicable_operators) const;
};

}

#endif
