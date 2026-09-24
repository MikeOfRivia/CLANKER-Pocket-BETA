#include "beta_clanker.h"

#include "nvs.h"

namespace {
std::string LoadStoredProviderKey()
{
    nvs_handle_t handle = 0;
    if (nvs_open("openai", NVS_READONLY, &handle) != ESP_OK) return {};
    size_t len = 0;
    if (nvs_get_str(handle, "api_key", nullptr, &len) != ESP_OK || len <= 1) {
        nvs_close(handle);
        return {};
    }
    std::string value(len, '\0');
    if (nvs_get_str(handle, "api_key", value.data(), &len) != ESP_OK) {
        nvs_close(handle);
        return {};
    }
    nvs_close(handle);
    if (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}
}  // namespace

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
