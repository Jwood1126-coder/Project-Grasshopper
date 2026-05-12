import { Hono } from 'hono'
import type { ServerWebSocket } from 'bun'
import { store, type PreviewFrame } from './store'
import { USER_UI_HTML } from './user_ui'

const app = new Hono()

// ---- HTTP routes (dashboard + agent-readable JSON) ---------------------

app.get('/health', (c) =>
  c.json({
    ok: true,
    service: 'grasshopper-relay',
    version: '0.1.0',
    devices: store.list().length,
    ts: Date.now(),
    // UI uses this to surface a security banner when the deployment is
    // using the dev-token fallback. Don't expose the actual token —
    // just the boolean. RAILWAY_PUBLIC_DOMAIN is set on Railway deploys
    // so the warning specifically targets public exposure.
    tokenIsDevDefault: process.env.RELAY_TOKEN === undefined ||
                       process.env.RELAY_TOKEN === 'dev-token',
    publiclyExposed: !!process.env.RAILWAY_PUBLIC_DOMAIN,
  })
)

app.get('/api/devices', (c) =>
  c.json({
    devices: store.list().map((d) => ({
      deviceId: d.deviceId,
      fwVersion: d.fwVersion,
      gitSha: d.gitSha,
      state: d.state,
      ip: d.ip,
      lastSeenMs: d.lastSeenMs,
      online: d.socket?.readyState === 1,
      logCount: d.logs.length,
      eventCount: d.events.length,
    })),
  })
)

app.get('/api/devices/:id/state', (c) => {
  const d = store.get(c.req.param('id'))
  if (!d) return c.json({ error: 'unknown device' }, 404)
  return c.json({
    deviceId: d.deviceId,
    fwVersion: d.fwVersion,
    gitSha: d.gitSha,
    state: d.state,
    ip: d.ip,
    lastSeenMs: d.lastSeenMs,
    online: d.socket?.readyState === 1,
    init: d.init,
    tick: d.tick,
  })
})

app.get('/api/devices/:id/logs', (c) => {
  const d = store.get(c.req.param('id'))
  if (!d) return c.json({ error: 'unknown device' }, 404)
  const since = Number(c.req.query('since') ?? '0')
  return c.json({ logs: store.logsSince(d.deviceId, since) })
})

app.get('/api/devices/:id/events', (c) => {
  const d = store.get(c.req.param('id'))
  if (!d) return c.json({ error: 'unknown device' }, 404)
  // Optional `?since=<ms>` filter for UI polling — returns only events
  // newer than the given relay-receive timestamp. UI uses this to find
  // the cmd.result matching its outstanding command id.
  const sinceParam = c.req.query('since')
  const since = sinceParam ? Number(sinceParam) : 0
  const events = since > 0 ? d.events.filter((e) => e.ts > since) : d.events
  return c.json({ events })
})

app.get('/api/devices/:id/last-frame.jpg', (c) => {
  const d = store.get(c.req.param('id'))
  if (!d) return c.text('unknown device', 404)
  const m = c.req.query('modality')
  if (m !== 'vis' && m !== 'thermal') {
    return c.text('modality must be "vis" or "thermal"', 400)
  }
  const frame = m === 'thermal' ? d.previewTherm : d.previewVis
  if (!frame) return c.text('no frame yet', 404)
  // Return a raw Response so the typed body accepts Uint8Array directly
  // (Hono's c.body only accepts string | ArrayBuffer | ReadableStream).
  return new Response(frame.jpeg, {
    headers: {
      'Content-Type':    'image/jpeg',
      'Cache-Control':   'no-store',
      'X-Frame-Width':   String(frame.width),
      'X-Frame-Height':  String(frame.height),
      'X-Frame-Age-Ms':  String(Date.now() - frame.ts),
    },
  })
})

app.post('/api/devices/:id/cmd', async (c) => {
  // Require the same shared secret the device uses to register, sent
  // as a Bearer header. Prevents anonymous internet traffic from
  // forwarding commands to a connected device.
  const auth = c.req.header('Authorization') || ''
  const presented = auth.startsWith('Bearer ') ? auth.slice(7) : ''
  if (!presented || presented !== RELAY_TOKEN) {
    return c.json({ error: 'unauthorized' }, 401)
  }
  const d = store.get(c.req.param('id'))
  if (!d) return c.json({ error: 'unknown device' }, 404)
  if (!d.socket || d.socket.readyState !== 1)
    return c.json({ error: 'device offline' }, 409)
  const body = await c.req.json().catch(() => null)
  if (!body) return c.json({ error: 'invalid json' }, 400)
  d.socket.send(JSON.stringify({ ...body, type: 'cmd' }))
  return c.json({ ok: true })
})

// ---- Sessions endpoints --------------------------------------------------
//
// These wrap the device-side sessions.* commands so the UI calls a regular
// HTTP endpoint and gets back the data field directly. The relay generates
// the cmd id, sends over WS, and waits for the matching cmd.result. The
// pending-cmd table is resolved by the WS event handler below.
//
// All sessions endpoints require Bearer auth — they trigger SD work on the
// device, so we don't want anonymous internet traffic spamming them.

