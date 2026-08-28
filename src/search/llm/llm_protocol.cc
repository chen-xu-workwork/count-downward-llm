#include "llm_protocol.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

using namespace std;

namespace llm {

Request::Request()
    : iteration(1),
      state_id(StateID::no_state),
      state_index(0),
      g(0),
      h(0),
      search_expansions(0),
      phase_elapsed_seconds(0.0) {
}

Response::Response()
    : iteration(1),
      state_id(StateID::no_state),
      state_index(0),
      transport_ok(false),
      http_status(0) {
}

ParsedResponse::ParsedResponse()
    : valid(false) {
}

namespace {
const char *get_setting(const string &suffix) {
    string primary = "HYBRID_LLM_" + suffix;
    const char *value = getenv(primary.c_str());
    if (value)
        return value;
    string legacy = "NLM_LLM_" + suffix;
    return getenv(legacy.c_str());
}

void skip_whitespace(const string &text, size_t &position) {
    while (position < text.size() &&
           isspace(static_cast<unsigned char>(text[position]))) {
        ++position;
    }
}

bool parse_string(
    const string &text, size_t &position, string &result, string &error) {
    skip_whitespace(text, position);
    if (position >= text.size() || text[position] != '"') {
        error = "expected JSON string";
        return false;
    }
    ++position;
    result.clear();
    while (position < text.size()) {
        char ch = text[position++];
        if (ch == '"')
            return true;
        if (ch != '\\') {
            result += ch;
            continue;
        }
        if (position >= text.size()) {
            error = "truncated JSON escape";
            return false;
        }
        char escaped = text[position++];
        switch (escaped) {
        case '"': result += '"'; break;
        case '\\': result += '\\'; break;
        case '/': result += '/'; break;
        case 'b': result += '\b'; break;
        case 'f': result += '\f'; break;
        case 'n': result += '\n'; break;
        case 'r': result += '\r'; break;
        case 't': result += '\t'; break;
        case 'u':
            if (position + 4 > text.size()) {
                error = "truncated JSON unicode escape";
                return false;
            }
            position += 4;
            result += '?';
            break;
        default:
            error = "invalid JSON escape";
            return false;
        }
    }
    error = "unterminated JSON string";
    return false;
}

bool skip_value(const string &text, size_t &position, string &error);

bool skip_array(const string &text, size_t &position, string &error) {
    ++position;
    skip_whitespace(text, position);
    if (position < text.size() && text[position] == ']') {
        ++position;
        return true;
    }
    while (position < text.size()) {
        if (!skip_value(text, position, error))
            return false;
        skip_whitespace(text, position);
        if (position < text.size() && text[position] == ',') {
            ++position;
            continue;
        }
        if (position < text.size() && text[position] == ']') {
            ++position;
            return true;
        }
        error = "expected ',' or ']' in JSON array";
        return false;
    }
    error = "unterminated JSON array";
    return false;
}

bool skip_object(const string &text, size_t &position, string &error) {
    ++position;
    skip_whitespace(text, position);
    if (position < text.size() && text[position] == '}') {
        ++position;
        return true;
    }
    while (position < text.size()) {
        string ignored_key;
        if (!parse_string(text, position, ignored_key, error))
            return false;
        skip_whitespace(text, position);
        if (position >= text.size() || text[position] != ':') {
            error = "expected ':' in JSON object";
            return false;
        }
        ++position;
        if (!skip_value(text, position, error))
            return false;
        skip_whitespace(text, position);
        if (position < text.size() && text[position] == ',') {
            ++position;
            continue;
        }
        if (position < text.size() && text[position] == '}') {
            ++position;
            return true;
        }
        error = "expected ',' or '}' in JSON object";
        return false;
    }
    error = "unterminated JSON object";
    return false;
}

bool skip_value(const string &text, size_t &position, string &error) {
    skip_whitespace(text, position);
    if (position >= text.size()) {
        error = "missing JSON value";
        return false;
    }
    if (text[position] == '"') {
        string ignored;
        return parse_string(text, position, ignored, error);
    }
    if (text[position] == '[')
        return skip_array(text, position, error);
    if (text[position] == '{')
        return skip_object(text, position, error);

    size_t start = position;
    while (position < text.size() && text[position] != ',' &&
           text[position] != ']' && text[position] != '}' &&
           !isspace(static_cast<unsigned char>(text[position]))) {
        ++position;
    }
    if (position == start) {
        error = "invalid JSON value";
        return false;
    }
    return true;
}

bool parse_string_array(
    const string &text, size_t &position, vector<string> &result,
    string &error) {
    skip_whitespace(text, position);
    if (position >= text.size() || text[position] != '[') {
        error = "actions must be a JSON array";
        return false;
    }
    ++position;
    skip_whitespace(text, position);
    if (position < text.size() && text[position] == ']') {
        ++position;
        return true;
    }
    while (position < text.size()) {
        string item;
        if (!parse_string(text, position, item, error))
            return false;
        result.push_back(item);
        skip_whitespace(text, position);
        if (position < text.size() && text[position] == ',') {
            ++position;
            continue;
        }
        if (position < text.size() && text[position] == ']') {
            ++position;
            return true;
        }
        error = "expected ',' or ']' in actions array";
        return false;
    }
    error = "unterminated actions array";
    return false;
}

bool parse_string_matrix(
    const string &text, size_t &position, vector<vector<string>> &result,
    string &error) {
    skip_whitespace(text, position);
    if (position >= text.size() || text[position] != '[') {
        error = "action_chains must be a JSON array";
        return false;
    }
    ++position;
    skip_whitespace(text, position);
    if (position < text.size() && text[position] == ']') {
        ++position;
        return true;
    }
    while (position < text.size()) {
        vector<string> chain;
        if (!parse_string_array(text, position, chain, error))
            return false;
        result.push_back(chain);
        skip_whitespace(text, position);
        if (position < text.size() && text[position] == ',') {
            ++position;
            continue;
        }
        if (position < text.size() && text[position] == ']') {
            ++position;
            return true;
        }
        error = "expected ',' or ']' in action_chains array";
        return false;
    }
    error = "unterminated action_chains array";
    return false;
}
}

bool setting_enabled(const string &suffix, bool default_value) {
    const char *value = get_setting(suffix);
    if (!value)
        return default_value;
    string setting(value);
    return !setting.empty() && setting != "0" && setting != "false" &&
           setting != "FALSE";
}

int setting_int(const string &suffix, int default_value) {
    const char *value = get_setting(suffix);
    return value ? atoi(value) : default_value;
}

ap_float setting_float(const string &suffix, ap_float default_value) {
    const char *value = get_setting(suffix);
    return value ? atof(value) : default_value;
}

string setting_string(const string &suffix, const string &default_value) {
    const char *value = get_setting(suffix);
    return value ? string(value) : default_value;
}

bool setting_equals_ignore_case(const string &suffix, const string &expected) {
    string value = setting_string(suffix, "");
    transform(value.begin(), value.end(), value.begin(),
              [](unsigned char ch) { return static_cast<char>(tolower(ch)); });
    return value == expected;
}

string state_id_label(StateID state_id) {
    ostringstream stream;
    stream << state_id;
    return stream.str();
}

string json_escape(const string &value) {
    ostringstream out;
    for (char ch : value) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                const char *hex = "0123456789abcdef";
                out << "\\u00"
                    << hex[(static_cast<unsigned char>(ch) >> 4) & 0x0f]
                    << hex[static_cast<unsigned char>(ch) & 0x0f];
            } else {
                out << ch;
            }
        }
    }
    return out.str();
}

