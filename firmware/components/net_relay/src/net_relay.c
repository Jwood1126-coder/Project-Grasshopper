#include "net_relay.h"

#include <string.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hal_lepton.h"
#include "proto_gen.h"
#include "sdkconfig.h"

#ifndef GRASSHOPPER_FW_VERSION
#define GRASSHOPPER_FW_VERSION "0.1.0-dev"
#endif

static const char *TAG = "net_relay";
static esp_websocket_client_handle_t s_client = NULL;
static volatile bool s_connected = false;
static net_relay_cmd_handler_t s_cmd_handler = NULL;

// ---- Inbound message buffer ----
//
// Bun fragments large WS messages on the relay side, so we have to be
// able to reassemble incoming text frames. Practical inbound JSON is
// small (cmd payloads), so 4 KB is plenty.
#define INBOUND_BUF_SIZE 4096
static char s_inbound_buf[INBOUND_BUF_SIZE];
static size_t s_inbound_len = 0;

static void send_hello(void) {
    Hello_t h = {
        .type = "hello",
        .deviceId = CONFIG_GRASSHOPPER_DEVICE_ID,
        .token = CONFIG_GRASSHOPPER_RELAY_TOKEN,
        .fwVersion = GRASSHOPPER_FW_VERSION,
        .gitSha = "dev",
        .bootReason = "cold",
    };
    char buf[512];
    size_t n = Hello_to_json(buf, sizeof(buf), &h);
    if (n > 0 && n < sizeof(buf)) {
        esp_websocket_client_send_text(s_client, buf, (int)n,
                                        pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "hello sent (%u B)", (unsigned)n);
    }
}

// ---- Command result emit ----
//
// Every command returns a `cmd.result` event back to the relay. UI
// matches the `id` field to its outstanding command so retries over a
// flaky link don't produce stale state.
//
// Built with cJSON so the msg field is properly escaped. Earlier snprintf
// version was fine for fixed messages but unsafe once SD paths, file
// names, or user labels could appear in the msg (a single embedded
// quote or backslash would corrupt the JSON envelope). cJSON also lets
// us attach an optional structured `data` object for future use (e.g.
// returning the new session id alongside capture.now's success message).
static void send_cmd_result(const char *cmd_type, const char *cmd_id,
                             bool ok, const char *msg) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "kind", "cmd.result");
    cJSON_AddStringToObject(root, "id",   cmd_id   ? cmd_id   : "");
    cJSON_AddStringToObject(root, "cmd",  cmd_type ? cmd_type : "");
    cJSON_AddBoolToObject(root,   "ok",   ok);
    cJSON_AddStringToObject(root, "msg",  msg      ? msg      : "");
    cJSON_AddNumberToObject(root, "ts",   (double)(esp_timer_get_time() / 1000));

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    if (s_connected) {
        size_t len = strlen(json);
        esp_websocket_client_send_text(s_client, json, (int)len, pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "cmd.result %s id=%s %s msg=\"%s\"",
                 cmd_type, cmd_id, ok ? "OK" : "FAIL", msg);
    }
    free(json);
}

// ---- Command dispatch ----
//
// Recognized commands (Phase 2):
//   thermal.ffc    — call hal_lepton_run_ffc()
//   device.reboot  — esp_restart() after sending result
//   capture.now    — STUB (Phase 3 needs SD session backend)
//   timelapse.start, timelapse.stop — STUB (Phase 3)
//   ping           — health-check, always OK
//
// Each command MUST emit exactly one result event referencing the cmd id.
static void dispatch_cmd(const cJSON *root) {
    const cJSON *cmd_field = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    const cJSON *id_field  = cJSON_GetObjectItemCaseSensitive(root, "id");
    const char *cmd = cJSON_IsString(cmd_field) ? cmd_field->valuestring : "";
    const char *id  = cJSON_IsString(id_field)  ? id_field->valuestring  : "";

    if (!cmd || !*cmd) {
        send_cmd_result("(unknown)", id, false, "missing cmd field");
        return;
    }

    ESP_LOGI(TAG, "cmd received: %s id=%s", cmd, id);

    if (strcmp(cmd, "ping") == 0) {
        send_cmd_result(cmd, id, true, "pong");
        return;
    }

    if (strcmp(cmd, "thermal.ffc") == 0) {
        esp_err_t err = hal_lepton_run_ffc();
        if (err == ESP_OK) {
            send_cmd_result(cmd, id, true, "FFC executed");
        } else {
            send_cmd_result(cmd, id, false, esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(cmd, "device.reboot") == 0) {
        // Send result FIRST, then reboot. Brief delay so the WS frame
        // makes it onto the wire before esp_restart kills the radio.
        send_cmd_result(cmd, id, true, "rebooting in 500 ms");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return;  // unreachable
    }

    // Forward anything else to the registered handler (capture.now,
    // timelapse.start/stop, settings.update, etc.). Handler signature
    // takes the payload as a void* to keep cJSON out of the public header.
    if (s_cmd_handler) {
        char msg[160] = {0};
        bool ok = s_cmd_handler(cmd, id, root, msg, sizeof(msg));
        send_cmd_result(cmd, id, ok, msg);
        return;
    }

    send_cmd_result(cmd, id, false, "unknown command (no handler registered)");
}

static void handle_text_frame(const char *data, size_t len) {
    if (len == 0 || len >= INBOUND_BUF_SIZE - 1) {
        ESP_LOGW(TAG, "rx text len=%u out of range", (unsigned)len);
        return;
    }
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "rx text: invalid JSON (%u B)", (unsigned)len);
        return;
    }
    const cJSON *type_field = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *type = cJSON_IsString(type_field) ? type_field->valuestring : "";
    if (strcmp(type, "cmd") == 0) {
        dispatch_cmd(root);
    } else {
        ESP_LOGI(TAG, "rx text type=\"%s\" (no handler)", type);
    }
    cJSON_Delete(root);
}

