#pragma once

// session_store: durable on-SD session record-keeping.
//
// Owns the SD layout for /sdcard/timelapse/session_<id>/:
//   - session.json   (summary, derived from journal)
//   - captures.jsonl (append-only journal — the COMMIT POINT)
//   - NNNNNN_vis.jpg, NNNNNN_therm.jpg, NNNNNN_therm.raw16,
//     NNNNNN_therm.json (per-capture artifacts)
//
// Invariant: a capture is "committed" only after its line is in
// captures.jsonl. Anything finalized but not in the journal (image
// files with seq > journal max) is treated as never-happened and
// deleted on recovery. Same for orphan .tmp files.
//
// Single API used by:
//   - live timelapse loop (tl_capture_iteration)
//   - single-shot capture (capture_now)
//   - deep-sleep wake handler (future PR)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "capture.h"   // capture_artifacts_t

#ifdef __cplusplus
extern "C" {
#endif

typedef struct session_store_handle session_store_handle_t;

// Args passed to session_store_open(). Mode is "timelapse" or
// "single" (matches the value written to session.json so the UI's
// existing parsers don't need changes). interval_sec=0 for "single".
typedef struct {
    const char *mode;
    uint32_t    interval_sec;
    bool        capture_vis;
    bool        capture_therm;
} session_store_open_args_t;

// Optional per-capture metadata. NULL is fine for live captures —
// epoch defaults to time(NULL), quality is derived (synced if
// time(NULL) > 0 else unknown), wake_ms is 0. Deep-sleep wake
// handlers fill these to record intended-vs-actual timing and the
// "unknown" / "estimated" time-quality flag.
typedef struct {
    uint64_t intended_epoch;       // 0 → use actual_epoch
    uint8_t  time_quality;         // 0=unknown, 1=estimated, 2=synced; 0=auto
    uint32_t wake_ms;              // 0 = not a wake
} session_capture_meta_t;

// Open or resume a session. Creates /sdcard/timelapse/<session_id>/
// if absent, replays existing captures.jsonl (sets capture_count +
// aggregates from prior records — handles cold-boot resume of an
// in-progress session), writes initial session.json with
// complete=false. Acquires SD lock internally.
//
// Caller owns the returned handle; must close with session_store_close.
// Returns NULL on failure.
session_store_handle_t *session_store_open(const char *session_id,
                                            const session_store_open_args_t *args);

// Synchronous transactional commit of one capture. Acquires SD lock
// internally, writes each non-NULL artifact via the atomic_write
// pattern (.tmp → fsync → rename), appends a journal line (the
// COMMIT), updates RAM aggregates, rewrites session.json.
//
// Per-artifact failures are tolerated and recorded in the journal
// (visOk / thermOk / raw16Ok / thermMetaOk false). Only journal-
// append failure causes the whole commit to fail; in that case any
// finalized image files for this seq are cleaned up by the next
// recovery pass (they're "orphans" — finalized but not in journal).
//
// Returns ESP_OK on commit, ESP_FAIL on journal-append failure.
// `art` is NOT consumed — caller still calls capture_artifacts_free.
esp_err_t session_store_commit(session_store_handle_t *h,
                                const capture_artifacts_t *art,
                                const session_capture_meta_t *meta);

// Mark complete=true, rewrite session.json, free handle. After this
// the handle is invalid. Idempotent w.r.t. the underlying SD state —
// safe to call from a self-stop or a clean shutdown.
esp_err_t session_store_close(session_store_handle_t *h);

// Free the handle WITHOUT marking complete=true. session.json on
// disk keeps whatever state the most recent commit left it in
// (complete=false). Used by the deep-sleep wake handler between
// captures: each wake opens, commits, releases, then sleeps. Only
// the wake that fires the LAST capture calls session_store_close.
//
// Equivalent to session_store_close minus the SD lock + json write.
void session_store_release(session_store_handle_t *h);

// Read accessors for the timelapse status reporter (UI live status).
uint32_t session_store_capture_count(const session_store_handle_t *h);
uint64_t session_store_started_ms(const session_store_handle_t *h);
const char *session_store_id(const session_store_handle_t *h);
const char *session_store_dir(const session_store_handle_t *h);

// Boot-time recovery. Scans /sdcard/timelapse/ and for each session
// dir whose session.json says complete=false (or is missing):
//   1. Replay captures.jsonl, get max committed seq + aggregates
//   2. Delete orphan finalized files (NNNNNN > max seq)
//   3. Delete any *.tmp orphans
//   4. Rewrite session.json from journal aggregates (still complete=false)
// Idempotent. Safe to call before any session is open. Acquires
// SD lock per-session, releases between sessions so it doesn't
// starve other workers.
esp_err_t session_store_recover_all(void);

#ifdef __cplusplus
}
#endif
