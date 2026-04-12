#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H
#include <string>
int req_task(std::string &uid,std::string &access_code,std::string & json_response) ;
int upload_results(const std::string& uid,
                   const std::string& access_code,
                   const std::string& file_path,
                   const std::string& session_id,
                   std::string& json_response);
int client_registration(std::string & uid,std::string& json_response);
#endif
