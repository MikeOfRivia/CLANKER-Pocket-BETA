#ifndef BETA_NETWORK_H_
#define BETA_NETWORK_H_

#include <string>
#include "esp_err.h"

namespace beta_network {

enum class Mode {
    kDisconnected,
    kProvisioning,
    kConnected,
};

struct Snapshot {
    Mode mode = Mode::kDisconnected;
    std::string status;
    std::string ap_name;
    std::string current_ssid;
    bool wifi_enabled = false;
    bool ap_enabled = false;
    int http_status = 0;
    std::string probe_error;
};

esp_err_t Init();
esp_err_t StartProvisioning();
esp_err_t SetWifiEnabled(bool enabled);
esp_err_t SetProvisioningEnabled(bool enabled);
Snapshot GetSnapshot();
void RunHttpsProbe();

}  // namespace beta_network

#endif
