#pragma once

// Visible-light camera HAL — wraps espressif/esp32-camera.
//
// Auto-detects OV2640 / OV5640 on the configured pin map (board_pins.h),
// configures JPEG output at the requested framesize/quality, and exposes
// a thread-safe latest-frame grab. Frame buffers live in PSRAM
// (CAMERA_FB_IN_PSRAM).

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HAL_CAM_FRAMESIZE_QVGA = 0, //  320x240
    HAL_CAM_FRAMESIZE_VGA  = 1, //  640x480
    HAL_CAM_FRAMESIZE_SVGA = 2, //  800x600
    HAL_CAM_FRAMESIZE_XGA  = 3, // 1024x768
    HAL_CAM_FRAMESIZE_HD   = 4, // 1280x720
    HAL_CAM_FRAMESIZE_UXGA = 5, // 1600x1200
} hal_cam_framesize_t;

typedef struct {
    hal_cam_framesize_t framesize;
    int      jpeg_quality; // 0 (best) – 63 (worst); recommend 8–14
    int      fb_count;     // 1 or 2; 2 enables CAMERA_GRAB_LATEST
} hal_camera_cfg_t;

// Bring the sensor up. Reads sensor name into `out_sensor_name` (≤32 B,
// nullable). Returns ESP_FAIL if no camera was detected.
esp_err_t hal_camera_init(const hal_camera_cfg_t *cfg, char *out_sensor_name);

// True after a successful hal_camera_init.
bool hal_camera_ready(void);

// Lock the latest JPEG frame. Caller MUST call hal_camera_release()
// before grabbing again. `*out_buf` and `*out_len` are valid until
// release. Returns ESP_FAIL if no frame is available or sensor isn't up.
esp_err_t hal_camera_grab_jpeg(const uint8_t **out_buf, size_t *out_len,
                                uint32_t *out_w, uint32_t *out_h);
void      hal_camera_release(void);

// Live status — populated by the grab path; safe to read from any task.
typedef struct {
    bool     ready;
    uint32_t width;
    uint32_t height;
    uint32_t jpeg_quality;
    uint32_t fps;        // smoothed; updated on hal_camera_tick_fps()
    uint32_t frames;     // monotonic since boot
} hal_camera_stats_t;

void hal_camera_get_stats(hal_camera_stats_t *out);

// Call from the tick task once per ~1.5 s to update the smoothed fps.
void hal_camera_tick_fps(void);

#ifdef __cplusplus
}
#endif