// Strict input validation for sessions.* — accept only the names the
// firmware itself produces. Prevents path traversal at the relay layer
// even before the firmware's own check runs.
const SESSION_ID_RE       = /^session_[0-9]+$/
const SESSION_FILENAME_RE = /^(session\.json|captures\.jsonl|[0-9]{6}_(vis|therm)\.jpg)$/

interface PendingCmd {
  resolve: (data: unknown, msg: string) => void
  reject: (msg: string) => void
  timer: ReturnType<typeof setTimeout>
}
const pendingCmds = new Map<string, PendingCmd>()
let nextCmdSeq = 1

function executeCmd(
  deviceId: string,
  cmd: string,
  payload: Record<string, unknown>,
  timeoutMs = 10000,
): Promise<unknown> {
  return new Promise((resolve, reject) => {
    const d = store.get(deviceId)
    if (!d) { reject('unknown device'); return }
    if (!d.socket || d.socket.readyState !== 1) { reject('device offline'); return }
    const id = `srv-${nextCmdSeq++}-${Date.now().toString(36)}`
    const timer = setTimeout(() => {
      pendingCmds.delete(id)
      reject(`timeout waiting for cmd.result (${cmd})`)
    }, timeoutMs)
    pendingCmds.set(id, {
      resolve: (data) => resolve(data),
      reject: (msg) => reject(msg),
      timer,
    })
    d.socket.send(JSON.stringify({ type: 'cmd', cmd, id, ...payload }))
  })
}

function requireBearer(c: any): true | Response {
  const auth = c.req.header('Authorization') || ''
  const presented = auth.startsWith('Bearer ') ? auth.slice(7) : ''
  if (!presented || presented !== RELAY_TOKEN) {
    return c.json({ error: 'unauthorized' }, 401)
  }
  return true
}

app.get('/api/devices/:id/sessions', async (c) => {
  const auth = requireBearer(c); if (auth !== true) return auth
  try {
    const data = await executeCmd(c.req.param('id'), 'sessions.list', {})
    return c.json(data ?? {})
  } catch (msg) {
    return c.json({ error: String(msg) }, 502)
  }
})

app.get('/api/devices/:id/sessions/:sid', async (c) => {
  const auth = requireBearer(c); if (auth !== true) return auth
  const sid = c.req.param('sid')
  if (!SESSION_ID_RE.test(sid)) return c.json({ error: 'bad sessionId' }, 400)
  try {
    const data = await executeCmd(c.req.param('id'), 'sessions.get', {
      sessionId: sid,
    })
    return c.json(data ?? {})
  } catch (msg) {
    return c.json({ error: String(msg) }, 502)
  }
})

// File fetch — loops session.read_file chunks until eof, decodes base64,
// returns binary. Small in-memory LRU cache so repeated thumbnail loads
// don't hit the device every time. Cache is byte-bounded; entries are
// evicted in insertion order (Map iteration order = insertion order).
const FILE_CACHE_MAX_BYTES = 32 * 1024 * 1024 // 32 MB
const fileCache = new Map<string, { bytes: Uint8Array; ts: number }>()
let fileCacheBytes = 0

function cacheGet(key: string): Uint8Array | undefined {
  const e = fileCache.get(key)
  if (!e) return undefined
  // Refresh insertion order on hit
  fileCache.delete(key); fileCache.set(key, e)
  return e.bytes
}
function cachePut(key: string, bytes: Uint8Array) {
  if (bytes.byteLength > FILE_CACHE_MAX_BYTES / 4) return // skip oversize
  fileCache.set(key, { bytes, ts: Date.now() })
  fileCacheBytes += bytes.byteLength
  while (fileCacheBytes > FILE_CACHE_MAX_BYTES) {
    const oldest = fileCache.keys().next().value
    if (!oldest) break
    const drop = fileCache.get(oldest)!
    fileCache.delete(oldest)
    fileCacheBytes -= drop.bytes.byteLength
  }
}

