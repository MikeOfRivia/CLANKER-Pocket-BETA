#ifndef BETA_CLANKER_H_
#define BETA_CLANKER_H_

#include <string>

namespace beta_clanker {
struct Result {
    bool success = false;
    int http_status = 0;
    std::string response;
    std::string response_id;
    std::string error_code;
    std::string error_message;
};
Result Ask(const std::string& transcript, const std::string& previous_response_id = {});
}
#endif