ParsedResponse parse_response_body(const string &body) {
    ParsedResponse result;
    size_t position = 0;
    skip_whitespace(body, position);
    if (position >= body.size() || body[position] != '{') {
        result.error = "response is not a JSON object";
        return result;
    }
    ++position;
    bool saw_status = false;
    bool saw_actions = false;
    bool saw_action_chains = false;
    bool object_closed = false;
    while (position < body.size()) {
        skip_whitespace(body, position);
        if (position < body.size() && body[position] == '}') {
            ++position;
            object_closed = true;
            break;
        }
        string key;
        if (!parse_string(body, position, key, result.error))
            return result;
        skip_whitespace(body, position);
        if (position >= body.size() || body[position] != ':') {
            result.error = "expected ':' after response field";
            return result;
        }
        ++position;
        if (key == "status") {
            if (!parse_string(body, position, result.status, result.error))
                return result;
            saw_status = true;
        } else if (key == "actions") {
            if (!parse_string_array(
                    body, position, result.actions, result.error)) {
                return result;
            }
            saw_actions = true;
        } else if (key == "action_chains") {
            if (!parse_string_matrix(
                    body, position, result.action_chains, result.error)) {
                return result;
            }
            saw_action_chains = true;
        } else if (!skip_value(body, position, result.error)) {
            return result;
        }
        skip_whitespace(body, position);
        if (position < body.size() && body[position] == ',') {
            ++position;
            continue;
        }
        if (position < body.size() && body[position] == '}') {
            ++position;
            object_closed = true;
            break;
        }
        result.error = "expected ',' or '}' in response object";
        return result;
    }
    if (!object_closed) {
        result.error = "unterminated response object";
        return result;
    }
    if (!saw_status || (!saw_actions && !saw_action_chains)) {
        result.error = "response is missing status and action payload";
        return result;
    }
    skip_whitespace(body, position);
    if (position != body.size()) {
        result.error = "response contains trailing content";
        return result;
    }
    if (!saw_action_chains)
        result.action_chains.push_back(result.actions);
    result.valid = true;
    return result;
}

}
