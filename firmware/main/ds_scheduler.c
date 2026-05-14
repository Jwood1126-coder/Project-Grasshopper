// ds_scheduler: deep-sleep timelapse scheduler.
//
// State stores:
//   RTC_NOINIT memory: fast hint across deep sleep (lost on cold boot).
//   NVS namespace "ds_sess": durable backstop. "active" key gates
//     whether boot considers itself mid-session.
//   SD captures.jsonl (owned by session_store): source of truth for
//     what's actually committed.
//
// Wake-cycle outline (per timer wake):
//   1. recover_all → clean any orphan files from a prior crash
//   2. mount-not-needed (storage already mounted by app_main)
//   3. bring up Lepton (full phase sequence) and camera (if vis)
//   4. capture_engine_take_one
//   5. session_store_open(id, args) → resume journal
//   6. session_store_commit(handle, art, meta)
//   7. release handle (no complete=true) OR close (last capture)
//   8. compute next due / sleep_us
//   9. power_manager_prep_for_sleep + enter_deep_sleep (no return)
//
// Time:
//   intended_epoch is computed deterministically from session_start +
//   (next_seq-1) * interval_sec. If NTP is synced, time(NULL) is used
//   as actual_epoch and timeQuality=2 (synced). Otherwise time_quality
//   degrades to 1 (estimated) or 0 (unknown).

#include "ds_scheduler.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "capture.h"
#include "hal_camera.h"
#include "hal_lepton.h"
#include "hal_storage.h"
#include "net_wifi.h"
#include "net_relay.h"
#include "power_manager.h"
#include "sdkconfig.h"
#include "session_store.h"

static const char *TAG = "ds_sched";

// ---- RTC state (survives deep sleep, lost on cold boot) ----

#define DS_RTC_MAGIC   0xD550DEEFu
#define DS_RTC_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t crc32;                    // of bytes after this field
    uint32_t next_seq;                 // 1..max_captures inclusive (next to take)
    uint32_t max_captures;
    uint32_t interval_sec;
    uint64_t session_start_epoch;      // 0 if NTP wasn't synced when armed
    uint64_t next_due_epoch;           // 0 if scheduling by interval-from-now
    uint32_t consecutive_failures;
    uint32_t reserved0;
    uint64_t cumulative_awake_ms;
    uint64_t cumulative_sleep_ms;
    char     session_id[32];
    uint8_t  capture_vis;
    uint8_t  capture_therm;
    uint8_t  wake_wifi;
    uint8_t  reserved1;
    uint32_t wake_window_sec;
    uint32_t wake_wifi_every;     // PR-G schema-stable; not yet gated on
} ds_rtc_state_t;

_Static_assert(sizeof(ds_rtc_state_t) <= 256,
               "RTC state should stay tiny — we have ~8KB of RTC slow memory");

static RTC_NOINIT_ATTR ds_rtc_state_t s_rtc;

static uint32_t rtc_crc(const ds_rtc_state_t *s) {
    // Skip the leading magic+version+crc fields. CRC covers the
    // payload that defines the session.
    const uint8_t *p = (const uint8_t *)&s->next_seq;
    size_t len = sizeof(*s) - offsetof(ds_rtc_state_t, next_seq);
    return esp_rom_crc32_le(0, p, len);
}

static bool rtc_valid(void) {
    if (s_rtc.magic != DS_RTC_MAGIC) return false;
    if (s_rtc.version != DS_RTC_VERSION) return false;
    return s_rtc.crc32 == rtc_crc(&s_rtc);
}

static void rtc_seal(void) {
    s_rtc.magic = DS_RTC_MAGIC;
    s_rtc.version = DS_RTC_VERSION;
    s_rtc.crc32 = rtc_crc(&s_rtc);
}

// ---- NVS persistence ----

#define DS_NVS_NS "ds_sess"

