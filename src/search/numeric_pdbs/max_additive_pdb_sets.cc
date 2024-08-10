#include "max_additive_pdb_sets.h"

#include "max_cliques.h"
#include "numeric_helper.h"
#include "numeric_task_proxy.h"
#include "pattern_database.h"

using namespace std;
using namespace numeric_pdb_helper;

namespace numeric_pdbs {
bool are_patterns_additive(const Pattern &pattern1,
                           const Pattern &pattern2,
                           const NumericVariableAdditivity &are_additive) {
    for (int v1 : pattern1.regular) {
        for (int v2 : pattern2.regular) {
            if (!are_additive.prop_to_prop[v1][v2]) {
                return false;
            }
        }
        for (int v2 : pattern2.numeric) {
            if (!are_additive.prop_to_num[v1][v2]) {
                return false;
            }
        }
    }
    for (int v1 : pattern1.numeric) {
        for (int v2 : pattern2.numeric) {
            if (!are_additive.num_to_num[v1][v2]) {
                return false;
            }
        }
        for (int v2 : pattern2.regular) {
            if (!are_additive.num_to_prop[v1][v2]) {
                return false;
            }
        }
    }
    return true;
}

NumericVariableAdditivity compute_additive_vars(const NumericTaskProxy &task_proxy) {
    NumericVariableAdditivity are_additive;
    size_t num_prop_vars = task_proxy.get_variables().size();
    size_t num_num_vars = task_proxy.get_numeric_variables().size();
    are_additive.prop_to_prop.resize(num_prop_vars, vector<bool>(num_prop_vars, true));
    are_additive.prop_to_num.resize(num_prop_vars, vector<bool>(num_num_vars, true));
    are_additive.num_to_prop.resize(num_num_vars, vector<bool>(num_prop_vars, true));
    are_additive.num_to_num.resize(num_num_vars, vector<bool>(num_num_vars, true));
    for (NumericOperatorProxy op : task_proxy.get_operators()) {
        for (EffectProxy e1 : op.get_propositional_effects()) {
            auto var1 = e1.get_fact().get_variable();
            assert(!task_proxy.is_derived_variable(var1) &&
                   !task_proxy.is_derived_numeric_variable(var1));
            int e1_var_id = var1.get_id();
            // additivity for propositional<->propositional variables
            for (EffectProxy e2 : op.get_propositional_effects()) {
                auto var2 = e1.get_fact().get_variable();
                assert(!task_proxy.is_derived_variable(var2) &&
                       !task_proxy.is_derived_numeric_variable(var2));
                int e2_var_id = e2.get_fact().get_variable().get_id();
                are_additive.prop_to_prop[e1_var_id][e2_var_id] = false;
            }
            // additivity for propositional<->numeric variables
            for (const auto &[eff_var, val] : op.get_additive_effects()){
                if (val != 0) {
                    are_additive.prop_to_num[e1_var_id][eff_var] = false;
                    are_additive.num_to_prop[eff_var][e1_var_id] = false;
                }
            }
            // additivity for propositional<->numeric variables (assign effects)
            for (const auto &[assgn_var, assgn_val] : op.get_assign_effects()){
                are_additive.prop_to_num[e1_var_id][assgn_var] = false;
                are_additive.num_to_prop[assgn_var][e1_var_id] = false;
            }
        }
        for (const auto &[eff1_var, val1] : op.get_additive_effects()) {
            if (val1 != 0) {
                // additivity for numeric<->numeric variables
                for (const auto &[eff2_var, val2] : op.get_additive_effects()) {
                    if (val2 != 0) {
                        are_additive.num_to_num[eff1_var][eff2_var] = false;
                        are_additive.num_to_num[eff2_var][eff1_var] = false;
                    }
                }
                // additivity for numeric<->numeric variables (assign effects)
                for (const auto &[assgn_var, assgn_val] : op.get_assign_effects()){
                    are_additive.num_to_num[eff1_var][assgn_var] = false;
                    are_additive.num_to_num[assgn_var][eff1_var] = false;
                }
            }
        }
        // additivity for numeric<->numeric variables (both assign effects)
        for (const auto &[assgn_var1, assgn_val1] : op.get_assign_effects()){
            for (const auto &[assgn_var2, assgn_val2] : op.get_assign_effects()) {
                are_additive.num_to_num[assgn_var1][assgn_var2] = false;
                are_additive.num_to_num[assgn_var2][assgn_var1] = false;
            }
        }
    }
    return are_additive;
}

shared_ptr<MaxAdditivePDBSubsets> compute_max_additive_subsets(
    const PDBCollection &pdbs, const NumericVariableAdditivity &are_additive) {
    // Initialize compatibility graph.
    vector<vector<int>> cgraph;
    cgraph.resize(pdbs.size());

    for (size_t i = 0; i < pdbs.size(); ++i) {
        for (size_t j = i + 1; j < pdbs.size(); ++j) {
            if (are_patterns_additive(pdbs[i]->get_pattern(),
                                      pdbs[j]->get_pattern(),
                                      are_additive)) {
                /* If the two patterns are additive, there is an edge in the
                   compatibility graph. */
                cgraph[i].push_back(j);
                cgraph[j].push_back(i);
            }
        }
    }

    vector<vector<int>> max_cliques;
    compute_max_cliques(cgraph, max_cliques);

    shared_ptr<MaxAdditivePDBSubsets> max_additive_sets =
        make_shared<MaxAdditivePDBSubsets>();
    max_additive_sets->reserve(max_cliques.size());
    for (const vector<int> &max_clique : max_cliques) {
        PDBCollection max_additive_subset;
        max_additive_subset.reserve(max_clique.size());
        for (int pdb_id : max_clique) {
            max_additive_subset.push_back(pdbs[pdb_id]);
        }
        max_additive_sets->push_back(max_additive_subset);
    }
    return max_additive_sets;
}

MaxAdditivePDBSubsets compute_max_additive_subsets_with_pattern(
    const MaxAdditivePDBSubsets &known_additive_subsets,
    const Pattern &new_pattern,
    const NumericVariableAdditivity &are_additive) {
    MaxAdditivePDBSubsets subsets_additive_with_pattern;
    for (const auto &known_subset : known_additive_subsets) {
        // Take all patterns which are additive to new_pattern.
        PDBCollection new_subset;
        new_subset.reserve(known_subset.size());
        for (const shared_ptr<PatternDatabase> &pdb : known_subset) {
            if (are_patterns_additive(
                    new_pattern, pdb->get_pattern(), are_additive)) {
                new_subset.push_back(pdb);
            }
        }
        if (!new_subset.empty()) {
            subsets_additive_with_pattern.push_back(new_subset);
        }
    }
    if (subsets_additive_with_pattern.empty()) {
        // If nothing was additive with the new variable, then
        // the only additive subset is the empty set.
        subsets_additive_with_pattern.emplace_back();
    }
    return subsets_additive_with_pattern;
}
}
