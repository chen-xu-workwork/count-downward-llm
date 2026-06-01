#include "domain_abstraction_heuristic.h"

#include "domain_abstraction_generator.h"

#include "../option_parser.h"
#include "../plugin.h"
#include "../task_proxy.h"
#include "../task_tools.h"

#include <limits>
#include <memory>

using namespace std;

namespace domain_abstractions {
static DomainAbstraction get_domain_abstraction_from_options(
    const options::Options &opts, const TaskProxy &task_proxy) {
    verify_no_non_numeric_axioms(task_proxy);
    verify_no_conditional_effects(task_proxy);

    shared_ptr<DomainAbstractionGenerator> generator =
        opts.get<shared_ptr<DomainAbstractionGenerator>>(
            "domain_abstraction_generator");

    return generator->build_abstraction(task_proxy);
}

DomainAbstractionHeuristic::DomainAbstractionHeuristic(
    const options::Options &opts)
    : Heuristic(opts),
    abstraction(get_domain_abstraction_from_options(opts, task_proxy)) {
}

ap_float DomainAbstractionHeuristic::compute_heuristic(const GlobalState &global_state) {
    State state = convert_global_state(global_state);
    return compute_heuristic(state);
}

ap_float DomainAbstractionHeuristic::compute_heuristic(const State &state) {
    ap_float h = abstraction.get_value(state);
    if (h == numeric_limits<ap_float>::max()) {
        return DEAD_END;
    }
    return h;
}

static Heuristic *_parse(OptionParser &parser) {
    parser.document_synopsis("Domain DomainMapping Heuristic", "TODO");
    parser.document_language_support("action costs", "supported");
    parser.document_language_support("conditional effects", "not supported");
    parser.document_language_support("axioms", "not supported");
    parser.document_property("admissible", "yes");
    parser.document_property("consistent", "yes");
    parser.document_property("safe", "yes");
    parser.document_property("preferred operators", "no");

    parser.add_option<shared_ptr<DomainAbstractionGenerator>>(
        "domain_abstraction_generator", "TODO");
    Heuristic::add_options_to_parser(parser);

    Options opts = parser.parse();
    if (parser.help_mode() || parser.dry_run()) {
        return nullptr;
    }
    return new DomainAbstractionHeuristic(opts);
}

static Plugin<Heuristic> _plugin("domain_abstraction", _parse);
}
