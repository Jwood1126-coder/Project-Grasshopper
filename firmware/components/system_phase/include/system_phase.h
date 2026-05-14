#pragma once

// system_phase: lightweight in-memory state of "what is the device
// doing right now". Pure observability + bulk-throttling hint.
//
// THIS IS NOT A SCHEDULER OR PERMISSION SYSTEM. Subsystems own
// their own concurrency. Phase is a breadcrumb that other code MAY
// inspect to defer non-critical work; it does not authorize or
// reject commands. Cmd handlers continue to enforce their own
// preconditions independently.
//
// Lifecycle:
//   - phase is RAM-only; deep sleep wipes it (intentional)
//   - one writer per transition site (no nested wrapping; no
//     save/restore — every enter() is explicit and one-directional)
//   - readers do a single volatile word load; no locks
//   - corrupt phase fails OPEN (pauses_bulk → false), never closed
//
// The string names returned by system_phase_name() are WIRE PROTOCOL.
// The dashboard parses them. Renaming or adding values requires a
// coordinated dashboard update.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PHASE_UNKNOWN              = 0,   // pre-init or corrupted state
    PHASE_BOOT                 = 1,   // app_main early init
    PHASE_RECOVERY             = 2,   // session_store_recover_all sweep
    PHASE_LIVE                 = 3,   // normal operation, peripherals ready
    PHASE_CAPTURE              = 4,   // live-mode capture path holds the SD/sensors
    PHASE_DEEP_SLEEP_CAPTURE   = 5,   // ds_scheduler wake cycle (capture + commit)
    PHASE_WAKE_RADIO           = 6,   // wake-Wi-Fi window inside a DS cycle
    PHASE_OTA                  = 7,   // OTA download / write / pre-restart
    PHASE__COUNT,
} system_phase_t;

// Initialize to PHASE_BOOT. Idempotent — safe to call exactly once
// from app_main as the first executable line. Does NOT fire a
// transition observer (no observer is registered yet, and the
// initial state isn't a transition).
void system_phase_init(void);

// Set the current phase. No-op if `next` already matches the current
// phase (no log spam, no event, no entered_ms drift). Invalid enum
// values are silently ignored — the device stays in whatever phase
// it was in. Logs the transition and (if registered) calls the
// observer with from / to / how-long-we-were-in-from.
//
// Concurrency: callers from different tasks should serialize at
// their own layer. Two tasks racing enter() with different values
// will leave the phase in whichever wrote last; both transitions
// will log; the observer will see both.
void system_phase_enter(system_phase_t next);

// Read accessors. Single volatile word loads, lock-free. Both
// clamp out-of-range stored values to PHASE_UNKNOWN — never returns
// a garbage enum.
system_phase_t system_phase_get(void);
uint32_t       system_phase_entered_ms(void);   // monotonic ms (esp_timer/1000)

// Name lookup. Returns "UNKNOWN" for any out-of-range or unrecognized
// value, never NULL. Names are wire-protocol; do not rename in place.
const char *system_phase_name(system_phase_t p);

// Hint to bulk-work emitters (preview tasks, sessions worker) that
// they should defer for now. RETURN VALUE FOR CORRUPT PHASE IS FALSE
// (fail-open) — a stuck "true" return would silently disable preview
// and library access on a healthy device. Better to ship a stale
// preview frame than go dark. Phase set used as of commit 1:
//   true:  CAPTURE, DEEP_SLEEP_CAPTURE, OTA
//   false: everything else (including UNKNOWN)
//
// Commit 1 ships this function but no caller uses it yet. Commit 2
// wires it into preview tasks + sessions worker.
bool system_phase_pauses_bulk(void);

// Convenience: how long has the current phase been active.
// Returns 0 if no transition has occurred since boot/init.
uint32_t system_phase_in_phase_ms(void);

// Transition observer. Called from inside system_phase_enter() AFTER
// the phase has been updated. Best-effort: do not block, do not
// allocate large buffers; the call happens on the writer's task and
// must return quickly. If the observer's downstream (e.g. WS send)
// is unavailable, it should drop the event silently — the log line
// is the durable record.
//
// Only one observer at a time. Pass NULL to unregister. The intended
// caller is app_main, which registers an emitter that ships a small
// JSON event to the relay.
typedef void (*system_phase_observer_fn)(system_phase_t from,
                                          system_phase_t to,
                                          uint32_t       prev_dur_ms);
void system_phase_register_observer(system_phase_observer_fn fn);

#ifdef __cplusplus
}
#endif
