// Over-the-air firmware update. Uses esp_https_ota to fetch the new
// binary, write it to the inactive OTA slot, and reboot. The new
// image starts in pending-verify state; ota_pending_verify_arm()
// installs a timer that marks the image valid after the relay has
// reconnected for a few seconds. Crashes / disconnects in that
// window leave the pending mark in place — the bootloader rolls
// back on next boot.

#include "ota.h"

#include <string.h>

#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "net_relay.h"

static const char *TAG = "ota";

typedef struct {
    char cmd_id[40];
    char url[256];
} ota_req_t;

static void emit_progress(const char *cmd_id, int pct, const char *msg) {
    char data[80];
    snprintf(data, sizeof(data), "{\"phase\":\"download\",\"pct\":%d}", pct);
    net_relay_emit_cmd_result("firmware.update", cmd_id, true, msg, data);
}

static void ota_task(void *arg) {
    ota_req_t *req = (ota_req_t *)arg;
    ESP_LOGI(TAG, "starting HTTPS OTA from %s", req->url);
    net_relay_emit_cmd_result("firmware.update", req->cmd_id, true,
                               "starting download", NULL);

    esp_http_client_config_t http = {
        .url = req->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota = {
        .http_config = &http,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota, &handle);
    if (err != ESP_OK) {
        char msg[120];
        snprintf(msg, sizeof(msg), "ota_begin failed: %s", esp_err_to_name(err));
        net_relay_emit_cmd_result("firmware.update", req->cmd_id, false, msg, NULL);
        free(req);
        vTaskDelete(NULL);
        return;
    }

    int total = esp_https_ota_get_image_size(handle);
    ESP_LOGI(TAG, "image size: %d bytes", total);

    int last_pct = -1;
    int loops = 0;
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int read = esp_https_ota_get_image_len_read(handle);
        int pct = (total > 0) ? (read * 100 / total) : 0;
        // Throttle progress events to ~every 5 % so we don't flood the WS.
        if (pct >= last_pct + 5) {
            char m[80];
            snprintf(m, sizeof(m), "downloaded %d%% (%d / %d B)",
                     pct, read, total);
            emit_progress(req->cmd_id, pct, m);
            last_pct = pct;
        }
        // Tiny yield so the watchdog stays happy.
        if (++loops % 16 == 0) vTaskDelay(1);
    }

    if (err != ESP_OK) {
        esp_https_ota_abort(handle);
        char msg[120];
        snprintf(msg, sizeof(msg), "ota_perform failed: %s", esp_err_to_name(err));
        net_relay_emit_cmd_result("firmware.update", req->cmd_id, false, msg, NULL);
        free(req);
        vTaskDelete(NULL);
        return;
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        esp_https_ota_abort(handle);
        net_relay_emit_cmd_result("firmware.update", req->cmd_id, false,
                                   "incomplete data received", NULL);
        free(req);
        vTaskDelete(NULL);
        return;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        char msg[120];
        snprintf(msg, sizeof(msg), "ota_finish failed: %s", esp_err_to_name(err));
        net_relay_emit_cmd_result("firmware.update", req->cmd_id, false, msg, NULL);
        free(req);
        vTaskDelete(NULL);
        return;
    }

    net_relay_emit_cmd_result("firmware.update", req->cmd_id, true,
                               "download complete — rebooting into new image",
                               "{\"phase\":\"reboot\",\"pct\":100}");
    ESP_LOGI(TAG, "OTA complete — rebooting in 1 s");
    free(req);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

esp_err_t ota_start_https(const char *cmd_id, const char *url) {
    if (!url || !*url) return ESP_ERR_INVALID_ARG;
    ota_req_t *req = calloc(1, sizeof(*req));
    if (!req) return ESP_ERR_NO_MEM;
    snprintf(req->cmd_id, sizeof(req->cmd_id), "%s", cmd_id ? cmd_id : "");
    snprintf(req->url, sizeof(req->url), "%s", url);
    // Pinned to core 0 alongside the other workers; OTA holds TCP
    // sockets + does flash writes, both of which we want away from
    // the VoSPI reader on core 1.
    BaseType_t r = xTaskCreatePinnedToCore(ota_task, "ota", 8192, req, 5, NULL, 0);
    if (r != pdPASS) { free(req); return ESP_FAIL; }
    return ESP_OK;
}

// ─── Pending-verify watchdog ──────────────────────────────────────
//
// On boot, esp_ota_get_state_partition() tells us if we're a fresh
// image awaiting validation. If yes, we delay marking the image as
// valid until the relay has been continuously connected for a while
// — proves the new firmware can do its job. If anything blows up
// before then (crash, watchdog, indefinite disconnect), the pending
// mark stays and the bootloader picks the previous slot next time.

static esp_timer_handle_t s_verify_timer = NULL;

static void verify_timer_cb(void *arg) {
    (void)arg;
    if (!net_relay_is_connected()) {
        ESP_LOGW(TAG, "verify timer fired but relay not connected — leaving pending");
        // Re-arm to check again in a few seconds.
        esp_timer_start_once(s_verify_timer, 5ULL * 1000 * 1000);
        return;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "image marked VALID — rollback cancelled");
    } else {
        ESP_LOGW(TAG, "mark_app_valid failed: %s", esp_err_to_name(err));
    }
}

void ota_pending_verify_arm(uint32_t confirm_ms) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return;
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) != ESP_OK) return;
    if (st != ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "image already %s — no verify needed",
                 (st == ESP_OTA_IMG_VALID) ? "valid" :
                 (st == ESP_OTA_IMG_INVALID) ? "invalid" : "?");
        return;
    }
    ESP_LOGW(TAG, "image in PENDING_VERIFY state — will mark valid once relay connects (%lu ms)",
             (unsigned long)confirm_ms);
    if (!s_verify_timer) {
        const esp_timer_create_args_t args = {
            .callback = verify_timer_cb,
            .name = "ota_verify",
        };
        if (esp_timer_create(&args, &s_verify_timer) != ESP_OK) return;
    }
    esp_timer_start_once(s_verify_timer, (uint64_t)confirm_ms * 1000);
}
