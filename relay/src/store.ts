// In-memory store for the relay. Keep it simple — the relay is a debug
// telescope, not a system of record. All data is lost on restart.

import type { ServerWebSocket } from 'bun'

export interface PreviewFrame {
  modality: 'vis' | 'thermal'
  width: number
  height: number
  ts: number          // ms when received by relay
  jpeg: Uint8Array
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
  logs: LogEntry[]
  events: EventEntry[]
  previewVis?: PreviewFrame
  previewTherm?: PreviewFrame
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
}

const LOGS_PER_DEVICE = 1000
const EVENTS_PER_DEVICE = 200

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
  }

  logsSince(deviceId: string, since: number): LogEntry[] {
    const d = this.devices.get(deviceId)
    if (!d) return []
    return d.logs.filter((e) => e.ts > since)
  }

  setPreview(deviceId: string, frame: PreviewFrame) {
    const d = this.devices.get(deviceId)
    if (!d) return
    if (frame.modality === 'vis') d.previewVis = frame
    else d.previewTherm = frame
  }
}

export const store = new Store()
