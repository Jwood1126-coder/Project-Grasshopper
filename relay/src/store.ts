// In-memory store for the relay. Keep it simple — the relay is a debug
// telescope, not a system of record. All data is lost on restart.

import type { ServerWebSocket } from 'bun'

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
}

export const store = new Store()
