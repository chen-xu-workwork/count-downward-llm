#include "global_state.h"

#include "globals.h"
#include "state_registry.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
using namespace std;

namespace {
bool starts_with(const string &text, const string &prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

string trim_copy(const string &text) {
    size_t begin = 0;
    while (begin < text.size() &&
           isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin &&
           isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

string collapse_spaces(const string &text) {
    string result;
    bool last_was_space = true;
    for (char ch : text) {
        if (isspace(static_cast<unsigned char>(ch))) {
            if (!last_was_space) {
                result += ' ';
                last_was_space = true;
            }
        } else {
            result += ch;
            last_was_space = false;
        }
    }
    if (!result.empty() && result.back() == ' ')
        result.pop_back();
    return result;
}

string fd_name_to_pddl_tuple(string name) {
    replace(name.begin(), name.end(), '(', ' ');
    replace(name.begin(), name.end(), ')', ' ');
    replace(name.begin(), name.end(), ',', ' ');
    name = collapse_spaces(trim_copy(name));
    return name.empty() ? "" : "(" + name + ")";
}

string fact_name_to_pddl_atom(string fact_name) {
    if (fact_name == "<none of those>" ||
        fact_name.find("derived!") != string::npos ||
        fact_name.find("new-axiom@") != string::npos ||
        starts_with(fact_name, "NegatedAtom ")) {
        return "";
    }
    if (!starts_with(fact_name, "Atom "))
        return "";
    return fd_name_to_pddl_tuple(fact_name.substr(5));
}

string numeric_name_to_pddl_function(string name) {
    if (name.find("derived!") != string::npos ||
        name.find("new-axiom@") != string::npos) {
        return "";
    }
    if (starts_with(name, "PNE "))
        name = name.substr(4);
    return fd_name_to_pddl_tuple(name);
}
}

GlobalState::GlobalState(const PackedStateBin *buffer_, const StateRegistry &registry_,
             StateID id_)
    : buffer(buffer_),
      registry(&registry_),
      id(id_) {
    assert(buffer);
    assert(id != StateID::no_state);
}

GlobalState::~GlobalState() {
}

container_int GlobalState::operator[](size_t index) const {
    return g_state_packer->get(buffer, index);
}

bool GlobalState::same_values(const GlobalState &state) const {
    for (size_t i = 0; i < g_variable_domain.size(); ++i) {
        if (this->operator[](i) != state[i]) return false;
    }
    std::vector<ap_float> this_numeric_values = this->get_numeric_vars();
    std::vector<ap_float> numeric_values = state.get_numeric_vars();
    for (size_t i = 0; i < this_numeric_values.size(); ++i) {
        if (g_numeric_var_types[i] == regular) {
            if (std::fabs(this_numeric_values[i] - numeric_values[i]) > 0.00001) return false;
        }
    }
    return true;
}

bool GlobalState::same_values(const std::vector<container_int> &values, const std::vector<ap_float> &numeric_values) const {
    for (size_t i = 0; i < g_variable_domain.size(); ++i) {
        if (this->operator[](i) != values[i]) return false;
    }
    std::vector<ap_float> this_numeric_values = this->get_numeric_vars();
    for (size_t i = 0; i < this_numeric_values.size(); ++i) {
        if (g_numeric_var_types[i] == regular) {
            if (std::fabs(this_numeric_values[i] - numeric_values[i]) > 0.00001) return false;
        }
    }
    return true;
}

void GlobalState::dump_pddl() const {
    for (size_t i = 0; i < g_variable_domain.size(); ++i) {
        const string &fact_name = g_fact_names[i][(*this)[i]];
        if (fact_name != "<none of those>")
            cout << fact_name << endl;
    }
}

void GlobalState::dump_pddl_init() const {
    cout << get_pddl_init_string();
}

//std::vector<ap_float> GlobalState::get_instrumentation_vars() const {
//	vector<ap_float> instvars;
//	assert(g_initial_state_data.size() == g_variable_domain.size());
//	assert(g_initial_state_numeric.size() == g_numeric_var_types.size());
//	for (size_t i = g_initial_state_data.size(); i< g_variable_domain.size() + g_initial_state_numeric.size(); ++i) {
//		if(g_numeric_var_types[i-g_initial_state_data.size()] == instrumentation) {
//			instvars.push_back(g_state_packer->unpackDouble((*this)[i]));
//		}
//	}
//	return instvars;
//}

void GlobalState::dump_fdr() const {
    for (size_t i = 0; i < g_variable_domain.size(); ++i)
        cout << "  #" << i << " [" << g_variable_name[i] << "] -> "
             << g_fact_names[i][(*this)[i]] << " (" << (*this)[i] << ")" << endl;
    vector<ap_float> numeric_vals = registry->get_numeric_vars(*this);
    for (size_t i = 0; i < g_numeric_var_names.size(); ++i) {
    	cout << "  #" << g_variable_domain.size()+i << " [" << g_numeric_var_names[i] << "] -> "
    			<< numeric_vals[i] << endl;
    }
}

std::vector<ap_float> GlobalState::get_numeric_vars() const {
	return registry->get_numeric_vars(*this);
}

string GlobalState::get_pddl_init_string() const {
    stringstream out;
    out << "(:init\n";
    set<string> emitted_facts;

    for (const string &constant_fact : g_init_constant_facts) {
        string fact = trim_copy(constant_fact);
        if (!fact.empty() && emitted_facts.insert(fact).second)
            out << "  " << fact << "\n";
    }

    for (size_t index = 0; index < g_variable_domain.size(); ++index) {
        string atom = fact_name_to_pddl_atom(
            g_fact_names[index][(*this)[index]]);
        if (!atom.empty() && emitted_facts.insert(atom).second)
            out << "  " << atom << "\n";
    }

    vector<ap_float> numeric_values = registry->get_numeric_vars(*this);
    for (size_t index = 0; index < g_numeric_var_names.size(); ++index) {
        if (g_numeric_var_types[index] != regular &&
            g_numeric_var_types[index] != instrumentation) {
            continue;
        }
        string function_name =
            numeric_name_to_pddl_function(g_numeric_var_names[index]);
        if (!function_name.empty()) {
            stringstream assignment;
            assignment << "(= " << function_name << " "
                       << setprecision(12) << numeric_values[index] << ")";
            if (emitted_facts.insert(assignment.str()).second)
                out << "  " << assignment.str() << "\n";
        }
    }
    out << ")\n";
    return out.str();
}

std::string GlobalState::dump_plan_vis_log() const {
	stringstream outstream;
    for (size_t i = 0; i < g_variable_domain.size(); ++i)
        outstream << "{\"" << i << "\":"
             << (*this)[i] << "},";
    vector<ap_float> numeric_vals = registry->get_numeric_vars(*this);
    for (size_t i = 0; i < g_numeric_var_names.size(); ++i) {
    	outstream << " {\"" <<  g_variable_domain.size() + i  << "\":"
    			<< numeric_vals[i] << "},";
    }
    string returnstring = outstream.str();
    returnstring.pop_back();
    return returnstring;
}

std::string GlobalState::get_numeric_state_vals_string() const {
	stringstream outstream;
  	for (size_t i = 0; i < g_numeric_var_names.size(); ++i)
    	if (g_numeric_var_types[i] == regular) {
    		outstream << fixed << g_numeric_var_names[i] << "=" << registry->get_numeric_vars(*this)[i] << ";"; }
    string returnstring = outstream.str();
    if(returnstring.length() > 0)
    	returnstring.pop_back();
    return returnstring;
}

std::string GlobalState::dump_plan_vis_log(const GlobalState& parent) const {
	stringstream outstream;
    for (size_t i = 0; i < g_variable_domain.size(); ++i) {
    	if((*this)[i] != parent[i])
    		outstream << "{\"" << i << "\":"
			<< (*this)[i] << "},";
    }
    vector<ap_float> numeric_vals = registry->get_numeric_vars(*this);
    for (size_t i = 0; i < g_numeric_var_names.size(); ++i) {
    	if(numeric_vals[i] != parent.get_numeric_vars()[i])
    	outstream << "{\"" <<  g_variable_domain.size() + i  << "\":"
    			<< numeric_vals[i] << "},";
    }
    string returnstring = outstream.str();
    returnstring.pop_back();
    return returnstring;
}
