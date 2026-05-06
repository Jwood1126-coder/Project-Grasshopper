// Grasshopper — phase 2 entry point.
//
// Brings up Wi-Fi STA, optionally connects to the debug relay over WSS,
// and emits a Hello + Init then a periodic Tick of static state. There
// is no real Lepton, no real camera, no SD writer in this build — phase
// 3 onward layers those in via the hal_* and app_* components.

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "net_relay.h"
#include "net_wifi.h"
#include "proto_gen.h"
#include "sdkconfig.h"

#ifndef GRASSHOPPER_FW_VERSION
#define GRASSHOPPER_FW_VERSION "0.1.0-dev"
#endif

static const char *TAG = "app";

static int64_t g_boot_ms = 0;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static uint64_t uptime_ms(void) { return (uint64_t)(now_ms() - g_boot_ms); }

static void send_init(void) {
    Init_t init = {
        .type = "init",
        .fwVersion = GRASSHOPPER_FW_VERSION,
        .gitSha = "dev",
        .deviceId = CONFIG_GRASSHOPPER_DEVICE_ID,
        .state = DEVICESTATE_IDLE,
        .uptimeMs = uptime_ms(),
        .freeHeap = (uint32_t)esp_get_free_heap_size(),
        .freePsram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        .wifi = {
            .mode = "STA",
            .ssid = CONFIG_GRASSHOPPER_WIFI_SSID,
            .rssi = -50,
            .ip = "",
        },
        .ntpSynced = false,
        .epoch = 0,
        .visible = { .fps = 0, .w = 0, .h = 0, .quality = 0 },
        .thermal = { .fps = 0, .gain = "auto", .agc = false,
                     .spliceDetected = 0, .lastFFCMs = 0 },
    };

    char buf[1024];
    size_t n = Init_to_json(buf, sizeof(buf), &init);
    if (n > 0 && n < sizeof(buf)) {
        net_relay_send(buf, n);
        ESP_LOGI(TAG, "init sent (%u B)", (unsigned)n);
    } else {
        ESP_LOGE(TAG, "init buffer too small (n=%u)", (unsigned)n);
    }
}

static void tick_task(void *arg) {
    char buf[1024];
    Tick_t tick = {
        .type = "tick",
        .thermal = { .fps = 9, .gain = "auto", .agc = false,
                     .spliceDetected = 0, .lastFFCMs = 0 },
    };

    bool sent_init = false;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1500));

        if (!net_relay_is_connected()) {
            sent_init = false;
            continue;
        }
        if (!sent_init) {
            send_init();
            sent_init = true;
            continue;
        }

        tick.uptimeMs = uptime_ms();
        tick.freeHeap = (uint32_t)esp_get_free_heap_size();
        tick.freePsram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        tick.epoch = (uint64_t)time(NULL);
        tick.state = DEVICESTATE_IDLE;
        size_t n = Tick_to_json(buf, sizeof(buf), &tick);
        if (n > 0 && n < sizeof(buf)) {
            net_relay_send(buf, n);
        }
    }
}

void app_main(void) {
    g_boot_ms = now_ms();

    ESP_LOGI(TAG, "grasshopper " GRASSHOPPER_FW_VERSION " booting");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (strlen(CONFIG_GRASSHOPPER_WIFI_SSID) > 0) {
        ESP_ERROR_CHECK(net_wifi_init());
        net_wifi_connect_blocking(CONFIG_GRASSHOPPER_WIFI_SSID,
                                   CONFIG_GRASSHOPPER_WIFI_PASS);
    } else {
        ESP_LOGW(TAG, "no Wi-Fi SSID configured — skipping STA");
    }

#if CONFIG_GRASSHOPPER_RELAY_ENABLED
    ESP_LOGI(TAG, "starting relay → %s", CONFIG_GRASSHOPPER_RELAY_URL);
    ESP_ERROR_CHECK(net_relay_start());
#else
    ESP_LOGI(TAG, "relay disabled (CONFIG_GRASSHOPPER_RELAY_ENABLED=n)");
#endif

    xTaskCreate(tick_task, "tick", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "boot complete; free heap=%u psram=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
