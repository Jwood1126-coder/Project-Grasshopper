#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t net_wifi_init(void);

// Block until associated to the given SSID, or until 30s of retries
// have elapsed. Returns ESP_OK on success, ESP_FAIL on timeout.
esp_err_t net_wifi_connect_blocking(const char *ssid, const char *password);

bool net_wifi_is_connected(void);

#ifdef __cplusplus
}
#endif
