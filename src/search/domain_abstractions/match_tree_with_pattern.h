#ifndef PDBS_MATCH_TREE_WITH_PATTERN_H
#define PDBS_MATCH_TREE_WITH_PATTERN_H

#include "../abstract_task.h"

#include <vector>
#include <iostream>

namespace domain_abstractions {
/*
  Successor Generator for abstract operators.

  NOTE: MatchTree keeps a reference to the task proxy passed to the constructor.
  Therefore, users of the class must ensure that the task lives at least as long
  as the match tree.
*/

class MatchTreeWithPattern {
    struct Node;
    const std::vector<int> &domain_sizes;
    const std::vector<int> &hash_multipliers;
    Node *root;
    void insert_recursive(int op_id,
                          const std::vector<Fact> &regression_preconditions,
                          int pre_index,
                          Node **edge_from_parent);
    void get_applicable_operator_ids_recursive(
        Node *node, int state_index, std::vector<int> &operator_ids) const;
    void dump_recursive(Node *node) const;
public:
    // Initialize an empty match tree.
    MatchTreeWithPattern(
        const std::vector<int> &domain_sizes,
        const std::vector<int> &hash_multipliers);
    ~MatchTreeWithPattern();
    /* Insert an abstract operator into the match tree, creating or
       enlarging it. */
    void insert(int op_id, const std::vector<Fact> &regression_preconditions);

    /*
      Extracts all IDs of applicable abstract operators for the abstract state
      given by state_index (the index is converted back to variable/values
      pairs).
    */
    void get_applicable_operator_ids(
        int state_index, std::vector<int> &operator_ids) const;
    void dump() const;
};
}

#endif
