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
    int http_status = 0;
    std::string probe_error;
};

esp_err_t Init();
Snapshot GetSnapshot();
void RunHttpsProbe();

}  // namespace beta_network

#endif
