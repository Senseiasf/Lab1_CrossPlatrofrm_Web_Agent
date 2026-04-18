#ifndef JSON_FUNCTIONS_H
#define JSON_FUNCTIONS_H

#include <string>

int parse_registration_response(const std::string& jsonResponse, std::string& access_code);
int parse_req_task(const std::string& jsonResponse, std::string& out_session_id);

#endif
