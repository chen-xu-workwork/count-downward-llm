#include "pdb_heuristic.h"

#include "numeric_helper.h"
#include "pattern_generator.h"

#include "../option_parser.h"
#include "../plugin.h"

#include <limits>
#include <memory>

using namespace std;
using numeric_pdb_helper::NumericTaskProxy;

namespace numeric_pdbs {
PatternDatabase get_pdb_from_options(const shared_ptr<AbstractTask> &task,
                                     const Options &opts) {
    auto pattern_generator =
        opts.get<shared_ptr<PatternGenerator>>("pattern");

    bool drop_pdb = opts.get<int>("drop_pdb");
    bool use_lmcut = opts.get<int>("use_lmcut");
    bool blind_if_no_goal = opts.get<int>("blind_if_no_goal");
    bool extend_abstract_state_space = opts.get<int>("extend_abstract_state_space");
    int extension_h0_until_goal = opts.get<int>("extension_h0_until_goal");
    int extension_h1_until_goal = opts.get<int>("extension_h1_until_goal");
    double f_layer_offset_ratio = opts.get<double>("f_layer_offset_ratio");
    int hierarchy = opts.get<int>("hierarchy");
    int need_goal = opts.get<int>("need_goal");
    cout << "f_layer_offset_ratio!!!!!!!!!!: " << f_layer_offset_ratio << endl;

    
    shared_ptr<NumericTaskProxy> task_proxy = make_shared<NumericTaskProxy>(task);
    Pattern pattern = pattern_generator->generate(task, task_proxy);
    return {task_proxy, pattern, pattern_generator->get_max_number_pdb_states(), drop_pdb, use_lmcut, blind_if_no_goal, extend_abstract_state_space, extension_h0_until_goal, extension_h1_until_goal, f_layer_offset_ratio, static_cast<bool>(need_goal), true, hierarchy};
}

NumericPDBHeuristic::NumericPDBHeuristic(const Options &opts)
    : Heuristic(opts),
      pdb(get_pdb_from_options(task, opts)),
      number_lookup_misses(0) {
}

ap_float NumericPDBHeuristic::compute_heuristic(const GlobalState &global_state) {
    State state = convert_global_state(global_state);
    return compute_heuristic(state);
}

ap_float NumericPDBHeuristic::compute_heuristic(const State &state) {
    auto [found_state, h] = pdb.get_value(state);
    if (!found_state){
        number_lookup_misses++;
    }
    if (h == numeric_limits<ap_float>::max()) {
        return DEAD_END;
    }
    return h;
}

void NumericPDBHeuristic::print_statistics() const {
    cout << "Number of failed heuristic lookups: " << number_lookup_misses << endl;
}

static Heuristic *_parse(OptionParser &parser) {
    parser.document_synopsis("Numeric pattern database heuristic", "TODO");
    parser.document_language_support("action costs", "supported");
    parser.document_language_support("conditional effects", "not supported");
    parser.document_language_support("axioms", "not supported");
    parser.document_property("admissible", "yes");
    parser.document_property("consistent", "yes");
    parser.document_property("safe", "TODO");
    parser.document_property("preferred operators", "no");

    parser.add_option<shared_ptr<PatternGenerator>>(
        "pattern",
        "pattern generation method",
        "greedy_numeric()");

    parser.add_option<int>(
            "drop_pdb",
            "drop inner pdb.",
            "0",
            Bounds("0", "1"));

    parser.add_option<int>(
            "use_lmcut",
            "lmcut vs hierarchical pdbs.",
            "0",
            Bounds("0", "1"));

    parser.add_option<int>(
            "blind_if_no_goal",
            "throw away pdb if not abstract goal found.",
            "0",
            Bounds("0", "1"));

    parser.add_option<int>(
            "extend_abstract_state_space",
            "extend abstract PDB state spaces on misses.",
            "0",
            Bounds("0", "1"));

    parser.add_option<int>(
            "extension_h0_until_goal",
            "if 'extend_abstract_state_space' is true extend either until goal has been found (set to -1), otherwise up to the given number of states.",
            "0",
            Bounds("-1", "infinity"));

    parser.add_option<int>(
            "extension_h1_until_goal",
            "if 'extend_abstract_state_space' is true extend either until goal has been found (set to -1), otherwise up to the given number of states.",
            "0",
            Bounds("-1", "infinity"));

    parser.add_option<double>(
            "f_layer_offset_ratio",
            "stop A* in abstract state space until all states in the open list have f value >= f(goal) + x * g(goal) .",
            "0.0",
            Bounds("-1000", "infinity"));

    parser.add_option<int>(
            "need_goal",
            "Ignore max_states and continue searching in the abstract state space until an abstract goal is found.",
            "0",
            Bounds("0", "1"));

    parser.add_option<int>(
            "hierarchy",
            "What stage of hierarchy (0: BFS).",
            "1",
            Bounds("0", "1"));

    Heuristic::add_options_to_parser(parser);

    Options opts = parser.parse();
    if (parser.dry_run())
        return nullptr;

    return new NumericPDBHeuristic(opts);
}

static Plugin<Heuristic> _plugin("numeric_pdb", _parse);
}