// Auth required — exposing captured images publicly would leak content
// once an attacker enumerates device IDs (which /api/devices reveals)
// and the small-integer session IDs. UI loads thumbnails via
// fetch(..., {Authorization}) + URL.createObjectURL.
app.get('/api/devices/:id/sessions/:sid/file/:filename', async (c) => {
  const auth = requireBearer(c); if (auth !== true) return auth
  const deviceId = c.req.param('id')
  const sid = c.req.param('sid')
  const filename = c.req.param('filename')
  if (!SESSION_ID_RE.test(sid)) return c.json({ error: 'bad sessionId' }, 400)
  if (!SESSION_FILENAME_RE.test(filename))
    return c.json({ error: 'bad filename' }, 400)
  const cacheKey = `${deviceId}:${sid}:${filename}`
  const cached = cacheGet(cacheKey)
  if (cached) {
    c.header('Content-Type', guessMime(filename))
    c.header('X-Cache', 'HIT')
    return c.body(cached as any)
  }

  // Pull chunks until eof.
  const CHUNK = 16 * 1024
  let offset = 0
  const chunks: Uint8Array[] = []
  let totalSize = 0
  for (;;) {
    let data: any
    try {
      data = await executeCmd(deviceId, 'session.read_file', {
        sessionId: sid, filename, offset, maxLen: CHUNK,
      }, 15000)
    } catch (msg) {
      return c.json({ error: String(msg) }, 502)
    }
    if (!data || typeof data !== 'object' || typeof data.b64 !== 'string') {
      return c.json({ error: 'malformed read_file response' }, 502)
    }
    const buf = Uint8Array.from(Buffer.from(data.b64, 'base64'))
    chunks.push(buf)
    offset += buf.byteLength
    totalSize = data.totalSize ?? totalSize
    if (data.eof || buf.byteLength === 0) break
    if (offset > 50 * 1024 * 1024) {
      return c.json({ error: 'file too large (>50MB)' }, 413)
    }
  }
  const bytes = concatBytes(chunks, totalSize)
  cachePut(cacheKey, bytes)
  c.header('Content-Type', guessMime(filename))
  c.header('X-Cache', 'MISS')
  return c.body(bytes as any)
})

function guessMime(filename: string): string {
  if (filename.endsWith('.jpg') || filename.endsWith('.jpeg')) return 'image/jpeg'
  if (filename.endsWith('.json')) return 'application/json'
  if (filename.endsWith('.jsonl')) return 'application/x-ndjson'
  return 'application/octet-stream'
}
function concatBytes(parts: Uint8Array[], expected: number): Uint8Array {
  const total = parts.reduce((s, p) => s + p.byteLength, 0)
  const out = new Uint8Array(expected || total)
  let o = 0
  for (const p of parts) { out.set(p, o); o += p.byteLength }
  return out
}

// User-facing UI (Phase 1: Live View). The polished surface end-users see.
app.get('/', (c) => {
  c.header('Content-Type', 'text/html; charset=utf-8')
  return c.body(USER_UI_HTML)
})

// Debug telescope — moved from /. Still here for agent debugging.
// Framework-free HTML that polls /api/devices for state/logs/events.
app.get('/debug', (c) => {
  c.header('Content-Type', 'text/html; charset=utf-8')
  return c.body(DASHBOARD_HTML)
})

// ---- WebSocket upgrade & handlers --------------------------------------

interface WsCtx {
  deviceId: string | null
  authed: boolean
  remoteIp: string
}

const RELAY_TOKEN = process.env.RELAY_TOKEN ?? 'dev-token'

function handlePreview(ws: ServerWebSocket<WsCtx>, buf: Buffer) {
  if (!ws.data.authed || !ws.data.deviceId) return
  const modalityByte = buf[4]
  const w = buf.readUInt32LE(8)
  const h = buf.readUInt32LE(12)
  const jpegLen = buf.readUInt32LE(16)
  if (jpegLen + 24 !== buf.length) {
    console.warn(`[ws] preview length mismatch: hdr says ${jpegLen}, got ${buf.length - 24}`)
    return
  }
  const jpeg = new Uint8Array(buf.buffer, buf.byteOffset + 24, jpegLen)
  const frame: PreviewFrame = {
    modality: modalityByte === 2 ? 'thermal' : 'vis',
    width: w,
    height: h,
    ts: Date.now(),
    jpeg: new Uint8Array(jpeg), // copy out of the WS buffer
  }
  store.setPreview(ws.data.deviceId, frame)
  store.upsert(ws.data.deviceId, { lastSeenMs: Date.now() })
}

const port = Number(process.env.PORT ?? 3000)

