#include "pattern_generator_manual.h"

#include "numeric_helper.h"
#include "validation.h"

#include "../option_parser.h"
#include "../plugin.h"

#include "../utils/logging.h"

#include <iostream>

using namespace std;

namespace numeric_pdbs {
PatternGeneratorManual::PatternGeneratorManual(const Options &opts) :
        PatternGenerator(numeric_limits<int>::max()){
    pattern.regular = opts.get_list<int>("pattern");
    cout << "not adapted to numeric variables: PatternGeneratorManual" << endl;
    utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
}

Pattern PatternGeneratorManual::generate(shared_ptr<AbstractTask> task) {
    numeric_pdb_helper::NumericTaskProxy task_proxy(task);
    validate_and_normalize_pattern(task_proxy, pattern);
    cout << "Manual pattern: " << pattern.regular << endl;
    return pattern;
}

static shared_ptr<PatternGenerator> _parse(OptionParser &parser) {
    parser.add_list_option<int>(
        "pattern",
        "list of variable numbers of the planning task that should be used as "
        "pattern.");

    Options opts = parser.parse();
    if (parser.dry_run())
        return nullptr;

    return make_shared<PatternGeneratorManual>(opts);
}

static PluginShared<PatternGenerator> _plugin("manual_numeric", _parse);
}
