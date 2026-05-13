#include "net_wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "net_wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          10

static EventGroupHandle_t s_wifi_group;
static int s_retry_num = 0;
static bool s_connected = false;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry %d/%d", s_retry_num, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_num = 0;
        s_connected = true;
        xEventGroupSetBits(s_wifi_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t net_wifi_init(void) {
    if (s_wifi_group) return ESP_OK;
    s_wifi_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    return ESP_OK;
}

esp_err_t net_wifi_connect_blocking(const char *ssid, const char *password) {
    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(30000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to %s", ssid);
        // iPhone hotspots are hostile to STA clients in modem-sleep
        // mode (the AP deauths devices that don't respond promptly to
        // its tight beacon cadence). Symptom: WS connection thrashes
        // with code=1006 every 5-15 s, preview fps drops to 0. Costs
        // ~80 mA extra current — fine for USB / external battery.
        esp_err_t pserr = esp_wifi_set_ps(WIFI_PS_NONE);
        if (pserr != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_set_ps(NONE) -> %s", esp_err_to_name(pserr));
        } else {
            ESP_LOGI(TAG, "WiFi power save: NONE (always-on)");
        }
        return ESP_OK;
    }
    ESP_LOGE(TAG, "failed to connect to %s", ssid);
    return ESP_FAIL;
}

bool net_wifi_is_connected(void) { return s_connected; }

void net_wifi_get_link(char *ip_buf, size_t ip_len,
                       char *ssid_buf, size_t ssid_len,
                       int *rssi) {
    if (ip_buf && ip_len)     ip_buf[0] = 0;
    if (ssid_buf && ssid_len) ssid_buf[0] = 0;
    if (rssi) *rssi = 0;
    if (!s_connected) return;

    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif && ip_buf && ip_len) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(nif, &info) == ESP_OK) {
            snprintf(ip_buf, ip_len, IPSTR, IP2STR(&info.ip));
        }
    }

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        if (ssid_buf && ssid_len) {
            strlcpy(ssid_buf, (const char *)ap.ssid, ssid_len);
        }
        if (rssi) *rssi = ap.rssi;
    }
}
