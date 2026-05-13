// Visible-light camera HAL — see hal_camera.h.
//
// Notes:
//  - JPEG mode requires PSRAM-backed frame buffers; the sensor outputs
//    JPEG directly so no CPU encode is needed.
//  - CAMERA_GRAB_LATEST + fb_count=2 lets the driver overwrite stale
//    frames; the consumer always sees the freshest one (avoids a queue
//    of stale frames piling up if the consumer is slow).

#include "hal_camera.h"
#include "board_pins.h"

#include <string.h>

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "hal_cam";

static bool s_ready = false;
static SemaphoreHandle_t s_mutex = NULL;
static camera_fb_t *s_held_fb = NULL;

static hal_camera_stats_t s_stats = {0};
static uint32_t s_last_frame_count = 0;
static int64_t  s_last_fps_ms = 0;
static bool     s_hmirror = false;
static bool     s_vflip   = false;

static framesize_t map_framesize(hal_cam_framesize_t fs) {
    switch (fs) {
    case HAL_CAM_FRAMESIZE_QVGA: return FRAMESIZE_QVGA;
    case HAL_CAM_FRAMESIZE_VGA:  return FRAMESIZE_VGA;
    case HAL_CAM_FRAMESIZE_SVGA: return FRAMESIZE_SVGA;
    case HAL_CAM_FRAMESIZE_XGA:  return FRAMESIZE_XGA;
    case HAL_CAM_FRAMESIZE_HD:   return FRAMESIZE_HD;
    case HAL_CAM_FRAMESIZE_UXGA: return FRAMESIZE_UXGA;
    default:                     return FRAMESIZE_VGA;
    }
}

static void framesize_dims(framesize_t fs, uint32_t *w, uint32_t *h) {
    switch (fs) {
    case FRAMESIZE_QVGA: *w = 320;  *h = 240;  break;
    case FRAMESIZE_VGA:  *w = 640;  *h = 480;  break;
    case FRAMESIZE_SVGA: *w = 800;  *h = 600;  break;
    case FRAMESIZE_XGA:  *w = 1024; *h = 768;  break;
    case FRAMESIZE_HD:   *w = 1280; *h = 720;  break;
    case FRAMESIZE_UXGA: *w = 1600; *h = 1200; break;
    default:             *w = 0;    *h = 0;    break;
    }
}

esp_err_t hal_camera_init(const hal_camera_cfg_t *cfg, char *out_sensor_name) {
    if (s_ready) return ESP_OK;
    if (!cfg)    return ESP_ERR_INVALID_ARG;

    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    framesize_t fs = map_framesize(cfg->framesize);

    camera_config_t conf = {
        .pin_pwdn       = CAM_PIN_PWDN,
        .pin_reset      = CAM_PIN_RESET,
        .pin_xclk       = CAM_PIN_XCLK,
        .pin_sccb_sda   = CAM_PIN_SIOD,
        .pin_sccb_scl   = CAM_PIN_SIOC,
        .pin_d7         = CAM_PIN_D7,
        .pin_d6         = CAM_PIN_D6,
        .pin_d5         = CAM_PIN_D5,
        .pin_d4         = CAM_PIN_D4,
        .pin_d3         = CAM_PIN_D3,
        .pin_d2         = CAM_PIN_D2,
        .pin_d1         = CAM_PIN_D1,
        .pin_d0         = CAM_PIN_D0,
        .pin_vsync      = CAM_PIN_VSYNC,
        .pin_href       = CAM_PIN_HREF,
        .pin_pclk       = CAM_PIN_PCLK,
        .xclk_freq_hz   = 20000000,
        .ledc_timer     = LEDC_TIMER_0,
        .ledc_channel   = LEDC_CHANNEL_0,
        .pixel_format   = PIXFORMAT_JPEG,
        .frame_size     = fs,
        .jpeg_quality   = cfg->jpeg_quality,
        .fb_count       = cfg->fb_count > 0 ? cfg->fb_count : 2,
        .fb_location    = CAMERA_FB_IN_PSRAM,
        .grab_mode      = (cfg->fb_count > 1) ? CAMERA_GRAB_LATEST
                                              : CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
        const char *name = info ? info->name : "unknown";
        if (out_sensor_name) {
            strncpy(out_sensor_name, name, 31);
            out_sensor_name[31] = 0;
        }
        ESP_LOGI(TAG, "sensor=%s pid=0x%02x", name, s->id.PID);
    }

    uint32_t w = 0, h = 0;
    framesize_dims(fs, &w, &h);

    s_stats.ready = true;
    s_stats.width = w;
    s_stats.height = h;
    s_stats.jpeg_quality = cfg->jpeg_quality;
    s_last_fps_ms = esp_timer_get_time() / 1000;

    s_ready = true;
    ESP_LOGI(TAG, "init ok: %ux%u q=%d fb=%d", (unsigned)w, (unsigned)h,
             cfg->jpeg_quality, conf.fb_count);
    return ESP_OK;
}

bool hal_camera_ready(void) { return s_ready; }

esp_err_t hal_camera_grab_jpeg(const uint8_t **out_buf, size_t *out_len,
                                uint32_t *out_w, uint32_t *out_h) {
    if (!s_ready) return ESP_FAIL;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_held_fb) {
        // Caller forgot to release — return the held buffer to the pool
        // before grabbing a new one. Surfaces in logs as a misuse, but
        // we keep going so the stream doesn't deadlock.
        ESP_LOGW(TAG, "grab without release; auto-releasing");
        esp_camera_fb_return(s_held_fb);
        s_held_fb = NULL;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "esp_camera_fb_get returned NULL");
        return ESP_FAIL;
    }
    s_held_fb = fb;

    if (out_buf) *out_buf = fb->buf;
    if (out_len) *out_len = fb->len;
    if (out_w)   *out_w   = fb->width;
    if (out_h)   *out_h   = fb->height;

    s_stats.frames++;
    s_stats.width  = fb->width;
    s_stats.height = fb->height;
    return ESP_OK;
}

void hal_camera_release(void) {
    if (!s_ready) return;
    if (s_held_fb) {
        esp_camera_fb_return(s_held_fb);
        s_held_fb = NULL;
    }
    xSemaphoreGive(s_mutex);
}

void hal_camera_get_stats(hal_camera_stats_t *out) {
    if (!out) return;
    *out = s_stats;
}

esp_err_t hal_camera_set_hmirror(bool on) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->set_hmirror) return ESP_ERR_NOT_SUPPORTED;
    if (s->set_hmirror(s, on ? 1 : 0) != 0) return ESP_FAIL;
    s_hmirror = on;
    return ESP_OK;
}

esp_err_t hal_camera_set_vflip(bool on) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->set_vflip) return ESP_ERR_NOT_SUPPORTED;
    if (s->set_vflip(s, on ? 1 : 0) != 0) return ESP_FAIL;
    s_vflip = on;
    return ESP_OK;
}

bool hal_camera_get_hmirror(void) { return s_hmirror; }
bool hal_camera_get_vflip(void)   { return s_vflip; }

void hal_camera_tick_fps(void) {
    int64_t t = esp_timer_get_time() / 1000;
    int64_t dt = t - s_last_fps_ms;
    if (dt <= 0) return;
    uint32_t df = s_stats.frames - s_last_frame_count;
    s_stats.fps = (uint32_t)((df * 1000ULL) / (uint64_t)dt);
    s_last_frame_count = s_stats.frames;
    s_last_fps_ms = t;
}
