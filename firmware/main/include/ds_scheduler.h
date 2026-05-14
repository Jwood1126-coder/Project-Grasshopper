#pragma once

// ds_scheduler: deep-sleep timelapse scheduler.
//
// Two state stores:
//
//   - RTC_NOINIT memory:  fast wake hint. Carries next_seq, due
//     time, session_id across deep sleep. Lost on cold boot — that's
//     fine; NVS is the durable backstop.
//
//   - NVS namespace "ds_sess":  durable. "active" key tells us a
//     session is in progress. Survives power loss / cold boot.
//     Cleared only when the session completes (or is explicitly
//     aborted).
//
// SD journal (captures.jsonl owned by session_store) is the source
// of truth for what's actually committed. RTC/NVS are hints —
// whenever they disagree with the journal, the journal wins.
//
// Single API used by:
//   - cmd handler (timelapse.start with deepSleep:true) → arm + run first cycle
//   - app_main early boot path → maybe_handle_wake() before the
//     normal init runs

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DS_INACTIVE   = 0,   // no deep-sleep session
    DS_ACTIVE     = 1,   // session in progress (NVS active=1)
    DS_FINALIZED  = 2,   // last capture committed, complete=true
} ds_state_t;

// Args from timelapse.start payload (deepSleep:true variant).
typedef struct {
    uint32_t max_captures;       // 1..1000 (PR-C tested at 3)
    uint32_t interval_sec;       // ≥60 enforced — Lepton boot eats ~10s of awake budget
    bool     capture_vis;
    bool     capture_therm;
    // PR-D: wake-Wi-Fi window. After each capture commits to SD, the
    // device brings up Wi-Fi + relay for `wake_window_sec`, sends a
    // ds-state init/tick so the dashboard sees the in-progress
    // session, and accepts timelapse.stop. If stop arrives, the
    // session is finalized + the device returns to normal boot.
    // Wake-window adds ~5-10s for Wi-Fi connect + the window itself,
    // so it costs battery — opt in only when visibility matters.
    bool     wake_wifi;
    uint32_t wake_window_sec;    // 5..60; ignored when wake_wifi=false

    // PR-G schema-stable placeholder: open the wake-Wi-Fi window only
    // every Nth wake (1 = every wake, 6 at a 10-min interval = once
    // per hour). Stored across boots so a future firmware can honor
    // it without changing the cmd schema. Currently parsed, validated,
    // persisted — but NOT yet gated on in run_one_cycle. Run a no-
    // radio + a wake-every-cycle soak first so we know what the per-
    // wake radio cost actually IS before optimizing it away.
    uint32_t wake_wifi_every;    // ≥1; default 1 = every wake
} ds_arm_args_t;

// Initialize an empty (DS_INACTIVE) state. Reads NVS — if a session
// is active, picks up its config. Safe to call multiple times.
void ds_scheduler_init(void);

// Arm a fresh deep-sleep session. Persists config to NVS (so a
// power loss before the first capture still knows what was asked
// for), seeds the RTC state with next_seq=1 and intended_epoch=now.
//
// Caller is responsible for triggering the first capture cycle —
// most natural pattern is to follow up with ds_scheduler_run_one_cycle
// from the cmd handler context, after sending cmd.result back to the
// dashboard.
//
// Returns ESP_ERR_INVALID_STATE if a session is already active —
// caller must explicitly clear it first (cmd handler can refuse).
esp_err_t ds_scheduler_arm(const char *session_id,
                            const ds_arm_args_t *args);

// Boot-time entry. Examines wakeup cause + NVS state:
//   - timer wake + active NVS session → run one cycle (returns ESP_OK
//     if the session completed and caller should continue normal
//     boot, OR DOES NOT RETURN if it slept again)
//   - any other wake → ESP_ERR_INVALID_STATE (caller proceeds to
//     normal boot unchanged, no DS work to do)
//
// Cold boot with NVS active=1 (interrupted session): currently logs
// a warning and clears NVS — the partial session stays on SD with
// complete=false (recover_all will tidy any orphan files but won't
// auto-resume in PR-C).
esp_err_t ds_scheduler_maybe_handle_wake(void);

// Run one capture-and-decide cycle. Used by both:
//   - cmd handler immediately after arm (fires capture #1, then sleeps)
//   - timer wake handler on each wake (fires capture #N, then sleeps
//     or returns if last)
//
// Returns ESP_OK if the session is now complete (last capture
// committed + marked complete + NVS cleared). DOES NOT RETURN if
// more captures are due — calls esp_deep_sleep_start() instead.
esp_err_t ds_scheduler_run_one_cycle(void);

// Mark current session aborted: free RTC + NVS state without
// finalizing the session.json (recovery sweep will mop up). Used
// for explicit user-cancel; not called automatically.
void ds_scheduler_abort(void);

// Request the in-progress session to stop at the end of the current
// wake window. Called by the cmd handler when timelapse.stop arrives
// during a wake-window phase. Sets a flag the wake-window loop polls;
// effect is observed only between wakes (the current capture cycle
// always completes its commit). Idempotent.
void ds_scheduler_request_stop(void);

// True iff a wake-window stop has been requested this wake.
bool ds_scheduler_stop_requested(void);

// Status accessors — used by tick payload + UI.
ds_state_t  ds_scheduler_state(void);
uint32_t    ds_scheduler_next_seq(void);
uint32_t    ds_scheduler_max_captures(void);
const char *ds_scheduler_session_id(void);

#ifdef __cplusplus
}
#endif
