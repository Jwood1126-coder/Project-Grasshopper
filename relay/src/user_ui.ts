// Grasshopper user-facing UI — Phase 1-4.
//
// Self-contained HTML/CSS/JS, no framework. Served at GET /.
//
// Phase 4 changes:
//  - Active timelapse detection: when device reports timelapse.active,
//    swap the Start button for a red Stop button + show a session-active
//    banner with capture count + elapsed time.
//  - Timelapse settings modal: tap Start → choose interval (5s / 10s /
//    30s / 1 min / 5 min / 15 min) and toggle vis / thermal capture.
//  - Loosened staleness thresholds: thermal preview now updates every
//    750 ms on the device side; UI marks panels stale at 6 s (warn at 3 s).
//  - Better desktop / iPad layout: max-width 1600 px, larger panels,
//    actions inline beside the views on wide screens.

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
  .app {
    max-width: 1600px; margin: 0 auto;
    padding: 18px clamp(12px, 3vw, 32px);
    padding-bottom: env(safe-area-inset-bottom, 18px);
  }

  /* ─── Header ─── */
  header {
    display: flex; align-items: center; justify-content: space-between;
    padding: 8px 4px 22px; gap: 14px; flex-wrap: wrap;
  }
  .logo { font: 600 22px/1 -apple-system; letter-spacing: -0.01em; flex-shrink: 0; }
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

  /* ─── Active timelapse banner ─── */
  .tl-banner {
    background: linear-gradient(180deg, #1a3a2c 0%, #0e2418 100%);
    border: 1px solid #2a5a3c; border-radius: 12px;
    padding: 14px 18px; margin-bottom: 16px;
    display: flex; align-items: center; gap: 16px; flex-wrap: wrap;
  }
  .tl-banner .icon {
    width: 38px; height: 38px; flex-shrink: 0;
    border-radius: 50%; background: var(--ok);
    display: flex; align-items: center; justify-content: center;
    color: #001a08; font-size: 16px; font-weight: 700;
    animation: pulse 2s ease-in-out infinite;
  }
  .tl-banner .info { flex: 1; min-width: 200px; }
  .tl-banner .info .title { font-size: 14px; font-weight: 600; color: var(--ok); }
  .tl-banner .info .meta { font: 13px/1.4 ui-monospace, monospace; color: #a8d4b8; margin-top: 2px; }
  .tl-banner .stop-btn {
    background: var(--err); color: #1a0e10; border: none;
    border-radius: 8px; padding: 10px 18px; font: 600 14px -apple-system;
    cursor: pointer; transition: all 0.15s;
  }
  .tl-banner .stop-btn:hover:not(:disabled) { background: #ff8a8c; }
  .tl-banner .stop-btn:disabled { opacity: 0.5; cursor: not-allowed; }

  /* ─── View grid (mobile-first stack, desktop side-by-side) ─── */
  .views {
    display: grid; gap: 14px;
    grid-template-columns: 1fr;
    margin-bottom: 18px;
  }
  @media (min-width: 700px) {
    .views { grid-template-columns: 1fr 1fr; gap: 16px; }
  }

  .panel {
    background: var(--panel-bg); border: 1px solid var(--border); border-radius: 14px;
    overflow: hidden; position: relative; aspect-ratio: 4/3;
    display: flex; flex-direction: column;
    transition: opacity 0.4s;
  }
  .panel.stale img { opacity: 0.55; filter: saturate(0.5) brightness(0.85); }
  .panel-label {
    position: absolute; top: 12px; left: 14px; z-index: 2;
    font: 600 10px/1 ui-sans-serif; letter-spacing: 0.08em; text-transform: uppercase;
    color: var(--text); padding: 6px 10px; border-radius: 5px;
    background: rgba(10, 14, 20, 0.75); backdrop-filter: blur(8px);
  }
  .panel-label.thermal { color: var(--accent-warm); }
  .panel-meta {
    position: absolute; top: 12px; right: 14px; z-index: 2;
    font: 500 11px/1 ui-monospace, monospace; color: var(--muted);
    padding: 6px 10px; border-radius: 5px;
    background: rgba(10, 14, 20, 0.75); backdrop-filter: blur(8px);
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
    content: 'stale'; position: absolute; bottom: 12px; left: 14px;
    font: 600 10px/1 ui-sans-serif; letter-spacing: 0.08em; text-transform: uppercase;
    color: var(--warn); padding: 6px 10px; border-radius: 5px;
    background: rgba(26, 22, 14, 0.85); backdrop-filter: blur(8px);
  }

  /* ─── Action buttons ─── */
  .actions {
    display: grid; grid-template-columns: 1fr 1fr; gap: 10px;
    margin-bottom: 12px;
  }
  .actions-secondary {
    display: grid; grid-template-columns: 1fr 1fr; gap: 10px;
    margin-bottom: 22px;
  }
  @media (min-width: 1100px) {
    /* On wide screens, use a single 4-column row for primary + secondary actions */
    .actions, .actions-secondary { grid-template-columns: repeat(2, minmax(0, 1fr)); }
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
  .btn.sm { height: 42px; font-size: 13px; }
  .btn:hover:not(:disabled) { background: var(--surface-2); border-color: #3d4a5c; }
  .btn:active:not(:disabled) { transform: scale(0.98); }
  .btn:disabled { opacity: 0.4; cursor: not-allowed; }
  .btn.primary { background: var(--accent); color: #001620; border-color: var(--accent); }
  .btn.primary:hover:not(:disabled) { background: #6cc8e3; border-color: #6cc8e3; }
  .btn.danger { color: var(--err); border-color: #3a1f23; }
  .btn.danger:hover:not(:disabled) { background: #1a0e10; }
  .btn .icon { font-size: 18px; line-height: 1; }
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

  /* ─── Modal (timelapse settings) ─── */
  .modal-bg {
    position: fixed; inset: 0; z-index: 200;
    background: rgba(0, 0, 0, 0.6); backdrop-filter: blur(4px);
    display: none; align-items: flex-end; justify-content: center;
    padding: 0;
  }
  .modal-bg.show { display: flex; animation: fade-in 0.18s ease; }
  @keyframes fade-in { from { opacity: 0; } to { opacity: 1; } }
  @media (min-width: 600px) {
    .modal-bg { align-items: center; padding: 20px; }
  }
  .modal {
    background: var(--surface); border: 1px solid var(--border);
    border-top-left-radius: 16px; border-top-right-radius: 16px;
    width: 100%; max-width: 480px; padding: 20px 22px 22px;
    box-shadow: 0 -8px 40px rgba(0, 0, 0, 0.4);
    max-height: 92vh; overflow-y: auto;
  }
  @media (min-width: 600px) { .modal { border-radius: 16px; } }
  .modal h3 { font-size: 17px; font-weight: 600; margin-bottom: 14px; }
  .modal-row { margin-bottom: 16px; }
  .modal-row > label { display: block; font-size: 12px; color: var(--muted);
                        text-transform: uppercase; letter-spacing: 0.06em;
                        margin-bottom: 8px; }
  .interval-grid {
    display: grid; grid-template-columns: repeat(3, 1fr); gap: 6px;
  }
  .interval-grid .iv {
    background: var(--panel-bg); border: 1px solid var(--border);
    border-radius: 8px; padding: 12px 8px; font: 500 14px ui-monospace;
    cursor: pointer; text-align: center; transition: all 0.15s;
    color: var(--text);
  }
  .interval-grid .iv:hover { background: var(--surface-2); }
  .interval-grid .iv.active {
    background: var(--accent); color: #001620;
    border-color: var(--accent);
  }
  .toggle-row {
    display: flex; gap: 8px;
  }
  .toggle-row .tog {
    flex: 1; padding: 12px; background: var(--panel-bg);
    border: 1px solid var(--border); border-radius: 8px;
    cursor: pointer; transition: all 0.15s; text-align: center;
    color: var(--muted); font: 500 13px -apple-system;
    -webkit-user-select: none; user-select: none;
  }
  .toggle-row .tog.on {
    background: var(--surface-2); color: var(--text);
    border-color: #3d4a5c;
  }
  .toggle-row .tog.on .check { color: var(--ok); }
  .modal-actions {
    display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 20px;
  }

  /* Security banner */
  .insecure-banner {
    background: #3a1f23; border: 1px solid var(--err); border-radius: 8px;
    padding: 12px 14px; margin-bottom: 16px; color: #ffc7c9;
    font-size: 13px; line-height: 1.5;
  }
  .insecure-banner strong { color: var(--err); }
  .insecure-banner code {
    background: rgba(0,0,0,0.3); padding: 1px 6px; border-radius: 3px;
    font-size: 12px;
  }

  /* Tab bar */
  .tabs { display: flex; gap: 4px; margin-left: auto; }
  .tab {
    padding: 8px 16px; border-radius: 8px; cursor: pointer;
    font: 500 13px/1 system-ui; color: var(--muted);
    background: transparent; border: 1px solid transparent;
    transition: background 0.15s, color 0.15s;
  }
  .tab:hover { background: var(--surface-2); color: var(--text); }
  .tab.active {
    background: var(--surface-2); color: var(--text);
    border-color: var(--border);
  }

  /* Library view */
  .library-view { padding-top: 4px; }
  .library-header {
    display: flex; align-items: center; justify-content: space-between;
    margin-bottom: 16px;
  }
  .library-header h2 { margin: 0; font-size: 18px; font-weight: 600; }
  .library-header .meta { color: var(--muted); font-size: 13px; }
  .session-grid {
    display: grid; gap: 14px;
    grid-template-columns: repeat(auto-fill, minmax(220px, 1fr));
  }
  .session-card {
    background: var(--surface); border: 1px solid var(--border);
    border-radius: 10px; overflow: hidden; cursor: pointer;
    transition: transform 0.12s, border-color 0.12s;
    display: flex; flex-direction: column;
  }
  .session-card:hover { transform: translateY(-2px); border-color: var(--accent); }
  .session-card .thumb-wrap {
    aspect-ratio: 4/3; background: #0a0a0a; overflow: hidden;
    display: flex; align-items: center; justify-content: center;
  }
  .session-card .thumb {
    width: 100%; height: 100%; object-fit: cover;
  }
  .session-card .thumb-empty {
    color: var(--muted); font-size: 12px;
  }
  .session-card .info { padding: 10px 12px; }
  .session-card .title {
    font: 600 13px/1.3 ui-monospace, monospace;
    color: var(--text); margin-bottom: 4px;
    overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
  }
  .session-card .row { font: 12px/1.5 system-ui; color: var(--muted); }
  .session-card .badges { margin-top: 6px; display: flex; gap: 4px; flex-wrap: wrap; }
  .session-card .badge {
    font: 10px/1 system-ui; padding: 3px 6px; border-radius: 4px;
    background: var(--surface-2); color: var(--muted); text-transform: uppercase;
    letter-spacing: 0.05em;
  }
  .session-card .badge.tl { background: #1f3a28; color: var(--ok); }
  .session-card .badge.single { background: #2a2a3a; color: #88a; }
  .session-card .badge.incomplete { background: #3a1f23; color: var(--err); }

  .empty-library {
    text-align: center; padding: 60px 20px; color: var(--muted);
  }
  .empty-library h3 { color: var(--text); margin-bottom: 8px; }

  /* Session detail view */
  .detail-header {
    display: flex; align-items: center; gap: 12px; margin-bottom: 16px;
    flex-wrap: wrap;
  }
  .detail-header .back {
    background: var(--surface-2); border: 1px solid var(--border);
    color: var(--text); padding: 6px 12px; border-radius: 6px;
    cursor: pointer; font-size: 13px;
  }
  .detail-header .back:hover { border-color: var(--accent); }
  .detail-header h2 {
    margin: 0; font: 600 16px/1 ui-monospace, monospace;
  }
  .detail-meta {
    background: var(--surface); border: 1px solid var(--border);
    border-radius: 8px; padding: 12px 16px; margin-bottom: 16px;
    display: grid; gap: 8px 18px;
    grid-template-columns: repeat(auto-fit, minmax(140px, 1fr));
  }
  .detail-meta .item { font-size: 13px; }
  .detail-meta .item .k { color: var(--muted); font-size: 11px; text-transform: uppercase; letter-spacing: 0.05em; }
  .detail-meta .item .v { color: var(--text); }
  .capture-grid {
    display: grid; gap: 8px;
    grid-template-columns: repeat(auto-fill, minmax(140px, 1fr));
  }
  .capture-tile {
    aspect-ratio: 4/3; background: #0a0a0a; cursor: pointer;
    border-radius: 6px; overflow: hidden; position: relative;
    border: 1px solid var(--border);
  }
  .capture-tile img { width: 100%; height: 100%; object-fit: cover; }
  .capture-tile .seq {
    position: absolute; top: 4px; left: 4px;
    background: rgba(0,0,0,0.7); color: white;
    font: 11px/1 ui-monospace, monospace; padding: 2px 5px; border-radius: 3px;
  }
  .capture-tile:hover { border-color: var(--accent); }

  /* Lightbox */
  .lightbox {
    position: fixed; inset: 0; background: rgba(0,0,0,0.95);
    z-index: 200; display: none; align-items: center; justify-content: center;
    flex-direction: column;
  }
  .lightbox.show { display: flex; }
  .lightbox img {
    max-width: 95vw; max-height: 80vh; object-fit: contain;
    border-radius: 6px;
  }
  .lightbox .lb-info {
    color: var(--muted); font: 13px/1.4 system-ui;
    margin-top: 12px; text-align: center;
  }
  .lightbox .lb-info .seq { color: var(--text); font-weight: 600; }
  .lightbox .lb-close {
    position: absolute; top: 16px; right: 24px;
    background: transparent; border: none; color: white;
    font-size: 28px; cursor: pointer; line-height: 1; padding: 4px 12px;
  }
  .lightbox .lb-nav {
    position: absolute; top: 50%; transform: translateY(-50%);
    background: rgba(255,255,255,0.08); border: none; color: white;
    font-size: 32px; cursor: pointer; padding: 12px 18px; border-radius: 8px;
  }
  .lightbox .lb-nav:hover { background: rgba(255,255,255,0.16); }
  .lightbox .lb-prev { left: 16px; }
  .lightbox .lb-next { right: 16px; }
  .lightbox .lb-nav:disabled { opacity: 0.3; cursor: default; }

  /* Toast */
  .toast {
    position: fixed; bottom: 24px; left: 50%; transform: translateX(-50%);
    background: var(--surface-2); border: 1px solid var(--border);
    color: var(--text); padding: 12px 18px; border-radius: 10px;
    font-size: 14px; box-shadow: 0 10px 30px rgba(0,0,0,0.4);
    opacity: 0; pointer-events: none; transition: opacity 0.2s;
    max-width: 90%; z-index: 100;
  }
  .toast.show { opacity: 1; }
  .toast.ok { border-color: #1f3a28; color: var(--ok); }
  .toast.err { border-color: #3a1f23; color: var(--err); }
</style>
</head>
<body>
<div class="app">
  <header>
    <div class="logo">🦗 <span class="accent">Grasshopper</span></div>
    <div class="chips" id="chips"></div>
    <div class="tabs" id="tabs">
      <div class="tab active" data-view="live">Live View</div>
      <div class="tab" data-view="library">Library</div>
    </div>
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

<!-- Timelapse settings modal -->
<div class="modal-bg" id="tl-modal">
  <div class="modal">
    <h3>Start Timelapse</h3>
    <div class="modal-row">
      <label>Interval</label>
      <div class="interval-grid" id="iv-grid"></div>
    </div>
    <div class="modal-row">
      <label>Capture</label>
      <div class="toggle-row">
        <div class="tog on" id="tog-vis"><span class="check">●</span> Visible</div>
        <div class="tog on" id="tog-therm"><span class="check">●</span> Thermal</div>
      </div>
    </div>
    <div class="modal-actions">
      <button class="btn" onclick="closeTlModal()">Cancel</button>
      <button class="btn primary" id="tl-confirm">Start</button>
    </div>
  </div>
</div>

<!-- Lightbox overlay -->
<div class="lightbox" id="lightbox">
  <button class="lb-close" id="lb-close" aria-label="Close">×</button>
  <button class="lb-nav lb-prev" id="lb-prev" aria-label="Previous">‹</button>
  <img id="lb-img" alt="" />
  <button class="lb-nav lb-next" id="lb-next" aria-label="Next">›</button>
  <div class="lb-info" id="lb-info"></div>
</div>

<div class="toast" id="toast"></div>

<script>
  // ───── Auth token ─────
  function loadToken() {
    const m = location.hash.match(/(?:^|[#&])token=([^&]+)/);
    if (m) {
      const t = decodeURIComponent(m[1]);
      try { localStorage.setItem('gh_token', t); } catch {}
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
  const fmtElapsed = (ms) => {
    const s = Math.floor(ms / 1000);
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    const sec = s % 60;
    if (h > 0) return h + 'h ' + m + 'm ' + sec + 's';
    if (m > 0) return m + 'm ' + sec + 's';
    return sec + 's';
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

  // ───── Per-device state (built once, mutated on poll) ─────
  let currentDeviceId = null;
  const fields = {};
  let lastThermFrames = 0;
  let lastThermFramesTs = 0;
  let thermPreviewAgeMs = null;
  let visPreviewAgeMs = null;
  const pendingCmds = new Map();
  let lastEventsTs = Date.now();

  // ───── Timelapse settings modal ─────
  const TL_INTERVALS = [
    { label: '5s',   sec: 5   },
    { label: '10s',  sec: 10  },
    { label: '30s',  sec: 30  },
    { label: '1 min', sec: 60  },
    { label: '5 min', sec: 300 },
    { label: '15 min',sec: 900 },
  ];
  let tlSelectedInterval = 30;
  let tlCaptureVis = true;
  let tlCaptureTherm = true;

  function buildIvGrid() {
    const grid = document.getElementById('iv-grid');
    grid.innerHTML = '';
    TL_INTERVALS.forEach(opt => {
      const e = el('div', {
        class: 'iv' + (opt.sec === tlSelectedInterval ? ' active' : ''),
        onclick: () => {
          tlSelectedInterval = opt.sec;
          buildIvGrid();
        },
      }, opt.label);
      grid.appendChild(e);
    });
  }

  function setupModalToggles() {
    const v = document.getElementById('tog-vis');
    const t = document.getElementById('tog-therm');
    v.classList.toggle('on', tlCaptureVis);
    t.classList.toggle('on', tlCaptureTherm);
    v.onclick = () => { tlCaptureVis = !tlCaptureVis; v.classList.toggle('on', tlCaptureVis); };
    t.onclick = () => { tlCaptureTherm = !tlCaptureTherm; t.classList.toggle('on', tlCaptureTherm); };
  }

  function openTlModal() {
    buildIvGrid();
    setupModalToggles();
    document.getElementById('tl-modal').classList.add('show');
  }
  window.closeTlModal = () => {
    document.getElementById('tl-modal').classList.remove('show');
  };
  document.getElementById('tl-confirm').onclick = () => {
    if (!tlCaptureVis && !tlCaptureTherm) {
      toast('Enable at least one of vis / thermal', 'err');
      return;
    }
    closeTlModal();
    sendCmd('timelapse.start',
      { intervalSec: tlSelectedInterval, captureVis: tlCaptureVis, captureTherm: tlCaptureTherm },
      null, 'Start Timelapse');
  };

  // ───── Live view DOM ─────
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
      onclick: openTlModal,
    },
      el('span', { class: 'icon' }, '▶'),
      el('span', { class: 'btn-label' }, 'Start Timelapse…'));

    fields.ffcBtn = el('button', {
      class: 'btn sm',
      onclick: () => sendCmd('thermal.ffc', {}, fields.ffcBtn, 'Run FFC'),
    },
      el('span', { class: 'btn-label' }, 'Run FFC'));

    fields.rebootBtn = el('button', {
      class: 'btn sm danger',
      onclick: () => {
        if (!confirm('Reboot the device?')) return;
        sendCmd('device.reboot', {}, fields.rebootBtn, 'Reboot');
      },
    },
      el('span', { class: 'btn-label' }, 'Reboot'));

    fields.actions = el('div', { class: 'actions' }, fields.captureBtn, fields.tlBtn);
    fields.actionsSec = el('div', { class: 'actions-secondary' }, fields.ffcBtn, fields.rebootBtn);
    fields.tlBannerSlot = el('div', { id: 'tl-banner-slot' });

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

    return el('div', {}, fields.tlBannerSlot, views, fields.actions, fields.actionsSec, diagnostics);
  }

  // ───── Live view update ─────
  function updateLiveView(d, state) {
    // Prefer tick (live) over init (boot snapshot). init.thermal.frames freezes
    // at startup; tick.thermal.frames keeps moving. Same for timelapse.active —
    // it never updates without this.
    const live = state?.tick ?? state?.init ?? {};
    const therm = live.thermal;
    const vis = live.visible;
    const tl = live.timelapse;

    if (therm && therm.frames > 0 && !fields.thermalImg.parentElement) {
      fields.thermalPanel.appendChild(fields.thermalImg);
      fields.thermalEmpty.remove();
    }
    if (vis && vis.ready && !fields.visImg.parentElement) {
      fields.visPanel.appendChild(fields.visImg);
      fields.visEmpty.remove();
    }

    const now = Date.now();
    if (therm && therm.frames !== lastThermFrames) {
      lastThermFrames = therm.frames;
      lastThermFramesTs = now;
    }
    const acqStaleMs = now - lastThermFramesTs;
    const acqStale = lastThermFramesTs === 0 || acqStaleMs > 10000;

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

    // Loosened: thermal preview updates every ~750 ms on the device,
    // mark stale only at >6 s (warn at 3 s on the per-panel age label).
    if (therm && therm.frames > 0) {
      const thermStale = (thermPreviewAgeMs != null && thermPreviewAgeMs > 6000) || acqStale;
      fields.thermalPanel.classList.toggle('stale', thermStale);
    }
    if (vis && vis.ready) {
      const visStale = visPreviewAgeMs != null && visPreviewAgeMs > 6000;
      fields.visPanel.classList.toggle('stale', visStale);
    }

    // Active timelapse banner + Start/Stop button toggle
    updateTlBanner(tl);

    // Diagnostics
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
      stat('Timelapse', tl?.active ? 'active' : 'idle', tl?.active ? 'ok' : ''),
      stat('Device', d.deviceId, ''),
      stat('Firmware', d.fwVersion || '—', ''),
      stat('Wi-Fi', live.wifi?.ssid || '—', ''),
      stat('RSSI',
           live.wifi?.rssi != null ? live.wifi.rssi + ' dBm' : '—',
           live.wifi?.rssi > -65 ? 'ok' : (live.wifi?.rssi > -80 ? 'warn' : '')),
      stat('Free heap',
           live.freeHeap != null ? Math.round(live.freeHeap / 1024) + ' KB' : '—', ''),
      stat('Free PSRAM',
           live.freePsram != null ? Math.round(live.freePsram / 1048576) + ' MB' : '—', ''),
      stat('Last seen',
           d.lastSeenMs ? fmtAge(Date.now() - d.lastSeenMs) + ' ago' : '—', ''),
    );
  }

  function updateTlBanner(tl) {
    const slot = fields.tlBannerSlot;
    if (!slot) return;
    const active = !!(tl && tl.active);

    // Toggle Start button label/visibility.
    if (fields.tlBtn) {
      const lbl = fields.tlBtn.querySelector('.btn-label');
      if (lbl) lbl.textContent = active ? 'Timelapse running…' : 'Start Timelapse…';
      fields.tlBtn.disabled = active;
      fields.tlBtn.style.opacity = active ? '0.55' : '';
    }

    if (!active) {
      slot.replaceChildren();
      return;
    }

    const elapsed = Date.now() - (tl.startedMs || Date.now());
    const stopBtn = el('button', {
      class: 'stop-btn',
      id: 'tl-stop',
      onclick: () => {
        sendCmd('timelapse.stop', {}, stopBtn, 'Stop');
      },
    }, '■  Stop');

    const banner = el('div', { class: 'tl-banner' },
      el('div', { class: 'icon' }, '●'),
      el('div', { class: 'info' },
        el('div', { class: 'title' },
          'Timelapse running · ' + tl.sessionId),
        el('div', { class: 'meta' },
          'every ' + tl.intervalSec + 's · ' +
          tl.captureCount + ' captures · ' +
          fmtElapsed(elapsed) +
          (tl.captureVis && tl.captureTherm ? ' · vis+therm' :
           tl.captureVis ? ' · vis only' : ' · therm only'))
      ),
      stopBtn);
    slot.replaceChildren(banner);
  }

  function setAge(elNode, ageMs) {
    if (!elNode) return;
    elNode.textContent = ageMs == null ? '—' : fmtAge(ageMs) + ' ago';
    elNode.className = 'age' +
      (ageMs == null ? '' :
       ageMs > 6000 ? ' err' :
       ageMs > 3000 ? ' warn' : '');
  }

  function stat(k, v, kind) {
    return el('div', { class: 'stat' },
      el('div', { class: 'k' }, k),
      el('div', { class: 'v ' + (kind || '') }, String(v)));
  }

  function updateChips(d, state) {
    const chips = document.getElementById('chips');
    chips.innerHTML = '';
    const tChip = el('div', {
      class: d ? (d.online ? 'chip ok' : 'chip err') : 'chip err',
      title: 'Transport: WebSocket from device to relay',
    },
      el('span', { class: 'dot' }),
      d ? (d.online ? 'Live' : 'Offline') : 'No device');
    chips.appendChild(tChip);

    if (d && d.online) {
      const acqStale = lastThermFramesTs === 0 || (Date.now() - lastThermFramesTs) > 10000;
      const acqOk = lastThermFrames > 0 && !acqStale;
      chips.appendChild(el('div', {
        class: acqOk ? 'chip ok' : (lastThermFrames > 0 ? 'chip warn' : 'chip err'),
        title: 'Acquisition: thermal frame counter increasing',
      }, acqOk ? 'Acquiring' : (lastThermFrames > 0 ? 'Stalled' : 'No frames')));
    }
  }

  // ───── Image refresh ─────
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
        img.onload = () => { if (old.startsWith('blob:')) URL.revokeObjectURL(old); };
      } else {
        URL.revokeObjectURL(next);
      }
    } catch {}
  }

  function refreshImgs() {
    if (!currentDeviceId) return;
    if (currentView !== 'live') return;
    refreshImg('thermal');
    refreshImg('vis');
  }

  // ───── Commands with id/result ─────
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
    if (button) setBtnPending(button, true);

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
        if (button) setBtnPending(button, false);
        const msg = status === 401 ? 'Unauthorized — set token in diagnostics' :
                    status === 404 ? 'Device not found' :
                    status === 409 ? 'Device offline' :
                    (err.error || ('HTTP ' + status));
        toast('Failed: ' + msg, 'err');
        return;
      }
      // Success → wait for cmd.result. Timeout 8s (covers thermal.ffc + slack).
      setTimeout(() => {
        if (pendingCmds.has(id)) {
          pendingCmds.delete(id);
          if (button) setBtnPending(button, false);
          toast('No response from device for ' + cmdType, 'err');
        }
      }, 8000);
    } catch (e) {
      pendingCmds.delete(id);
      if (button) setBtnPending(button, false);
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
        delete button.dataset.origLabel;
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
          if (pending.button) setBtnPending(pending.button, false);
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
        if (currentView === 'live' && !content.querySelector('.empty')) {
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

      // Only rebuild Live View when actually showing it. Otherwise just
      // update tracking state so chips stay correct on other tabs.
      if (currentView !== 'live') {
        if (currentDeviceId !== d.deviceId) currentDeviceId = d.deviceId;
        // Still want chips updated even on Library/Detail.
        const sr = await fetch('/api/devices/' + encodeURIComponent(d.deviceId) + '/state',
                                { cache: 'no-store' });
        const state = await sr.json();
        // Track frame counter for Acquisition chip without touching DOM.
        const live = state?.tick ?? state?.init;
        if (live?.thermal && live.thermal.frames !== lastThermFrames) {
          lastThermFrames = live.thermal.frames;
          lastThermFramesTs = Date.now();
        }
        updateChips(d, state);
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

  // ───── Library + detail views ─────
  //
  // currentView controls what poll() renders into #content. The Live View
  // path is unchanged. Library view doesn't need 250 ms refresh — it loads
  // once on entry and on explicit refresh. Detail view loads once on open.
  let currentView = 'live';        // 'live' | 'library' | 'detail'
  let currentSessionId = null;     // when in detail view
  let detailCaptureList = [];      // for lightbox prev/next nav
  let lightboxIdx = -1;
  // Track blob URLs created for authenticated thumbnail/lightbox loads
  // so we can revoke them when the user leaves the view.
  let activeBlobUrls = [];

  function revokeBlobUrls() {
    for (const u of activeBlobUrls) {
      try { URL.revokeObjectURL(u); } catch {}
    }
    activeBlobUrls = [];
  }

  // Load an image with Authorization header and assign the resulting
  // blob URL to <img>. Returns the blob URL (also tracked for cleanup).
  // Falls back to onerror handler if the fetch fails.
  async function loadAuthImg(img, url, onErr) {
    try {
      const r = await fetch(url, {
        headers: { 'Authorization': 'Bearer ' + authToken },
        cache: 'no-store',
      });
      if (!r.ok) { if (onErr) onErr(r.status); return null; }
      const blob = await r.blob();
      const u = URL.createObjectURL(blob);
      activeBlobUrls.push(u);
      img.src = u;
      return u;
    } catch (e) {
      if (onErr) onErr(0);
      return null;
    }
  }

  function setView(view, sid) {
    revokeBlobUrls();
    currentView = view;
    currentSessionId = sid || null;
    document.querySelectorAll('.tab').forEach((t) => {
      t.classList.toggle('active',
        view === 'detail' ? t.dataset.view === 'library'
                          : t.dataset.view === view);
    });
    const content = document.getElementById('content');
    content.innerHTML = '';
    if (view === 'live') {
      // Force re-render of live view on next poll() tick.
      currentDeviceId = null;
      poll();
    } else if (view === 'library') {
      content.appendChild(buildLibraryView());
      loadLibrary();
    } else if (view === 'detail') {
      content.appendChild(buildDetailView(sid));
      loadDetail(sid);
    }
  }

  function buildLibraryView() {
    const v = el('div', { class: 'library-view' });
    const header = el('div', { class: 'library-header' },
      el('h2', {}, 'Sessions'),
      el('div', { class: 'meta', id: 'lib-meta' }, 'Loading…'));
    const grid = el('div', { class: 'session-grid', id: 'session-grid' });
    v.appendChild(header);
    v.appendChild(grid);
    return v;
  }

  async function loadLibrary() {
    const meta = document.getElementById('lib-meta');
    const grid = document.getElementById('session-grid');
    if (!grid) return;
    if (!currentDeviceId) {
      meta.textContent = 'Waiting for device…';
      return;
    }
    try {
      const r = await fetch(
        '/api/devices/' + encodeURIComponent(currentDeviceId) + '/sessions',
        { headers: { 'Authorization': 'Bearer ' + authToken }, cache: 'no-store' }
      );
      if (!r.ok) {
        meta.textContent = 'Failed: HTTP ' + r.status;
        return;
      }
      const j = await r.json();
      const list = j.sessions || [];
      meta.textContent = (j.total ?? list.length) + ' sessions' +
                         (j.truncated ? ' (showing newest ' + j.listed + ')' : '');
      if (list.length === 0) {
        grid.replaceChildren(el('div', { class: 'empty-library' },
          el('h3', {}, 'No sessions yet'),
          el('div', {},
            'Press Capture or Start Timelapse on the Live View to record one.')));
        return;
      }
      grid.replaceChildren(...list.map(renderSessionCard));
    } catch (e) {
      meta.textContent = 'Network error';
    }
  }

  function renderSessionCard(s) {
    const sid = s.sessionId || 'session_?';
    const thumbUrl = currentDeviceId
      ? '/api/devices/' + encodeURIComponent(currentDeviceId) +
        '/sessions/' + encodeURIComponent(sid) + '/file/000001_vis.jpg'
      : '';
    const isTl = s.mode === 'timelapse';
    const incomplete = isTl && s.complete === false;
    const dur = (s.durationSec != null && s.durationSec > 0)
      ? fmtElapsed(s.durationSec * 1000) : '—';
    const ts = s.timestamp ? new Date(s.timestamp * 1000) : null;
    const tsTxt = ts ? ts.toLocaleString() : '';

    const card = el('div', {
      class: 'session-card',
      onclick: () => setView('detail', sid),
    });
    const thumbWrap = el('div', { class: 'thumb-wrap' });
    if (thumbUrl) {
      const img = el('img', { class: 'thumb' });
      thumbWrap.appendChild(img);
      // Auth-fetch the thumbnail. Browser <img loading=lazy> can't help
      // here (no Authorization header), so fire fetch immediately. Cards
      // render fast either way since the relay caches.
      loadAuthImg(img, thumbUrl, () => {
        thumbWrap.replaceChildren(el('div', { class: 'thumb-empty' }, 'no preview'));
      });
    } else {
      thumbWrap.appendChild(el('div', { class: 'thumb-empty' }, 'no preview'));
    }
    const info = el('div', { class: 'info' },
      el('div', { class: 'title' }, sid),
      el('div', { class: 'row' }, (s.captureCount ?? '?') + ' captures · ' +
        (isTl ? ('every ' + (s.intervalSec ?? '?') + 's · ' + dur) : 'single')),
      tsTxt && el('div', { class: 'row' }, tsTxt),
      el('div', { class: 'badges' },
        el('span', { class: 'badge ' + (isTl ? 'tl' : 'single') },
           isTl ? 'timelapse' : 'single'),
        s.captureVis && s.captureTherm
          ? el('span', { class: 'badge' }, 'vis+therm')
          : (s.captureVis ? el('span', { class: 'badge' }, 'vis')
                          : el('span', { class: 'badge' }, 'therm')),
        incomplete && el('span', { class: 'badge incomplete' }, 'incomplete')));
    card.appendChild(thumbWrap);
    card.appendChild(info);
    return card;
  }

  function buildDetailView(sid) {
    const v = el('div', {});
    const header = el('div', { class: 'detail-header' },
      el('button', { class: 'back', onclick: () => setView('library') }, '← Library'),
      el('h2', {}, sid));
    const meta = el('div', { class: 'detail-meta', id: 'detail-meta' });
    const grid = el('div', { class: 'capture-grid', id: 'capture-grid' });
    grid.appendChild(el('div', { class: 'empty-library' }, 'Loading…'));
    v.appendChild(header);
    v.appendChild(meta);
    v.appendChild(grid);
    return v;
  }

  async function loadDetail(sid) {
    const meta = document.getElementById('detail-meta');
    const grid = document.getElementById('capture-grid');
    if (!grid || !currentDeviceId) return;
    try {
      const r = await fetch(
        '/api/devices/' + encodeURIComponent(currentDeviceId) +
        '/sessions/' + encodeURIComponent(sid),
        { headers: { 'Authorization': 'Bearer ' + authToken }, cache: 'no-store' }
      );
      if (!r.ok) {
        grid.replaceChildren(el('div', { class: 'empty-library' },
          'Failed: HTTP ' + r.status));
        return;
      }
      const j = await r.json();
      const m = j.meta || {};
      meta.replaceChildren(
        metaItem('Mode',      m.mode || '?'),
        metaItem('Captures',  String(j.captureCount ?? '?') +
                              (j.truncated ? ' (truncated)' : '')),
        m.intervalSec > 0 ? metaItem('Interval', m.intervalSec + 's') : null,
        m.durationSec > 0 ? metaItem('Duration', fmtElapsed(m.durationSec * 1000)) : null,
        metaItem('Started',   m.timestamp
                              ? new Date(m.timestamp * 1000).toLocaleString()
                              : '?'),
        metaItem('Complete',  m.complete === false ? 'no' : 'yes'),
      );
      const captures = j.captures || [];
      detailCaptureList = captures.map((c) => ({
        seq: c.seq,
        sessionId: sid,
        visFile: pad6(c.seq) + '_vis.jpg',
        thermFile: pad6(c.seq) + '_therm.jpg',
        visOk: c.visOk, thermOk: c.thermOk,
        sessionMs: c.sessionMs, timestamp: c.timestamp,
      }));
      if (detailCaptureList.length === 0) {
        grid.replaceChildren(el('div', { class: 'empty-library' },
          'No captures recorded.'));
        return;
      }
      grid.replaceChildren(...detailCaptureList.map((c, idx) => {
        const tile = el('div', { class: 'capture-tile',
          onclick: () => openLightbox(idx) });
        if (c.visOk) {
          const img = el('img', {});
          tile.appendChild(img);
          loadAuthImg(img,
            '/api/devices/' + encodeURIComponent(currentDeviceId) +
            '/sessions/' + encodeURIComponent(sid) +
            '/file/' + c.visFile);
        } else {
          tile.appendChild(el('div', { class: 'thumb-empty',
            style: 'display:flex;align-items:center;justify-content:center;height:100%;color:var(--muted);font-size:11px' },
            'no vis'));
        }
        tile.appendChild(el('div', { class: 'seq' }, '#' + c.seq));
        return tile;
      }));
    } catch (e) {
      grid.replaceChildren(el('div', { class: 'empty-library' }, 'Network error'));
    }
  }

  function pad6(n) { return String(n).padStart(6, '0'); }
  function metaItem(k, v) {
    if (v == null) return null;
    return el('div', { class: 'item' },
      el('div', { class: 'k' }, k),
      el('div', { class: 'v' }, String(v)));
  }

  // ───── Lightbox ─────
  function openLightbox(idx) {
    if (idx < 0 || idx >= detailCaptureList.length) return;
    lightboxIdx = idx;
    const lb = document.getElementById('lightbox');
    lb.classList.add('show');
    renderLightbox();
  }
  function closeLightbox() {
    document.getElementById('lightbox').classList.remove('show');
    lightboxIdx = -1;
  }
  function navLightbox(delta) {
    const next = lightboxIdx + delta;
    if (next < 0 || next >= detailCaptureList.length) return;
    lightboxIdx = next;
    renderLightbox();
  }
  function renderLightbox() {
    const c = detailCaptureList[lightboxIdx];
    if (!c) return;
    const img = document.getElementById('lb-img');
    img.removeAttribute('src');
    loadAuthImg(img,
      '/api/devices/' + encodeURIComponent(currentDeviceId) +
      '/sessions/' + encodeURIComponent(c.sessionId) +
      '/file/' + c.visFile);
    const info = document.getElementById('lb-info');
    info.replaceChildren(
      el('span', { class: 'seq' }, '#' + c.seq + ' / ' + detailCaptureList.length),
      el('span', {}, ' · ' + c.visFile),
      c.timestamp ? el('span', {}, ' · ' + new Date(c.timestamp * 1000).toLocaleString()) : null,
    );
    document.getElementById('lb-prev').disabled = lightboxIdx === 0;
    document.getElementById('lb-next').disabled = lightboxIdx === detailCaptureList.length - 1;
  }
  document.getElementById('lb-close').onclick = closeLightbox;
  document.getElementById('lb-prev').onclick = () => navLightbox(-1);
  document.getElementById('lb-next').onclick = () => navLightbox(1);
  document.addEventListener('keydown', (e) => {
    const lb = document.getElementById('lightbox');
    if (!lb.classList.contains('show')) return;
    if (e.key === 'Escape') closeLightbox();
    else if (e.key === 'ArrowLeft') navLightbox(-1);
    else if (e.key === 'ArrowRight') navLightbox(1);
  });

  // Tab clicks
  document.querySelectorAll('#tabs .tab').forEach((t) => {
    t.onclick = () => setView(t.dataset.view);
  });

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
