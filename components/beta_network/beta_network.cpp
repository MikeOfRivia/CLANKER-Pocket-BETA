#include "beta_network.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "esp_crt_bundle.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace beta_network {
namespace {

constexpr const char* kTag = "BetaNetwork";
constexpr const char* kNvsNamespace = "wifi";
constexpr const char* kSsidKey = "ssid";
constexpr const char* kPasswordKey = "password";
constexpr const char* kOpenAiNvsNamespace = "openai";
constexpr const char* kOpenAiApiKeyKey = "api_key";
constexpr EventBits_t kConnectedBit = BIT0;
constexpr EventBits_t kFailedBit = BIT1;
constexpr int kMaxRetries = 8;
constexpr int kProbeTimeoutMs = 15000;

std::mutex s_mutex;
Snapshot s_snapshot = {};
EventGroupHandle_t s_wifi_events = nullptr;
httpd_handle_t s_server = nullptr;

std::atomic<bool> s_manual_transition{false};
bool s_wifi_started = false;
bool s_sta_enabled = false;
bool s_ap_enabled = false;
bool s_sta_connected = false;
int s_retry_count = 0;
std::string s_station_ssid;
std::string s_station_password;
std::string s_ap_name;

bool LoadString(nvs_handle_t handle, const char* key, std::string* out)
{
    size_t len = 0;
    if (nvs_get_str(handle, key, nullptr, &len) != ESP_OK || len <= 1) {
        out->clear();
        return false;
    }
    std::string value(len, '\0');
    if (nvs_get_str(handle, key, value.data(), &len) != ESP_OK) {
        out->clear();
        return false;
    }
    if (!value.empty() && value.back() == '\0') value.pop_back();
    *out = std::move(value);
    return !out->empty();
}

bool LoadCredentials(std::string* ssid, std::string* password)
{
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    const bool ok = LoadString(handle, kSsidKey, ssid);
    (void)LoadString(handle, kPasswordKey, password);
    nvs_close(handle);
    return ok;
}

bool SaveCredentials(const std::string& ssid, const std::string& password)
{
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(handle, kSsidKey, ssid.c_str());
    if (err == ESP_OK) err = nvs_set_str(handle, kPasswordKey, password.c_str());
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

bool SaveOpenAiKey(const std::string& api_key)
{
    nvs_handle_t handle = 0;
    if (nvs_open(kOpenAiNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(handle, kOpenAiApiKeyKey, api_key.c_str());
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

bool HasStoredOpenAiKey()
{
    nvs_handle_t handle = 0;
    if (nvs_open(kOpenAiNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    std::string key;
    const bool ok = LoadString(handle, kOpenAiApiKeyKey, &key);
    nvs_close(handle);
    return ok;
}

std::string UrlDecode(const char* input)
{
    std::string out;
    if (!input) return out;
    for (size_t i = 0; input[i] != '\0'; ++i) {
        if (input[i] == '+') {
            out.push_back(' ');
        } else if (input[i] == '%' &&
                   input[i + 1] != '\0' && input[i + 2] != '\0' &&
                   std::isxdigit(static_cast<unsigned char>(input[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(input[i + 2]))) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return c - 'a' + 10;
            };
            out.push_back(static_cast<char>(
                (hex(input[i + 1]) << 4) | hex(input[i + 2])));
            i += 2;
        } else {
            out.push_back(input[i]);
        }
    }
    return out;
}

void PublishSnapshot(const char* status)
{
    Snapshot snap = {};
    snap.wifi_enabled = s_sta_enabled;
    snap.ap_enabled = s_ap_enabled;
    snap.ap_name = s_ap_enabled ? s_ap_name : "";
    snap.current_ssid = s_sta_connected ? s_station_ssid : "";
    snap.status = status ? status : "";

    if (s_sta_connected) {
        snap.mode = Mode::kConnected;
    } else if (s_ap_enabled) {
        snap.mode = Mode::kProvisioning;
    } else {
        snap.mode = Mode::kDisconnected;
    }

    std::lock_guard<std::mutex> lock(s_mutex);
    snap.http_status = s_snapshot.http_status;
    snap.probe_error = s_snapshot.probe_error;
    s_snapshot = std::move(snap);
}

void StopHttpServer()
{
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = nullptr;
}

esp_err_t RootHandler(httpd_req_t* req)
{
    const bool has_key = HasStoredOpenAiKey();
    std::string page =
        "<!doctype html><html><head>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>CLANKER Pocket Setup</title>"
        "<style>body{font-family:sans-serif;max-width:520px;margin:32px auto;padding:0 18px}"
        "input{font-size:16px;width:100%;box-sizing:border-box;padding:10px;margin-top:5px}"
        "button{font-size:16px;padding:12px 18px}</style></head><body>"
        "<h1>CLANKER Pocket Setup</h1>"
        "<p>Update Wi-Fi and OpenAI settings, then the Pocket will reboot.</p>"
        "<form action='/save' method='post'>"
        "<label>Wi-Fi SSID<input name='ssid' required maxlength='32'></label><br><br>"
        "<label>Wi-Fi Password<input name='password' type='password' maxlength='64'></label><br><br>"
        "<label>OpenAI API Key<input name='openai' type='password' maxlength='256'";

    if (!has_key) page += " required";
    page += "></label>";
    page += has_key
        ? "<p><small>An API key is already stored. Leave this blank to keep it.</small></p>"
        : "<p><small>An OpenAI API key is required the first time.</small></p>";
    page += "<button type='submit'>Save & Reboot</button></form></body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page.c_str(), page.size());
}

esp_err_t SaveHandler(httpd_req_t* req)
{
    const size_t body_len = req->content_len;
    if (body_len == 0 || body_len > 1024) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing settings");
    }

    std::string body(body_len, '\0');
    size_t received = 0;
    while (received < body_len) {
        const int got = httpd_req_recv(
            req, body.data() + received, body_len - received);
        if (got <= 0) {
            return httpd_resp_send_err(
                req, HTTPD_400_BAD_REQUEST, "Invalid settings");
        }
        received += static_cast<size_t>(got);
    }

    char ssid_raw[160] = {};
    char password_raw[256] = {};
    char openai_raw[384] = {};
    if (httpd_query_key_value(
            body.c_str(), "ssid", ssid_raw, sizeof(ssid_raw)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
    }
    (void)httpd_query_key_value(
        body.c_str(), "password", password_raw, sizeof(password_raw));
    (void)httpd_query_key_value(
        body.c_str(), "openai", openai_raw, sizeof(openai_raw));

    const std::string ssid = UrlDecode(ssid_raw);
    const std::string password = UrlDecode(password_raw);
    const std::string openai = UrlDecode(openai_raw);

    if (ssid.empty() || ssid.size() > 32 || password.size() > 64 ||
        openai.size() > 256) {
        return httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST, "Invalid settings");
    }
    if (openai.empty() && !HasStoredOpenAiKey()) {
        return httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST, "OpenAI API key required");
    }

    if (!SaveCredentials(ssid, password)) {
        return httpd_resp_send_err(
            req, HTTPD_500_INTERNAL_SERVER_ERROR, "Wi-Fi save failed");
    }
    if (!openai.empty() && !SaveOpenAiKey(openai)) {
        return httpd_resp_send_err(
            req, HTTPD_500_INTERNAL_SERVER_ERROR, "API key save failed");
    }

    static constexpr char kSaved[] =
        "<html><body><h2>Saved.</h2>"
        "<p>CLANKER Pocket is rebooting with the new settings.</p></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, kSaved, HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t StartHttpServer()
{
    if (s_server) return ESP_OK;

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(
        httpd_start(&s_server, &server_config), kTag, "HTTP server");

    httpd_uri_t root = {
        .uri="/",
        .method=HTTP_GET,
        .handler=RootHandler,
        .user_ctx=nullptr
    };
    httpd_uri_t save = {
        .uri="/save",
        .method=HTTP_POST,
        .handler=SaveHandler,
        .user_ctx=nullptr
    };

    esp_err_t err = httpd_register_uri_handler(s_server, &root);
    if (err == ESP_OK) err = httpd_register_uri_handler(s_server, &save);
    if (err != ESP_OK) {
        StopHttpServer();
        return err;
    }
    return ESP_OK;
}

void EnsureApName()
{
    if (!s_ap_name.empty()) return;
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_name[32] = {};
    std::snprintf(ap_name, sizeof(ap_name), "CLANKERBETA-%02X%02X%02X",
                  mac[3], mac[4], mac[5]);
    s_ap_name = ap_name;
}

esp_err_t ConfigureAp()
{
    EnsureApName();

    wifi_config_t ap = {};
    std::strncpy(reinterpret_cast<char*>(ap.ap.ssid),
                 s_ap_name.c_str(), sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = static_cast<uint8_t>(s_ap_name.size());
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    return esp_wifi_set_config(WIFI_IF_AP, &ap);
}

esp_err_t ConfigureStation()
{
    if (s_station_ssid.empty() &&
        !LoadCredentials(&s_station_ssid, &s_station_password)) {
        return ESP_ERR_NOT_FOUND;
    }

    wifi_config_t sta = {};
    std::strncpy(reinterpret_cast<char*>(sta.sta.ssid),
                 s_station_ssid.c_str(), sizeof(sta.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(sta.sta.password),
                 s_station_password.c_str(), sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    return esp_wifi_set_config(WIFI_IF_STA, &sta);
}

esp_err_t ApplyRadioState()
{
    s_manual_transition.store(true);
    s_retry_count = 0;
    s_sta_connected = false;

    if (s_wifi_events) {
        xEventGroupClearBits(s_wifi_events, kConnectedBit | kFailedBit);
    }

    if (!s_ap_enabled) StopHttpServer();

    if (s_wifi_started) {
        (void)esp_wifi_disconnect();
        (void)esp_wifi_stop();
        s_wifi_started = false;
    }

    if (!s_sta_enabled && !s_ap_enabled) {
        s_manual_transition.store(false);
        PublishSnapshot("RADIO OFF");
        return ESP_OK;
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    if (s_sta_enabled && s_ap_enabled) mode = WIFI_MODE_APSTA;
    else if (s_sta_enabled) mode = WIFI_MODE_STA;
    else mode = WIFI_MODE_AP;

    esp_err_t err = esp_wifi_set_mode(mode);
    if (err != ESP_OK) {
        s_manual_transition.store(false);
        PublishSnapshot("WIFI MODE ERROR");
        return err;
    }

    if (s_sta_enabled) {
        err = ConfigureStation();
        if (err != ESP_OK) {
            s_sta_enabled = false;
            if (!s_ap_enabled) {
                s_manual_transition.store(false);
                PublishSnapshot("NO WIFI SETTINGS");
                return err;
            }
            mode = WIFI_MODE_AP;
            ESP_RETURN_ON_ERROR(
                esp_wifi_set_mode(mode), kTag, "fallback AP mode");
        }
    }

    if (s_ap_enabled) {
        err = ConfigureAp();
        if (err != ESP_OK) {
            s_manual_transition.store(false);
            PublishSnapshot("AP CONFIG ERROR");
            return err;
        }
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        s_manual_transition.store(false);
        PublishSnapshot("WIFI START ERROR");
        return err;
    }
    s_wifi_started = true;
    s_manual_transition.store(false);

    if (s_ap_enabled) {
        err = StartHttpServer();
        if (err != ESP_OK) {
            PublishSnapshot("AP SERVER ERROR");
            return err;
        }
        ESP_LOGI(kTag, "Setup AP ready: %s", s_ap_name.c_str());
    }

    if (s_sta_enabled) PublishSnapshot("WIFI CONNECTING");
    else PublishSnapshot("SETUP AP ON");
    return ESP_OK;
}

void WifiEventHandler(void*, esp_event_base_t event_base, int32_t event_id, void*)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_sta_enabled) (void)esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        if (s_manual_transition.load() || !s_sta_enabled) {
            PublishSnapshot(s_ap_enabled ? "SETUP AP ON" : "WIFI OFF");
            return;
        }

        if (s_retry_count < kMaxRetries) {
            ++s_retry_count;
            PublishSnapshot("WIFI CONNECTING");
            (void)esp_wifi_connect();
        } else {
            PublishSnapshot("WIFI DISCONNECTED");
            if (s_wifi_events) xEventGroupSetBits(s_wifi_events, kFailedBit);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        s_sta_connected = true;
        PublishSnapshot("WIFI CONNECTED");
        if (s_wifi_events) xEventGroupSetBits(s_wifi_events, kConnectedBit);
    }
}

}  // namespace

esp_err_t Init()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, kTag, "NVS init");
    ESP_RETURN_ON_ERROR(esp_netif_init(), kTag, "netif init");

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) return ESP_ERR_NO_MEM;

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), kTag, "Wi-Fi init");

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEventHandler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiEventHandler, nullptr));

