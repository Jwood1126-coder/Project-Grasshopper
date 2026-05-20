// In-memory store for the relay. Keep it simple — the relay is a debug
// telescope, not a system of record. All data is lost on restart.

import type { ServerWebSocket } from 'bun'

export interface PreviewFrame {
  modality: 'vis' | 'thermal'
  width: number
  height: number
  ts: number          // ms when received by relay
  jpeg: Uint8Array
  // Optional raw uint16 thermal frame (160 × 120 = 38400 bytes,
  // little-endian). Present on thermal previews v2+; let UI compute
  // per-pixel temperatures on hover without an extra round-trip.
  raw16?: Uint8Array
}

export interface DeviceRecord {
  deviceId: string
  socket: ServerWebSocket<unknown> | null
  fwVersion: string
  gitSha: string
  state: string
  ip: string
  lastSeenMs: number
  init: unknown // last full Init message
  tick: unknown // last Tick (or merged snapshot)
  // Latest {phase, phaseEnteredMs} block, sourced from whichever of
  // hello / init / tick most recently carried one. Hello is critical:
  // during a wake-Wi-Fi window the device sends hello + cmd.results
  // but no init/tick, so without this field the dashboard would have
  // no phase data to display until the next live boot.
  system?: { phase: string; phaseEnteredMs: number; updatedMs: number }
  logs: LogEntry[]
  events: EventEntry[]
  // Parallel ring for lifecycle / fault signals — see SIGNAL_KINDS
  // below. Populated by appendEvent at the same time as events but
  // never evicted by routine cmd.result traffic, so a Library scan
  // can't bury phase transitions / disconnects / failed commands.
  signals: EventEntry[]
  previewVis?: PreviewFrame
  previewTherm?: PreviewFrame
  // Latest uploaded firmware blob — held in memory only so it survives
  // until the OTA worker on the device finishes pulling it. Big (~1.5 MB).
  pendingFirmware?: { bytes: Uint8Array; sha256: string; uploadedMs: number }
}

export interface LogEntry {
  ts: number
  level: string
  tag: string
  msg: string
}

export interface EventEntry {
  ts: number
  kind: string
  msg: string
  // Optional fields populated for cmd.result events. UI matches outgoing
  // commands to results by `id`, so retries over a flaky link don't
  // produce stale UI state. See firmware net_relay::send_cmd_result.
  id?: string
  cmd?: string
  ok?: boolean
  // Optional fields populated for kind="phase" events. Wire-protocol
  // phase names — see firmware components/system_phase. The dashboard
  // builds a transition timeline from these, independent of the 1.5 s
  // tick sampling rate.
  from?: string
  to?: string
  prevDurMs?: number
  uptimeMs?: number
  // Structured payload attached to a cmd.result by the firmware (e.g.
  // sessions.list returns {sessions:[...], total, listed, truncated};
  // session.read_file returns {filename, offset, len, totalSize, eof, b64}).
  // Type intentionally `unknown` — endpoints that consume it should
  // narrow with a runtime check or a per-cmd schema.
  data?: unknown
}

const LOGS_PER_DEVICE = 1000
const EVENTS_PER_DEVICE = 200
// Separate ring for low-frequency lifecycle / fault signals so a burst
// of session.read_file cmd.results from a Library scan can't push them
// out within seconds. Keep modest — this is for triage, not history.
const SIGNALS_PER_DEVICE = 200

// Event kinds that always go into BOTH rings (events + signals). Plus
// any cmd.result with ok=false. The intent: anything an operator or
// diagnostic agent would care about during a debug session survives
// even when the events ring is being hammered by routine traffic.
const SIGNAL_KINDS = new Set([
  'phase',
  'phase.stuck',
  'connected',
  'disconnected',
])

class Store {
  private devices = new Map<string, DeviceRecord>()

  upsert(deviceId: string, patch: Partial<DeviceRecord>): DeviceRecord {
    const existing = this.devices.get(deviceId)
    const merged: DeviceRecord = {
      deviceId,
      socket: null,
      fwVersion: '',
      gitSha: '',
      state: 'UNKNOWN',
      ip: '',
      lastSeenMs: Date.now(),
      init: null,
      tick: null,
      logs: [],
      events: [],
      signals: [],
      ...existing,
      ...patch,
    }
    this.devices.set(deviceId, merged)
    return merged
  }

  get(deviceId: string): DeviceRecord | undefined {
    return this.devices.get(deviceId)
  }

  list(): DeviceRecord[] {
    return [...this.devices.values()]
  }

  setSocket(deviceId: string, socket: ServerWebSocket<unknown> | null) {
    const d = this.devices.get(deviceId)
    if (d) d.socket = socket
  }

  appendLog(deviceId: string, entry: LogEntry) {
    const d = this.devices.get(deviceId)
    if (!d) return
    d.logs.push(entry)
    if (d.logs.length > LOGS_PER_DEVICE) {
      d.logs.splice(0, d.logs.length - LOGS_PER_DEVICE)
    }
  }

  appendEvent(deviceId: string, entry: EventEntry) {
    const d = this.devices.get(deviceId)
    if (!d) return
    d.events.push(entry)
    if (d.events.length > EVENTS_PER_DEVICE) {
      d.events.splice(0, d.events.length - EVENTS_PER_DEVICE)
    }
    // Mirror lifecycle / fault signals into the parallel ring so a
    // Library-scan burst of session.read_file cmd.results can't push
    // them out within seconds. Criteria: any event whose kind is in
    // SIGNAL_KINDS, OR any cmd.result with ok=false UNLESS that
    // failure is an expected probe miss (session.read_file ENOENT
    // for a thumbnail the UI knows might not exist — the fallback
    // chain in renderSessionCard tries seq=1 then captureCount, and
    // ENOENT on either is a normal data shape, not a fault).
    //
    // Without this filter, opening Library produces dozens of
    // "open /sdcard/.../000001_therm.jpg: errno=2" entries in
    // /signals — exactly the noise the separate ring was built to
    // suppress.
    let shouldMirror = SIGNAL_KINDS.has(entry.kind)
    if (!shouldMirror && entry.kind === 'cmd.result' && entry.ok === false) {
      const msg = entry.msg || ''
      const isExpectedFileMiss =
        entry.cmd === 'session.read_file' &&
        (/errno=2\b/.test(msg) || /no such file/i.test(msg))
      shouldMirror = !isExpectedFileMiss
    }
    if (shouldMirror) {
      d.signals.push(entry)
      if (d.signals.length > SIGNALS_PER_DEVICE) {
        d.signals.splice(0, d.signals.length - SIGNALS_PER_DEVICE)
      }
    }
  }

  logsSince(deviceId: string, since: number): LogEntry[] {
    const d = this.devices.get(deviceId)
    if (!d) return []
    return d.logs.filter((e) => e.ts > since)
  }

  // Lifecycle / fault signals — populated by appendEvent for kinds in
  // SIGNAL_KINDS plus failed cmd.results. Use this when triaging "what
  // just happened to the device" without the noise of routine cmd.results.
  signalsSince(deviceId: string, since: number): EventEntry[] {
    const d = this.devices.get(deviceId)
    if (!d) return []
    return d.signals.filter((e) => e.ts > since)
  }

  setPreview(deviceId: string, frame: PreviewFrame) {
    const d = this.devices.get(deviceId)
    if (!d) return
    if (frame.modality === 'vis') d.previewVis = frame
    else d.previewTherm = frame
  }
}

export const store = new Store()
