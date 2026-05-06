import { Hono } from 'hono'
import type { ServerWebSocket } from 'bun'
import { store } from './store'

const app = new Hono()

// ---- HTTP routes (dashboard + agent-readable JSON) ---------------------

app.get('/health', (c) =>
  c.json({
    ok: true,
    service: 'grasshopper-relay',
    version: '0.1.0',
    devices: store.list().length,
    ts: Date.now(),
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
  return c.json({ events: d.events })
})

app.post('/api/devices/:id/cmd', async (c) => {
  const d = store.get(c.req.param('id'))
  if (!d) return c.json({ error: 'unknown device' }, 404)
  if (!d.socket || d.socket.readyState !== 1)
    return c.json({ error: 'device offline' }, 409)
  const body = await c.req.json().catch(() => null)
  if (!body) return c.json({ error: 'invalid json' }, 400)
  d.socket.send(JSON.stringify({ ...body, type: 'cmd' }))
  return c.json({ ok: true })
})

// Dashboard — small, framework-free HTML that polls /api/devices.
app.get('/', (c) => {
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
      const text = typeof raw === 'string' ? raw : new TextDecoder().decode(raw)
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
          })
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
      }
    },
  },
})

console.log(`grasshopper-relay listening on :${port}`)
console.log(`  HTTP/dashboard  → http://localhost:${port}/`)
console.log(`  device WS       → ws://localhost:${port}/relay`)
console.log(`  RELAY_TOKEN=${RELAY_TOKEN === 'dev-token' ? '(dev default)' : 'set'}`)

// ---- Embedded dashboard HTML (no framework) ----------------------------

const DASHBOARD_HTML = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Grasshopper relay</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
  :root { color-scheme: light dark; --fg:#222; --bg:#fafafa; --muted:#777; --accent:#0a7; --warn:#c33; }
  @media (prefers-color-scheme: dark) { :root { --fg:#eee; --bg:#1a1a1a; --muted:#888; --accent:#4f8; } }
  * { box-sizing: border-box }
  body { font: 14px/1.5 -apple-system, system-ui, Segoe UI, sans-serif; margin: 0; color: var(--fg); background: var(--bg); }
  header { padding: 16px 24px; border-bottom: 1px solid #8881; display: flex; align-items: center; gap: 12px; }
  header h1 { font-size: 18px; margin: 0; font-weight: 600; }
  header .live { color: var(--accent); font-weight: 500; }
  main { max-width: 1100px; margin: 0 auto; padding: 24px; }
  .card { border: 1px solid #8882; border-radius: 8px; padding: 16px 20px; margin-bottom: 16px; background: #fff1; }
  .card h2 { font-size: 14px; font-weight: 600; margin: 0 0 8px; color: var(--muted); text-transform: uppercase; letter-spacing: .05em; }
  .empty { color: var(--muted); font-style: italic; }
  .device { border: 1px solid #8883; border-radius: 6px; padding: 12px 16px; margin-bottom: 12px; }
  .device h3 { margin: 0 0 4px; font-size: 16px; display: flex; gap: 8px; align-items: center; }
  .device .id { font-family: ui-monospace, monospace; font-size: 13px; }
  .device .pill { display: inline-block; font-size: 11px; padding: 2px 8px; border-radius: 999px; background: #8884; }
  .pill.online { background: var(--accent); color: #000; }
  .pill.offline { background: var(--warn); color: #fff; }
  .device .meta { color: var(--muted); font-size: 12px; }
  .device pre { font: 11px/1.4 ui-monospace, monospace; background: #0001; padding: 8px; border-radius: 4px; max-height: 200px; overflow: auto; margin: 8px 0 0; }
  .footer { color: var(--muted); font-size: 11px; padding: 24px; text-align: center; }
</style>
</head>
<body>
<header>
  <h1>🦗 Grasshopper relay</h1>
  <span class="live" id="status">connecting…</span>
</header>
<main>
  <div class="card">
    <h2>Devices</h2>
    <div id="devices" class="empty">No devices connected yet.</div>
  </div>
</main>
<div class="footer">
  Agent-readable endpoints: <code>/api/devices</code>, <code>/api/devices/:id/state</code>,
  <code>/api/devices/:id/logs?since=&lt;ts&gt;</code>, <code>/api/devices/:id/events</code>
</div>
<script>
  async function tick() {
    try {
      const r = await fetch('/api/devices', { cache: 'no-store' })
      const data = await r.json()
      const devs = data.devices || []
      const root = document.getElementById('devices')
      const status = document.getElementById('status')
      status.textContent = devs.length + ' device' + (devs.length === 1 ? '' : 's')
      if (!devs.length) {
        root.className = 'empty'
        root.innerHTML = 'No devices connected yet. Start a device with relay enabled to see it here.'
        return
      }
      root.className = ''
      const blocks = await Promise.all(devs.map(async d => {
        const stateRes = await fetch('/api/devices/' + encodeURIComponent(d.deviceId) + '/state', { cache: 'no-store' })
        const stateData = await stateRes.json()
        const ageS = Math.floor((Date.now() - d.lastSeenMs) / 1000)
        return \`<div class="device">
          <h3><span class="id">\${d.deviceId}</span>
              <span class="pill \${d.online ? 'online' : 'offline'}">\${d.online ? 'online' : 'offline'}</span></h3>
          <div class="meta">fw \${d.fwVersion || '?'} · \${d.gitSha || '?'} · ip \${d.ip || '?'} · last seen \${ageS}s ago</div>
          <pre>\${JSON.stringify(stateData.tick || stateData.init || {}, null, 2)}</pre>
        </div>\`
      }))
      root.innerHTML = blocks.join('')
    } catch (e) {
      document.getElementById('status').textContent = 'error: ' + e.message
    }
  }
  tick(); setInterval(tick, 2000)
</script>
</body>
</html>
`
