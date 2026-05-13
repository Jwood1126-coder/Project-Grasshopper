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

// Encode the latest committed thermal frame into a JPEG using the iron
// palette with per-frame auto-ranging. Output goes into `dst`, capacity
// `cap`. Returns the JPEG byte length, 0 on failure (no frame yet, or
// out of memory). Thread-safe (uses internal lazy-allocated buffers).
size_t capture_encode_thermal_jpeg(uint8_t *dst, size_t cap);

// Thermal rotation applied during JPEG encode. Affects both preview
// and recorded captures. Argument is 0/1/2/3 → 0/90/180/270 CW.
void    capture_set_thermal_rotation(uint8_t r);
uint8_t capture_get_thermal_rotation(void);

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
