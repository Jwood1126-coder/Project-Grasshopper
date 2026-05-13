#pragma once

// Capture: single immediate captures + (later) timelapse sessions.
//
// SD layout (Fox-compatible per memory:lepton_breakout_damaged.md and
// the Fox session.json/captures.jsonl format):
//
//   /sdcard/timelapse/session_<id>/
//     ├── session.json       — metadata (intervalSec, captureCount, ...)
//     ├── captures.jsonl     — per-capture log (one JSON per line)
//     ├── 000001_vis.jpg     — visible JPEG, sequence 1
//     └── 000001_therm.jpg   — thermal colorized JPEG, sequence 1
//
// Phase 3 (this file) implements capture_now() — a single-capture
// "session of one". timelapse_start/stop come next on top of this.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Per-frame thermal stats. Codex called out the race where stats /
// raw / encode happen across separate mutex acquires; the atomic
// variant below populates all three from the same frame under one lock.
typedef struct {
    uint32_t min_raw;
    uint32_t max_raw;
    uint32_t center_raw;
    uint16_t resolution;   // active TLinear scale: 0 = 0.1K, 1 = 0.01K
    bool     valid;
} therm_frame_stats_t;

// Encode the latest committed thermal frame into a JPEG using the iron
// palette with per-frame auto-ranging. Output goes into `dst`, capacity
// `cap`. Returns the JPEG byte length, 0 on failure (no frame yet, or
// out of memory). Thread-safe (uses internal lazy-allocated buffers).
//
// Side effect: updates the last-seen temperature stats accessible via
// capture_get_last_thermal_temps_ck() — so calling this from preview
// is what keeps the tick's "temp" block fresh.
size_t capture_encode_thermal_jpeg(uint8_t *dst, size_t cap);

// Atomic variant: encode + raw snapshot + stats all from the SAME
// frame, under one mutex acquire. Use this whenever you'll later
// derive temps or write a .raw16 sidecar — guarantees the JPEG and
// the raw bytes describe the same Lepton commit.
//
// `stats_out`  — non-NULL: filled with min/max/center + resolution.
// `raw_out`    — non-NULL (≥ 38400 B): copy of the pre-rotation raw frame.
// Returns the JPEG byte length, 0 on failure (no frame yet, etc).
size_t capture_encode_thermal_jpeg_atomic(uint8_t *dst, size_t dst_cap,
                                           therm_frame_stats_t *stats_out,
                                           uint16_t *raw_out, size_t raw_out_bytes);

// Per-frame thermal temperature stats from the most recent encode.
// Returns RAW Lepton counts; caller multiplies by the active TLinear
// resolution scale (queried via lepton_cci_get_tlinear_state) to get
// centi-Kelvin. With resolution=1 (default) raw counts ARE centi-K.
//   centi-K to °C: ck / 100 - 273.15
//   centi-K to °F: (ck/100 - 273.15) * 9/5 + 32
void capture_get_last_thermal_temps_ck(uint32_t *min_ck, uint32_t *max_ck,
                                        uint32_t *center_ck);

// Snapshot the most recent committed thermal frame as raw uint16
// pixels (always 160 × 120, pre-rotation, the Lepton's native
// orientation). dst_bytes must be ≥ 38400. Returns false if no frame
// has been encoded yet. Used by timelapse/single-capture to save a
// .raw16 sidecar for offline reprocessing.
bool capture_snapshot_thermal_raw(uint16_t *dst, size_t dst_bytes);

// Thermal rotation applied during JPEG encode. Affects both preview
// and recorded captures. Argument is 0/1/2/3 → 0/90/180/270 CW.
void    capture_set_thermal_rotation(uint8_t r);
uint8_t capture_get_thermal_rotation(void);

// Visible rotation 0/1/2/3 → 0/90/180/270 CW. 0 and 180 use the
// OV2640's hardware H/V flip (free); 90 and 270 require a software
// JPEG decode → rotate → re-encode (~250 ms per VGA frame on S3).
// Configures the sensor flips internally as a side-effect of set.
void    capture_set_visible_rotation(uint8_t r);
uint8_t capture_get_visible_rotation(void);

// If visible rotation is 90 or 270, decode the input JPEG, rotate the
// pixel buffer, and re-encode. On success, *out_jpg is heap-allocated
// (caller frees with free()) and *out_w / *out_h are swapped.
// If rotation is 0 or 180 (or input is invalid), returns false and
// leaves outputs untouched — caller should use the original JPEG.
bool capture_rotate_visible_jpeg_if_needed(const uint8_t *in_jpg, size_t in_len,
                                            uint32_t in_w, uint32_t in_h,
                                            uint8_t **out_jpg, size_t *out_len,
                                            uint32_t *out_w, uint32_t *out_h);

