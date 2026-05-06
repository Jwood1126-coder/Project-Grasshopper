# Grasshopper — Architecture & Migration Plan

> **Status:** Phase 1 of [the rewrite plan](./prompt.md). This document is
> the design contract: stack choice, module map, what we recycle from
> [Scout Fox](https://github.com/Jwood1126-coder/thermal-scout-fox), what
> we discard, and the protocol/state-machine shape. Nothing is built
> yet. Approval here gates phase 2 (scaffold).

---

## 1. Product framing

A battery-powered dual-camera (visible RGB + thermal LWIR) field unit
that streams live to a phone over local WiFi (or its own SoftAP) and
captures unattended timelapses to SD with both modalities. Hardware is
fixed; we're rewriting the software around it.

**Non-goals:** cloud sync, accounts, fleet management, BLE pairing.
Design hooks for them, do not implement.

**Differentiators vs. tCam-Mini / FLIR ONE:** dual-camera (not
thermal-only), unattended deep-sleep timelapse with battery budgeting,
on-device session browser, no cloud dependency.

## 2. Stack — recommendation: ESP-IDF v5.x

**Pick: ESP-IDF v5.x with the Arduino-as-component layer optionally
available for the camera HAL only.** Justification, in one paragraph:

Fox's last few sessions burned heavily on Arduino-ESP32 plumbing —
AsyncTCP/ESPAsyncWebServer 3.x had recurring core-pinning issues
(AsyncTCP defaults to Core 1, fighting the VoSPI reader pinned at
Core 1, priority 10 — see [HANDOFF.md:193](#)), `WS_MAX_QUEUED_MESSAGES`
overrides didn't propagate from `arduino-cli --build-property`, and at
least two same-endpoint static-buffer concurrency hazards
(`handleStatus`, `handleSyncCapture` at
[thermal-scout-fox.ino:1373, 2480](#)) survive only because the user
doesn't open two tabs at once. ESP-IDF gives us
`esp_http_server` (clean async, per-request response context, native
WS), proper Kconfig/menuconfig, first-class OTA + signed images +
two-slot rollback, NVS, partition table, and `idf.py monitor` with
backtrace decode. Pinning the VoSPI task to Core 1 and HTTP/Wi-Fi to
Core 0 is just a `xTaskCreatePinnedToCore` argument, not a build-flag
fight. The user already has the IDF v5.3 toolchain installed for
[Eagle](#) so there's no new infra. The OV2640 camera driver is
the one piece more polished in Arduino — we'll pull it in via
`arduino-esp32` as a managed component (`espressif/arduino-esp32`),
which IDF supports cleanly.

## 3. Repo layout

```
Project-Grasshopper/
├── firmware/                    ESP-IDF v5.x project
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   ├── partitions.csv           factory + ota_0 + ota_1 + nvs + littlefs + spiffs
│   ├── main/
│   │   ├── CMakeLists.txt
│   │   └── app_main.c           bootstraps components, then idle
│   └── components/
│       ├── hal_lepton/          VoSPI reader + CCI (recycled from Fox)
│       ├── hal_camera/          OV2640/5640 wrapper (esp32-camera)
│       ├── hal_storage/         LittleFS (assets) + SD_MMC (sessions)
│       ├── hal_oled/            SSD1306 + screen scheduler
│       ├── hal_power/           MOSFETs, brownout, battery estimate
│       ├── net_wifi/            STA + SoftAP fallback + captive portal
│       ├── net_http/            esp_http_server + REST handlers
│       ├── net_ws/              WebSocket: init / tick / frame / event
│       ├── net_relay/           outbound WSS to Railway debug relay (opt-in)
│       ├── app_state/           single source of truth + transitions
│       ├── app_capture/         sync + async capture pipeline
│       ├── app_timelapse/       normal + deep-sleep, shared writer
│       ├── app_session/         SD layout, indexer, exporters
│       ├── app_ota/             signed OTA + rollback
│       └── proto/               generated C structs from proto/schema.ts
├── app/                         Vite + Preact + TS frontend
│   ├── src/
│   │   ├── main.tsx
│   │   ├── routes/{Live,Capture,Library}.tsx
│   │   ├── lib/{ws.ts, render-thermal.ts, palette.ts, range.ts}
│   │   └── proto/               generated TS from proto/schema.ts
│   ├── vite.config.ts           builds → app.html.gz, flashed to LittleFS
│   └── package.json
├── proto/
│   ├── schema.ts                source of truth for WS messages
│   ├── codegen.py               emits C structs + serializers + TS types
│   └── README.md
├── relay/                       Bun + Hono debug sidecar (deployed to Railway)
│   ├── src/
│   │   ├── server.ts            WS + dashboard
│   │   ├── ringbuf.ts
│   │   ├── store.ts             devices, logs, frames, panics
│   │   └── dashboard/           static HTML/JS, no framework
│   ├── package.json
│   └── tsconfig.json
├── docs/
│   ├── grasshopper-architecture.md   ← this file
│   ├── hardware.md              pin map + the GPIO 48→GPIO 1 SDA story
│   ├── protocol.md              wire protocol details (after schema lands)
│   └── reliability.md           soak + brownout + OTA rollback test plan
├── Dockerfile                   relay build (repo-root context)
├── railway.json                 Railway build config
├── .dockerignore
├── .github/workflows/           CI (firmware build, app build, relay deploy)
├── .gitignore
└── README.md
```

## 4. Module responsibilities

### Firmware components

| Component       | Responsibilities                                                    | Recycle source           |
|-----------------|---------------------------------------------------------------------|--------------------------|
| `hal_lepton`    | VoSPI reader task, double-buffer in PSRAM, splice detector, CCI commands | Fox `lepton_vospi.h` + `lepton_cci.h` (lift wholesale) |
| `hal_camera`    | OV2640/OV5640 init, frame grab (`CAMERA_GRAB_LATEST`), JPEG quality, framesize | Fox `setup()` cam-init block, [thermal-scout-fox.ino:4098–4142](#) |
| `hal_storage`   | LittleFS for `app.html.gz` + boot assets; SD_MMC for sessions; single `sd_mutex` | Fox SD_LOCK macros + atomic-write helper |
| `hal_oled`      | SSD1306 driver wrapper + screen scheduler (boot, status, runtime, OTA progress) | Fox OLED screens 0/1, runtime task |
| `hal_power`     | Lepton MOSFET, brownout monitor, battery mAh estimate                | Fox power-est block      |
| `net_wifi`      | STA priority list, SoftAP fallback, captive portal, NTP              | Fox `setupWiFi`, `startApFallback` ([thermal-scout-fox.ino:3447–3489](#)) |
| `net_http`      | `esp_http_server`, all `/api/v1/*` handlers, ETag on `/`             | Fox handler logic (re-shaped per handler)    |
| `net_ws`        | WS server at `/ws`, init/tick split, binary frame push               | Fox onWsEvent + tick/init builders           |
| `net_relay`     | Outbound WSS client to Railway, exponential backoff, opt-in toggle   | new                      |
| `app_state`     | Device state machine + atomic snapshot struct                        | new (replaces 80+ free globals) |
| `app_capture`   | Sync `/capture/sync`, async fan-out to capture-saved subscribers     | Fox `handleSyncCapture` ([:2480](#)) |
| `app_timelapse` | Normal mode loop + deep-sleep RTC_DATA + shared session writer       | Fox normal+DS paths, unified |
| `app_session`   | SD session dir layout, indexer, exporter (zip), orphan finalize on cold boot | Fox SD writer + orphan finalize ([:708–948](#)) |
| `app_ota`       | Signed app image, two-slot, rollback on bootloop, panic-log persist  | new                      |
| `proto`         | Generated C structs + serializers (compile-time)                     | from `proto/schema.ts`   |

### Frontend (`app/`)

| Module            | Responsibilities                                            | Recycle from Fox `html.h` |
|-------------------|-------------------------------------------------------------|----------------------------|
| `routes/Live`     | Full-bleed swipeable view (visible / thermal / blended), bottom sheet for palette / range / gain / FFC / orientation | range modes, palette, orientation lock, render kernel ([html.h:500, 650–820](#)) |
| `routes/Capture`  | Single-tap snapshot + timelapse drawer (interval, duration cap, deep-sleep toggle, "recording now" indicator from `Snapshot.timelapse`) | timelapse form fields, deep-sleep options |
| `routes/Library`  | Server-rendered thumbnails, infinite scroll, lightbox w/ V/T toggle + arrow keys, per-session zip export | lightbox component ([html.h:1200–1450](#)) |
| `lib/ws.ts`       | Single WS connection, schema-aware message dispatch         | new                        |
| `lib/render-thermal.ts` | Canvas kernel for 160×120 uint16 → RGBA w/ palette + range + orientation | render kernel from `html.h` |
| `lib/palette.ts`  | Iron, Rainbow, Grayscale, etc.                              | palette LUTs               |
| `lib/range.ts`    | Auto / Lock / Manual / HistEQ                               | `computeRange`, `buildHistLUT`, `mapPixel` |

Build target: single `app.html.gz` flashed to LittleFS partition. Dev
mode runs `vite dev` against a recorded `/ws` capture (mock) so frontend
iteration doesn't need a connected device.

### Relay (`relay/`, deployed to Railway)

Already seeded ([phase 0 commit](#)). Lives at
**`https://project-grasshopper-production.up.railway.app/`**.

| Endpoint                          | Purpose |
|-----------------------------------|---------|
| `GET /health`                     | Liveness probe (now live) |
| `GET /api/devices`                | List of connected devices, last-seen, fw_version, state |
| `GET /api/devices/:id/state`      | Latest `Snapshot` |
| `GET /api/devices/:id/logs?since` | Ring-buffer log tail (JSON) |
| `GET /api/devices/:id/last-frame.jpg?modality=vis|thermal` | Latest preview frame |
| `GET /api/devices/:id/panics`     | Recent crash dumps |
| `POST /api/devices/:id/cmd`       | Relay-to-device command (token-gated) |
| `WS /relay`                       | Devices connect outbound, identify with token |
| `GET /`                           | Human dashboard (also agent-readable HTML) |

## 5. What we recycle from Fox (lift wholesale)

Concrete file:line refs into Fox so phase 2 knows what to copy:

| Asset | Fox location | Why preserve |
|-------|--------------|--------------|
| VoSPI reader, double-buffer in PSRAM, mutex, frame deadline (35ms), per-segment gap (22ms) | [`lepton_vospi.h:1–200`](#) | Field-tuned; touching this without instrumentation is risky |
| `vospi_frame_has_splice()` content-based splice detector at row boundaries 30/60/90, 2.5× ratio + 8000 centi-K floor | [`lepton_vospi.h`](#) | Catches cross-broadcast splices the timing guards miss; the hard part of franken handling |
| `vospi_abort_frame()` — zeros 76KB write buffer on every reset path | [`lepton_vospi.h:124–132`](#) | Architectural fix that addressed the 3-band pattern; ~1 ms cost on uncommon paths |
| CCI driver + shared `wire_mutex` w/ OLED | [`lepton_cci.h:1–343`](#) | Register-level I2C protocol; works |
| Pin map + the GPIO 48 → GPIO 1 SDA fix story | [`pins.h:1–66`](#) | GPIO 48 drives onboard flash LED; SDA pull-up was holding the LED on. Documented as `docs/hardware.md` |
| AP-fallback flow + SSID/password (`scout-fox-setup` / `scoutfox123` at `192.168.4.1`) | [`thermal-scout-fox.ino:3447–3469`](#) | Handles iPhone hotspot 5GHz, no-known-SSID-at-boot, after-4-timeouts trigger |
| Deep-sleep `RTC_DATA_ATTR` struct (`magic = 0xDEAD5EEF`) + cold-boot orphan finalize | [`thermal-scout-fox.ino:200–216`, `:708–876`](#) | Survives crash mid-session; finalizes interrupted timelapses on next boot |
| Atomic SD-write 3-tier fallback (tmp+rename → delete+rename → direct) | [`thermal-scout-fox.ino:879–948`](#) | Hard-won; FATFS rename semantics are not always honored |
| PSRAM-backed reusable buffers (`psWsThermBuf`, `psFrameBuf`, `psDbgFrame`, `psStatusBuf`, `psWsTickBuf`, `psWsInitBuf`) | [`thermal-scout-fox.ino:97–122, 3949–3969`](#) | Frees ~118 KB of internal SRAM for AsyncTCP/SD |
| Range modes (Auto / Lock / Manual / HistEQ) | [`html.h:~750–820`](#) | All client-side; lift the kernel directly |
| Palette set (Iron, Rainbow, Grayscale, etc.) | [`html.h:~650–750`](#) | LUTs |
| Lightbox V/T toggle + arrow-key nav | [`html.h:~1200–1450`](#) | Used pattern; works |
| Session model (`session.json` + `captures.jsonl` + `NNNNNN_vis.jpg` + `NNNNNN_therm.json`) | Fox SD writers | Append-only `captures.jsonl` is the right primitive; keep it |
| `orientLockUntil` optimistic UI guard (500 ms after orientation interaction) | [`html.h:~500, ~1700`](#) | Prevents flicker; cheap |

## 6. What we throw out from Fox

| Asset | Fox location | Why discard |
|-------|--------------|-------------|
| Monolithic `.ino` (4626 lines) | [`thermal-scout-fox.ino`](#) | Mixes WiFi, web server, OLED, camera, capture, timelapse, OTA, debug. Not maintainable as a commercial codebase. |
| PROGMEM HTML blob (1880 lines) | [`html.h`](#) | Edit-compile-flash cycle for every UI tweak. Replaced with Vite + LittleFS-served `app.html.gz`. |
| ~80 free-floating globals (camera state, WiFi, Lepton, OLED, capture, NVS, deep-sleep, timelapse) | [`thermal-scout-fox.ino:15–250`](#) | Source of orientation-drift bug ([HANDOFF #3 issue](#)) and `tlSessionId not in /status` bug. Replaced with single snapshot struct in `app_state`. |
| Hand-rolled `snprintf` JSON for `/status`, init, tick | [`:1373–1650`](#) | Brittle; the bug "schema knows about `tlSessionId` but `/status` doesn't" is a class of bug we eliminate via codegen. |
| Three styles of debug endpoint: sync (`flip`, `config`), async (`sync capture`), spawn-and-poll (`/debug/cci`, `/debug/reset`, `/debug/i2c_test`, `/debug/job`) | [`:4203, 4216, 4249, 4262, 4276, 4324`](#) | Pick one async pattern. All long-running ops return 202 + work in a job; UI polls `/api/v1/jobs/:id` uniformly. |
| Static-buffer concurrency hazard in `handleStatus` / `handleSyncCapture` (response built into `static char buf[]`, async send captures pointer; concurrent requests corrupt each other) | [`:1373, 2480`](#) | Each response gets its own backing buffer (`esp_http_server`'s normal pattern). |
| Orientation state in 3 places (server globals, JS, session.json snapshot) | [`:166–171`, `html.h:500`, session writes at `:3330–3336`] | Single source of truth in `app_state.snapshot.orientation`. Frontend reads from `Snapshot`, captures.jsonl logs the snapshot value at frame-grab time. |
| Half-done telemetry-header / frame-counter dedup (`lep_enable_telemetry` defined but unused) | [`lepton_cci.h`, comment in HANDOFF deferrals](#) | Punted in Fox; punted again in Grasshopper v1. Telemetry adds 240 bytes to seg 1; driver pixel-offset arithmetic must change. Re-evaluate after v1 ships. |
| `/status` polling baseline (the WS subsumes it) | [`:1373`](#) | REST `/status` stays for one polling fallback only; primary status comes from WS init/tick. |
| Stale `wifi_credentials.h` (verbatim copy from `scout-delta` with `#line` directive that re-defined `WiFiNetwork`) | Fox repo | Stub out; secrets via NVS (set by captive portal, never in-tree). |

## 7. Schema & codegen

**One source file:** [`proto/schema.ts`](#). Hand-written. Defines:

```ts
// Sketch — final shape lands in phase 2.
export type DeviceState =
  | "BOOT" | "SAFE_MODE" | "IDLE" | "STREAMING"
  | "CAPTURING" | "TIMELAPSE_ACTIVE" | "DEEP_SLEEP_PREP" | "SHUTDOWN";

export type Orientation = {
  visHFlip: boolean; visVFlip: boolean; visRot: 0|90|180|270;
  thermHFlip: boolean; thermVFlip: boolean; thermRot: 0|90|180|270;
};

export type Snapshot = {
  schemaVersion: 1;
  fwVersion: string;
  gitSha: string;
  deviceId: string;
  state: DeviceState;
  uptimeMs: number;
  freeHeap: number;
  freePsram: number;
  wifi: { mode: "STA"|"AP"; ssid: string; rssi: number; ip: string };
  ntpSynced: boolean;
  epoch: number;
  battery?: { mah: number; estPctRemaining: number };
  visible: { fps: number; w: number; h: number; quality: number };
  thermal: {
    fps: number; gain: "high"|"low"|"auto"; agc: boolean;
    spliceDetected: number; lastFFCMs: number;
  };
  orientation: Orientation;
  storage: { totalKB: number; usedKB: number; sessions: number };
  timelapse?: {
    active: boolean; mode: "normal"|"deep_sleep";
    sessionId: string; sessionDir: string;
    capturesDone: number; capturesTotal?: number;
    intervalMs: number; nextCaptureInMs: number;
    startEpoch: number;
  };
  relay?: { connected: boolean; lastTickMs: number };
  lastError?: { code: string; msg: string; ts: number };
};

export type Tick = Partial<Snapshot> & { type: "tick" };
export type Init = Snapshot & { type: "init" };

export type ThermalFrameHeader = {
  // 32 bytes, fixed layout — wire as a packed struct, not JSON.
  type: "frame";
  modality: "thermal";
  seqNo: number;       // u32, monotonic per-boot
  epochMs: number;     // u64
  minCK: number;       // u16, frame min in centi-K
  maxCK: number;       // u16, frame max in centi-K
  gainMode: 0|1|2;     // u8
  orientationByte: number; // u8 packed (vis flips/rot + therm flips/rot)
  flagsByte: number;   // u8 (bit 0 = splice_detected)
  reserved: number;    // u32
};

export type Event =
  | { type: "event"; kind: "capture-saved"; sessionId: string; idx: number }
  | { type: "event"; kind: "ffc-done"; durationMs: number }
  | { type: "event"; kind: "state-changed"; from: DeviceState; to: DeviceState }
  | { type: "event"; kind: "error"; code: string; msg: string };

export type Command =
  | { type: "cmd"; id: string; op: "ffc" }
  | { type: "cmd"; id: string; op: "set-orientation"; orientation: Orientation }
  | { type: "cmd"; id: string; op: "tl-start"; intervalMs: number; deepSleep: boolean; cap?: number }
  | { type: "cmd"; id: string; op: "tl-stop" }
  | { type: "cmd"; id: string; op: "reboot" };

// Same schema covers the relay link. Just an outer envelope:
export type RelayMsg =
  | { v: 1; from: string; ts: number; payload: Init|Tick|Event|ThermalFrameHeader|LogLine|Panic|FramePreview }
  | { v: 1; cmd: Command };
```

**Codegen** (`proto/codegen.py`, ~200 lines):
- Reads `schema.ts` AST via TypeScript compiler API or a simpler regex pass over the typed unions.
- Emits:
  - `firmware/components/proto/include/proto.h` — packed C structs, enums, field constants.
  - `firmware/components/proto/src/proto_serialize.c` — `proto_init_to_json(...)`, `proto_tick_to_json(...)` etc., writing into a caller-provided buffer (no malloc), with explicit length checks.
  - `app/src/proto/index.ts` — the exact same TS types, re-exported for the frontend.
- Run as a CMake pre-build step in firmware; as a Vite plugin in the frontend.
- **CI gate:** the firmware build fails if a field appears in `schema.ts` but isn't covered by the serializer (the `tlSessionId not in /status` class of bug becomes uncompilable).

**Not protobuf**, by design. The relay log tail must be JSON-readable; binary protobuf turns logs into hex. Frame payloads stay binary via the fixed-layout `ThermalFrameHeader` struct + the raw uint16 pixel block; only the JSON envelope is schema-managed.

## 8. Wire protocol (LAN WS at `/ws`)

```
client connects to ws://<device>/ws
device sends:                  Init  (full Snapshot, ~2KB JSON)
client receives:               Init  → renders UI

every 1500 ms:
device sends:                  Tick  (diff vs. last Snapshot, ~300B JSON)

every ~150 ms (~6 Hz):
device sends:                  binary message:
                                 [32 bytes ThermalFrameHeader]
                                 [38400 bytes uint16 LE pixel data]
                               total 38432 bytes

on capture/state/error:
device sends:                  Event (JSON, <200B)

client → device commands:      JSON Command messages.
device replies with:           Event { kind: "cmd-result", id, ok, ... }
```

The MJPEG visible stream stays out-of-band on `GET /api/v1/cam/stream`
(simpler than tunneling JPEG over WS, and lets browsers natively render
it without a JS decode path).

REST `/api/v1/status` returns the same Snapshot for clients that can't
do WS (one fallback path; not the primary).

## 9. State machine

Single source of truth in `app_state`. All transitions logged + the
target state persisted to NVS so cold-boot can decide whether to
finalize an orphaned timelapse.

```
                     ┌────────┐
                     │  BOOT  │
                     └────┬───┘
                          │ if NVS shows tl session pending
                          ▼
                  ┌─────────────────┐
                  │  finalize orphan │ (run, then transition to IDLE)
                  └─────────┬───────┘
                            │
        ┌───────────────────┼─────────────────────────┐
        ▼                   ▼                         ▼
 ┌────────────┐      ┌────────────┐           ┌────────────────┐
 │  SAFE_MODE │      │   IDLE     │ ◄───────► │   STREAMING     │
 │ (Lepton    │      │ (no clients,│           │ (≥1 WS client)  │
 │  failed)   │      │  no TL)    │           │                 │
 └────────────┘      └────┬───────┘           └────────┬───────┘
                          │                            │
                          │ POST /timelapse/start      │
                          ▼                            ▼
                  ┌─────────────────────┐    ┌─────────────────┐
                  │ TIMELAPSE_ACTIVE    │    │   CAPTURING     │
                  │ (normal or DS)      │    │ (one-shot snap) │
                  └─────┬───────┬───────┘    └────────┬────────┘
                        │       │                     │
                        │       │ deepSleep flag      │
                        │       ▼                     │
                        │ ┌─────────────────┐         │
                        │ │ DEEP_SLEEP_PREP │         │
                        │ └────────┬────────┘         │
                        │          │ esp_deep_sleep   │
                        │          ▼                  │
                        │       (sleep)               │
                        │                             │
                        └─────────────────────────────┘
                          on stop or completion → IDLE
```

Allowed transitions are an explicit table; everything else is dropped
with a warning and an `Event { kind: "error" }` push.

Capture and timelapse pull `orientation`, `range`, `palette`, `gain`
from the live Snapshot at frame-grab time and write them into
`captures.jsonl` per capture (not just `session.json`). Lightbox
reads the per-capture entry. **No drift.**

## 10. Capture pipeline & timelapse

One pipeline; multiple subscribers.

```
hal_camera.grab()  ────┐
                       │
hal_lepton.latest() ───┼──► app_capture.snap()   ──► returns CaptureResult
                       │       (atomic snapshot of   {visBytes, thermPixels,
                       │        both modalities + a   orientation, range,
                       │        snapshot of state)    fwVersion, ts, ...}
                       │            │
                       │            ▼
                       │      subscribers:
                       │        - HTTP /capture/sync handler (sends as JSON)
                       │        - WS event listener (pushes "capture-saved")
                       │        - app_session.write() (atomic SD write)
                       │
hal_storage.sd_mutex   │
```

Normal-mode timelapse and deep-sleep timelapse both go through
`app_session.write()`. The same `session.json` and `captures.jsonl`
schema. Cold-boot orphan finalize runs first thing in `app_state`
init.

## 11. SD layout (unchanged conceptually from Fox)

```
/timelapse/
  sessions.jsonl                 append-only index of completed sessions
  session_<NNNNNNNN>/            zero-padded session ID
    session.json                 metadata snapshot at start, rewritten
                                   every 10 captures + on finalize
    captures.jsonl               per-capture record, append-only:
                                   {idx, ts, vis: {file, w, h}, therm: {file, w, h, min, max, gain},
                                    orientation: {...}, palette: "iron", range: {...}}
    000000_vis.jpg
    000000_therm.json            uint16 array compressed via base85? or binary?
    000001_vis.jpg
    000001_therm.json
    ...
```

**Open question for phase 2:** binary thermal frames on SD
(`000000_therm.bin`, raw 38400 bytes) are 24× smaller than the JSON
representation Fox uses (`{"data":"AAAA..."}` base85). Decoders only
need a 32-byte header inline. Recommend binary.

## 12. Reliability bar (commercial floor)

These tests must pass before any release tag:

| Test | Pass criteria |
|------|---------------|
| **1-hour soak** with 1 WS client + 5s timelapse + visible MJPEG | Free heap stable (drift < 5KB), no franken in the stream, no missed timelapse intervals |
| **Brownout mid-capture** | Capture in flight is finalized on next boot via orphan recovery |
| **OTA happy path** | Two-slot signed image, switches partition, boots into new slot |
| **OTA bad image rollback** | Bootloop detection rolls back to known-good slot within 3 boot attempts |
| **Factory reset** | Long-press boot button (or `/api/v1/factory-reset` with a token) clears NVS and SD `/grasshopper-config/` |
| **Crash log retention** | Last 4 panics persisted to NVS, retrievable from UI and from relay |
| **AP fallback** | After 4 STA timeouts, SoftAP comes up and serves UI within 8s |
| **Deep-sleep cycle, 24 captures** | Battery estimate within 10% of measured, no orphans |

Test suite lives in `firmware/test/host/` (host-runnable splice
detector + session-writer fuzz) and `firmware/test/on_device/`
(smoke + brownout + OTA via a powered USB-C relay).

## 13. Debug relay (Railway sidecar)

**URL:** `https://project-grasshopper-production.up.railway.app/`
**Status:** seeded in phase 0. `GET /health` returns
`{"ok":true,...}`. WS endpoint and dashboard land in phase 2.

Constraints (unchanged from prompt §12):
- **Outbound-only from device** — no port-forwarding. Works on iPhone
  hotspot, home WiFi, anywhere.
- **Off by default.** Enabled via Kconfig + NVS toggle + per-unit
  token. UI shows "debug relay ON" pill when active.
- **Tokenized.** Per-unit `device_id` + bearer token; relay rejects
  unauthenticated traffic.
- **Privacy.** Visible/thermal preview frames downsampled to ≤80×60 +
  JPEG q=20 before upload. Full-res only on explicit one-shot command.
- **Soft-fail.** If relay unreachable, drop and log locally; reconnect
  with exponential backoff capped at 5 min. **Never** affects product
  UX.

The dashboard is built **agent-first**: every panel has a JSON
endpoint that's `WebFetch`-able, stable URLs, structured fields. The
agent operating on this codebase should be able to soak-test by
hitting these endpoints rather than asking the user "what does the
device show?"

## 14. Frontend build

```
app/
  src/main.tsx              mounts <App/>
  src/App.tsx               three-tab shell (Live / Capture / Library)
  src/lib/ws.ts             one WS connection, schema-typed dispatch
  src/lib/render-thermal.ts canvas kernel, recycled from Fox
  src/lib/palette.ts        LUTs (Iron, Rainbow, Grayscale...)
  src/lib/range.ts          Auto/Lock/Manual/HistEQ
  src/proto/                generated from proto/schema.ts (codegen)

vite.config.ts:
  - viteSingleFile() bundles into one HTML
  - postbuild script:
      gzip dist/index.html → dist/index.html.gz
      copy → ../firmware/main/assets/app.html.gz
  - LittleFS partition is built from main/assets/ at flash time
```

The firmware serves `app.html.gz` from LittleFS with
`Content-Encoding: gzip`. Every browser handles it natively. Cache
hint: `Cache-Control: public, max-age=600, must-revalidate` + ETag
based on git SHA.

Dev mode runs Vite against a recorded `/ws` capture
(`app/dev/recordings/*.ndjson`), so we iterate on UI without a
flashed device.

## 15. Build & flash

```bash
# firmware
cd firmware
. ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor   # ESP-IDF native USB CDC

# frontend  (re-runs codegen, builds, drops gz into firmware/main/assets/)
cd app
bun install
bun run build

# relay (Railway picks up on git push)
cd relay
bun install
bun run dev          # local
```

Note: switching to `CDCOnBoot=cdc` (native USB) is a deliberate change
from Fox. Fox used the CH340 UART bridge because of a hardware quirk
with the GOOUUU board; on a clean ESP-IDF build with native USB CDC
the port is `/dev/ttyACM0` and the firmware can use the proper
`esp_log` infrastructure (gzip-compressed, structured) over USB.
*Verify on hardware in phase 3* — if CH340 is the only working path,
fall back.

## 16. Phases & checkpoints

| Phase | Deliverable | Stop & wait? |
|-------|-------------|--------------|
| 0     | Repo seed + minimal relay deployed                    | done |
| 1     | This doc                                              | **← stop and wait for review** |
| 2     | Firmware skeleton + frontend Vite + schema codegen + relay full WS round-trip (echo `Init`/`Tick` of static data; dashboard shows it). Verified on device. | yes |
| 3     | `hal_lepton` ported and verified: VoSPI integrity tests pass on host with golden frames + on device (10-min soak, no franken). | yes |
| 4     | `hal_camera` + `hal_storage` + `hal_oled` + live MJPEG view in Live tab.                                                                                  | yes |
| 5     | `app_state` + `app_capture` + sync capture endpoint.                                                                                                       | yes |
| 6     | `app_timelapse` (normal + DS, shared writer) + `app_session` indexer.                                                                                      | yes |
| 7     | Frontend complete: Live → Capture → Library; Lighthouse mobile ≥ 90.                                                                                       | yes |
| 8     | `app_ota` + brownout + factory reset + crash logs + 1-hour soak.                                                                                           | yes |
| 9     | CI green, tag `grasshopper-v1.0.0`.                                                                                                                        | done |

## 17. Open questions before phase 2

1. **Native USB CDC vs CH340 UART bridge.** Fox used CH340 because the
   GOOUUU's native port behaves oddly. Worth a 5-minute test on a
   blank board to see if `CDCOnBoot=cdc` works for ESP-IDF v5.x.
   *Resolution:* try native first; fall back to CH340 if it doesn't
   enumerate cleanly.

2. **OV2640 vs OV5640.** Fox auto-detects; Grasshopper should too.
   Driver supports both. No decision needed.

3. **Telemetry frame-counter dedup.** Deferred to v1.x. Document
   `lep_enable_telemetry` as a future hook in `hal_lepton`.

4. **Battery gauge.** Fox uses an mAh-budget estimate. Real-world
   commercial would want a fuel gauge IC (BQ27441 or MAX17048). For
   v1, keep the estimate; reserve I2C address space.

5. **"Blended view" in Live tab.** Fox stripped overlay/blend/match
   code. Grasshopper v1 punts blending too — not a 1.0 feature.
   Snapshot shows visible, thermal, or split, not blended.

6. **Authentication.** Mentioned as deferred in the prompt. For v1,
   destructive endpoints (`/factory-reset`, `/ota`, relay-initiated
   `Reboot`) need a token. Recommend NVS-stored bearer set on first
   boot via captive portal. Read endpoints (status, frames) stay open
   on the LAN — assumption is the user controls the network.

---

**Awaiting your review.** Once approved, phase 2 scaffolds:

- `firmware/` ESP-IDF project skeleton, builds + boots.
- `app/` Vite + Preact skeleton, builds to `app.html.gz`, dev mode
  runs against a mock WS.
- `proto/schema.ts` + `codegen.py` generating real C and TS.
- `relay/` upgraded to handle `Hello` + `Init` + `Tick` from a fake
  device, dashboard renders it.
- One round-trip verified end to end with static data.
