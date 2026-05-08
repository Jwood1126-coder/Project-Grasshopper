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
  :root { color-scheme: light dark; --fg:#222; --bg:#fafafa; --muted:#777; --accent:#0a7; --warn:#c33; --ok:#2a8; --line:#8883; }
  @media (prefers-color-scheme: dark) { :root { --fg:#eee; --bg:#161616; --muted:#888; --accent:#4f8; --ok:#3c9; --line:#fff2; } }
  * { box-sizing: border-box }
  body { font: 14px/1.5 -apple-system, system-ui, Segoe UI, sans-serif; margin: 0; color: var(--fg); background: var(--bg); }
  header { padding: 14px 24px; border-bottom: 1px solid var(--line); display: flex; align-items: center; gap: 12px; }
  header h1 { font-size: 18px; margin: 0; font-weight: 600; }
  header .live { color: var(--accent); font-weight: 500; font-size: 13px; }
  main { max-width: 1200px; margin: 0 auto; padding: 20px 24px; }
  .empty { color: var(--muted); font-style: italic; }
  .device { border: 1px solid var(--line); border-radius: 8px; padding: 14px 18px; margin-bottom: 18px; background: #fff1; }
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
<main id="devices"><div class="empty">No devices connected yet.</div></main>
<div class="footer">
  Agent-readable: <code>/api/devices</code> · <code>/api/devices/:id/state</code> ·
  <code>/api/devices/:id/logs?since=</code> · <code>/api/devices/:id/events</code>
</div>
<script>
  const fmtKB = (n) => n == null ? '—' : n < 1024 ? n + ' KB' : (n / 1024).toFixed(1) + ' MB';
  const fmtPct = (used, total) => (total > 0) ? Math.round((used / total) * 100) + '%' : '—';
  const fmtAge = (ms) => {
    const s = Math.floor(ms / 1000);
    if (s < 60) return s + 's';
    if (s < 3600) return Math.floor(s/60) + 'm ' + (s%60) + 's';
    return Math.floor(s/3600) + 'h ' + Math.floor((s%3600)/60) + 'm';
  };
  const rssiClass = (r) => r === 0 ? 'warn' : r > -65 ? 'ok' : '';

  function row(k, v, cls) {
    const c = cls ? \` class="v \${cls}"\` : ' class="v"';
    return \`<div class="row"><span class="k">\${k}</span><span\${c}>\${v ?? '—'}</span></div>\`;
  }

  function panelWifi(s) {
    const w = s?.wifi || {};
    return \`<div class="panel"><h4>WiFi</h4>
      \${row('SSID', w.ssid || '—')}
      \${row('IP', w.ip || '—')}
      \${row('RSSI', w.rssi != null && w.rssi !== 0 ? w.rssi + ' dBm' : 'no link', rssiClass(w.rssi))}
      \${row('Mode', w.mode || '—')}
    </div>\`;
  }

  function panelThermal(s) {
    const t = s?.thermal || {};
    const valid = t.totalPackets ? Math.round(100 * t.validPackets / t.totalPackets) + '%' : '—';
    return \`<div class="panel"><h4>Lepton</h4>
      \${row('State', t.state || '—')}
      \${row('FPS', t.fps ?? '—')}
      \${row('Frames', t.frames ?? '—')}
      \${row('Valid', valid)}
      \${row('Splices', t.spliceDetected ?? 0)}
      \${row('HW resets', t.hwResets ?? 0, t.hwResets > 0 ? 'warn' : '')}
      \${row('Gain · AGC', (t.gain || '?') + ' · ' + (t.agc ? 'on' : 'off'))}
    </div>\`;
  }

  function panelCamera(s) {
    const v = s?.visible || {};
    return \`<div class="panel"><h4>Camera</h4>
      \${row('Sensor', v.sensor || '—')}
      \${row('Ready', v.ready ? 'yes' : 'no', v.ready ? 'ok' : 'warn')}
      \${row('Frame', v.w && v.h ? v.w + '×' + v.h : '—')}
      \${row('FPS', v.fps ?? '—')}
      \${row('JPEG q', v.quality ?? '—')}
    </div>\`;
  }

  function panelStorage(s) {
    const st = s?.storage || {};
    return \`<div class="panel"><h4>Storage</h4>
      \${row('SD', st.sdMounted ? 'mounted' : 'absent', st.sdMounted ? 'ok' : 'warn')}
      \${row('SD used', st.sdMounted ? (fmtKB(st.sdUsedKB) + ' / ' + fmtKB(st.sdTotalKB) + ' (' + fmtPct(st.sdUsedKB, st.sdTotalKB) + ')') : '—')}
      \${row('LittleFS', st.lfsMounted ? fmtKB(st.lfsUsedKB) + ' / ' + fmtKB(st.lfsTotalKB) : 'absent', st.lfsMounted ? '' : 'warn')}
    </div>\`;
  }

  function panelSystem(s) {
    return \`<div class="panel"><h4>System</h4>
      \${row('Uptime', s?.uptimeMs != null ? fmtAge(s.uptimeMs) : '—')}
      \${row('Free heap', s?.freeHeap != null ? fmtKB(s.freeHeap / 1024) : '—')}
      \${row('Free PSRAM', s?.freePsram != null ? fmtKB(s.freePsram / 1024) : '—')}
      \${row('NTP', s?.ntpSynced ? 'synced' : (s?.epoch ? 'set' : 'no'))}
    </div>\`;
  }

  async function fetchEvents(id) {
    try {
      const r = await fetch('/api/devices/' + encodeURIComponent(id) + '/events', { cache: 'no-store' });
      const j = await r.json();
      return (j.events || []).slice(-5).reverse();
    } catch { return []; }
  }

  function eventBlock(events) {
    if (!events.length) return '';
    return '<div class="events"><h4 style="margin:14px 0 4px;font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:.06em;">Recent events</h4>' +
      events.map(e => \`<div class="ev"><span class="t">\${new Date(e.ts).toLocaleTimeString()}</span> [\${e.kind}] \${e.msg || ''}</div>\`).join('') +
      '</div>';
  }

  async function tick() {
    try {
      const r = await fetch('/api/devices', { cache: 'no-store' });
      const data = await r.json();
      const devs = data.devices || [];
      const status = document.getElementById('status');
      status.textContent = devs.length + ' device' + (devs.length === 1 ? '' : 's');
      const root = document.getElementById('devices');
      if (!devs.length) {
        root.innerHTML = '<div class="empty">No devices connected yet. Start a device with relay enabled to see it here.</div>';
        return;
      }
      const blocks = await Promise.all(devs.map(async d => {
        const sr = await fetch('/api/devices/' + encodeURIComponent(d.deviceId) + '/state', { cache: 'no-store' });
        const sd = await sr.json();
        const s = sd.tick || sd.init || {};
        const events = await fetchEvents(d.deviceId);
        const ageS = Math.floor((Date.now() - d.lastSeenMs) / 1000);
        return \`<div class="device">
          <h3><span class="id">\${d.deviceId}</span>
              <span class="pill \${d.online ? 'online' : 'offline'}">\${d.online ? 'online' : 'offline'}</span>
              <span class="pill state">\${d.state || '—'}</span></h3>
          <div class="meta">fw \${d.fwVersion || '?'} · sha \${d.gitSha || '?'} · ip \${d.ip || '?'} · last seen \${ageS}s ago</div>
          <div class="grid">
            \${panelWifi(s)}
            \${panelThermal(s)}
            \${panelCamera(s)}
            \${panelStorage(s)}
            \${panelSystem(s)}
          </div>
          \${eventBlock(events)}
          <details><summary>raw tick JSON</summary><pre>\${JSON.stringify(s, null, 2)}</pre></details>
        </div>\`;
      }));
      root.innerHTML = blocks.join('');
    } catch (e) {
      document.getElementById('status').textContent = 'error: ' + e.message;
    }
  }
  tick(); setInterval(tick, 2000);
</script>
</body>
</html>
`
