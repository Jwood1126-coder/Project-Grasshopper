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

// ---- Command handler registration ----
//
// net_relay handles a small set of built-in commands itself (ping,
// thermal.ffc, device.reboot). Anything else is forwarded to a handler
// registered by main, so capture/timelapse logic doesn't have to live
// inside the relay component.
//
// Handler signature:
//   - cmd       — the command name (e.g. "capture.now")
//   - id        — the original command id (for matching back results)
//   - payload   — the full inbound JSON message; handler can pull fields
//                 with cJSON if it links cJSON. Type erased here so
//                 net_relay's public header doesn't drag cJSON in.
//   - msg_out   — caller-owned buffer for the human-readable result msg
//   - msg_cap   — capacity of msg_out
//
// Returns:
//   NET_RELAY_CMD_OK       — synchronous success; dispatcher emits cmd.result
//   NET_RELAY_CMD_FAIL     — synchronous failure; dispatcher emits cmd.result
//   NET_RELAY_CMD_DEFERRED — handler took ownership; it must call
//                            net_relay_emit_cmd_result() once work completes
//                            (used for SD scans / file reads on a worker)
typedef enum {
    NET_RELAY_CMD_OK = 0,
    NET_RELAY_CMD_FAIL = 1,
    NET_RELAY_CMD_DEFERRED = 2,
} net_relay_cmd_status_t;

typedef net_relay_cmd_status_t (*net_relay_cmd_handler_t)(
    const char *cmd, const char *id,
    const void *payload_json_root,
    char *msg_out, size_t msg_cap);

void net_relay_register_cmd_handler(net_relay_cmd_handler_t fn);

// Emit a cmd.result event from anywhere (including a worker task).
// `msg` is the human-readable result; `data_json` is an optional
// already-serialized JSON value (object/array) embedded as the "data"
// field — pass NULL to omit. Caller retains ownership of all strings.
// Drops silently if the WS is not connected.
void net_relay_emit_cmd_result(const char *cmd, const char *id, bool ok,
                                const char *msg, const char *data_json);

#ifdef __cplusplus
}
#endif
