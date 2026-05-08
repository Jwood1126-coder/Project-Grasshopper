#include "net_relay.h"

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "proto_gen.h"
#include "sdkconfig.h"

#ifndef GRASSHOPPER_FW_VERSION
#define GRASSHOPPER_FW_VERSION "0.1.0-dev"
#endif

static const char *TAG = "net_relay";
static esp_websocket_client_handle_t s_client = NULL;
static volatile bool s_connected = false;

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

static void on_event(void *arg, esp_event_base_t base, int32_t event_id,
                      void *event_data) {
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "connected");
            s_connected = true;
            send_hello();
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "disconnected");
            s_connected = false;
            break;
        case WEBSOCKET_EVENT_DATA:
            if (ev->op_code == 0x01 || ev->op_code == 0x02) {
                ESP_LOGI(TAG, "rx %d bytes", ev->data_len);
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
