#include "beta_clanker.h"

namespace beta_clanker {

Result Ask(const std::string& transcript)
{
    Result result = {};
    if (transcript.empty()) {
        result.error_code = "empty_input";
        result.error_message = "Transcript was empty";
    }
    return result;
}

}  // namespace beta_clanker