    s_station_ssid.clear();
    s_station_password.clear();
    const bool have_credentials =
        LoadCredentials(&s_station_ssid, &s_station_password);

    s_sta_enabled = have_credentials;
    s_ap_enabled = !have_credentials;

    ESP_RETURN_ON_ERROR(ApplyRadioState(), kTag, "initial radio state");

    if (!s_sta_enabled) return ESP_OK;

    const EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, kConnectedBit | kFailedBit,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));

    if ((bits & kConnectedBit) != 0) return ESP_OK;

    ESP_LOGW(kTag, "STA connect failed; enabling setup AP");
    s_ap_enabled = true;
    return ApplyRadioState();
}

esp_err_t SetWifiEnabled(bool enabled)
{
    if (enabled == s_sta_enabled) return ESP_OK;

    if (enabled) {
        s_station_ssid.clear();
        s_station_password.clear();
        if (!LoadCredentials(&s_station_ssid, &s_station_password)) {
            PublishSnapshot("NO WIFI SETTINGS");
            return ESP_ERR_NOT_FOUND;
        }
    }

    s_sta_enabled = enabled;
    return ApplyRadioState();
}

esp_err_t SetProvisioningEnabled(bool enabled)
{
    if (enabled == s_ap_enabled) return ESP_OK;
    s_ap_enabled = enabled;
    return ApplyRadioState();
}

esp_err_t StartProvisioning()
{
    return SetProvisioningEnabled(true);
}

Snapshot GetSnapshot()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_snapshot;
}

void RunHttpsProbe()
{
    Snapshot snap = GetSnapshot();
    if (snap.mode != Mode::kConnected) return;

    esp_http_client_config_t cfg = {};
    cfg.url = "https://example.com/";
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = kProbeTimeoutMs;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snap.probe_error = "CLIENT INIT FAILED";
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot = snap;
        return;
    }

    const esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        snap.http_status = esp_http_client_get_status_code(client);
        snap.probe_error.clear();
    } else {
        snap.http_status = 0;
        snap.probe_error = esp_err_to_name(err);
    }
    esp_http_client_cleanup(client);

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        // Keep live radio state that may have changed during the probe.
        s_snapshot.http_status = snap.http_status;
        s_snapshot.probe_error = snap.probe_error;
    }

    ESP_LOGI(kTag, "HTTPS probe: err=%s status=%d",
             esp_err_to_name(err), snap.http_status);
}

}  // namespace beta_network
