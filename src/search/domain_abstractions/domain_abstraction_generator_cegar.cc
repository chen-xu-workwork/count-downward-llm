#include "domain_abstraction_generator_cegar.h"
#include "domain_abstraction_generator.h"

#include "cegar.h"
#include "utils.h"

#include "../option_parser.h"
#include "../plugin.h"

#include "../utils/rng.h"
#include "../utils/rng_options.h"
#include "../task_proxy.h"

using namespace std;

namespace domain_abstractions {
DomainAbstractionGeneratorCEGAR::DomainAbstractionGeneratorCEGAR(
    const options::Options &opts)
    : DomainAbstractionGenerator(opts),
      max_abstraction_size(opts.get<int>("max_abstraction_size")),
      max_time(opts.get<double>("max_time")),
      use_wildcard_plans(opts.get<bool>("use_wildcard_plans")),
      flaw_treatment(FlawTreatment(opts.get_enum("flaw_treatment"))),
      init_split_method(InitSplitMethod(opts.get_enum("init_split_method"))),
      init_split_option(InitSplitOptions(opts.get_enum("init_split_option"))) {
    if (init_split_method == InitSplitMethod::GOAL_VALUE
        && !(init_split_option == InitSplitOptions::RANDOM_GOAL
             || init_split_option == InitSplitOptions::ALL_GOALS)) {
        cerr << "CEGAR domain abstraction generator was called with "
             << "init-split method *goal_value* but candidates that "
             << "are not goal variables." << endl;
        utils::exit_with(utils::ExitCode::UNSUPPORTED);
    }
}

DomainAbstraction DomainAbstractionGeneratorCEGAR::build_abstraction(
    const TaskProxy &task_proxy) {
    // TODO: do something with std::unordered_set<int> blacklisted_variables;?
    // TODO: do something for a single goal only?
    unordered_set<int> init_split_var_ids =
        get_init_split_var_ids(task_proxy);
    return generate_domain_abstraction_with_cegar(
        max_abstraction_size, max_time, use_wildcard_plans,
        flaw_treatment, init_split_method, rng, task_proxy,
        move(init_split_var_ids));
}

unordered_set<int> DomainAbstractionGeneratorCEGAR::get_init_split_var_ids(
    const TaskProxy &task_proxy) {

    unordered_set<int> var_ids;
    switch (init_split_option) {
    case InitSplitOptions::NONE:
        break;
    case InitSplitOptions::RANDOM_GOAL: {
        const GoalsProxy &goals = task_proxy.get_goals();
        int r = rng->random(goals.size());
        var_ids.insert(goals[r].get_variable().get_id());
        break;
    }
    case InitSplitOptions::RANDOM_NON_GOAL:
        var_ids.insert(*(rng->choose(get_non_goal_variables(task_proxy))));
        break;
    case InitSplitOptions::RANDOM_ANY: {
        const VariablesProxy &vars = task_proxy.get_variables();
        int r = rng->random(vars.size());
        var_ids.insert(vars[r].get_id());
        break;
    }
    case InitSplitOptions::ALL_GOALS:
        for (const FactProxy &goal : task_proxy.get_goals()) {
            var_ids.insert(goal.get_variable().get_id());
        }
        break;
    case InitSplitOptions::ALL_NON_GOALS: {
        const vector<int> non_goal_ids = get_non_goal_variables(task_proxy);
        var_ids.insert(non_goal_ids.begin(), non_goal_ids.end());
        break;
    }
    case InitSplitOptions::ALL:
        for (size_t i = 0; i < task_proxy.get_variables().size(); ++i) {
            var_ids.insert(i);
        }
        break;
    }
    return var_ids;
}

static shared_ptr<DomainAbstractionGenerator> _parse(OptionParser &parser) {
    parser.add_option<int>(
        "max_abstraction_size",
        "Max number of states of the final abstraction.",
        "1000000");
    parser.add_option<double>(
        "max_time",
        "Max time for building abstraction.",
        "infinity");
    vector<string> init_split_options;
    init_split_options.push_back("none");
    init_split_options.push_back("random_goal");
    init_split_options.push_back("random_non_goal");
    init_split_options.push_back("random_any");
    init_split_options.push_back("all_goals");
    init_split_options.push_back("all_non_goals");
    init_split_options.push_back("all");
    parser.add_enum_option(
        "init_split_option",
        init_split_options,
        "Specify an initialization for the abstraction generation.",
        "all_goals");
    add_domain_abstraction_cegar_options_to_parser(parser);
    add_domain_abstraction_generator_options_to_parser(parser);

    Options opts = parser.parse();
    if (parser.dry_run()) {
        return nullptr;
    }

    return make_shared<DomainAbstractionGeneratorCEGAR>(opts);
}

static PluginShared<DomainAbstractionGenerator> _plugin("domain_abstraction_cegar", _parse);
}

namespace options {
template <>
std::string TypeNamer<domain_abstractions::InitSplitOptions>::name() {
    return "InitSplitOptions";
}
}