const server = Bun.serve<WsCtx>({
  port,
  fetch(req, srv) {
    const url = new URL(req.url)
    if (url.pathname === '/relay') {
      const remoteIp =
        req.headers.get('x-forwarded-for') ??
        req.headers.get('x-real-ip') ??
        'unknown'
      if (
        srv.upgrade(req, {
          data: { deviceId: null, authed: false, remoteIp },
        })
      )
        return
      return new Response('upgrade failed', { status: 500 })
    }
    return app.fetch(req, srv)
  },
  websocket: {
    open(ws) {
      console.log(`[ws] open from ${ws.data.remoteIp}`)
    },
    message(ws, raw) {
      // Binary frames carry preview JPEGs with a 24-byte "GHFR" header;
      // anything else is JSON text. Bun delivers binary as Buffer.
      if (typeof raw !== 'string') {
        const buf: Buffer = raw
        if (buf.length >= 24 && buf[0] === 0x47 && buf[1] === 0x48 &&
            buf[2] === 0x46 && buf[3] === 0x52) {
          handlePreview(ws, buf)
          return
        }
      }
      const text = typeof raw === 'string' ? raw : raw.toString('utf-8')
      let msg: any
      try {
        msg = JSON.parse(text)
      } catch {
        console.warn(`[ws] bad json from ${ws.data.deviceId ?? ws.data.remoteIp}`)
        return
      }

      // First message must be hello.
      if (!ws.data.authed) {
        if (msg.type !== 'hello') {
          ws.close(1008, 'expected hello first')
          return
        }
        if (msg.token !== RELAY_TOKEN) {
          console.warn(`[ws] reject ${msg.deviceId ?? '?'}: bad token`)
          ws.close(1008, 'bad token')
          return
        }
        if (typeof msg.deviceId !== 'string' || !msg.deviceId) {
          ws.close(1008, 'missing deviceId')
          return
        }
        ws.data.deviceId = msg.deviceId
        ws.data.authed = true
        store.upsert(msg.deviceId, {
          fwVersion: msg.fwVersion ?? '',
          gitSha: msg.gitSha ?? '',
          ip: ws.data.remoteIp,
          lastSeenMs: Date.now(),
        })
        store.setSocket(msg.deviceId, ws)
        console.log(`[ws] hello from ${msg.deviceId} fw=${msg.fwVersion ?? '?'}`)
        // Push wall-clock time so the device can set its system clock
        // even when NTP is blocked/slow on the local network. Sent
        // immediately after hello; firmware applies via settimeofday()
        // so time(NULL) and session.json timestamps reflect epoch.
        ws.send(JSON.stringify({ type: 'time', epochMs: Date.now() }))
        store.appendEvent(msg.deviceId, {
          ts: Date.now(),
          kind: 'connected',
          msg: `boot=${msg.bootReason ?? '?'}`,
        })
        return
      }

      const deviceId = ws.data.deviceId!
      store.upsert(deviceId, { lastSeenMs: Date.now() })

      switch (msg.type) {
        case 'init':
          store.upsert(deviceId, {
            init: msg,
            tick: msg,
            state: msg.state ?? 'UNKNOWN',
            fwVersion: msg.fwVersion ?? '',
            gitSha: msg.gitSha ?? '',
          })
          break
        case 'tick': {
          const prev = store.get(deviceId)
          const merged = { ...(prev?.tick as object), ...msg }
          store.upsert(deviceId, {
            tick: merged,
            state: (msg as any).state ?? prev?.state ?? 'UNKNOWN',
          })
          break
        }
        case 'event':
          store.appendEvent(deviceId, {
            ts: Date.now(),
            kind: msg.kind ?? '?',
            msg: msg.msg ?? '',
            // cmd.result fields (optional — only present for command results)
            id: typeof msg.id === 'string' ? msg.id : undefined,
            cmd: typeof msg.cmd === 'string' ? msg.cmd : undefined,
            ok: typeof msg.ok === 'boolean' ? msg.ok : undefined,
            // Structured payload (sessions.list / sessions.get /
            // session.read_file). Anything that isn't an object/array is
            // dropped — the field is meant for typed responses, not
            // arbitrary scalars.
            data: (msg.data && typeof msg.data === 'object') ? msg.data : undefined,
          })
          // Resolve a pending HTTP wait if this is the cmd.result we issued.
          if (msg.kind === 'cmd.result' && typeof msg.id === 'string') {
            const pending = pendingCmds.get(msg.id)
            if (pending) {
              pendingCmds.delete(msg.id)
              clearTimeout(pending.timer)
              if (msg.ok === true) pending.resolve(msg.data, msg.msg ?? '')
              else                 pending.reject(msg.msg ?? 'cmd failed')
            }
          }
          break
        case 'log':
        case 'logline':
          store.appendLog(deviceId, {
            ts: msg.ts ?? Date.now(),
            level: msg.level ?? 'I',
            tag: msg.tag ?? '',
            msg: msg.msg ?? '',
          })
          break
        default:
          console.log(`[ws] unknown type from ${deviceId}: ${msg.type}`)
      }
    },
    close(ws, code, reason) {
      const id = ws.data.deviceId
      if (id) {
        store.setSocket(id, null)
        store.appendEvent(id, {
          ts: Date.now(),
          kind: 'disconnected',
          msg: `code=${code} reason=${reason || ''}`,
        })
        console.log(`[ws] close ${id} code=${code}`)
        // Fail any in-flight HTTP cmd waits so they don't hang to timeout.
        for (const [pid, p] of pendingCmds) {
          clearTimeout(p.timer)
          p.reject('device disconnected mid-cmd')
          pendingCmds.delete(pid)
        }
      }
    },
  },
})

console.log(`grasshopper-relay listening on :${port}`)
console.log(`  HTTP/dashboard  → http://localhost:${port}/`)
console.log(`  device WS       → ws://localhost:${port}/relay`)
console.log(`  RELAY_TOKEN=${RELAY_TOKEN === 'dev-token' ? '(dev default)' : 'set'}`)

