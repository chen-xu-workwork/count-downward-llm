#include "domain_abstraction_collection_generator_multiple_cegar.h"

#include "cegar.h"
#include "domain_abstraction.h"
#include "utils.h"

#include "../option_parser.h"
#include "../plugin.h"

using namespace std;

namespace domain_abstractions {
DomainAbstractionCollectionGeneratorMultipleCegar::DomainAbstractionCollectionGeneratorMultipleCegar(
    options::Options &opts)
    : DomainAbstractionCollectionGeneratorMultiple(opts),
      use_wildcard_plans(opts.get<bool>("use_wildcard_plans")),
      flaw_treatment(FlawTreatment(opts.get_enum("flaw_treatment"))),
      init_split_method(InitSplitMethod(opts.get_enum("init_split_method"))),
      numeric_split_strategy(NumericSplitStrategy(opts.get_enum("numeric_split_strategy"))),
      exec_entire_plan(opts.get<bool>("exec_entire_plan")) {
    if (init_split_method == InitSplitMethod::GOAL_VALUE
        && init_split_variables != VariableSubset::GOALS) {
        cerr << "CEGAR domain abstraction generator was called with "
             << "init-split method *goal_value* but candidates that "
             << "are not goal variables." << endl;
        utils::exit_with(utils::ExitCode::UNSUPPORTED);
    }
}

string DomainAbstractionCollectionGeneratorMultipleCegar::id() const {
    return "CEGAR";
}

DomainAbstraction DomainAbstractionCollectionGeneratorMultipleCegar::compute_abstraction(
    int max_abstraction_size,
    double max_time,
    const shared_ptr<utils::RandomNumberGenerator> &rng,
    const TaskProxy &task_proxy,
    const Fact &,
    unordered_set<int> &&init_split_var_ids,
    unordered_set<int> &&blacklisted_variables) {
    // Split blacklisted_variables into propositional and numeric variables.
    // Convention: IDs >= num_propositional_vars are numeric variable IDs (offset by num_prop_vars).
    int num_prop_vars = task_proxy.get_variables().size();
    
    unordered_set<int> blacklisted_prop_vars;
    unordered_set<int> blacklisted_numeric_vars;
    
    for (int var_id : blacklisted_variables) {
        if (var_id >= num_prop_vars) {
            // This is a numeric variable (encoded as prop_vars + numeric_var_id)
            blacklisted_numeric_vars.insert(var_id - num_prop_vars);
        } else {
            // This is a propositional variable
            blacklisted_prop_vars.insert(var_id);
        }
    }
    
    return generate_domain_abstraction_with_cegar(
        max_abstraction_size,
        max_time,
        use_wildcard_plans,
        flaw_treatment,
        init_split_method,
        numeric_split_strategy,
        exec_entire_plan,
        rng,
        task_proxy,
        move(init_split_var_ids),
        move(blacklisted_prop_vars),
        move(blacklisted_numeric_vars));
}

static shared_ptr<DomainAbstractionCollectionGenerator> _parse(OptionParser &parser) {
    parser.document_synopsis(
        "Multiple CEGAR",
        "This pattern collection generator implements the multiple CEGAR "
        "algorithm. It is an instantiation of the 'multiple algorithm framework'. "
        "To compute a pattern in each iteration, it uses the CEGAR algorithm "
        "restricted to a single goal variable. See below for descriptions of "
        "the algorithms.");
    add_cegar_implementation_notes_to_parser(parser);
    add_multiple_algorithm_implementation_notes_to_parser(parser);
    add_multiple_options_to_parser(parser);
    add_domain_abstraction_cegar_options_to_parser(parser);

    Options opts = parser.parse();
    if (parser.dry_run()) {
        return nullptr;
    }

    return make_shared<DomainAbstractionCollectionGeneratorMultipleCegar>(opts);
}

static PluginShared<DomainAbstractionCollectionGenerator> _plugin("multiple_domain_abstractions_cegar", _parse);
}
