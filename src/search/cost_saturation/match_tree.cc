#include "match_tree.h"

#include <cassert>

using namespace std;

namespace cost_saturation {

struct MatchTree::Node {
    static const int LEAF_NODE = -1;
    Node();
    ~Node();
    std::vector<int> applicable_operators;
    int var_id;
    int var_domain_size;
    Node **successors;
    Node *star_successor;

    void initialize(int var_id, int var_domain_size);
    bool is_leaf_node() const;
};

MatchTree::Node::Node()
    : var_id(LEAF_NODE),
      var_domain_size(0),
      successors(nullptr),
      star_successor(nullptr) {
}

MatchTree::Node::~Node() {
    if (successors) {
        for (int i = 0; i < var_domain_size; ++i) {
            delete successors[i];
        }
        delete[] successors;
    }
    delete star_successor;
}

void MatchTree::Node::initialize(int var_id_, int var_domain_size_) {
    assert(is_leaf_node());
    assert(var_id_ >= 0);
    var_id = var_id_;
    var_domain_size = var_domain_size_;
    if (var_domain_size > 0) {
        successors = new Node *[var_domain_size];
        for (int val = 0; val < var_domain_size; ++val) {
            successors[val] = nullptr;
        }
    }
}

bool MatchTree::Node::is_leaf_node() const {
    return var_id == LEAF_NODE;
}

MatchTree::MatchTree(const TaskProxy &task_proxy,
                     const vector<int> &pattern,
                     const vector<int> &hash_multipliers)
    : task_proxy(task_proxy),
      pattern(pattern),
      hash_multipliers(hash_multipliers),
      root(nullptr) {
}

MatchTree::~MatchTree() {
    delete root;
}

void MatchTree::insert_recursive(
    int op_id, const vector<Fact> &preconditions, int pre_index, Node **edge_from_parent) {
    if (*edge_from_parent == 0) {
        *edge_from_parent = new Node();
    }

    Node *node = *edge_from_parent;
    if (pre_index == static_cast<int>(preconditions.size())) {
        node->applicable_operators.push_back(op_id);
    } else {
        const Fact &fact = preconditions[pre_index];
        int pattern_var_id = fact.var;
        int var_id = pattern[pattern_var_id];
        VariableProxy var = task_proxy.get_variables()[var_id];
        int var_domain_size = var.get_domain_size();

        if (node->is_leaf_node()) {
            node->initialize(pattern_var_id, var_domain_size);
        } else if (node->var_id > pattern_var_id) {
            Node *new_node = new Node();
            new_node->initialize(pattern_var_id, var_domain_size);
            *edge_from_parent = new_node;
            new_node->star_successor = node;
            node = new_node;
        }

        Node **edge_to_child = 0;
        if (node->var_id == pattern_var_id) {
            edge_to_child = &node->successors[fact.value];
            ++pre_index;
        } else {
            assert(node->var_id < pattern_var_id);
            edge_to_child = &node->star_successor;
        }

        insert_recursive(op_id, preconditions, pre_index, edge_to_child);
    }
}

void MatchTree::insert(int op_id, const vector<Fact> &preconditions) {
    insert_recursive(op_id, preconditions, 0, &root);
}

void MatchTree::get_applicable_operator_ids_recursive(
    Node *node, int state_index,
    vector<int> &applicable_operators) const {

    applicable_operators.insert(applicable_operators.end(),
                                node->applicable_operators.begin(),
                                node->applicable_operators.end());

    if (node->is_leaf_node())
        return;

    int temp = state_index / hash_multipliers[node->var_id];
    int val = temp % node->var_domain_size;

    if (node->successors[val]) {
        get_applicable_operator_ids_recursive(node->successors[val], state_index,
                                           applicable_operators);
    }
    if (node->star_successor) {
        get_applicable_operator_ids_recursive(node->star_successor, state_index,
                                           applicable_operators);
    }
}

void MatchTree::get_applicable_operator_ids(
    int state_index,
    vector<int> &applicable_operators) const {
    if (root)
        get_applicable_operator_ids_recursive(root, state_index,
                                           applicable_operators);
}

}
