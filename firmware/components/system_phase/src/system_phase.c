// See system_phase.h. Implementation owns:
//   - the volatile phase + entered_ms pair (single word each, no
//     locks, accept the brief torn-pair window between writes)
//   - the wire-protocol name table (KEEP IN SYNC WITH DASHBOARD)
//   - one observer slot, called best-effort on real transitions
//
// Hard constraints:
//   - never block in enter() — observer must be quick
//   - never log on no-op enter (same → same)
//   - fail-open in pauses_bulk on any out-of-range stored value

#include "system_phase.h"

#include <stddef.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "phase";

static volatile system_phase_t        s_phase       = PHASE_UNKNOWN;
static volatile uint32_t              s_entered_ms  = 0;
static volatile system_phase_observer_fn s_observer = NULL;

// Wire protocol — committed names. Dashboard parses these. Adding
// a new entry is OK; renaming is a coordinated change.
static const char * const PHASE_NAMES[PHASE__COUNT] = {
    [PHASE_UNKNOWN]            = "UNKNOWN",
    [PHASE_BOOT]               = "BOOT",
    [PHASE_RECOVERY]           = "RECOVERY",
    [PHASE_LIVE]               = "LIVE",
    [PHASE_CAPTURE]            = "CAPTURE",
    [PHASE_DEEP_SLEEP_CAPTURE] = "DEEP_SLEEP_CAPTURE",
    [PHASE_WAKE_RADIO]         = "WAKE_RADIO",
    [PHASE_OTA]                = "OTA",
};

static inline uint32_t mono_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static inline bool valid_phase(system_phase_t p) {
    return p > PHASE_UNKNOWN && p < PHASE__COUNT;
}

void system_phase_init(void) {
    // Direct assignment — initial state isn't a transition. We
    // deliberately do NOT call the observer; nobody should be
    // subscribed yet anyway, and if they are, they'd see a
    // synthetic UNKNOWN→BOOT that didn't represent real work.
    s_phase      = PHASE_BOOT;
    s_entered_ms = mono_ms();
    s_observer   = NULL;
    ESP_LOGI(TAG, "init → %s @ %ums",
             PHASE_NAMES[PHASE_BOOT], (unsigned)s_entered_ms);
}

void system_phase_enter(system_phase_t next) {
    // Ignore garbage. Caller-side bug; don't let it cascade into
    // the live phase variable.
    if (!valid_phase(next)) {
        ESP_LOGW(TAG, "enter: ignoring out-of-range phase value %d", (int)next);
        return;
    }

    system_phase_t cur = s_phase;
    if (cur == next) {
        // Same-phase enter is a no-op. No log spam, no observer
        // call, no entered_ms drift. Subsystems that need to know
        // "we are in CAPTURE again" should track that themselves.
        return;
    }

    uint32_t now      = mono_ms();
    uint32_t prev_dur = (s_entered_ms != 0 && now >= s_entered_ms)
                        ? (now - s_entered_ms) : 0;

    s_phase      = next;
    s_entered_ms = now;

    ESP_LOGI(TAG, "%s → %s (prev_dur=%ums)",
             system_phase_name(cur),
             PHASE_NAMES[next],
             (unsigned)prev_dur);

    system_phase_observer_fn obs = s_observer;
    if (obs) {
        // Observer is best-effort. If it blocks, future transitions
        // pile up behind it. Document at the API boundary; do not
        // wrap in a timeout here (would add complexity for marginal
        // safety — the only registered observer is a tiny snprintf
        // + net_relay_send which never blocks on its own).
        obs(cur, next, prev_dur);
    }
}

system_phase_t system_phase_get(void) {
    system_phase_t p = s_phase;
    return valid_phase(p) ? p : PHASE_UNKNOWN;
}

uint32_t system_phase_entered_ms(void) {
    return s_entered_ms;
}

const char *system_phase_name(system_phase_t p) {
    if (!valid_phase(p)) return "UNKNOWN";
    return PHASE_NAMES[p];
}

bool system_phase_pauses_bulk(void) {
    // FAIL-OPEN: any value outside the known-pausing set returns
    // false. If s_phase ever holds garbage (memory corruption,
    // downstream bug, downgrade-to-old-firmware mid-call), bulk
    // emitters keep working — a chatty device is recoverable, a
    // silent one is not.
    system_phase_t p = s_phase;
    switch (p) {
        case PHASE_CAPTURE:
        case PHASE_DEEP_SLEEP_CAPTURE:
        case PHASE_OTA:
            return true;
        default:
            return false;
    }
}

uint32_t system_phase_in_phase_ms(void) {
    if (s_entered_ms == 0) return 0;
    uint32_t now = mono_ms();
    if (now < s_entered_ms) return 0;   // overflow guard
    return now - s_entered_ms;
}

void system_phase_register_observer(system_phase_observer_fn fn) {
    s_observer = fn;
}
