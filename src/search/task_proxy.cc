#include "task_proxy.h"

#include "axioms.h"
#include "causal_graph.h"

#include "numeric_pdbs/causal_graph.h"

using namespace std;

ap_float State::assign_effect(ap_float aff_value, f_operator fop, ap_float ass_value) {
    ap_float result = aff_value;
    switch(fop) {
        case assign:
            result = ass_value;
            break;
        case scale_up:
            result *= ass_value;
            break;
        case scale_down:
            result /= ass_value;
            break;
        case increase:
            result += ass_value;
            break;
        case decrease:
            result -= ass_value;
            break;
        default:
            cerr << "Error: Unknown assignment effect (" << fop << ")."<< endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
    }
    return result;
}

void State::get_numeric_successor(
        vector<ap_float> &new_num_state,
        const GlobalOperator &op,
        vector<int> &new_state) {

    for (const auto &ass_eff : op.get_assign_effects()) {
        assert((int) new_num_state.size() > ass_eff.aff_var);
        assert((int) new_num_state.size() > ass_eff.ass_var);

        ap_float result = assign_effect(new_num_state[ass_eff.aff_var], ass_eff.fop, new_num_state[ass_eff.ass_var]);

        switch (g_numeric_var_types[ass_eff.aff_var]) {
            case instrumentation:
                // we ignore costs here
                break;
            case regular:
                new_num_state[ass_eff.aff_var] = result;
                break;
            default:
                cerr << "ERROR: unexpected numeric variable type " << g_numeric_var_types[ass_eff.aff_var] << endl;
                utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        }
    }

    g_axiom_evaluator->evaluate_arithmetic_axioms(new_num_state);
    g_axiom_evaluator->evaluate(new_state, new_num_state); // evaluate logic + comparison axioms (we need this for the goal axiom)
}

State State::get_successor(OperatorProxy op) const {
    assert(!op.is_axiom());

    vector<int> new_values = values;
    for (EffectProxy effect : op.get_effects()) {
        if (does_fire(effect, *this)) {
            FactProxy effect_fact = effect.get_fact();
            new_values[effect_fact.get_variable().get_id()] = effect_fact.get_value();
        }
    }

    vector<ap_float> new_num_values = num_values;

    if(has_numeric_axioms()) {
        // TODO not sure if this is really needed here
        g_axiom_evaluator->evaluate_arithmetic_axioms(new_num_values);
    }

    get_numeric_successor(new_num_values,
                          *task->get_global_operator(op.get_id(), false),
                          new_values);

    return {*task, std::move(new_values), std::move(new_num_values)};
}

const CausalGraph &TaskProxy::get_causal_graph() const {
    return ::get_causal_graph(task);
}
