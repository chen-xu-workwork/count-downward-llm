#include "pdb_heuristic.h"

#include "pattern_generator.h"

#include "../option_parser.h"
#include "../plugin.h"
#include "../task_proxy.h"

#include <limits>
#include <memory>

using namespace std;

namespace numeric_pdbs {
PatternDatabase get_pdb_from_options(const shared_ptr<AbstractTask> &task,
                                     const Options &opts) {
    auto pattern_generator =
        opts.get<shared_ptr<PatternGenerator>>("pattern");
    Pattern pattern = pattern_generator->generate(task);
    TaskProxy task_proxy(*task);
    return {task_proxy, pattern, pattern_generator->get_max_number_pdb_states(), true};
}

NumericPDBHeuristic::NumericPDBHeuristic(const Options &opts)
    : Heuristic(opts),
      pdb(get_pdb_from_options(task, opts)) {
}

ap_float NumericPDBHeuristic::compute_heuristic(const GlobalState &global_state) {
    State state = convert_global_state(global_state);
    return compute_heuristic(state);
}

ap_float NumericPDBHeuristic::compute_heuristic(const State &state) const {
    ap_float h = pdb.get_value(state);
    if (h == numeric_limits<ap_float>::max()) {
        return DEAD_END;
    }
    return h;
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

    Heuristic::add_options_to_parser(parser);

    Options opts = parser.parse();
    if (parser.dry_run())
        return nullptr;

    return new NumericPDBHeuristic(opts);
}

static Plugin<Heuristic> _plugin("numeric_pdb", _parse);
}
