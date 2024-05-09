#include "validation.h"

#include "../task_proxy.h"

#include "../utils/system.h"

#include <algorithm>
#include <iostream>

using namespace std;
using utils::ExitCode;

namespace numeric_pdbs {
void inline validate_and_normalize_variable_list(vector<int> &vars, int num_variables) {
    sort(vars.begin(), vars.end());
    auto it = unique(vars.begin(), vars.end());
    if (it != vars.end()) {
        vars.erase(it, vars.end());
        cout << "Warning: duplicate variables in pattern have been removed"
             << endl;
    }
    if (!vars.empty()) {
        if (vars.front() < 0) {
            cerr << "Variable number too low in pattern" << endl;
            utils::exit_with(ExitCode::CRITICAL_ERROR);
        }
        if (vars.back() >= num_variables) {
            cerr << "Variable number too high in pattern" << endl;
            utils::exit_with(ExitCode::CRITICAL_ERROR);
        }
    }
}

void validate_and_normalize_pattern(const TaskProxy &task_proxy,
                                    Pattern &pattern) {
    /*
      - Sort by variable number and remove duplicate variables.
      - Warn if duplicate variables exist.
      - Error if patterns contain out-of-range variable numbers.
    */
    validate_and_normalize_variable_list(pattern.regular, task_proxy.get_variables().size());
    validate_and_normalize_variable_list(pattern.numeric, task_proxy.get_numeric_variables().size());
}

void validate_and_normalize_patterns(const TaskProxy &task_proxy,
                                     PatternCollection &patterns) {
    /*
      - Validate and normalize each pattern (see there).
      - Warn if duplicate patterns exist.
    */
    for (Pattern &pattern : patterns)
        validate_and_normalize_pattern(task_proxy, pattern);
    PatternCollection sorted_patterns(patterns);
    sort(sorted_patterns.begin(), sorted_patterns.end());
    auto it = unique(sorted_patterns.begin(), sorted_patterns.end());
    if (it != sorted_patterns.end()) {
        cout << "Warning: duplicate patterns have been detected" << endl;
    }
}
}
