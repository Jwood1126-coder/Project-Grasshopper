// AUTO-GENERATED from proto/schema.json — do not edit by hand.
// Run `python3 proto/codegen.py` from the repo root to regenerate.

export type DeviceState =
  | "BOOT"
  | "SAFE_MODE"
  | "IDLE"
  | "STREAMING"
  | "CAPTURING"
  | "TIMELAPSE_ACTIVE"
  | "DEEP_SLEEP_PREP"
  | "SHUTDOWN";

export interface Wifi {
  mode: string;
  ssid: string;
  rssi: number;
  ip: string;
}

export interface Visible {
  fps: number;
  w: number;
  h: number;
  quality: number;
}

export interface Thermal {
  fps: number;
  gain: string;
  agc: boolean;
  spliceDetected: number;
  lastFFCMs: number;
}

/** Sent once over the relay WS by a device on connect. Identifies + auths the device. */
export interface Hello {
  type: string;
  deviceId: string;
  token: string;
  fwVersion: string;
  gitSha: string;
  bootReason: string;
}

/** Full device snapshot. Sent on LAN WS connect, and over relay after Hello. */
export interface Init {
  type: string;
  fwVersion: string;
  gitSha: string;
  deviceId: string;
  state: DeviceState;
  uptimeMs: number;
  freeHeap: number;
  freePsram: number;
  wifi: Wifi;
  ntpSynced: boolean;
  epoch: number;
  visible: Visible;
  thermal: Thermal;
}

/** Subset of Init. Pushed every ~1500ms to update changed fields. */
export interface Tick {
  type: string;
  uptimeMs: number;
  freeHeap: number;
  freePsram: number;
  epoch: number;
  state: DeviceState;
  thermal: Thermal;
}

/** One-shot event (capture saved, FFC done, error, state change). */
export interface Event {
  type: string;
  kind: string;
  msg: string;
  ts: number;
}

/** Structured log entry forwarded to the relay. */
export interface LogLine {
  type: string;
  ts: number;
  level: string;
  tag: string;
  msg: string;
}

// Discriminated union of all server→client messages.
export type GrasshopperMessage =
  | (Hello & { type: "hello" })
  | (Init & { type: "init" })
  | (Tick & { type: "tick" })
  | (Event & { type: "event" })
  | (LogLine & { type: "logline" });

