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
