#include "causal_graph.h"

#include "../global_operator.h"
#include "../globals.h"
#include "../task_proxy.h"

#include "../utils/logging.h"
#include "../utils/timer.h"
#include "numeric_condition.h"
#include "numeric_helper.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using namespace std;

/*
  We only want to create one causal graph per task, so they are cached globally.

  TODO: We need to rethink the memory management here. Objects in this cache are
  never reclaimed (before termination of the program). Also, currently every
  heuristic that uses one would receive its own causal graph object even if it
  uses an unmodified task because it will create its own copy of
  CostAdaptedTask.
  We have the same problems for other objects that are associated with tasks
  (causal graphs, successor generators and axiom evlauators, DTGs, ...) and can
  maybe deal with all of them in the same way.
*/
static unordered_map<const AbstractTask *, unique_ptr<numeric_pdbs::CausalGraph>> causal_graph_cache;

namespace numeric_pdbs {
/*
  An IntRelationBuilder constructs an IntRelation by adding one pair
  to the relation at a time. Duplicates are automatically handled
  (i.e., it is OK to add the same pair twice), and the pairs need not
  be added in any specific sorted order.

  Define the following parameters:
  - K: range of the IntRelation (i.e., allowed values {0, ..., K - 1})
  - M: number of pairs added to the relation (including duplicates)
  - N: number of unique pairs in the final relation
  - D: maximal number of unique elements (x, y) in the relation for given x

  Then we get:
  - O(K + N) memory usage during construction and for final IntRelation
  - O(K + M + N log D) construction time
*/

class IntRelationBuilder {
    vector<unordered_set<int>> int_sets;

    int get_range() const;

public:
    explicit IntRelationBuilder(int range);

    void add_pair(int u, int v);

    void compute_relation(IntRelation &result) const;
};

IntRelationBuilder::IntRelationBuilder(int range)
        : int_sets(range) {
}

int IntRelationBuilder::get_range() const {
    return int_sets.size();
}

void IntRelationBuilder::add_pair(int u, int v) {
    assert(u >= 0 && u < get_range());
    assert(v >= 0 && v < get_range());
    int_sets[u].insert(v);
}

void IntRelationBuilder::compute_relation(IntRelation &result) const {
    int range = get_range();
    result.clear();
    result.resize(range);
    for (int i = 0; i < range; ++i) {
        result[i].assign(int_sets[i].begin(), int_sets[i].end());
        sort(result[i].begin(), result[i].end());
    }
}


struct CausalGraphBuilder {
    IntRelationBuilder pre_eff_builder;
    IntRelationBuilder eff_pre_builder;
    IntRelationBuilder eff_eff_builder;

    IntRelationBuilder succ_builder;
    IntRelationBuilder pred_builder;

    shared_ptr<numeric_pdb_helper::NumericTaskProxy> num_task;

    // map relevant variables to new contiguous ID range
    vector<int> prop_var_id_to_glob_var_id;
    vector<int> num_var_id_to_glob_var_id;

    explicit CausalGraphBuilder(int prop_var_count, int num_var_count,
                                const vector<int> &prop_var_id_to_glob_var_id,
                                const vector<int> &num_var_id_to_glob_var_id,
                                shared_ptr<numeric_pdb_helper::NumericTaskProxy> num_task)
            : pre_eff_builder(prop_var_count+num_var_count),
              eff_pre_builder(prop_var_count+num_var_count),
              eff_eff_builder(prop_var_count+num_var_count),
              succ_builder(prop_var_count+num_var_count),
              pred_builder(prop_var_count+num_var_count),
              num_task(std::move(num_task)),
              prop_var_id_to_glob_var_id(prop_var_id_to_glob_var_id),
              num_var_id_to_glob_var_id(num_var_id_to_glob_var_id) {
    }

    void handle_pre_eff_arc(int u, int v) {
        assert(u != v);
        pre_eff_builder.add_pair(u, v);
        succ_builder.add_pair(u, v);
        eff_pre_builder.add_pair(v, u);
        pred_builder.add_pair(v, u);
    }

    void handle_eff_eff_edge(int u, int v) {
        assert(u != v);
        eff_eff_builder.add_pair(u, v);
        eff_eff_builder.add_pair(v, u);
        succ_builder.add_pair(u, v);
        succ_builder.add_pair(v, u);
        pred_builder.add_pair(u, v);
        pred_builder.add_pair(v, u);
    }

