#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t net_wifi_init(void);

// Block until associated to the given SSID, or until 30s of retries
// have elapsed. Returns ESP_OK on success, ESP_FAIL on timeout.
esp_err_t net_wifi_connect_blocking(const char *ssid, const char *password);

bool net_wifi_is_connected(void);

// Live link info; rssi=0 means "not associated".
//   ip_buf must be ≥16 bytes (room for "255.255.255.255\0").
//   ssid_buf must be ≥33 bytes.
void net_wifi_get_link(char *ip_buf, size_t ip_len,
                       char *ssid_buf, size_t ssid_len,
                       int *rssi);

#ifdef __cplusplus
}
#endif
