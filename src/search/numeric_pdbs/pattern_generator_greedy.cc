#include "pattern_generator_greedy.h"

#include "numeric_helper.h"
#include "validation.h"

#include "../option_parser.h"
#include "../plugin.h"

#include "../utils/logging.h"
#include "../utils/math.h"
#include "../utils/rng_options.h"

#include <iostream>

using namespace std;
using namespace numeric_pdb_helper;

namespace numeric_pdbs {
PatternGeneratorGreedy::PatternGeneratorGreedy(const Options &opts)
    : PatternGeneratorGreedy(opts.get<int>("max_number_pdb_states"),
                             opts.get<bool>("prefer_numeric_variables"),
                             VariableOrderType(opts.get_enum("variable_order_type")),
                             utils::parse_rng_from_options(opts)) {
}

PatternGeneratorGreedy::PatternGeneratorGreedy(size_t max_number_pdb_states,
                                               bool prefer_numeric_variables,
                                               VariableOrderType var_order_type,
                                               shared_ptr<utils::RandomNumberGenerator> rng)
    : PatternGenerator(max_number_pdb_states),
      prefer_numeric_variables(prefer_numeric_variables),
      var_order_type(var_order_type),
      rng(std::move(rng)) {
}

Pattern PatternGeneratorGreedy::generate(shared_ptr<AbstractTask> task) {
    shared_ptr<NumericTaskProxy> task_proxy = make_shared<NumericTaskProxy>(task);
    Pattern pattern;
    VariableOrderFinder order(task_proxy, var_order_type, prefer_numeric_variables, rng);
    VariablesProxy variables = task_proxy->get_variables();
    ResNumericVariablesProxy num_variables = task_proxy->get_numeric_variables();

    int size = 1;
    while (true) {
        if (order.done())
            break;
        auto [next_var_id, is_numeric] = order.next();
        if (next_var_id == -1) {
            break; // No more regular variables to add, only derived variables left.
        }

        int next_var_size;
        if (is_numeric) {
            ResNumericVariableProxy next_var = num_variables[next_var_id];
            next_var_size = max(1, task_proxy->get_approximate_domain_size(next_var));
        } else {
            VariableProxy next_var = variables[next_var_id];
            next_var_size = next_var.get_domain_size();
        }

        if (!utils::is_product_within_limit(size, next_var_size, max_number_pdb_states))
            break;

        if (is_numeric) {
            pattern.numeric.push_back(next_var_id);
        } else {
            pattern.regular.push_back(next_var_id);
        }

        size *= next_var_size;
    }

    validate_and_normalize_pattern(*task_proxy, pattern);
    cout << "Greedy pattern: propositional " << pattern.regular << "; numeric " << pattern.numeric << endl;
    return pattern;
}

static shared_ptr<PatternGenerator> _parse(OptionParser &parser) {
    parser.add_option<int>(
            "max_number_pdb_states",
            "maximal number of abstract states in the pattern database.",
            "1000000",
            Bounds("1", "infinity"));
    parser.add_option<bool>(
            "prefer_numeric_variables",
            "When selecting the next variable, should it be numeric or propositional?",
            "false");
    vector<string> variable_order_types = {"CG_GOAL_LEVEL", "CG_GOAL_RANDOM", "GOAL_CG_LEVEL"};
    parser.add_enum_option(
            "variable_order_type",
            variable_order_types,
            "Which variable order should be used?",
            "GOAL_CG_LEVEL");

    utils::add_rng_options(parser);

    Options opts = parser.parse();
    if (parser.dry_run())
        return nullptr;

    return make_shared<PatternGeneratorGreedy>(opts);
}

static PluginShared<PatternGenerator> _plugin("greedy_numeric", _parse);
}
