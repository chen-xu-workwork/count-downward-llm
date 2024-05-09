#include "task_proxy.h"

#include "causal_graph.h"
#include "numeric_pdbs/causal_graph.h"


bool TaskProxy::is_derived_variable(VariableProxy var) const {
    for (auto ax : get_axioms()){
        for (auto eff : ax.get_effects()) {
            if (eff.get_fact().get_variable().get_id() == var.get_id()) {
                return true;
            }
        }
    }
    return false;
}

const CausalGraph &TaskProxy::get_causal_graph() const {
    return ::get_causal_graph(task);
}

const numeric_pdbs::CausalGraph &TaskProxy::get_numeric_causal_graph() const {
    return ::get_numeric_causal_graph(task);
}
