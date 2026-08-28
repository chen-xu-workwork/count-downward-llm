#ifndef LLM_LLM_PROTOCOL_H
#define LLM_LLM_PROTOCOL_H

#include "../globals.h"
#include "../state_id.h"

#include <cstddef>
#include <string>
#include <vector>

namespace llm {

struct Request {
    std::string request_id;
    std::string run_id;
    int iteration;
    StateID state_id;
    std::size_t state_index;
    std::string state_label;
    std::string problem_id;
    std::string reason;
    ap_float g;
    ap_float h;
    int search_expansions;
    double phase_elapsed_seconds;
    std::string init;

    Request();
};

struct Response {
    std::string request_id;
    std::string run_id;
    int iteration;
    StateID state_id;
    std::size_t state_index;
    std::string state_label;
    bool transport_ok;
    int http_status;
    std::string body;
    std::string error;

    Response();
};

struct ParsedResponse {
    bool valid;
    std::string status;
    std::vector<std::string> actions;
    std::vector<std::vector<std::string>> action_chains;
    std::string error;

    ParsedResponse();
};

bool setting_enabled(const std::string &suffix, bool default_value = false);
int setting_int(const std::string &suffix, int default_value);
ap_float setting_float(const std::string &suffix, ap_float default_value);
std::string setting_string(
    const std::string &suffix, const std::string &default_value);
bool setting_equals_ignore_case(
    const std::string &suffix, const std::string &expected);

std::string state_id_label(StateID state_id);
std::string json_escape(const std::string &value);
ParsedResponse parse_response_body(const std::string &body);

}

#endif
