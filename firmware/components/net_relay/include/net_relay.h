#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Open the outbound WSS connection to CONFIG_GRASSHOPPER_RELAY_URL.
// Sends a Hello automatically on connect using the configured device id
// and token. Reconnects forever with exponential backoff handled by the
// underlying esp_websocket_client.
esp_err_t net_relay_start(void);

// Send a JSON envelope to the relay. Drops if not connected. Thread-safe.
esp_err_t net_relay_send(const char *json, size_t len);

// Send a binary frame (preview image, etc) over the same WS connection.
// Caller frames its own header in the payload — the relay routes binary
// frames separately from JSON. Drops if not connected. Thread-safe.
esp_err_t net_relay_send_binary(const void *buf, size_t len);

bool net_relay_is_connected(void);

#ifdef __cplusplus
}
#endif
