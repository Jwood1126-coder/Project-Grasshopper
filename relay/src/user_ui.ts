// Grasshopper user-facing UI — Phase 1 (Live View).
//
// Self-contained HTML/CSS/JS, no framework. Served at GET /.
// Polls /api/devices and /api/devices/:id/state for live data, refreshes
// thermal + visible last-frame previews every ~250 ms.
//
// Phase 1 scope (intentionally small):
//   - Pick the first online device, or show a friendly empty state
//   - Big thermal panel + big visible panel side-by-side (stack on mobile)
//   - Status chips: connection, thermal fps, frames committed, storage
//   - Capture button + Start Timelapse button — present, disabled with
//     "needs firmware command handling (Phase 2)" tooltip
//   - Diagnostics in <details>, collapsed by default
//
// Architectural note: the existing debug telescope at /debug stays for
// agent debugging. This UI is what end-users see at the root URL.

export const USER_UI_HTML = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Grasshopper</title>
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0a0e14">
<style>
  :root {
    color-scheme: dark;
    --bg: #0a0e14;
    --surface: #11161e;
    --surface-2: #1a212c;
    --border: #2a3340;
    --text: #e6edf3;
    --muted: #7d8b9b;
    --accent: #4fb8d8;
    --accent-warm: #ff9457;
    --ok: #4ad07b;
    --warn: #ffb454;
    --err: #f47174;
    --panel-bg: #0d1218;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  html, body { height: 100%; }
  body {
    font: 15px/1.5 -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
    color: var(--text);
    background: var(--bg);
    -webkit-font-smoothing: antialiased;
    -webkit-tap-highlight-color: transparent;
  }
  .app { max-width: 1200px; margin: 0 auto; padding: 16px; padding-bottom: env(safe-area-inset-bottom, 16px); }

  /* ─── Header ─── */
  header {
    display: flex; align-items: center; justify-content: space-between;
    padding: 8px 4px 20px;
  }
  .logo { font: 600 20px/1 -apple-system; letter-spacing: -0.01em; }
  .logo .accent { color: var(--accent); }
  .chips { display: flex; gap: 6px; flex-wrap: wrap; }
  .chip {
    display: inline-flex; align-items: center; gap: 6px;
    padding: 5px 10px; border-radius: 999px;
    font: 500 12px/1 ui-monospace, monospace;
    background: var(--surface); border: 1px solid var(--border);
    color: var(--muted);
  }
  .chip.online { color: var(--ok); border-color: #1f3a28; background: #0e1a13; }
  .chip.offline { color: var(--err); border-color: #3a1f23; background: #1a0e10; }
  .chip.warn { color: var(--warn); border-color: #3a2f1f; background: #1a160e; }
  .chip .dot { width: 7px; height: 7px; border-radius: 50%; background: currentColor; }
  .chip.online .dot { animation: pulse 2.4s ease-in-out infinite; }
  @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.45; } }

  /* ─── Empty state ─── */
  .empty {
    display: flex; flex-direction: column; align-items: center; justify-content: center;
    text-align: center; padding: 80px 20px; color: var(--muted);
  }
  .empty h2 { font-size: 20px; color: var(--text); margin-bottom: 8px; font-weight: 500; }
  .empty p { font-size: 14px; line-height: 1.6; max-width: 380px; }
  .empty .pulse {
    width: 80px; height: 80px; border-radius: 50%;
    background: radial-gradient(circle, var(--accent) 0%, transparent 70%);
    margin-bottom: 24px; opacity: 0.4;
    animation: pulse-ring 2s ease-out infinite;
  }
  @keyframes pulse-ring { 0% { transform: scale(0.6); opacity: 0.6; } 100% { transform: scale(1.3); opacity: 0; } }

  /* ─── View grid (mobile-first stack, desktop side-by-side) ─── */
  .views {
    display: grid; gap: 12px;
    grid-template-columns: 1fr;
    margin-bottom: 16px;
  }
  @media (min-width: 720px) {
    .views { grid-template-columns: 1fr 1fr; }
  }

  .panel {
    background: var(--panel-bg); border: 1px solid var(--border); border-radius: 12px;
    overflow: hidden; position: relative; aspect-ratio: 4/3;
    display: flex; flex-direction: column;
  }
  .panel-label {
    position: absolute; top: 10px; left: 12px; z-index: 2;
    font: 600 10px/1 ui-sans-serif; letter-spacing: 0.08em; text-transform: uppercase;
    color: var(--text); padding: 5px 9px; border-radius: 4px;
    background: rgba(10, 14, 20, 0.7); backdrop-filter: blur(8px);
  }
  .panel-label.thermal { color: var(--accent-warm); }
  .panel-meta {
    position: absolute; top: 10px; right: 12px; z-index: 2;
    font: 500 11px/1 ui-monospace, monospace; color: var(--muted);
    padding: 5px 9px; border-radius: 4px;
    background: rgba(10, 14, 20, 0.7); backdrop-filter: blur(8px);
  }
  .panel img {
    width: 100%; height: 100%; object-fit: contain; background: #000;
    display: block;
  }
  .panel.thermal img { image-rendering: pixelated; }
  .panel-empty {
    flex: 1; display: flex; align-items: center; justify-content: center;
    color: var(--muted); font-size: 13px; font-style: italic;
  }

  /* ─── Action buttons ─── */
  .actions {
    display: grid; grid-template-columns: 1fr 1fr; gap: 10px;
    margin-bottom: 20px;
  }
  .btn {
    display: flex; align-items: center; justify-content: center; gap: 8px;
    height: 52px; padding: 0 20px; border-radius: 12px;
    font: 600 15px/1 -apple-system; cursor: pointer;
    background: var(--surface); color: var(--text); border: 1px solid var(--border);
    transition: all 0.15s ease;
    -webkit-user-select: none; user-select: none;
  }
  .btn:hover:not(:disabled) { background: var(--surface-2); border-color: #3d4a5c; }
  .btn:active:not(:disabled) { transform: scale(0.98); }
  .btn:disabled { opacity: 0.4; cursor: not-allowed; }
  .btn.primary { background: var(--accent); color: #001620; border-color: var(--accent); }
  .btn.primary:hover:not(:disabled) { background: #6cc8e3; border-color: #6cc8e3; }
  .btn.danger { background: var(--err); color: #1a0e10; border-color: var(--err); }
  .btn .icon { font-size: 18px; line-height: 1; }
  .btn .badge {
    font: 500 10px/1 ui-monospace, monospace;
    padding: 2px 6px; border-radius: 4px;
    background: rgba(0,0,0,0.2); margin-left: 4px; opacity: 0.7;
  }

  /* ─── Diagnostics ─── */
  details {
    background: var(--surface); border: 1px solid var(--border);
    border-radius: 10px; padding: 0; margin-bottom: 12px;
  }
  details summary {
    padding: 14px 16px; cursor: pointer; user-select: none;
    font: 500 13px -apple-system; color: var(--muted);
    list-style: none; display: flex; align-items: center; gap: 8px;
  }
  details summary::-webkit-details-marker { display: none; }
  details summary::before {
    content: '▸'; transition: transform 0.2s; display: inline-block;
    color: var(--muted); font-size: 10px;
  }
  details[open] summary::before { transform: rotate(90deg); }
  details .body { padding: 4px 16px 16px; border-top: 1px solid var(--border); }
  .stat-grid {
    display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr));
    gap: 10px; margin-top: 12px;
  }
  .stat {
    background: var(--panel-bg); border: 1px solid var(--border);
    border-radius: 8px; padding: 10px 12px;
  }
  .stat .k {
    font: 500 10px/1 ui-sans-serif; text-transform: uppercase; letter-spacing: 0.06em;
    color: var(--muted); margin-bottom: 4px;
  }
  .stat .v {
    font: 600 16px/1 ui-monospace, monospace;
    color: var(--text); font-variant-numeric: tabular-nums;
  }
  .stat .v.ok { color: var(--ok); }
  .stat .v.warn { color: var(--warn); }
  .stat .v.err { color: var(--err); }

  /* ─── Footer ─── */
  footer {
    text-align: center; color: var(--muted); font-size: 11px;
    padding: 24px 0 8px;
  }
  footer a { color: var(--muted); text-decoration: none; }
  footer a:hover { color: var(--accent); }

  /* Toast for command feedback */
  .toast {
    position: fixed; bottom: 24px; left: 50%; transform: translateX(-50%);
    background: var(--surface-2); border: 1px solid var(--border);
    color: var(--text); padding: 12px 18px; border-radius: 10px;
    font-size: 14px; box-shadow: 0 10px 30px rgba(0,0,0,0.4);
    opacity: 0; pointer-events: none; transition: opacity 0.2s;
    max-width: 90%; z-index: 100;
  }
  .toast.show { opacity: 1; }
</style>
</head>
<body>
<div class="app">
  <header>
    <div class="logo">🦗 <span class="accent">Grasshopper</span></div>
    <div class="chips" id="chips"></div>
  </header>

  <div id="content">
    <div class="empty">
      <div class="pulse"></div>
      <h2>Looking for your device…</h2>
      <p>Power on a Grasshopper unit and connect it to Wi-Fi. It should appear here within a few seconds.</p>
    </div>
  </div>

  <footer>
    Grasshopper relay · <a href="/debug">debug telescope</a> · <a href="/health">health</a>
  </footer>
</div>

<div class="toast" id="toast"></div>

<script>
  const fmtPct = (u, t) => (t > 0) ? Math.round(100 * u / t) + '%' : '—';
  const fmtMB  = (b) => b == null ? '—' : (b / 1048576).toFixed(1) + ' MB';
  const fmtAge = (ms) => {
    const s = Math.floor(ms / 1000);
    if (s < 60) return s + 's';
    if (s < 3600) return Math.floor(s/60) + 'm';
    return Math.floor(s/3600) + 'h';
  };

  const toast = (msg, kind = 'info') => {
    const t = document.getElementById('toast');
    t.textContent = msg;
    t.classList.add('show');
    setTimeout(() => t.classList.remove('show'), 2400);
  };

  const el = (tag, attrs = {}, ...children) => {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs)) {
      if (k === 'class') e.className = v;
      else if (k === 'onclick') e.onclick = v;
      else if (k.startsWith('data-')) e.setAttribute(k, v);
      else e.setAttribute(k, v);
    }
    for (const c of children) {
      if (c == null) continue;
      e.append(typeof c === 'string' ? document.createTextNode(c) : c);
    }
    return e;
  };

  // Build the per-device DOM ONCE; mutate fields on every poll. Avoids the
  // page-wide blink that comes from rebuilding innerHTML each tick.
  let currentDeviceId = null;
  let imgRefreshTimer = null;
  const fields = {};

  function buildLiveView(d) {
    fields.thermalImg = el('img', { id: 'therm-img', alt: 'Thermal preview' });
    fields.thermalEmpty = el('div', { class: 'panel-empty' }, 'no thermal preview yet');
    fields.thermalMeta = el('div', { class: 'panel-meta' }, '—');
    const thermalPanel = el('div', { class: 'panel thermal' },
      el('div', { class: 'panel-label thermal' }, 'Thermal'),
      fields.thermalMeta,
      fields.thermalEmpty
    );

    fields.visImg = el('img', { id: 'vis-img', alt: 'Visible preview' });
    fields.visEmpty = el('div', { class: 'panel-empty' }, 'no visible preview yet');
    fields.visMeta = el('div', { class: 'panel-meta' }, '—');
    const visPanel = el('div', { class: 'panel visible' },
      el('div', { class: 'panel-label' }, 'Visible'),
      fields.visMeta,
      fields.visEmpty
    );

    const views = el('div', { class: 'views' }, thermalPanel, visPanel);

    const captureBtn = el('button', {
      class: 'btn primary', disabled: true,
      title: 'Coming in next update — needs firmware command handling',
      onclick: () => sendCmd('capture.now', {}),
    },
      el('span', { class: 'icon' }, '📷'),
      el('span', {}, 'Capture'),
      el('span', { class: 'badge' }, 'soon')
    );

    const tlBtn = el('button', {
      class: 'btn', disabled: true,
      title: 'Coming in next update — needs firmware command handling',
      onclick: () => sendCmd('timelapse.start', { intervalSec: 30 }),
    },
      el('span', { class: 'icon' }, '▶'),
      el('span', {}, 'Start Timelapse'),
      el('span', { class: 'badge' }, 'soon')
    );

    const actions = el('div', { class: 'actions' }, captureBtn, tlBtn);

    fields.statBlock = el('div', { class: 'stat-grid' });
    const diagnostics = el('details', {},
      el('summary', {}, 'Diagnostics & device state'),
      el('div', { class: 'body' }, fields.statBlock)
    );

    return el('div', {}, views, actions, diagnostics);
  }

  function updateLiveView(d, state) {
    // Thermal preview: lazy-create img, swap src via preload to avoid blink.
    const therm = state?.init?.thermal;
    if (therm && therm.frames > 0) {
      attachImg(fields.thermalImg, fields.thermalEmpty, d.deviceId, 'thermal');
    }
    // Visible preview: same pattern.
    const vis = state?.init?.visible;
    if (vis && vis.ready) {
      attachImg(fields.visImg, fields.visEmpty, d.deviceId, 'vis');
    }

    // Meta text overlays (fps, frame count)
    if (therm) {
      fields.thermalMeta.textContent =
        therm.fps != null
          ? \`\${therm.fps.toFixed(1)} fps · \${therm.frames} frames\`
          : \`\${therm.frames} frames\`;
    }
    if (vis) {
      fields.visMeta.textContent =
        vis.fps != null && vis.fps > 0
          ? \`\${vis.fps.toFixed(1)} fps\`
          : \`\${vis.w || '?'}×\${vis.h || '?'}\`;
    }

    // Diagnostic stats
    fields.statBlock.replaceChildren(
      stat('Device', d.deviceId, ''),
      stat('Firmware', d.fwVersion || '—', ''),
      stat('Wi-Fi', state?.init?.wifi?.ssid || '—', ''),
      stat('RSSI', state?.init?.wifi?.rssi != null ? state.init.wifi.rssi + ' dBm' : '—',
           state?.init?.wifi?.rssi > -65 ? 'ok' : ''),
      stat('Free heap', state?.init?.freeHeap != null ? Math.round(state.init.freeHeap / 1024) + ' KB' : '—', ''),
      stat('Free PSRAM', state?.init?.freePsram != null ? Math.round(state.init.freePsram / 1048576) + ' MB' : '—', ''),
      stat('Therm valid',
           therm ? fmtPct(therm.validPackets, therm.totalPackets) : '—',
           therm && therm.totalPackets > 0 ?
             (therm.validPackets / therm.totalPackets > 0.5 ? 'ok' : 'warn') : ''),
      stat('Last seen', d.lastSeenMs ? fmtAge(Date.now() - d.lastSeenMs) + ' ago' : '—', ''),
    );
  }

  function stat(k, v, kind) {
    return el('div', { class: 'stat' },
      el('div', { class: 'k' }, k),
      el('div', { class: 'v ' + (kind || '') }, String(v)));
  }

  function attachImg(img, emptyEl, deviceId, modality) {
    if (!img.parentElement) {
      // First time: hide empty placeholder, attach img to the same panel.
      const panel = emptyEl.parentElement;
      panel.appendChild(img);
      emptyEl.remove();
    }
  }

  function refreshImgs() {
    if (!currentDeviceId) return;
    const ts = Date.now();
    if (fields.thermalImg && fields.thermalImg.parentElement) {
      const next = new Image();
      const url = '/api/devices/' + encodeURIComponent(currentDeviceId) +
                  '/last-frame.jpg?modality=thermal&t=' + ts;
      next.onload = () => { fields.thermalImg.src = url; };
      next.src = url;
    }
    if (fields.visImg && fields.visImg.parentElement) {
      const next = new Image();
      const url = '/api/devices/' + encodeURIComponent(currentDeviceId) +
                  '/last-frame.jpg?modality=vis&t=' + ts;
      next.onload = () => { fields.visImg.src = url; };
      next.src = url;
    }
  }

  function updateChips(d) {
    const chips = document.getElementById('chips');
    chips.innerHTML = '';
    if (!d) {
      chips.appendChild(el('div', { class: 'chip offline' },
        el('span', { class: 'dot' }), 'No device'));
      return;
    }
    const onlineChip = el('div', { class: d.online ? 'chip online' : 'chip offline' },
      el('span', { class: 'dot' }), d.online ? 'Live' : 'Offline');
    chips.appendChild(onlineChip);
    if (d.state) {
      chips.appendChild(el('div', { class: 'chip' }, d.state));
    }
  }

  async function sendCmd(type, payload) {
    // Phase 2: this will hit /api/devices/:id/cmd. For now, a stub that
    // toasts so the UX feels real even though firmware doesn't act yet.
    toast('Command "' + type + '" queued (firmware handler lands in Phase 2)');
  }

  async function poll() {
    try {
      const r = await fetch('/api/devices', { cache: 'no-store' });
      const j = await r.json();
      const devices = (j.devices || []).filter(d => d.online);
      const d = devices[0] || (j.devices || [])[0] || null;

      const content = document.getElementById('content');

      if (!d) {
        currentDeviceId = null;
        // Show empty state if not already showing
        if (!content.querySelector('.empty')) {
          content.innerHTML = '';
          content.appendChild(el('div', { class: 'empty' },
            el('div', { class: 'pulse' }),
            el('h2', {}, 'Looking for your device…'),
            el('p', {}, 'Power on a Grasshopper unit and connect it to Wi-Fi. It should appear here within a few seconds.')
          ));
        }
        updateChips(null);
        return;
      }

      // First time we see a device, build the live view DOM.
      if (currentDeviceId !== d.deviceId) {
        currentDeviceId = d.deviceId;
        const live = buildLiveView(d);
        content.innerHTML = '';
        content.appendChild(live);
      }

      updateChips(d);

      const sr = await fetch('/api/devices/' + encodeURIComponent(d.deviceId) + '/state',
                              { cache: 'no-store' });
      const state = await sr.json();
      updateLiveView(d, state);
    } catch (e) {
      console.warn('poll error', e);
    }
  }

  poll();
  setInterval(poll, 2000);

  // Image refresh on its own faster timer so previews feel live.
  setInterval(refreshImgs, 250);
</script>
</body>
</html>`
