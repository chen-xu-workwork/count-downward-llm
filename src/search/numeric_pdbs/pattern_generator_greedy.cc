#include "pattern_generator_greedy.h"

#include "validation.h"

#include "../option_parser.h"
#include "numeric_helper.h"
#include "../plugin.h"
#include "../task_proxy.h"
#include "variable_order_finder.h"

#include "../utils/logging.h"
#include "../utils/math.h"

#include <iostream>

using namespace std;

namespace numeric_pdbs {
PatternGeneratorGreedy::PatternGeneratorGreedy(const Options &opts)
    : PatternGeneratorGreedy(opts.get<int>("max_states")) {
}

PatternGeneratorGreedy::PatternGeneratorGreedy(int max_states)
    : max_states(max_states) {
}

Pattern PatternGeneratorGreedy::generate(shared_ptr<AbstractTask> task) {
    TaskProxy task_proxy(*task);
    shared_ptr<numeric_pdb_helper::NumericTaskProxy> num_task_proxy = make_shared<numeric_pdb_helper::NumericTaskProxy>(task_proxy);
    Pattern pattern;
    VariableOrderFinder order(task, num_task_proxy, GOAL_CG_LEVEL, true);
    VariablesProxy variables = task_proxy.get_variables();
//    NumericVariablesProxy num_variables = task_proxy.get_numeric_variables();

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
            // TODO approximate domain size based on the constants which are related to this variable
            next_var_size = 100;
        } else {
            VariableProxy next_var = variables[next_var_id];
            next_var_size = next_var.get_domain_size();
        }

        if (!utils::is_product_within_limit(size, next_var_size, max_states))
            break;

        if (is_numeric) {
            pattern.numeric.push_back(next_var_id);
        } else {
            pattern.regular.push_back(next_var_id);
        }

        size *= next_var_size;
    }

    validate_and_normalize_pattern(task_proxy, pattern);
    cout << "Greedy pattern: propositional " << pattern.regular << "; numeric " << pattern.numeric << endl;
    return pattern;
}

static shared_ptr<PatternGenerator> _parse(OptionParser &parser) {
    parser.add_option<int>(
        "max_states",
        "maximal number of abstract states in the pattern database.",
        "1000000",
        Bounds("1", "infinity"));

    Options opts = parser.parse();
    if (parser.dry_run())
        return nullptr;

    return make_shared<PatternGeneratorGreedy>(opts);
}

static PluginShared<PatternGenerator> _plugin("greedy_numeric", _parse);
}