static esp_err_t nvs_save(const ds_arm_args_t *args, const char *session_id,
                          uint64_t start_epoch) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(DS_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8 (h, "active",   1);
    nvs_set_str(h, "sessid",   session_id);
    nvs_set_u32(h, "max",      args->max_captures);
    nvs_set_u32(h, "interval", args->interval_sec);
    nvs_set_u8 (h, "vis",      args->capture_vis  ? 1 : 0);
    nvs_set_u8 (h, "therm",    args->capture_therm ? 1 : 0);
    nvs_set_u64(h, "start",    start_epoch);
    nvs_set_u8 (h, "wfi",      args->wake_wifi ? 1 : 0);
    nvs_set_u32(h, "wwin",     args->wake_window_sec);
    nvs_set_u32(h, "wevery",   args->wake_wifi_every);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool nvs_load(ds_arm_args_t *args, char *session_id, size_t cap,
                     uint64_t *start_epoch) {
    nvs_handle_t h;
    if (nvs_open(DS_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t active = 0;
    if (nvs_get_u8(h, "active", &active) != ESP_OK || active != 1) {
        nvs_close(h);
        return false;
    }
    size_t sl = cap;
    if (nvs_get_str(h, "sessid", session_id, &sl) != ESP_OK) {
        nvs_close(h);
        return false;
    }
    uint32_t mx = 0, iv = 0, wwin = 0, wevery = 0;
    uint8_t vis = 0, therm = 0, wfi = 0;
    uint64_t start = 0;
    nvs_get_u32(h, "max",      &mx);
    nvs_get_u32(h, "interval", &iv);
    nvs_get_u8 (h, "vis",      &vis);
    nvs_get_u8 (h, "therm",    &therm);
    nvs_get_u64(h, "start",    &start);
    nvs_get_u8 (h, "wfi",      &wfi);     // missing key on old sessions → 0
    nvs_get_u32(h, "wwin",     &wwin);
    nvs_get_u32(h, "wevery",   &wevery);  // missing → 0; arm() normalizes to 1
    nvs_close(h);

    args->max_captures    = mx;
    args->interval_sec    = iv;
    args->capture_vis     = (vis != 0);
    args->capture_therm   = (therm != 0);
    args->wake_wifi       = (wfi != 0);
    args->wake_window_sec = wwin;
    args->wake_wifi_every = (wevery == 0) ? 1 : wevery;
    if (start_epoch) *start_epoch = start;
    return true;
}

static void nvs_clear(void) {
    nvs_handle_t h;
    if (nvs_open(DS_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---- Internal: bring up sensors needed for one capture ----

// Returns true if Lepton came up OK. Lepton boot is staged: split
// phases let us measure each step (phase timing for the journal),
// skip the every-wake FFC, and bound the first-frame wait.
static bool bring_up_lepton(uint32_t *out_boot_ms) {
    int64_t t0 = esp_timer_get_time();
    if (hal_lepton_power_on(8000)       != ESP_OK) goto fail;
    if (hal_lepton_cci_bus_init()       != ESP_OK) goto fail;
    if (hal_lepton_cci_apply_config()   != ESP_OK) goto fail;
    if (hal_lepton_vospi_bring_up()     != ESP_OK) goto fail;
    // 15 s is generous: under normal conditions the first frame lands
    // ~500 ms after VoSPI start. Bigger budget covers cold-Lepton
    // edge cases without making a hung sensor wedge the wake forever.
    if (hal_lepton_wait_first_frame(15000) != ESP_OK) goto fail;
    // PR-C: skip per-wake FFC. Initial FFC fires only on the first
    // capture of the session (caller sets a flag).
    if (out_boot_ms) *out_boot_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    return true;
fail:
    if (out_boot_ms) *out_boot_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGE(TAG, "lepton bring-up failed");
    return false;
}

static bool bring_up_camera(uint32_t *out_boot_ms) {
    int64_t t0 = esp_timer_get_time();
    hal_camera_cfg_t cfg = {
        .framesize    = HAL_CAM_FRAMESIZE_VGA,
        .jpeg_quality = 12,
        .fb_count     = 2,
    };
    char sensor_name[32] = {0};
    bool ok = (hal_camera_init(&cfg, sensor_name) == ESP_OK);
    if (out_boot_ms) *out_boot_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (!ok) ESP_LOGW(TAG, "camera init failed (vis won't land this wake)");
    return ok;
}

// Apply persisted rotation settings — same NVS namespace as the live
// path, so deep-sleep captures land in the user's chosen orientation
// without re-doing the user-settings-load logic in app_main.
static void load_user_settings(void) {
    nvs_handle_t h;
    if (nvs_open("ghset", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v = 0;
    if (nvs_get_u8(h, "vr", &v) == ESP_OK) capture_set_visible_rotation(v);
    v = 0;
    if (nvs_get_u8(h, "tr", &v) == ESP_OK) capture_set_thermal_rotation(v);
    nvs_close(h);
}

// ---- Wake-window stop hook ----
//
// Set by the cmd handler when timelapse.stop arrives during a wake
// window. The wake-window loop polls this every 100ms; once it flips
// true, the loop exits early and run_one_cycle finalizes the session
// instead of sleeping again. RAM-only — wake windows fire fresh
// each wake, so no persistence needed.

static volatile bool s_stop_requested = false;

void ds_scheduler_request_stop(void) {
    ESP_LOGI(TAG, "stop requested via cmd");
    s_stop_requested = true;
}

bool ds_scheduler_stop_requested(void) { return s_stop_requested; }

// Run the wake-window phase: bring up Wi-Fi + relay, sit for up to
// `window_sec` seconds, return early if a stop arrives. Returns true
// if a stop was received (caller finalizes the session); false on
// natural timeout (caller sleeps and continues the schedule).
//
// On entry the cmd handler must already be registered with net_relay
// (app_main does this before ds_scheduler_maybe_handle_wake fires).
// Wi-Fi credentials come from sdkconfig CONFIG_GRASSHOPPER_WIFI_*.
//
// Tolerant of Wi-Fi connect failure: if the radio can't associate
// inside the bring-up budget, we still hold a brief settle (~2s) so
// the dashboard sees one last log line if it's looking, then return
// false (no stop). The session keeps going.
static bool run_wake_window(uint32_t window_sec) {
    if (window_sec < 1) window_sec = 1;
    if (window_sec > 60) window_sec = 60;

    s_stop_requested = false;

    int64_t t0 = esp_timer_get_time();

    // Bring up Wi-Fi. SSID/PASS from build config.
    const char *ssid = CONFIG_GRASSHOPPER_WIFI_SSID;
    const char *pass = CONFIG_GRASSHOPPER_WIFI_PASS;
    if (!ssid || !*ssid) {
        ESP_LOGW(TAG, "wake-window: no Wi-Fi SSID configured — skipping");
        return false;
    }
    if (net_wifi_init() != ESP_OK) {
        ESP_LOGW(TAG, "wake-window: net_wifi_init failed");
        return false;
    }
    esp_err_t werr = net_wifi_connect_blocking(ssid, pass);
    if (werr != ESP_OK) {
        ESP_LOGW(TAG, "wake-window: Wi-Fi connect failed (%s) — sleeping again",
                 esp_err_to_name(werr));
        net_wifi_stop();
        return false;
    }

    // Bring up the relay. Cmd handler should already be registered by
    // app_main; net_relay_start opens the WSS and sends hello.
    if (net_relay_start() != ESP_OK) {
        ESP_LOGW(TAG, "wake-window: net_relay_start failed");
        net_wifi_stop();
        return false;
    }

    int64_t end_us = t0 + (int64_t)window_sec * 1000000LL;
    ESP_LOGI(TAG, "wake-window open for %lus (will exit early on stop)",
             (unsigned long)window_sec);
    while (esp_timer_get_time() < end_us && !s_stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    bool stopped = s_stop_requested;

    int64_t up_ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "wake-window closed after %lldms (stopped=%d)",
             (long long)up_ms, (int)stopped);

    // Tear down cleanly. relay first so the close frame goes out
    // before we drop the radio.
    net_relay_stop();
    net_wifi_stop();
    return stopped;
}

// ---- Public API ----

void ds_scheduler_init(void) {
    // Validate RTC state. Cold boot → invalid → zero it out so
    // downstream code can tell "no session" via state().
    if (!rtc_valid()) {
        memset(&s_rtc, 0, sizeof(s_rtc));
    }
}

esp_err_t ds_scheduler_arm(const char *session_id,
                            const ds_arm_args_t *args) {
    if (!session_id || !args) return ESP_ERR_INVALID_ARG;
    if (args->max_captures == 0) return ESP_ERR_INVALID_ARG;
    if (args->interval_sec < 60) return ESP_ERR_INVALID_ARG;  // PR-C minimum
    if (!args->capture_vis && !args->capture_therm) return ESP_ERR_INVALID_ARG;

    if (rtc_valid() && s_rtc.next_seq <= s_rtc.max_captures) {
        ESP_LOGW(TAG, "arm: session %s already in progress (seq=%lu/%lu)",
                 s_rtc.session_id,
                 (unsigned long)s_rtc.next_seq, (unsigned long)s_rtc.max_captures);
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t start_epoch = (uint64_t)time(NULL);
    if (start_epoch < 1700000000ULL) start_epoch = 0;   // not NTP-synced

    memset(&s_rtc, 0, sizeof(s_rtc));
    snprintf(s_rtc.session_id, sizeof(s_rtc.session_id), "%s", session_id);
    s_rtc.next_seq            = 1;
    s_rtc.max_captures        = args->max_captures;
    s_rtc.interval_sec        = args->interval_sec;
    s_rtc.session_start_epoch = start_epoch;
    s_rtc.next_due_epoch      = start_epoch;     // first capture: now
    s_rtc.capture_vis         = args->capture_vis  ? 1 : 0;
    s_rtc.capture_therm       = args->capture_therm ? 1 : 0;
    s_rtc.wake_wifi           = args->wake_wifi ? 1 : 0;
    s_rtc.wake_window_sec     = args->wake_window_sec;
    // PR-G schema-stable: clamp 1..100. Default 1 = every wake (current
    // behavior). Stored across deep-sleep + persisted to NVS so a future
    // firmware can honor higher values without changing the cmd schema.
    {
        uint32_t every = args->wake_wifi_every ? args->wake_wifi_every : 1;
        if (every > 100) every = 100;
        s_rtc.wake_wifi_every = every;
    }
    rtc_seal();

    esp_err_t err = nvs_save(args, session_id, start_epoch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "arm: nvs_save failed: %s", esp_err_to_name(err));
        memset(&s_rtc, 0, sizeof(s_rtc));
        return err;
    }
    ESP_LOGI(TAG, "armed %s: %lu captures, %lus interval, vis=%d therm=%d, wakeWifi=%d window=%lus every=%lu",
             session_id,
             (unsigned long)args->max_captures,
             (unsigned long)args->interval_sec,
             (int)args->capture_vis, (int)args->capture_therm,
             (int)args->wake_wifi, (unsigned long)args->wake_window_sec,
             (unsigned long)s_rtc.wake_wifi_every);
    return ESP_OK;
}

void ds_scheduler_abort(void) {
    if (rtc_valid()) {
        ESP_LOGW(TAG, "abort: clearing %s at seq=%lu",
                 s_rtc.session_id, (unsigned long)s_rtc.next_seq);
    }
    memset(&s_rtc, 0, sizeof(s_rtc));
    nvs_clear();
}

esp_err_t ds_scheduler_run_one_cycle(void) {
    if (!rtc_valid()) {
        // Try to repopulate from NVS (e.g. armed in previous boot,
        // RTC lost, but NVS active=1 → cold-boot resume).
        ds_arm_args_t args = {0};
        char sid[32] = {0};
        uint64_t start = 0;
        if (!nvs_load(&args, sid, sizeof(sid), &start)) {
            return ESP_ERR_INVALID_STATE;
        }
        memset(&s_rtc, 0, sizeof(s_rtc));
        snprintf(s_rtc.session_id, sizeof(s_rtc.session_id), "%s", sid);
        s_rtc.next_seq            = 1;  // safe lower bound — replay will reconcile
        s_rtc.max_captures        = args.max_captures;
        s_rtc.interval_sec        = args.interval_sec;
        s_rtc.session_start_epoch = start;
        s_rtc.next_due_epoch      = start;
        s_rtc.capture_vis         = args.capture_vis  ? 1 : 0;
        s_rtc.capture_therm       = args.capture_therm ? 1 : 0;
        s_rtc.wake_wifi           = args.wake_wifi ? 1 : 0;
        s_rtc.wake_window_sec     = args.wake_window_sec;
        s_rtc.wake_wifi_every     = args.wake_wifi_every ? args.wake_wifi_every : 1;
        rtc_seal();
        ESP_LOGW(TAG, "run_one_cycle: rebuilt RTC from NVS (cold-boot resume)");
    }

    int64_t wake_t0 = esp_timer_get_time();

    // Sweep any orphans from a prior aborted commit (cheap when clean).
    session_store_recover_all();

    // Bring up just the sensors we need.
    bool need_vis   = s_rtc.capture_vis  != 0;
    bool need_therm = s_rtc.capture_therm != 0;

    uint32_t lepton_boot_ms = 0, camera_boot_ms = 0;
    bool lepton_ok = need_therm ? bring_up_lepton(&lepton_boot_ms) : true;
    bool camera_ok = need_vis   ? bring_up_camera(&camera_boot_ms) : true;
    load_user_settings();

    // First-capture-only FFC. The session_store has nothing yet
    // (capture_count==0) iff this is seq 1.
    if (lepton_ok && need_therm && s_rtc.next_seq == 1) {
        hal_lepton_run_initial_ffc();
    }

    // Capture artifacts.
    capture_artifacts_t art = {0};
    esp_err_t cap_err = capture_engine_take_one(
        need_vis && camera_ok,
        need_therm && lepton_ok,
        /*want_thermal_raw*/ need_therm && lepton_ok,
        &art);
    if (cap_err != ESP_OK) {
        ESP_LOGW(TAG, "wake seq=%lu: capture_engine produced nothing",
                 (unsigned long)s_rtc.next_seq);
        // Fall through and still commit a journal record — recording
        // the failure is more useful than silently re-trying.
    }

    // Open (or resume) the session store and commit.
    session_store_open_args_t open_args = {
        .mode          = "timelapse",
        .interval_sec  = s_rtc.interval_sec,
        .capture_vis   = need_vis,
        .capture_therm = need_therm,
    };
    session_store_handle_t *store = session_store_open(s_rtc.session_id, &open_args);
    if (!store) {
        ESP_LOGE(TAG, "wake seq=%lu: session_store_open failed — sleeping again",
                 (unsigned long)s_rtc.next_seq);
        capture_artifacts_free(&art);
        s_rtc.consecutive_failures++;
        rtc_seal();
        // Sleep for the interval and retry; abort after too many
        // consecutive failures to avoid burning battery on a stuck SD.
        if (s_rtc.consecutive_failures > 5) {
            ESP_LOGE(TAG, "%lu consecutive failures — aborting session",
                     (unsigned long)s_rtc.consecutive_failures);
            ds_scheduler_abort();
            return ESP_OK;     // continue to normal boot for triage
        }
        power_manager_prep_for_sleep(need_vis, need_therm);
        power_manager_enter_deep_sleep((uint64_t)s_rtc.interval_sec * 1000000ULL);
    }

    // Build the per-capture metadata.
    uint64_t actual_epoch = (uint64_t)time(NULL);
    uint64_t intended_epoch = 0;
    uint8_t  time_quality = 0;
    if (s_rtc.session_start_epoch > 0) {
        intended_epoch = s_rtc.session_start_epoch +
                         (uint64_t)(s_rtc.next_seq - 1) * (uint64_t)s_rtc.interval_sec;
    }
    if (actual_epoch >= 1700000000ULL) {
        time_quality = 2;   // synced
    } else if (s_rtc.session_start_epoch > 0) {
        time_quality = 1;   // estimated from session start + intervals
        actual_epoch = intended_epoch;
    } else {
        time_quality = 0;   // unknown
    }
    uint32_t wake_ms = (uint32_t)((esp_timer_get_time() - wake_t0) / 1000);
    session_capture_meta_t meta = {
        .intended_epoch = intended_epoch,
        .time_quality   = time_quality,
        .wake_ms        = wake_ms,
    };
    (void)actual_epoch;   // session_store fills its own actual_epoch via time()

    esp_err_t commit_err = session_store_commit(store, &art, &meta);
    capture_artifacts_free(&art);

    if (commit_err == ESP_OK) {
        s_rtc.consecutive_failures = 0;
    } else {
        s_rtc.consecutive_failures++;
        ESP_LOGW(TAG, "wake seq=%lu: commit failed (%s) — count=%lu",
                 (unsigned long)s_rtc.next_seq, esp_err_to_name(commit_err),
                 (unsigned long)s_rtc.consecutive_failures);
    }

    bool last_capture = (s_rtc.next_seq >= s_rtc.max_captures);

    // Wake-window phase (PR-D): post-commit, before sleep, optionally
    // bring up Wi-Fi + relay so the dashboard can see in-progress
    // sessions and issue timelapse.stop. Skipped on the last capture
    // (we're about to return to normal boot anyway, which brings up
    // Wi-Fi + relay through the standard path).
    bool stop_received = false;
    if (!last_capture && s_rtc.wake_wifi && s_rtc.wake_window_sec > 0) {
        stop_received = run_wake_window(s_rtc.wake_window_sec);
    }

    if (last_capture || stop_received) {
        // Finalize: complete=true, free handle, clear all DS state.
        session_store_close(store);
        nvs_clear();
        memset(&s_rtc, 0, sizeof(s_rtc));
        ESP_LOGI(TAG, "%s — returning to normal boot",
                 stop_received ? "session stopped via wake-window cmd"
                               : "session complete");
        return ESP_OK;
    }

    // More captures to come — release handle (no complete=true), update
    // RTC, sleep until next due time.
    session_store_release(store);
    s_rtc.next_seq++;
    s_rtc.cumulative_awake_ms += wake_ms;
    if (s_rtc.session_start_epoch > 0) {
        s_rtc.next_due_epoch = s_rtc.session_start_epoch +
                               (uint64_t)(s_rtc.next_seq - 1) * (uint64_t)s_rtc.interval_sec;
    } else {
        // Without epoch, schedule by elapsed time only.
        s_rtc.next_due_epoch = 0;
    }
    rtc_seal();

    // Decide sleep duration.
    uint64_t sleep_us;
    uint64_t now_epoch = (uint64_t)time(NULL);
    if (s_rtc.next_due_epoch > 0 && now_epoch >= 1700000000ULL &&
        s_rtc.next_due_epoch > now_epoch) {
        sleep_us = (s_rtc.next_due_epoch - now_epoch) * 1000000ULL;
    } else {
        // Fall back to "interval after this awake period" — cadence
        // will drift but the count is preserved (countStrict policy).
        sleep_us = (uint64_t)s_rtc.interval_sec * 1000000ULL;
    }
    s_rtc.cumulative_sleep_ms += sleep_us / 1000ULL;
    rtc_seal();

    ESP_LOGI(TAG, "wake seq=%lu/%lu OK (wake=%lums); sleeping %llus",
             (unsigned long)(s_rtc.next_seq - 1),    // just-committed
             (unsigned long)s_rtc.max_captures,
             (unsigned long)wake_ms,
             (unsigned long long)(sleep_us / 1000000ULL));

    power_manager_prep_for_sleep(need_vis, need_therm);
    power_manager_enter_deep_sleep(sleep_us);
    // Unreachable.
    return ESP_OK;
}

esp_err_t ds_scheduler_maybe_handle_wake(void) {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    bool timer_wake = (cause == ESP_SLEEP_WAKEUP_TIMER);

    // Cold-boot path with a stale NVS active=1 (interrupted prior
    // session, e.g. brown-out before commit): in PR-C we clear the
    // marker and continue normal boot. session_store_recover_all
    // (called separately by app_main) handles any orphan files.
    // Future: option to auto-resume.
    if (!timer_wake) {
        ds_arm_args_t args = {0};
        char sid[32] = {0};
        if (nvs_load(&args, sid, sizeof(sid), NULL)) {
            ESP_LOGW(TAG, "cold boot found NVS-active session %s — clearing (no auto-resume in PR-C)",
                     sid);
            nvs_clear();
            memset(&s_rtc, 0, sizeof(s_rtc));
        }
        return ESP_ERR_INVALID_STATE;   // not a DS wake
    }

    // Timer wake — must have either valid RTC OR NVS active=1.
    if (!rtc_valid()) {
        ds_arm_args_t args = {0};
        char sid[32] = {0};
        if (!nvs_load(&args, sid, sizeof(sid), NULL)) {
            ESP_LOGW(TAG, "timer wake but no DS state anywhere — treating as normal boot");
            return ESP_ERR_INVALID_STATE;
        }
        // run_one_cycle will rebuild RTC from NVS.
    }

    return ds_scheduler_run_one_cycle();
}

// ---- Status accessors ----

ds_state_t ds_scheduler_state(void) {
    if (rtc_valid() && s_rtc.next_seq <= s_rtc.max_captures) return DS_ACTIVE;
    // NVS-only state — cold boot interrupted, will resume on next cmd.
    nvs_handle_t h;
    if (nvs_open(DS_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t a = 0;
        nvs_get_u8(h, "active", &a);
        nvs_close(h);
        if (a) return DS_ACTIVE;
    }
    return DS_INACTIVE;
}

uint32_t ds_scheduler_next_seq(void) {
    return rtc_valid() ? s_rtc.next_seq : 0;
}

uint32_t ds_scheduler_max_captures(void) {
    return rtc_valid() ? s_rtc.max_captures : 0;
}

const char *ds_scheduler_session_id(void) {
    return rtc_valid() ? s_rtc.session_id : "";
}
