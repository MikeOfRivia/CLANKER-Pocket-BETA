#include "beta_network.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>

#include "esp_crt_bundle.h"
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
constexpr EventBits_t kConnectedBit = BIT0;
constexpr EventBits_t kFailedBit = BIT1;
constexpr int kMaxRetries = 8;
constexpr int kProbeTimeoutMs = 15000;

std::mutex s_mutex;
Snapshot s_snapshot = {};
EventGroupHandle_t s_wifi_events = nullptr;
int s_retry_count = 0;
httpd_handle_t s_server = nullptr;

void SetSnapshot(const Snapshot& snapshot)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_snapshot = snapshot;
}

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

std::string UrlDecode(const char* input)
{
    std::string out;
    if (!input) return out;
    for (size_t i = 0; input[i] != '\0'; ++i) {
        if (input[i] == '+') {
            out.push_back(' ');
        } else if (input[i] == '%' &&
                   std::isxdigit(static_cast<unsigned char>(input[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(input[i + 2]))) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return c - 'a' + 10;
            };
            out.push_back(static_cast<char>((hex(input[i + 1]) << 4) | hex(input[i + 2])));
            i += 2;
        } else {
            out.push_back(input[i]);
        }
    }
    return out;
}

void WifiEventHandler(void*, esp_event_base_t event_base, int32_t event_id, void*)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < kMaxRetries) {
            ++s_retry_count;
            esp_wifi_connect();
        } else if (s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, kFailedBit);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        if (s_wifi_events) xEventGroupSetBits(s_wifi_events, kConnectedBit);
    }
}

esp_err_t RootHandler(httpd_req_t* req)
{
    static constexpr char kPage[] =
        "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>CLANKER Pocket BETA</title></head><body>"
        "<h1>CLANKER Pocket BETA</h1><p>Phase 3 Wi-Fi provisioning</p>"
        "<form action='/save' method='get'>"
        "<label>Wi-Fi SSID<br><input name='ssid' required></label><br><br>"
        "<label>Password<br><input name='password' type='password'></label><br><br>"
        "<button type='submit'>Save & Reboot</button></form></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, kPage, HTTPD_RESP_USE_STRLEN);
}

esp_err_t SaveHandler(httpd_req_t* req)
{
    const size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0 || query_len > 512) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing settings");
    }

    std::string query(query_len + 1, '\0');
    if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid settings");
    }

    char ssid_raw[160] = {};
    char password_raw[256] = {};
    if (httpd_query_key_value(query.c_str(), "ssid", ssid_raw, sizeof(ssid_raw)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
    }
    (void)httpd_query_key_value(query.c_str(), "password", password_raw, sizeof(password_raw));

    const std::string ssid = UrlDecode(ssid_raw);
    const std::string password = UrlDecode(password_raw);
    if (ssid.empty() || ssid.size() > 32 || password.size() > 64) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Wi-Fi settings");
    }
    if (!SaveCredentials(ssid, password)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
    }

    static constexpr char kSaved[] =
        "<html><body><h2>Saved.</h2><p>CLANKER Pocket is rebooting.</p></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, kSaved, HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t StartProvisioningAp()
{
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_name[32] = {};
    std::snprintf(ap_name, sizeof(ap_name), "ClankerBeta-%02X%02X%02X",
                  mac[3], mac[4], mac[5]);

    wifi_config_t ap = {};
    std::strncpy(reinterpret_cast<char*>(ap.ap.ssid), ap_name, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = static_cast<uint8_t>(std::strlen(ap_name));
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &server_config), kTag, "HTTP server");

    httpd_uri_t root = {.uri="/", .method=HTTP_GET, .handler=RootHandler, .user_ctx=nullptr};
    httpd_uri_t save = {.uri="/save", .method=HTTP_GET, .handler=SaveHandler, .user_ctx=nullptr};
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &save));

    Snapshot snap = {};
    snap.mode = Mode::kProvisioning;
    snap.status = "OPEN 192.168.4.1";
    snap.ap_name = ap_name;
    SetSnapshot(snap);
    ESP_LOGI(kTag, "Provisioning AP ready: %s", ap_name);
    return ESP_OK;
}

esp_err_t ConnectStation(const std::string& ssid, const std::string& password)
{
    wifi_config_t sta = {};
    std::strncpy(reinterpret_cast<char*>(sta.sta.ssid), ssid.c_str(), sizeof(sta.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(sta.sta.password), password.c_str(),
                 sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, kConnectedBit | kFailedBit,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    if ((bits & kConnectedBit) == 0) {
        ESP_LOGW(kTag, "STA connect failed; falling back to provisioning AP");
        esp_wifi_stop();
        return ESP_FAIL;
    }

    Snapshot snap = {};
    snap.mode = Mode::kConnected;
    snap.status = "WIFI CONNECTED";
    SetSnapshot(snap);
    ESP_LOGI(kTag, "Wi-Fi connected to %s", ssid.c_str());
    return ESP_OK;
}

}  // namespace

esp_err_t Init()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
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

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &WifiEventHandler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &WifiEventHandler, nullptr));

    std::string ssid;
    std::string password;
    if (!LoadCredentials(&ssid, &password)) {
        return StartProvisioningAp();
    }
    if (ConnectStation(ssid, password) == ESP_OK) {
        return ESP_OK;
    }
    return StartProvisioningAp();
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
        SetSnapshot(snap);
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
    SetSnapshot(snap);

    ESP_LOGI(kTag, "HTTPS probe: err=%s status=%d",
             esp_err_to_name(err), snap.http_status);
}

}  // namespace beta_network