    void handle_operator(const OperatorProxy &op) {
        EffectsProxy effects = op.get_effects();

        // Handle pre->eff links from preconditions.
        for (FactProxy pre: op.get_preconditions()) {
            int pre_var_id;
            if (prop_var_id_to_glob_var_id[pre.get_variable().get_id()] == -1) {
                pre_var_id = num_var_id_to_glob_var_id[num_task->get_regular_numeric_condition(pre)->get_var_id()];
            } else {
                pre_var_id = prop_var_id_to_glob_var_id[pre.get_variable().get_id()];
            }
            for (EffectProxy eff: effects) {
                if (!eff.get_conditions().empty()) {
                    cerr << "ERROR: conditional effects not supported in numeric causal graph" << endl;
                    utils::exit_with(utils::ExitCode::UNSUPPORTED);
                }
                int eff_var_id = prop_var_id_to_glob_var_id[eff.get_fact().get_variable().get_id()];
                if (pre_var_id != eff_var_id)
                    handle_pre_eff_arc(pre_var_id, eff_var_id);
            }
            for (auto eff: op.get_ass_effects()) {
                NumericVariableProxy eff_var = eff.get_assignment().get_affected_variable();
                int eff_var_id = num_var_id_to_glob_var_id[eff_var.get_id()];
                if (eff_var.get_var_type() == regular && eff_var_id != pre_var_id) {
                    handle_pre_eff_arc(pre_var_id, eff_var_id);
                }
            }
        }

        // Handle eff->eff links.
        for (size_t i = 0; i < effects.size(); ++i) {
            int eff1_var_id = prop_var_id_to_glob_var_id[effects[i].get_fact().get_variable().get_id()];
            for (size_t j = i + 1; j < effects.size(); ++j) {
                int eff2_var_id = prop_var_id_to_glob_var_id[effects[j].get_fact().get_variable().get_id()];
                if (eff1_var_id != eff2_var_id) {
                    handle_eff_eff_edge(eff1_var_id, eff2_var_id);
                }
            }
            for (auto eff: op.get_ass_effects()) {
                NumericVariableProxy eff_var = eff.get_assignment().get_affected_variable();
                if (eff_var.get_var_type() == regular) {
                    handle_eff_eff_edge(eff1_var_id, num_var_id_to_glob_var_id[eff_var.get_id()]);
                }
            }
        }

        for (auto eff1: op.get_ass_effects()) {
            NumericVariableProxy eff1_var = eff1.get_assignment().get_affected_variable();
            if (eff1_var.get_var_type() == regular) {
                for (auto eff2: op.get_ass_effects()) {
                    NumericVariableProxy eff2_var = eff2.get_assignment().get_affected_variable();
                    if (eff2_var.get_var_type() == regular && eff1_var != eff2_var) {
                        handle_eff_eff_edge(num_var_id_to_glob_var_id[eff1_var.get_id()],
                                            num_var_id_to_glob_var_id[eff2_var.get_id()]);
                    }
                }
            }
        }
    }
};

CausalGraph::CausalGraph(const TaskProxy &task_proxy,
                         const shared_ptr<numeric_pdb_helper::NumericTaskProxy> &num_task) {
    utils::Timer timer;
    cout << "building causal graph..." << flush;
    int num_prop_variables = 0;

    for (auto var: task_proxy.get_variables()) {
        if (!num_task->is_numeric_variable(var) && !task_proxy.is_derived_variable(var)) {
            ++num_prop_variables;
        }
    }

    int num_regular_num_vars = 0;
    for (auto var: task_proxy.get_numeric_variables()) {
        if (var.get_var_type() == numType::regular) {
            ++num_regular_num_vars;
        }
    }

    // compute variable id mappings
    prop_var_id_to_glob_var_id = vector<int>(task_proxy.get_variables().size(), -1);
    num_var_id_to_glob_var_id = vector<int>(task_proxy.get_numeric_variables().size(), -1);
    glob_var_id_to_var_id = vector<int>(num_prop_variables + num_regular_num_vars, -1);
    first_num_var_index = num_prop_variables;
    int i = 0;
    for (auto var: task_proxy.get_variables()) {
        if (!num_task->is_numeric_variable(var) && !task_proxy.is_derived_variable(var)) {
            glob_var_id_to_var_id[i] = var.get_id();
            prop_var_id_to_glob_var_id[var.get_id()] = i++;
        }
    }
    for (auto num_var: task_proxy.get_numeric_variables()) {
        if (num_var.get_var_type() == regular) {
            glob_var_id_to_var_id[i] = num_var.get_id();
            num_var_id_to_glob_var_id[num_var.get_id()] = i++;
        }
    }

    CausalGraphBuilder cg_builder(num_prop_variables,
                                  num_regular_num_vars,
                                  prop_var_id_to_glob_var_id,
                                  num_var_id_to_glob_var_id,
                                  num_task);

    for (OperatorProxy op: task_proxy.get_operators())
        cg_builder.handle_operator(op);

    cg_builder.pre_eff_builder.compute_relation(pre_to_eff);
    cg_builder.eff_pre_builder.compute_relation(eff_to_pre);
    cg_builder.eff_eff_builder.compute_relation(eff_to_eff);

    cg_builder.pred_builder.compute_relation(predecessors);
    cg_builder.succ_builder.compute_relation(successors);

//    dump(task_proxy);
    cout << "done! [t=" << timer << "]" << endl;
}

void CausalGraph::dump(const TaskProxy &task_proxy) const {
    cout << "Causal graph: " << endl;
    for (VariableProxy var: task_proxy.get_variables()) {
        int var_id = var.get_id();
        int mapped_var_id = prop_var_id_to_glob_var_id[var_id];
        if (mapped_var_id != -1) {
            cout << "#" << var_id << " [" << var.get_name() << "]:" << endl
                 << "    pre->eff arcs: " << pre_to_eff[mapped_var_id] << endl
                 << "    eff->pre arcs: " << eff_to_pre[mapped_var_id] << endl
                 << "    eff->eff arcs: " << eff_to_eff[mapped_var_id] << endl
                 << "    successors: " << successors[mapped_var_id] << endl
                 << "    predecessors: " << predecessors[mapped_var_id] << endl;
        }
    }
    for (auto var: task_proxy.get_numeric_variables()) {
        int var_id = var.get_id();
        int mapped_var_id = num_var_id_to_glob_var_id[var_id];
        if (mapped_var_id != -1) {
            cout << "#" << var_id << " [" << var.get_name() << "]:" << endl
                 << "    pre->eff arcs: " << pre_to_eff[mapped_var_id] << endl
                 << "    eff->pre arcs: " << eff_to_pre[mapped_var_id] << endl
                 << "    eff->eff arcs: " << eff_to_eff[mapped_var_id] << endl
                 << "    successors: " << successors[mapped_var_id] << endl
                 << "    predecessors: " << predecessors[mapped_var_id] << endl;
        }
    }
}

vector<int> CausalGraph::get_prop_eff_to_prop_pre(int prop_var) const {
    int mapped_var = prop_var_id_to_glob_var_id[prop_var];
    if (mapped_var == -1 || first_num_var_index == 0){
        return {};
    } else {
        vector<int> prop_pre;
        for (int var : eff_to_pre[mapped_var]){
            if (var < first_num_var_index) {
                prop_pre.push_back(glob_var_id_to_var_id[var]);
            }
        }
        return prop_pre;
    }
}

vector<int> CausalGraph::get_prop_eff_to_num_pre(int prop_var) const {
    assert(first_num_var_index >= 0);
    int mapped_var = prop_var_id_to_glob_var_id[prop_var];
    if (mapped_var == -1 || static_cast<size_t>(first_num_var_index) == glob_var_id_to_var_id.size()){
        return {};
    } else {
        vector<int> num_pre;
        for (int var : eff_to_pre[mapped_var]){
            if (var >= first_num_var_index) {
                num_pre.push_back(glob_var_id_to_var_id[var]);
            }
        }
        return num_pre;
    }
}

vector<int> CausalGraph::get_num_eff_to_prop_pre(int num_var) const {
    int mapped_var = num_var_id_to_glob_var_id[num_var];
    if (mapped_var == -1 || first_num_var_index == 0){
        return {};
    } else {
        vector<int> prop_pre;
        for (int var : eff_to_pre[mapped_var]){
            if (var < first_num_var_index) {
                prop_pre.push_back(glob_var_id_to_var_id[var]);
            }
        }
        return prop_pre;
    }
}

vector<int> CausalGraph::get_num_eff_to_num_pre(int num_var) const {
    assert(first_num_var_index >= 0);
    int mapped_var = num_var_id_to_glob_var_id[num_var];
    if (mapped_var == -1 || static_cast<size_t>(first_num_var_index) == glob_var_id_to_var_id.size()){
        return {};
    } else {
        vector<int> num_pre;
        for (int var : eff_to_pre[mapped_var]){
            if (var >= first_num_var_index) {
                num_pre.push_back(glob_var_id_to_var_id[var]);
            }
        }
        return num_pre;
    }
}
}
const numeric_pdbs::CausalGraph &get_numeric_causal_graph(const AbstractTask *task) {
    if (causal_graph_cache.count(task) == 0) {
        TaskProxy task_proxy(*task);
        shared_ptr<numeric_pdb_helper::NumericTaskProxy> num_proxy = make_shared<numeric_pdb_helper::NumericTaskProxy>(task_proxy);
        unique_ptr<numeric_pdbs::CausalGraph> cg(new numeric_pdbs::CausalGraph(task_proxy, num_proxy));
        causal_graph_cache.insert(make_pair(task, std::move(cg)));
    }
    return *causal_graph_cache[task];
}