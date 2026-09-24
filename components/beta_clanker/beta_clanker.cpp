#include "beta_clanker.h"

#include "nvs.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include <array>

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

Result Ask(const std::string& transcript, const std::string& previous_response_id)
{
    Result result = {};
    if (transcript.empty()) {
        result.error_code = "empty_input";
        result.error_message = "Transcript was empty";
        return result;
    }

    const std::string key = LoadStoredProviderKey();
    if (key.empty()) {
        result.error_code = "not_configured";
        result.error_message = "Provider key missing";
        return result;
    }

    cJSON* request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "model", "gpt-5.6-luna");
    cJSON_AddStringToObject(request, "instructions",
        "You are CLANKER Pocket. Answer directly and concisely in one or two short sentences.");
    cJSON_AddStringToObject(request, "input", transcript.c_str());
    if (!previous_response_id.empty()) {
        cJSON_AddStringToObject(request, "previous_response_id", previous_response_id.c_str());
    }
    cJSON_AddNumberToObject(request, "max_output_tokens", 160);

    char* raw = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!raw) {
        result.error_code = "json_encode_failed";
        result.error_message = "Could not encode request";
        return result;
    }
    std::string body(raw);
    cJSON_free(raw);

    esp_http_client_config_t cfg = {};
    cfg.url = "https://api.openai.com/v1/responses";
    cfg.method = HTTP_METHOD_POST;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 60000;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        result.error_code = "client_init_failed";
        result.error_message = "HTTP client init failed";
        return result;
    }

    const std::string auth = "Bearer " + key;
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, static_cast<int>(body.size()));
    if (err != ESP_OK) {
        result.error_code = "transport_error";
        result.error_message = esp_err_to_name(err);
        esp_http_client_cleanup(client);
        return result;
    }

    size_t offset = 0;
    while (offset < body.size()) {
        const int written = esp_http_client_write(client, body.data() + offset, body.size() - offset);
        if (written <= 0) {
            result.error_code = "upload_failed";
            result.error_message = "Failed to send request";
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return result;
        }
        offset += static_cast<size_t>(written);
    }

    if (esp_http_client_fetch_headers(client) < 0) {
        result.error_code = "response_headers_failed";
        result.error_message = "Failed to read response headers";
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return result;
    }

    result.http_status = esp_http_client_get_status_code(client);
    std::string response_body;
    std::array<char, 1024> buffer = {};
    while (true) {
        const int read = esp_http_client_read(client, buffer.data(), buffer.size());
        if (read < 0) {
            result.error_code = "response_read_failed";
            result.error_message = "Failed to read response body";
            break;
        }
        if (read == 0) break;
        response_body.append(buffer.data(), static_cast<size_t>(read));
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!result.error_code.empty()) return result;

    cJSON* root = cJSON_ParseWithLength(response_body.c_str(), response_body.size());
    if (result.http_status >= 200 && result.http_status < 300 && root) {
        cJSON* response_id = cJSON_GetObjectItemCaseSensitive(root, "id");
        if (cJSON_IsString(response_id) && response_id->valuestring) {
            result.response_id = response_id->valuestring;
        }
        cJSON* output = cJSON_GetObjectItemCaseSensitive(root, "output");
        if (cJSON_IsArray(output)) {
            cJSON* item = nullptr;
            cJSON_ArrayForEach(item, output) {
                cJSON* content = cJSON_GetObjectItemCaseSensitive(item, "content");
                if (!cJSON_IsArray(content)) continue;
                cJSON* part = nullptr;
                cJSON_ArrayForEach(part, content) {
                    cJSON* type = cJSON_GetObjectItemCaseSensitive(part, "type");
                    cJSON* text = cJSON_GetObjectItemCaseSensitive(part, "text");
                    if (cJSON_IsString(type) && type->valuestring &&
                        std::string(type->valuestring) == "output_text" &&
                        cJSON_IsString(text) && text->valuestring) {
                        result.response = text->valuestring;
                        break;
                    }
                }
                if (!result.response.empty()) break;
            }
        }
        result.success = !result.response.empty();
        if (!result.success) {
            result.error_code = "empty_response";
            result.error_message = "No output text returned";
        }
    } else {
        result.error_code = "openai_http_error";
        result.error_message = response_body.empty() ? "Response request failed"
                                                     : response_body.substr(0, 180);
    }

    if (root) cJSON_Delete(root);
    return result;
}

}  // namespace beta_clanker