// Single immediate capture — vis JPEG + thermal JPEG, written to a
// new SD session directory. On success, copies the new session id
// (e.g. "session_4517") into `session_id_out`.
//
// Returns ESP_OK on full success, ESP_ERR_NOT_FOUND if SD not mounted,
// ESP_ERR_TIMEOUT if SD lock can't be acquired, ESP_FAIL on other errors.
// On failure, `msg_out` (caller-owned, msg_cap bytes) gets a
// human-readable explanation suitable for the cmd.result event.
esp_err_t capture_now(char *session_id_out, size_t session_id_cap,
                      char *msg_out, size_t msg_cap);

// ---- Capture engine ----
//
// Synchronous "take one" producing in-memory artifacts only. Does NOT
// touch SD, does NOT update any session aggregates, does NOT depend
// on which scheduler called it. Live timelapse, single-shot capture,
// and (eventually) the deep-sleep wake handler all use this.
//
// On success at least one of vis_jpg / therm_jpg is non-NULL; check
// the per-artifact pointers and the `*_ms` timing to decide what to
// commit. Caller MUST call capture_artifacts_free() when done.
typedef struct {
    // JPEG buffers (heap-allocated in PSRAM, caller frees via free()).
    uint8_t *vis_jpg;
    size_t   vis_len;
    uint32_t vis_w;
    uint32_t vis_h;

    uint8_t *therm_jpg;
    size_t   therm_len;

    // Raw 160×120 uint16 thermal frame (76800 bytes is the buffer cap;
    // 38400 are the meaningful pixels). Pre-rotation, native sensor
    // orientation. Heap-allocated in PSRAM; NULL if not requested or
    // thermal failed.
    uint16_t *therm_raw;

    therm_frame_stats_t therm_stats;

    // Capture-time settings snapshot — needed by the sidecar JSON
    // writers so the file describes the exact frame, not whatever
    // state the live system is in by the time we commit to SD.
    bool     tlinear_active;
    bool     tlinear_auto_res;
    uint8_t  therm_rotation;
    uint8_t  vis_rotation;
    bool     agc_enabled;
    int      gain_mode;

    // Phase timing (ms) — useful for deep-sleep journal records.
    uint32_t visible_ms;
    uint32_t thermal_ms;
} capture_artifacts_t;

// Free any heap-allocated buffers inside `a` and zero the struct.
// Safe to call on a zero-initialized struct.
void capture_artifacts_free(capture_artifacts_t *a);

// Produce capture artifacts for one frame. Reads the current rotation
// + AGC + gain settings as the snapshot for this capture. Returns
// ESP_OK if at least one requested artifact was produced; otherwise
// the appropriate ESP_ERR_*.
//
// `want_thermal_raw` only matters when `want_thermal` is true; setting
// it allocates the raw buffer alongside the JPEG so callers that want
// the .raw16 sidecar don't need a second snapshot call.
esp_err_t capture_engine_take_one(bool want_visible,
                                   bool want_thermal,
                                   bool want_thermal_raw,
                                   capture_artifacts_t *out);

// ---- Timelapse ----
//
// Periodic capture loop. Creates a session directory at start, fires
// capture_to_session() at the requested interval, and finalizes the
// session.json on stop. Single active session at a time — calling start
// while already running returns an error.
typedef struct {
    bool active;
    char session_id[32];     // "session_<num>"
    char session_dir[80];    // "/sdcard/timelapse/session_<num>"
    uint32_t interval_sec;
    uint32_t capture_count;
    uint64_t started_ms;
    bool capture_vis;
    bool capture_therm;
} timelapse_status_t;

// max_duration_sec=0 ⇒ run until manually stopped. Anything else makes
// the loop self-stop after that many wall-clock seconds (final
// session.json gets complete=true regardless of stop reason).
esp_err_t timelapse_start(uint32_t interval_sec,
                          bool capture_vis, bool capture_therm,
                          uint32_t max_duration_sec,
                          char *session_id_out, size_t session_id_cap,
                          char *msg_out, size_t msg_cap);

esp_err_t timelapse_stop(char *msg_out, size_t msg_cap);

void timelapse_get_status(timelapse_status_t *out);

#ifdef __cplusplus
}
#endif