// If we're running behind a Railway public domain (i.e. on the open
// internet) and still using the dev-token fallback, scream loudly.
// Anyone could connect a device or hit /cmd without it.
if (RELAY_TOKEN === 'dev-token' && process.env.RAILWAY_PUBLIC_DOMAIN) {
  console.error('=========================================================')
  console.error('  WARNING: RELAY_TOKEN is "dev-token" on a public deploy.')
  console.error('  Set RELAY_TOKEN to a unique secret in Railway env vars.')
  console.error('  Until you do, anyone with this URL can register a fake')
  console.error('  device or POST commands to a connected one.')
  console.error('=========================================================')
}

// ---- Embedded dashboard HTML (no framework) ----------------------------

const DASHBOARD_HTML = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Grasshopper relay</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
  :root { color-scheme: light dark; --fg:#222; --bg:#fafafa; --muted:#777; --accent:#0a7; --warn:#c33; --ok:#2a8; --line:#8883; }
  @media (prefers-color-scheme: dark) { :root { --fg:#eee; --bg:#161616; --muted:#888; --accent:#4f8; --ok:#3c9; --line:#fff2; } }
  * { box-sizing: border-box }
  body { font: 14px/1.5 -apple-system, system-ui, Segoe UI, sans-serif; margin: 0; color: var(--fg); background: var(--bg); }
  header { padding: 14px 24px; border-bottom: 1px solid var(--line); display: flex; align-items: center; gap: 12px; }
  header h1 { font-size: 18px; margin: 0; font-weight: 600; }
  header .live { color: var(--accent); font-weight: 500; font-size: 13px; }
  main { max-width: 1200px; margin: 0 auto; padding: 20px 24px; }
  .no-devices { color: var(--muted); font-style: italic; }
  .device { border: 1px solid var(--line); border-radius: 8px; padding: 14px 18px; margin-bottom: 18px; background: #fff1; }
  .preview { display: flex; gap: 12px; flex-wrap: wrap; margin: 10px 0 6px; }
  .preview .frame { border: 1px solid var(--line); border-radius: 6px; overflow: hidden; background: #0006; min-width: 240px; max-width: 480px; }
  .preview .frame .label { font: 600 11px ui-sans-serif; padding: 4px 8px; color: var(--muted); text-transform: uppercase; letter-spacing: .06em; background: #0008; }
  .preview .frame .label .age { float: right; font-weight: 400; color: var(--muted); }
  .preview .frame img { display: block; width: 100%; height: auto; }
  .preview .frame.empty { padding: 20px; color: var(--muted); font-size: 12px; font-style: italic; min-width: 200px; }
  .device h3 { margin: 0 0 2px; font-size: 16px; display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
  .device .id { font-family: ui-monospace, monospace; font-size: 13px; }
  .pill { display: inline-block; font-size: 11px; padding: 2px 8px; border-radius: 999px; background: #8884; line-height: 1.5; }
  .pill.online { background: var(--ok); color: #000; }
  .pill.offline { background: var(--warn); color: #fff; }
  .pill.state { background: #5af3; color: var(--fg); font-family: ui-monospace, monospace; }
  .meta { color: var(--muted); font-size: 12px; margin-bottom: 10px; }
  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 10px; }
  .panel { border: 1px solid var(--line); border-radius: 6px; padding: 10px 12px; background: #fff1; }
  .panel h4 { font: 600 11px ui-sans-serif; text-transform: uppercase; letter-spacing: .06em; color: var(--muted); margin: 0 0 6px; }
  .row { display: flex; justify-content: space-between; gap: 8px; padding: 1px 0; font-size: 13px; }
  .row .k { color: var(--muted); }
  .row .v { font-family: ui-monospace, monospace; font-variant-numeric: tabular-nums; }
  .row .v.warn { color: var(--warn); }
  .row .v.ok { color: var(--ok); }
  details { margin-top: 12px; }
  details summary { font-size: 12px; color: var(--muted); cursor: pointer; user-select: none; }
  details pre { font: 11px/1.4 ui-monospace, monospace; background: #0001; padding: 8px; border-radius: 4px; max-height: 280px; overflow: auto; margin: 6px 0 0; }
  .events { margin-top: 12px; }
  .events .ev { font: 12px/1.5 ui-monospace, monospace; padding: 2px 0; border-bottom: 1px dotted var(--line); }
  .events .ev .t { color: var(--muted); }
  .footer { color: var(--muted); font-size: 11px; padding: 18px 24px; text-align: center; }
</style>
</head>
<body>
<header>
  <h1>Grasshopper relay</h1>
  <span class="live" id="status">connecting…</span>
</header>
<main id="devices"><div class="no-devices">No devices connected yet.</div></main>
<div class="footer">
  Agent-readable: <code>/api/devices</code> · <code>/api/devices/:id/state</code> ·
  <code>/api/devices/:id/logs?since=</code> · <code>/api/devices/:id/events</code>
</div>
<script>
  // Dashboard refresh strategy: build per-device card DOM ONCE on first
  // sight, then mutate text/class in place every status tick. Image
  // refresh is on its own timer using a preload-then-swap pattern so
  // the visible <img> never goes blank — eliminating the page-wide
  // blink that came from rebuilding innerHTML each tick.

  const fmtKB = (n) => n == null ? '—' : n < 1024 ? n.toFixed(0) + ' KB' : (n / 1024).toFixed(1) + ' MB';
  const fmtPct = (u, t) => (t > 0) ? Math.round(100 * u / t) + '%' : '—';
  const fmtAge = (ms) => {
    const s = Math.floor(ms / 1000);
    if (s < 60) return s + 's';
    if (s < 3600) return Math.floor(s/60) + 'm ' + (s%60) + 's';
    return Math.floor(s/3600) + 'h ' + Math.floor((s%3600)/60) + 'm';
  };
  const rssiClass = (r) => r === 0 ? 'warn' : r > -65 ? 'ok' : '';

  // Per-device state we keep in JS so we can update incrementally.
  const cards = new Map(); // deviceId → { rootEl, fields, lastVisTs, lastThermTs }

  function el(tag, attrs = {}, ...children) {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs)) {
      if (k === 'class') e.className = v;
      else if (k === 'style') e.setAttribute('style', v);
      else if (k.startsWith('data-')) e.setAttribute(k, v);
      else e[k] = v;
    }
    for (const c of children) {
      if (c == null) continue;
      e.appendChild(typeof c === 'string' ? document.createTextNode(c) : c);
    }
    return e;
  }

  function makeRow(label) {
    const v = el('span', { class: 'v' }, '—');
    return { row: el('div', { class: 'row' }, el('span', { class: 'k' }, label), v), v };
  }
  function setRow(rec, value, cls) {
    rec.v.textContent = value ?? '—';
    rec.v.className = 'v' + (cls ? ' ' + cls : '');
  }

  function makePanel(title, rows) {
    const f = {};
    const panel = el('div', { class: 'panel' }, el('h4', {}, title));
    for (const [key, label] of rows) {
      const r = makeRow(label);
      f[key] = r;
      panel.appendChild(r.row);
    }
    return { panel, f };
  }

  function buildCard(deviceId) {
    const idEnc = encodeURIComponent(deviceId);
    const f = {};

    // Header
    f.h_id    = el('span', { class: 'id' }, deviceId);
    f.h_pill  = el('span', { class: 'pill offline' }, 'offline');
    f.h_state = el('span', { class: 'pill state' }, '—');
    const h3 = el('h3', {}, f.h_id, f.h_pill, f.h_state);

    f.meta = el('div', { class: 'meta' }, '—');

    // Preview tiles built via innerHTML so both frames ALWAYS end up in
    // the DOM. (Earlier el()-based variadic-children construction was
    // somehow dropping the thermal frame in Chromium — repro'd via
    // chromium-headless --dump-dom; root cause unclear, likely a
    // browser-specific quirk. innerHTML sidesteps it.)
    const previewWrap = document.createElement('div');
    previewWrap.className = 'preview';
    previewWrap.innerHTML =
      '<div class="frame empty" data-modality="vis">' +
        '<div class="label">visible</div>' +
        '<img loading="lazy" alt="visible preview" style="display:none">' +
        '<span class="ph">no frame yet</span>' +
      '</div>' +
      '<div class="frame empty" data-modality="thermal">' +
        '<div class="label">thermal</div>' +
        '<img loading="lazy" alt="thermal preview" style="display:none">' +
        '<span class="ph">no frame yet</span>' +
      '</div>';
    f.frameVis    = previewWrap.querySelector('[data-modality="vis"]');
    f.frameTherm  = previewWrap.querySelector('[data-modality="thermal"]');
    f.imgVis      = f.frameVis.querySelector('img');
    f.imgTherm    = f.frameTherm.querySelector('img');
    f.phVis       = f.frameVis.querySelector('.ph');
    f.phTherm     = f.frameTherm.querySelector('.ph');

    // Panels
    const wifi = makePanel('WiFi', [
      ['ssid', 'SSID'], ['ip', 'IP'], ['rssi', 'RSSI'], ['mode', 'Mode'],
    ]);
    const therm = makePanel('Lepton', [
      ['state', 'State'], ['fps', 'FPS'], ['frames', 'Frames'],
      ['valid', 'Valid'], ['splices', 'Splices'], ['hwr', 'HW resets'],
      ['gain', 'Gain · AGC'],
    ]);
    const cam = makePanel('Camera', [
      ['sensor', 'Sensor'], ['ready', 'Ready'], ['frame', 'Frame'],
      ['fps', 'FPS'], ['q', 'JPEG q'],
    ]);
    const stor = makePanel('Storage', [
      ['sd', 'SD'], ['sdUsed', 'SD used'], ['lfs', 'LittleFS'],
    ]);
    const sys = makePanel('System', [
      ['uptime', 'Uptime'], ['heap', 'Free heap'], ['psram', 'Free PSRAM'], ['ntp', 'NTP'],
    ]);
    const grid = el('div', { class: 'grid' }, wifi.panel, therm.panel, cam.panel, stor.panel, sys.panel);

    f.panels = { wifi: wifi.f, therm: therm.f, cam: cam.f, stor: stor.f, sys: sys.f };

    f.events = el('div', { class: 'events', style: 'display:none' });
    f.rawPre = el('pre', {}, '');
    const det = el('details', {}, el('summary', {}, 'raw tick JSON'), f.rawPre);

    const root = el('div', { class: 'device', 'data-device': idEnc },
                    h3, f.meta, previewWrap, grid, f.events, det);

    return { rootEl: root, fields: f, baseId: idEnc, lastVisTs: 0, lastThermTs: 0 };
  }

  function applyState(card, d, s) {
    const f = card.fields;
    f.h_pill.className = 'pill ' + (d.online ? 'online' : 'offline');
    f.h_pill.textContent = d.online ? 'online' : 'offline';
    f.h_state.textContent = d.state || '—';

    const ageS = Math.max(0, Math.floor((Date.now() - d.lastSeenMs) / 1000));
    f.meta.textContent = 'fw ' + (d.fwVersion || '?') + ' · sha ' + (d.gitSha || '?') +
                         ' · ip ' + (d.ip || '?') + ' · last seen ' + ageS + 's ago';

    // WiFi
    const w = s?.wifi || {};
    setRow(f.panels.wifi.ssid, w.ssid || '—');
    setRow(f.panels.wifi.ip, w.ip || '—');
    setRow(f.panels.wifi.rssi,
      (w.rssi != null && w.rssi !== 0) ? w.rssi + ' dBm' : 'no link',
      rssiClass(w.rssi));
    setRow(f.panels.wifi.mode, w.mode || '—');

    // Thermal
    const t = s?.thermal || {};
    const valid = t.totalPackets ? Math.round(100 * t.validPackets / t.totalPackets) + '%' : '—';
    setRow(f.panels.therm.state, t.state || '—');
    setRow(f.panels.therm.fps, t.fps ?? '—');
    setRow(f.panels.therm.frames, t.frames ?? '—');
    setRow(f.panels.therm.valid, valid);
    setRow(f.panels.therm.splices, t.spliceDetected ?? 0);
    setRow(f.panels.therm.hwr, t.hwResets ?? 0, t.hwResets > 0 ? 'warn' : '');
    setRow(f.panels.therm.gain, (t.gain || '?') + ' · ' + (t.agc ? 'on' : 'off'));

    // Camera
    const v = s?.visible || {};
    setRow(f.panels.cam.sensor, v.sensor || '—');
    setRow(f.panels.cam.ready, v.ready ? 'yes' : 'no', v.ready ? 'ok' : 'warn');
    setRow(f.panels.cam.frame, (v.w && v.h) ? (v.w + '×' + v.h) : '—');
    setRow(f.panels.cam.fps, v.fps ?? '—');
    setRow(f.panels.cam.q, v.quality ?? '—');

    // Storage
    const st = s?.storage || {};
    setRow(f.panels.stor.sd, st.sdMounted ? 'mounted' : 'absent', st.sdMounted ? 'ok' : 'warn');
    setRow(f.panels.stor.sdUsed,
      st.sdMounted
        ? (fmtKB(st.sdUsedKB) + ' / ' + fmtKB(st.sdTotalKB) + ' (' + fmtPct(st.sdUsedKB, st.sdTotalKB) + ')')
        : '—');
    setRow(f.panels.stor.lfs,
      st.lfsMounted ? (fmtKB(st.lfsUsedKB) + ' / ' + fmtKB(st.lfsTotalKB)) : 'absent',
      st.lfsMounted ? '' : 'warn');

    // System
    setRow(f.panels.sys.uptime, s?.uptimeMs != null ? fmtAge(s.uptimeMs) : '—');
    setRow(f.panels.sys.heap, s?.freeHeap != null ? fmtKB(s.freeHeap / 1024) : '—');
    setRow(f.panels.sys.psram, s?.freePsram != null ? fmtKB(s.freePsram / 1024) : '—');
    setRow(f.panels.sys.ntp, s?.ntpSynced ? 'synced' : (s?.epoch ? 'set' : 'no'));

    f.rawPre.textContent = JSON.stringify(s, null, 2);
  }

  function applyEvents(card, events) {
    const f = card.fields;
    if (!events || !events.length) {
      f.events.style.display = 'none';
      return;
    }
    f.events.style.display = '';
    // Build via DOM, not innerHTML — event kind/msg come from the
    // device, so we treat them as untrusted text.
    f.events.replaceChildren();
    const h = document.createElement('h4');
    h.setAttribute('style', 'margin:14px 0 4px;font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:.06em;');
    h.textContent = 'Recent events';
    f.events.appendChild(h);
    for (const e of events) {
      const row = document.createElement('div');
      row.className = 'ev';
      const t = document.createElement('span');
      t.className = 't';
      t.textContent = new Date(e.ts).toLocaleTimeString();
      row.appendChild(t);
      // Plain text node for the message — no HTML interpretation.
      row.appendChild(document.createTextNode(' [' + (e.kind || '?') + '] ' + (e.msg || '')));
      f.events.appendChild(row);
    }
  }

  // --- image refresh ---
  // The browser keeps the previously decoded image visible until the
  // new one finishes loading and decoding, so a plain img.src swap
  // doesn't flash. The original "blink" was caused by tearing down the
  // <img> element each tick — which we no longer do.
  function refreshImage(card, modality) {
    const f = card.fields;
    const img = modality === 'vis' ? f.imgVis : f.imgTherm;
    const url = '/api/devices/' + card.baseId + '/last-frame.jpg?modality=' +
                modality + '&t=' + Date.now();
    img.onload = () => {
      img.style.display = '';
      const frame = modality === 'vis' ? f.frameVis : f.frameTherm;
      const ph    = modality === 'vis' ? f.phVis    : f.phTherm;
      if (frame.classList.contains('empty')) {
        frame.classList.remove('empty');
        if (ph) ph.style.display = 'none';
      }
    };
    img.onerror = () => { /* keep showing previous frame */ };
    img.src = url;
  }

  function refreshAllImages() {
    for (const card of cards.values()) {
      refreshImage(card, 'vis');
      refreshImage(card, 'thermal');
    }
  }

  async function fetchEvents(id) {
    try {
      const r = await fetch('/api/devices/' + encodeURIComponent(id) + '/events', { cache: 'no-store' });
      const j = await r.json();
      return (j.events || []).slice(-5).reverse();
    } catch { return []; }
  }

  // --- tick reentrancy ---
  // setInterval keeps firing even if the previous tick is still
  // awaiting. We've seen on hotspot networks that /state can take >2s,
  // which let two ticks race and produce DOM where cards in the Map
  // weren't in the DOM. Single-flight here is a hard guarantee.
  let ticking = false;

  // /api/devices briefly returns [] during Railway redeploys (the
  // in-memory store is wiped). Don't immediately tear down cards on
  // a single empty response; require N consecutive empties first.
  let emptyStreak = 0;
  const EMPTY_THRESHOLD = 3;

  async function tick() {
    if (ticking) return;
    ticking = true;
    try {
      const r = await fetch('/api/devices', { cache: 'no-store' });
      const data = await r.json();
      const devs = data.devices || [];
      const status = document.getElementById('status');
      const root = document.getElementById('devices');

      if (!devs.length) {
        emptyStreak++;
        status.textContent = cards.size > 0
          ? cards.size + ' device' + (cards.size === 1 ? '' : 's') + ' (relay quiet)'
          : '0 devices';
        if (emptyStreak >= EMPTY_THRESHOLD) {
          // Real empty — tear down.
          for (const [id, card] of cards) {
            if (card.rootEl.parentNode) card.rootEl.parentNode.removeChild(card.rootEl);
          }
          cards.clear();
          if (!root.querySelector('.no-devices')) {
            root.innerHTML = '<div class="no-devices">No devices connected yet. Start a device with relay enabled to see it here.</div>';
          }
        }
        return;
      }
      emptyStreak = 0;
      status.textContent = devs.length + ' device' + (devs.length === 1 ? '' : 's');

      // Remove the no-devices placeholder if present, but DON'T blow
      // away any existing cards in root. (Earlier the selector here
      // was '.empty' — which matched .frame.empty preview tiles too,
      // and silently tore them out one per tick.)
      const empty = root.querySelector('.no-devices');
      if (empty) empty.remove();

      const seen = new Set();
      for (const d of devs) {
        seen.add(d.deviceId);
        let card = cards.get(d.deviceId);
        if (!card) {
          card = buildCard(d.deviceId);
          cards.set(d.deviceId, card);
        }
        // Defensive: if the DOM and Map got out of sync (e.g. earlier
        // teardown left an orphan in Map), make sure the card is in
        // root. appendChild on an existing child is a no-op move.
        if (card.rootEl.parentNode !== root) root.appendChild(card.rootEl);

        const [sr, events] = await Promise.all([
          fetch('/api/devices/' + encodeURIComponent(d.deviceId) + '/state', { cache: 'no-store' }).then(r => r.json()),
          fetchEvents(d.deviceId),
        ]);
        const s = sr.tick || sr.init || {};
        applyState(card, d, s);
        applyEvents(card, events);
      }

      for (const [id, card] of cards) {
        if (!seen.has(id)) {
          if (card.rootEl.parentNode) card.rootEl.parentNode.removeChild(card.rootEl);
          cards.delete(id);
        }
      }
    } catch (e) {
      document.getElementById('status').textContent = 'error: ' + e.message;
    } finally {
      ticking = false;
    }
  }

  tick();
  setInterval(tick, 2000);
  setInterval(refreshAllImages, 1500);
</script>
</body>
</html>
`
