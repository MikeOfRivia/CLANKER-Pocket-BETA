#include "beta_transcription.h"

#include <array>
#include <cstring>
#include <string>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"

namespace beta_transcription {
namespace {

constexpr const char* kTag = "BetaTranscription";
constexpr const char* kOpenAiNvsNamespace = "openai";
constexpr const char* kOpenAiApiKeyKey = "api_key";
constexpr const char* kUrl = "https://api.openai.com/v1/audio/transcriptions";
constexpr const char* kModel = "gpt-4o-mini-transcribe";
constexpr const char* kBoundary = "----ClankerPocketBetaBoundary9Vv7Yx";
constexpr int kTimeoutMs = 45000;

#pragma pack(push, 1)
struct WavHeader {
    char riff[4];
    uint32_t chunk_size;
    char wave[4];
    char fmt[4];
    uint32_t subchunk1_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
};
#pragma pack(pop)

static_assert(sizeof(WavHeader) == 44, "WAV header must be 44 bytes");

std::string LoadApiKey()
{
    nvs_handle_t handle = 0;
    if (nvs_open(kOpenAiNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return {};

    size_t len = 0;
    if (nvs_get_str(handle, kOpenAiApiKeyKey, nullptr, &len) != ESP_OK || len <= 1) {
        nvs_close(handle);
        return {};
    }

    std::string value(len, '\0');
    if (nvs_get_str(handle, kOpenAiApiKeyKey, value.data(), &len) != ESP_OK) {
        nvs_close(handle);
        return {};
    }
    nvs_close(handle);
    if (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}

bool WriteAll(esp_http_client_handle_t client, const char* data, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        const int written = esp_http_client_write(client, data + offset, len - offset);
        if (written <= 0) return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

WavHeader MakeWavHeader(size_t sample_count, uint32_t sample_rate_hz)
{
    const uint32_t data_size = static_cast<uint32_t>(sample_count * sizeof(int16_t));
    WavHeader h = {};
    std::memcpy(h.riff, "RIFF", 4);
    h.chunk_size = 36U + data_size;
    std::memcpy(h.wave, "WAVE", 4);
    std::memcpy(h.fmt, "fmt ", 4);
    h.subchunk1_size = 16;
    h.audio_format = 1;
    h.num_channels = 1;
    h.sample_rate = sample_rate_hz;
    h.byte_rate = sample_rate_hz * sizeof(int16_t);
    h.block_align = sizeof(int16_t);
    h.bits_per_sample = 16;
    std::memcpy(h.data, "data", 4);
    h.data_size = data_size;
    return h;
}

std::string JsonError(cJSON* root, const char* field)
{
    if (!root) return {};
    cJSON* error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (!cJSON_IsObject(error)) return {};
    cJSON* item = cJSON_GetObjectItemCaseSensitive(error, field);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

}  // namespace

bool HasApiKey()
{
    return !LoadApiKey().empty();
}

Result TranscribePcm16(const int16_t* samples, size_t sample_count, uint32_t sample_rate_hz)
{
    Result result = {};
    if (!samples || sample_count == 0 || sample_rate_hz == 0) {
        result.error_code = "empty_audio";
        result.error_message = "No PCM audio available";
        return result;
    }

    const std::string api_key = LoadApiKey();
    if (api_key.empty()) {
        result.error_code = "not_configured";
        result.error_message = "OpenAI API key missing";
        return result;
    }

    std::string prefix;
    prefix.reserve(384);
    prefix += "--";
    prefix += kBoundary;
    prefix += "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n";
    prefix += kModel;
    prefix += "\r\n--";
    prefix += kBoundary;
    prefix += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"recording.wav\"\r\n";
    prefix += "Content-Type: audio/wav\r\n\r\n";

    const std::string suffix = std::string("\r\n--") + kBoundary + "--\r\n";
    const WavHeader wav = MakeWavHeader(sample_count, sample_rate_hz);
    const size_t pcm_bytes = sample_count * sizeof(int16_t);
    const size_t content_length = prefix.size() + sizeof(wav) + pcm_bytes + suffix.size();

    esp_http_client_config_t cfg = {};
    cfg.url = kUrl;
    cfg.method = HTTP_METHOD_POST;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = kTimeoutMs;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        result.error_code = "client_init_failed";
        result.error_message = "HTTP client init failed";
        return result;
    }

    const std::string auth = "Bearer " + api_key;
    const std::string content_type = std::string("multipart/form-data; boundary=") + kBoundary;
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    esp_http_client_set_header(client, "Content-Type", content_type.c_str());
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "clanker-pocket-beta");

    esp_err_t err = esp_http_client_open(client, static_cast<int>(content_length));
    if (err != ESP_OK) {
        result.error_code = "transport_error";
        result.error_message = esp_err_to_name(err);
        esp_http_client_cleanup(client);
        return result;
    }

    const bool wrote =
        WriteAll(client, prefix.data(), prefix.size()) &&
        WriteAll(client, reinterpret_cast<const char*>(&wav), sizeof(wav)) &&
        WriteAll(client, reinterpret_cast<const char*>(samples), pcm_bytes) &&
        WriteAll(client, suffix.data(), suffix.size());

    if (!wrote) {
        result.error_code = "upload_failed";
        result.error_message = "Failed while uploading audio";
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return result;
    }

    if (esp_http_client_fetch_headers(client) < 0) {
        result.error_code = "response_headers_failed";
        result.error_message = "Failed to read response headers";
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return result;
    }

    result.http_status = esp_http_client_get_status_code(client);

    std::string body;
    std::array<char, 1024> buffer = {};
    while (true) {
        const int read = esp_http_client_read(client, buffer.data(), buffer.size());
        if (read < 0) {
            result.error_code = "response_read_failed";
            result.error_message = "Failed to read response body";
            break;
        }
        if (read == 0) break;
        body.append(buffer.data(), static_cast<size_t>(read));
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!result.error_code.empty()) return result;

    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (result.http_status >= 200 && result.http_status < 300 && root) {
        cJSON* text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && text->valuestring) {
            result.transcript = text->valuestring;
            result.success = !result.transcript.empty();
        }
        if (!result.success) {
            result.error_code = "empty_transcript";
            result.error_message = "OpenAI returned no transcript text";
        }
    } else {
        result.error_code = JsonError(root, "code");
        if (result.error_code.empty()) result.error_code = JsonError(root, "type");
        if (result.error_code.empty()) result.error_code = "openai_http_error";
        result.error_message = JsonError(root, "message");
        if (result.error_message.empty()) {
            result.error_message = body.empty() ? "OpenAI request failed" : body.substr(0, 160);
        }
    }

    if (root) cJSON_Delete(root);

    ESP_LOGI(kTag, "transcription done: success=%d http=%d chars=%u code=%s",
             result.success ? 1 : 0, result.http_status,
             static_cast<unsigned>(result.transcript.size()),
             result.error_code.empty() ? "<none>" : result.error_code.c_str());
    return result;
}

}  // namespace beta_transcription
