// Grasshopper user-facing UI — Phase 1 (Live View) + Phase 2 (commands).
//
// Self-contained HTML/CSS/JS, no framework. Served at GET /.
// Polls /api/devices and /api/devices/:id/state for live data, refreshes
// thermal + visible last-frame previews every ~250 ms.
//
// Three health indicators (codex's distinction):
//   1. Transport health — relay says device is online (WS connected)
//   2. Acquisition health — Lepton frame counter increasing, valid:total ratio
//   3. Preview freshness — age of last JPEG (could be stale even if 1+2 are OK)
// All three are surfaced separately so a stale preview doesn't read as live.
//
// Phase 2 commands:
//   - UI generates a unique id per command, POSTs to /api/devices/:id/cmd
//   - Polls /api/devices/:id/events?since=<ts> looking for cmd.result with id
//   - Result toast shows ok/fail + msg, button reverts from pending state
//
// Auth: token from URL hash (#token=...) or localStorage; defaults to
// 'dev-token' which works on the current Railway deploy.

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
    padding: 8px 4px 20px; gap: 12px; flex-wrap: wrap;
  }
  .logo { font: 600 20px/1 -apple-system; letter-spacing: -0.01em; flex-shrink: 0; }
  .logo .accent { color: var(--accent); }
  .chips { display: flex; gap: 6px; flex-wrap: wrap; justify-content: flex-end; }
  .chip {
    display: inline-flex; align-items: center; gap: 6px;
    padding: 5px 10px; border-radius: 999px;
    font: 500 12px/1 ui-monospace, monospace;
    background: var(--surface); border: 1px solid var(--border);
    color: var(--muted);
    cursor: default; transition: opacity 0.2s;
  }
  .chip[title] { cursor: help; }
  .chip.ok    { color: var(--ok);   border-color: #1f3a28; background: #0e1a13; }
  .chip.warn  { color: var(--warn); border-color: #3a2f1f; background: #1a160e; }
  .chip.err   { color: var(--err);  border-color: #3a1f23; background: #1a0e10; }
  .chip .dot { width: 7px; height: 7px; border-radius: 50%; background: currentColor; }
  .chip.ok .dot { animation: pulse 2.4s ease-in-out infinite; }
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
    transition: opacity 0.4s;
  }
  .panel.stale img { opacity: 0.5; filter: saturate(0.5); }
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
    display: flex; gap: 6px; align-items: center;
  }
  .panel-meta .age.warn { color: var(--warn); }
  .panel-meta .age.err  { color: var(--err); }
  .panel img {
    width: 100%; height: 100%; object-fit: contain; background: #000;
    display: block;
  }
  .panel.thermal img { image-rendering: pixelated; }
  .panel-empty {
    flex: 1; display: flex; align-items: center; justify-content: center;
    color: var(--muted); font-size: 13px; font-style: italic;
  }
  .panel.stale::after {
    content: 'stale preview'; position: absolute; bottom: 10px; left: 12px;
    font: 600 10px/1 ui-sans-serif; letter-spacing: 0.08em; text-transform: uppercase;
    color: var(--warn); padding: 5px 9px; border-radius: 4px;
    background: rgba(26, 22, 14, 0.85); backdrop-filter: blur(8px);
  }

  /* ─── Action buttons ─── */
  .actions {
    display: grid; grid-template-columns: 1fr 1fr; gap: 10px;
    margin-bottom: 12px;
  }
  .actions-secondary {
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
    position: relative;
  }
  .btn.sm { height: 40px; font-size: 13px; }
  .btn:hover:not(:disabled) { background: var(--surface-2); border-color: #3d4a5c; }
  .btn:active:not(:disabled) { transform: scale(0.98); }
  .btn:disabled { opacity: 0.4; cursor: not-allowed; }
  .btn.primary { background: var(--accent); color: #001620; border-color: var(--accent); }
  .btn.primary:hover:not(:disabled) { background: #6cc8e3; border-color: #6cc8e3; }
  .btn.danger { color: var(--err); border-color: #3a1f23; }
  .btn.danger:hover:not(:disabled) { background: #1a0e10; }
  .btn .icon { font-size: 18px; line-height: 1; }
  .btn .badge {
    font: 500 10px/1 ui-monospace, monospace;
    padding: 2px 6px; border-radius: 4px;
    background: rgba(0,0,0,0.2); margin-left: 4px; opacity: 0.7;
  }
  .btn.pending { animation: pending 1.2s ease-in-out infinite; }
  @keyframes pending { 0%, 100% { opacity: 1; } 50% { opacity: 0.55; } }

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
  .token-row {
    display: flex; gap: 8px; align-items: center; margin-top: 12px;
    padding: 10px 12px; background: var(--panel-bg);
    border: 1px solid var(--border); border-radius: 8px;
  }
  .token-row label { font-size: 12px; color: var(--muted); flex-shrink: 0; }
  .token-row input {
    flex: 1; min-width: 0; background: transparent; color: var(--text);
    border: 1px solid var(--border); border-radius: 6px; padding: 6px 8px;
    font: 13px ui-monospace, monospace;
  }

  /* ─── Footer ─── */
  footer {
    text-align: center; color: var(--muted); font-size: 11px;
    padding: 24px 0 8px;
  }
  footer a { color: var(--muted); text-decoration: none; }
  footer a:hover { color: var(--accent); }

  /* Security banner — shown when relay is publicly exposed with the
     dev-token default. Anyone with the URL can issue commands. */
  .insecure-banner {
    background: #3a1f23; border: 1px solid var(--err); border-radius: 8px;
    padding: 12px 14px; margin-bottom: 16px; color: #ffc7c9;
    font-size: 13px; line-height: 1.5;
  }
  .insecure-banner strong { color: var(--err); }

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
  .toast.ok { border-color: #1f3a28; }
  .toast.err { border-color: #3a1f23; color: var(--err); }
</style>
</head>
<body>
<div class="app">
  <header>
    <div class="logo">🦗 <span class="accent">Grasshopper</span></div>
    <div class="chips" id="chips"></div>
  </header>

  <div id="banner-slot"></div>

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
  // ───── Auth token (URL hash > localStorage > 'dev-token') ─────
  function loadToken() {
    const m = location.hash.match(/(?:^|[#&])token=([^&]+)/);
    if (m) {
      const t = decodeURIComponent(m[1]);
      try { localStorage.setItem('gh_token', t); } catch {}
      // Strip from URL so it isn't visible to anyone glancing at the address bar.
      history.replaceState(null, '', location.pathname + location.search);
      return t;
    }
    try {
      const t = localStorage.getItem('gh_token');
      if (t) return t;
    } catch {}
    return 'dev-token';
  }
  function saveToken(t) {
    try { localStorage.setItem('gh_token', t || 'dev-token'); } catch {}
    authToken = t || 'dev-token';
  }
  let authToken = loadToken();

  // ───── Formatters ─────
  const fmtPct = (u, t) => (t > 0) ? Math.round(100 * u / t) + '%' : '—';
  const fmtAge = (ms) => {
    if (ms == null) return '—';
    const s = Math.floor(ms / 1000);
    if (s < 1)    return 'now';
    if (s < 60)   return s + 's';
    if (s < 3600) return Math.floor(s/60) + 'm';
    return Math.floor(s/3600) + 'h';
  };

  const toast = (msg, kind) => {
    const t = document.getElementById('toast');
    t.textContent = msg;
    t.className = 'toast show ' + (kind || '');
    setTimeout(() => t.classList.remove('show'), 3000);
  };

  const el = (tag, attrs = {}, ...children) => {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (v == null || v === false) continue;
      if (k === 'class') e.className = v;
      else if (k === 'onclick') e.onclick = v;
      else if (k === 'oninput') e.oninput = v;
      else e.setAttribute(k, v === true ? '' : String(v));
    }
    for (const c of children) {
      if (c == null || c === false) continue;
      e.append(typeof c === 'string' || typeof c === 'number'
        ? document.createTextNode(String(c)) : c);
    }
    return e;
  };

  // ───── Per-device DOM (built once, mutated on poll) ─────
  let currentDeviceId = null;
  const fields = {};
  // Track frame count progression for acquisition-health detection.
  let lastThermFrames = 0;
  let lastThermFramesTs = 0;
  // Track preview age (relay-side timestamp from X-Frame-Age-Ms).
  let thermPreviewAgeMs = null;
  let visPreviewAgeMs = null;
  // Pending command tracking: id → { type, sentAtMs, button, originalText }
  const pendingCmds = new Map();
  let lastEventsTs = Date.now();

  function buildLiveView() {
    fields.thermalImg = el('img', { id: 'therm-img', alt: 'Thermal preview' });
    fields.thermalEmpty = el('div', { class: 'panel-empty' }, 'no thermal frames yet');
    fields.thermalAge = el('span', { class: 'age' }, '—');
    fields.thermalMeta = el('div', { class: 'panel-meta' },
      el('span', { id: 'therm-fps' }, '—'),
      el('span', {}, '·'),
      fields.thermalAge);
    fields.thermalPanel = el('div', { class: 'panel thermal' },
      el('div', { class: 'panel-label thermal' }, 'Thermal'),
      fields.thermalMeta,
      fields.thermalEmpty);

    fields.visImg = el('img', { id: 'vis-img', alt: 'Visible preview' });
    fields.visEmpty = el('div', { class: 'panel-empty' }, 'no visible preview yet');
    fields.visAge = el('span', { class: 'age' }, '—');
    fields.visMeta = el('div', { class: 'panel-meta' },
      el('span', { id: 'vis-res' }, '—'),
      el('span', {}, '·'),
      fields.visAge);
    fields.visPanel = el('div', { class: 'panel visible' },
      el('div', { class: 'panel-label' }, 'Visible'),
      fields.visMeta,
      fields.visEmpty);

    const views = el('div', { class: 'views' }, fields.thermalPanel, fields.visPanel);

    fields.captureBtn = el('button', {
      class: 'btn primary',
      onclick: () => sendCmd('capture.now', {}, fields.captureBtn, 'Capture'),
    },
      el('span', { class: 'icon' }, '📷'),
      el('span', { class: 'btn-label' }, 'Capture'));

    fields.tlBtn = el('button', {
      class: 'btn',
      onclick: () => sendCmd('timelapse.start', { intervalSec: 30 }, fields.tlBtn, 'Start Timelapse'),
    },
      el('span', { class: 'icon' }, '▶'),
      el('span', { class: 'btn-label' }, 'Start Timelapse'));

    fields.ffcBtn = el('button', {
      class: 'btn sm',
      onclick: () => sendCmd('thermal.ffc', {}, fields.ffcBtn, 'Run FFC'),
    },
      el('span', {}, 'Run FFC'));

    fields.rebootBtn = el('button', {
      class: 'btn sm danger',
      onclick: () => {
        if (!confirm('Reboot the device?')) return;
        sendCmd('device.reboot', {}, fields.rebootBtn, 'Reboot');
      },
    },
      el('span', {}, 'Reboot'));

    const actions = el('div', { class: 'actions' }, fields.captureBtn, fields.tlBtn);
    const actionsSec = el('div', { class: 'actions-secondary' }, fields.ffcBtn, fields.rebootBtn);

    fields.statBlock = el('div', { class: 'stat-grid' });
    const tokenInput = el('input', {
      type: 'password',
      placeholder: 'relay token (Bearer auth for /cmd)',
      value: authToken === 'dev-token' ? '' : authToken,
      oninput: (e) => saveToken(e.target.value || 'dev-token'),
    });
    const tokenRow = el('div', { class: 'token-row' },
      el('label', {}, 'Token:'), tokenInput);

    const diagnostics = el('details', {},
      el('summary', {}, 'Diagnostics & device state'),
      el('div', { class: 'body' }, fields.statBlock, tokenRow));

    return el('div', {}, views, actions, actionsSec, diagnostics);
  }

  function updateLiveView(d, state) {
    const therm = state?.init?.thermal;
    const vis = state?.init?.visible;

    // Attach preview imgs once thermal/vis report ready.
    if (therm && therm.frames > 0 && !fields.thermalImg.parentElement) {
      fields.thermalPanel.appendChild(fields.thermalImg);
      fields.thermalEmpty.remove();
    }
    if (vis && vis.ready && !fields.visImg.parentElement) {
      fields.visPanel.appendChild(fields.visImg);
      fields.visEmpty.remove();
    }

    // Acquisition health: is the frame counter increasing?
    const now = Date.now();
    if (therm && therm.frames !== lastThermFrames) {
      lastThermFrames = therm.frames;
      lastThermFramesTs = now;
    }
    const acqStaleMs = now - lastThermFramesTs;
    const acqStale = lastThermFramesTs === 0 || acqStaleMs > 8000;

    // Preview-age display per panel.
    if (therm) {
      const fpsTxt = therm.fps != null && therm.fps > 0
        ? therm.fps.toFixed(1) + ' fps' : therm.frames + ' frames';
      const ageEl = fields.thermalMeta.querySelector('#therm-fps');
      if (ageEl) ageEl.textContent = fpsTxt;
    }
    if (vis) {
      const resEl = fields.visMeta.querySelector('#vis-res');
      if (resEl) resEl.textContent = (vis.w || '?') + '×' + (vis.h || '?');
    }
    setAge(fields.thermalAge, thermPreviewAgeMs);
    setAge(fields.visAge, visPreviewAgeMs);

    // Mark thermal panel stale if preview age > 5s OR acquisition stalled
    if (therm && therm.frames > 0) {
      const thermStale = (thermPreviewAgeMs != null && thermPreviewAgeMs > 5000) || acqStale;
      fields.thermalPanel.classList.toggle('stale', thermStale);
    }
    if (vis && vis.ready) {
      const visStale = visPreviewAgeMs != null && visPreviewAgeMs > 5000;
      fields.visPanel.classList.toggle('stale', visStale);
    }

    // Diagnostic stats — three-tier health
    const validRatio = therm && therm.totalPackets > 0
      ? therm.validPackets / therm.totalPackets : 0;
    fields.statBlock.replaceChildren(
      stat('Transport', d.online ? 'online' : 'offline', d.online ? 'ok' : 'err'),
      stat('Acquisition',
           lastThermFrames > 0
             ? (acqStale ? 'stalled (' + Math.round(acqStaleMs / 1000) + 's)' : 'live')
             : 'no frames yet',
           lastThermFrames > 0 && !acqStale ? 'ok' : (lastThermFrames > 0 ? 'warn' : 'err')),
      stat('Therm valid:total',
           therm ? fmtPct(therm.validPackets, therm.totalPackets) : '—',
           validRatio > 0.5 ? 'ok' : (validRatio > 0.2 ? 'warn' : 'err')),
      stat('Frames committed', therm ? therm.frames : '—', therm && therm.frames > 0 ? 'ok' : ''),
      stat('Device', d.deviceId, ''),
      stat('Firmware', d.fwVersion || '—', ''),
      stat('Wi-Fi', state?.init?.wifi?.ssid || '—', ''),
      stat('RSSI',
           state?.init?.wifi?.rssi != null ? state.init.wifi.rssi + ' dBm' : '—',
           state?.init?.wifi?.rssi > -65 ? 'ok' : (state?.init?.wifi?.rssi > -80 ? 'warn' : '')),
      stat('Free heap',
           state?.init?.freeHeap != null ? Math.round(state.init.freeHeap / 1024) + ' KB' : '—', ''),
      stat('Free PSRAM',
           state?.init?.freePsram != null ? Math.round(state.init.freePsram / 1048576) + ' MB' : '—', ''),
      stat('Last seen',
           d.lastSeenMs ? fmtAge(Date.now() - d.lastSeenMs) + ' ago' : '—', ''),
    );
  }

  function setAge(elNode, ageMs) {
    if (!elNode) return;
    elNode.textContent = ageMs == null ? '—' : fmtAge(ageMs) + ' ago';
    elNode.className = 'age' +
      (ageMs == null ? '' :
       ageMs > 5000 ? ' err' :
       ageMs > 2000 ? ' warn' : '');
  }

  function stat(k, v, kind) {
    return el('div', { class: 'stat' },
      el('div', { class: 'k' }, k),
      el('div', { class: 'v ' + (kind || '') }, String(v)));
  }

  function updateChips(d, state) {
    const chips = document.getElementById('chips');
    chips.innerHTML = '';

    // 1. Transport health
    const tChip = el('div', {
      class: d ? (d.online ? 'chip ok' : 'chip err') : 'chip err',
      title: 'Transport: WebSocket from device to relay',
    },
      el('span', { class: 'dot' }),
      d ? (d.online ? 'Live' : 'Offline') : 'No device');
    chips.appendChild(tChip);

    // 2. Acquisition health (only if device is online)
    if (d && d.online) {
      const acqStale = lastThermFramesTs === 0 || (Date.now() - lastThermFramesTs) > 8000;
      const acqOk = lastThermFrames > 0 && !acqStale;
      chips.appendChild(el('div', {
        class: acqOk ? 'chip ok' : (lastThermFrames > 0 ? 'chip warn' : 'chip err'),
        title: 'Acquisition: thermal frame counter increasing',
      }, acqOk ? 'Acquiring' : (lastThermFrames > 0 ? 'Stalled' : 'No frames')));
    }
  }

  // ───── Image refresh: preload-then-swap, capture relay-side age header ─────
  async function refreshImg(modality) {
    if (!currentDeviceId) return;
    const url = '/api/devices/' + encodeURIComponent(currentDeviceId) +
                '/last-frame.jpg?modality=' + modality + '&t=' + Date.now();
    try {
      const r = await fetch(url, { cache: 'no-store' });
      if (!r.ok) return;
      const ageHdr = r.headers.get('X-Frame-Age-Ms');
      const age = ageHdr ? Number(ageHdr) : null;
      if (modality === 'thermal') thermPreviewAgeMs = age;
      else visPreviewAgeMs = age;
      const blob = await r.blob();
      const next = URL.createObjectURL(blob);
      const img = modality === 'thermal' ? fields.thermalImg : fields.visImg;
      if (img && img.parentElement) {
        const old = img.src;
        img.src = next;
        // Revoke previous blob URL after new image loads (avoid leak).
        img.onload = () => { if (old.startsWith('blob:')) URL.revokeObjectURL(old); };
      } else {
        URL.revokeObjectURL(next);
      }
    } catch {}
  }

  function refreshImgs() {
    if (!currentDeviceId) return;
    refreshImg('thermal');
    refreshImg('vis');
  }

  // ───── Command round-trip with id/result matching ─────
  function newCmdId() {
    return 'c-' + Date.now().toString(36) + '-' +
           Math.random().toString(36).slice(2, 7);
  }

  async function sendCmd(cmdType, payload, button, originalText) {
    if (!currentDeviceId) {
      toast('No device connected', 'err');
      return;
    }
    const id = newCmdId();
    pendingCmds.set(id, {
      type: cmdType,
      sentAtMs: Date.now(),
      button,
      originalText,
    });
    setBtnPending(button, true);

    try {
      const r = await fetch(
        '/api/devices/' + encodeURIComponent(currentDeviceId) + '/cmd',
        {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json',
            'Authorization': 'Bearer ' + authToken,
          },
          body: JSON.stringify({ cmd: cmdType, id, ...payload }),
        }
      );
      if (!r.ok) {
        const err = await r.json().catch(() => ({}));
        const status = r.status;
        pendingCmds.delete(id);
        setBtnPending(button, false);
        const msg = status === 401 ? 'Unauthorized — set token in diagnostics' :
                    status === 404 ? 'Device not found' :
                    status === 409 ? 'Device offline' :
                    (err.error || ('HTTP ' + status));
        toast('Failed: ' + msg, 'err');
        return;
      }
      // Success means command was forwarded to device. Now wait for cmd.result.
      // 6s timeout — long enough for thermal.ffc (1.2s actual) + slack.
      setTimeout(() => {
        if (pendingCmds.has(id)) {
          pendingCmds.delete(id);
          setBtnPending(button, false);
          toast('No response from device for ' + cmdType, 'err');
        }
      }, 6000);
    } catch (e) {
      pendingCmds.delete(id);
      setBtnPending(button, false);
      toast('Network error: ' + (e?.message || e), 'err');
    }
  }

  function setBtnPending(button, pending) {
    if (!button) return;
    const lbl = button.querySelector('.btn-label');
    if (pending) {
      button.classList.add('pending');
      button.disabled = true;
      if (lbl) {
        if (!button.dataset.origLabel) button.dataset.origLabel = lbl.textContent;
        lbl.textContent = 'Working…';
      }
    } else {
      button.classList.remove('pending');
      button.disabled = false;
      if (lbl && button.dataset.origLabel) {
        lbl.textContent = button.dataset.origLabel;
      }
    }
  }

  async function pollEvents() {
    if (!currentDeviceId) return;
    try {
      const r = await fetch(
        '/api/devices/' + encodeURIComponent(currentDeviceId) +
        '/events?since=' + lastEventsTs,
        { cache: 'no-store' }
      );
      const j = await r.json();
      const events = j.events || [];
      for (const e of events) {
        if (e.ts > lastEventsTs) lastEventsTs = e.ts;
        if (e.kind === 'cmd.result' && e.id && pendingCmds.has(e.id)) {
          const pending = pendingCmds.get(e.id);
          pendingCmds.delete(e.id);
          setBtnPending(pending.button, false);
          toast(
            (e.ok ? '✓ ' : '✗ ') + pending.type + ': ' + (e.msg || ''),
            e.ok ? 'ok' : 'err'
          );
        }
      }
    } catch {}
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
        lastThermFrames = 0;
        lastThermFramesTs = 0;
        if (!content.querySelector('.empty')) {
          content.innerHTML = '';
          content.appendChild(el('div', { class: 'empty' },
            el('div', { class: 'pulse' }),
            el('h2', {}, 'Looking for your device…'),
            el('p', {}, 'Power on a Grasshopper unit and connect it to Wi-Fi. It should appear here within a few seconds.')
          ));
        }
        updateChips(null, null);
        return;
      }

      if (currentDeviceId !== d.deviceId) {
        currentDeviceId = d.deviceId;
        lastThermFrames = 0;
        lastThermFramesTs = 0;
        lastEventsTs = Date.now();
        const live = buildLiveView();
        content.innerHTML = '';
        content.appendChild(live);
      }

      const sr = await fetch('/api/devices/' + encodeURIComponent(d.deviceId) + '/state',
                              { cache: 'no-store' });
      const state = await sr.json();
      updateLiveView(d, state);
      updateChips(d, state);
    } catch (e) {
      console.warn('poll error', e);
    }
  }

  // One-time security check at boot: if the relay is publicly exposed
  // (Railway) AND still using the dev-token default, anyone with this
  // URL can issue commands. Surface a loud banner.
  async function checkSecurityPosture() {
    try {
      const r = await fetch('/health', { cache: 'no-store' });
      const j = await r.json();
      if (j.tokenIsDevDefault && j.publiclyExposed) {
        const slot = document.getElementById('banner-slot');
        if (slot && !slot.firstChild) {
          slot.appendChild(el('div', { class: 'insecure-banner' },
            el('strong', {}, '⚠ Insecure default token in use. '),
            'Anyone with this URL can issue commands (Capture, Reboot, FFC). ',
            'Set ',
            el('code', {}, 'RELAY_TOKEN'),
            ' to a unique secret in Railway env vars, then update the Token field in Diagnostics below.'
          ));
        }
      }
    } catch {}
  }
  checkSecurityPosture();

  poll();
  setInterval(poll, 2000);
  setInterval(refreshImgs, 250);
  setInterval(pollEvents, 800);
</script>
</body>
</html>`