static void on_event(void *arg, esp_event_base_t base, int32_t event_id,
                      void *event_data) {
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "connected");
            s_connected = true;
            s_inbound_len = 0;
            send_hello();
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "disconnected");
            s_connected = false;
            s_inbound_len = 0;
            break;
        case WEBSOCKET_EVENT_DATA:
            // op_code 0x01 = text, 0x02 = binary, 0x00 = continuation,
            // 0x09 = ping, 0x0A = pong. We only care about text for cmd.
            if (ev->op_code == 0x01 || ev->op_code == 0x00) {
                if (ev->data_len > 0 && ev->data_ptr) {
                    if (s_inbound_len + ev->data_len < INBOUND_BUF_SIZE) {
                        memcpy(s_inbound_buf + s_inbound_len, ev->data_ptr, ev->data_len);
                        s_inbound_len += ev->data_len;
                    } else {
                        ESP_LOGW(TAG, "inbound buffer overflow, dropping");
                        s_inbound_len = 0;
                    }
                }
                // ev->payload_offset + ev->data_len == payload_len → final fragment
                if (ev->payload_offset + ev->data_len >= ev->payload_len) {
                    if (s_inbound_len > 0) {
                        handle_text_frame(s_inbound_buf, s_inbound_len);
                    }
                    s_inbound_len = 0;
                }
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "ws error");
            break;
        default:
            break;
    }
}

esp_err_t net_relay_start(void) {
    esp_websocket_client_config_t cfg = {
        .uri = CONFIG_GRASSHOPPER_RELAY_URL,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .task_stack = 6144,
        // Sized for VGA JPEG previews (~30-50 KB at q=12) plus our 24 B
        // header. esp_websocket_client_send_bin() chunks larger payloads
        // automatically, but a single big buffer is simpler.
        .buffer_size = 65536,
        // ---- Connection liveness ----
        // iPhone hotspot NAT silently drops idle TCP connections after
        // ~60-180 s. Without these, the WS appears alive until the next
        // send, then closes with code 1006. Three layers:
        //
        //   1. ping_interval_sec=15  — application-level WS PING every
        //      15 s; provokes a PONG so the NAT keeps the mapping warm.
        //   2. pingpong_timeout_sec=30 — if the device doesn't get a
        //      PONG within 30 s, drop and reconnect (don't sit there
        //      thinking we're connected).
        //   3. keep_alive_*            — TCP-layer keepalives so the
        //      kernel notices a dead peer even between WS frames.
        .ping_interval_sec       = 15,
        .pingpong_timeout_sec    = 30,
        .keep_alive_enable       = true,
        .keep_alive_idle         = 30,
        .keep_alive_interval     = 10,
        .keep_alive_count        = 3,
    };
    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) return ESP_FAIL;

    ESP_ERROR_CHECK(esp_websocket_register_events(
        s_client, WEBSOCKET_EVENT_ANY, on_event, NULL));
    return esp_websocket_client_start(s_client);
}

esp_err_t net_relay_send(const char *json, size_t len) {
    if (!s_connected || !s_client) return ESP_ERR_INVALID_STATE;
    int sent = esp_websocket_client_send_text(s_client, json, (int)len,
                                              pdMS_TO_TICKS(1000));
    return sent > 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t net_relay_send_binary(const void *buf, size_t len) {
    if (!s_connected || !s_client) return ESP_ERR_INVALID_STATE;
    int sent = esp_websocket_client_send_bin(s_client, (const char *)buf,
                                              (int)len, pdMS_TO_TICKS(2000));
    return sent > 0 ? ESP_OK : ESP_FAIL;
}

bool net_relay_is_connected(void) { return s_connected; }

void net_relay_register_cmd_handler(net_relay_cmd_handler_t fn) {
    s_cmd_handler = fn;
}
