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

  /* ─── View grid: always two columns so both feeds fit on-screen
     without scrolling on tablet, desktop, AND phone (Fox parity).
     Panel height is capped to the viewport minus the chrome that sits
     above + below the views, so the row never pushes content off
     the visible area. ─── */
  .views {
    display: grid; gap: clamp(6px, 1.5vw, 16px);
    grid-template-columns: 1fr 1fr;
    margin-bottom: 14px;
  }

  /* Each modality is wrapped: header (label + stats above the image),
     panel (image + interactive overlays), footer (legend etc). The
     image stays clean — no text or controls on top of it by default.
     Header and footer can be collapsed by toggling .compact on the wrap. */
  .panel-wrap {
    display: flex; flex-direction: column; gap: 6px;
    min-width: 0;
  }
  .panel-bar {
    display: flex; align-items: center; justify-content: space-between;
    gap: 8px; padding: 0 4px;
    font: 12px/1.2 ui-sans-serif; color: var(--muted);
  }
  .panel-bar .ptitle {
    font: 600 11px/1 ui-sans-serif; letter-spacing: 0.08em;
    text-transform: uppercase; color: var(--text);
  }
  .panel-bar.thermal .ptitle { color: var(--accent-warm); }
  .panel-bar .pmeta {
    font: 500 11px/1.4 ui-monospace, monospace;
    overflow: hidden; text-overflow: ellipsis;
  }
  .panel-bar .pbtns { display: flex; gap: 4px; align-items: center; }
  .panel-bar .pbtn {
    background: var(--surface); border: 1px solid var(--border);
    color: var(--text); min-width: 32px; height: 32px; padding: 0 8px; border-radius: 6px;
    display: flex; align-items: center; justify-content: center;
    cursor: pointer; font: 600 14px/1 system-ui;
  }
  .panel-bar .pbtn:hover { border-color: var(--accent); }
  .panel-bar .pbtn:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
  .panel-bar .pbtn.active { background: var(--accent); color: #fff; border-color: var(--accent); }
  /* Compact mode hides the bars, leaving just the image — useful for
     fullscreen / pure-image viewing. A small corner toggle stays
     visible so the user can always get back to full chrome. */
  .panel-wrap.compact .panel-bar,
  .panel-wrap.compact .panel-legend { display: none; }
  .panel-wrap .corner-toggle {
    position: absolute; top: 6px; right: 6px; z-index: 6;
    background: rgba(10,14,20,0.78); backdrop-filter: blur(6px);
    color: white; border: 1px solid var(--border); border-radius: 5px;
    width: 28px; height: 28px;
    display: none; align-items: center; justify-content: center;
    cursor: pointer; font: 600 13px/1 system-ui;
  }
  .panel-wrap.compact .corner-toggle { display: flex; }
  .panel-wrap { position: relative; }   /* anchor for corner toggle */

  .panel {
    background: var(--panel-bg); border: 1px solid var(--border); border-radius: 14px;
    overflow: hidden; position: relative; aspect-ratio: 4/3;
    /* Cap so side-by-side panels never overflow the viewport. The 380 px
       reservation covers header + chips + bars + actions + diagnostics. */
    max-height: calc(100vh - 380px);
    min-height: 160px;
    margin: 0 auto;
    width: 100%;
    display: flex; flex-direction: column;
    transition: opacity 0.4s;
  }
  /* Fullscreen takes the panel out of the grid and fills the viewport. */
  .panel-wrap.fullscreen {
    position: fixed; inset: 0; z-index: 300;
    background: #000;
    display: flex; flex-direction: column;
    padding: 12px;
  }
  .panel-wrap.fullscreen .panel {
    flex: 1; max-height: none; aspect-ratio: auto;
    border: none; border-radius: 8px;
  }
  .panel.stale img { opacity: 0.55; filter: saturate(0.5) brightness(0.85); }
  /* Old in-image overlays — only hidden when used as direct children
     of .panel (legacy overlay position). The same fields are reused
     inside the new .panel-bar (.pmeta > .panel-meta), where they
     should stay visible. */
  .panel > .panel-label, .panel > .panel-meta { display: none; }
  /* Inside the bar, .panel-meta is just a flex row of spans. */
  .panel-bar .pmeta .panel-meta {
    display: flex; gap: 6px; align-items: center; padding: 0;
    background: none; backdrop-filter: none; position: static;
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

  /* Per-panel orientation controls (bottom-right overlay) */
  .panel-ctrls {
    position: absolute; bottom: 10px; right: 10px; z-index: 3;
    display: flex; gap: 4px;
  }
  .panel-ctrls .pc {
    background: rgba(10, 14, 20, 0.78); backdrop-filter: blur(8px);
    border: 1px solid var(--border); color: var(--text);
    width: 32px; height: 32px; border-radius: 6px;
    display: flex; align-items: center; justify-content: center;
    cursor: pointer; font: 600 13px/1 system-ui;
  }
  .panel-ctrls .pc:hover { border-color: var(--accent); }
  .panel-ctrls .pc.active {
    background: var(--accent); color: #fff; border-color: var(--accent);
  }
  .panel-ctrls .pc.rotate { font-size: 18px; line-height: 1; }
  /* Firmware does the actual rotation/flip in the JPEG it sends, so
     no CSS transform here — that would double-rotate. We just track
     orientation classes so the panel can adapt its aspect-ratio when
     either modality goes portrait (90° / 270°). */
  .panel.portrait { aspect-ratio: 3/4; }

  /* Zoom transforms applied via inline style; CSS just makes the img
     respect the parent's clip and stay performant during transforms. */
  .panel img, .lightbox img { transform-origin: 0 0; will-change: transform; }
  .panel.zoomed { cursor: grab; }
  .panel.zoomed.dragging { cursor: grabbing; }
  .lightbox.zoomed img { cursor: grab; }
  .lightbox.zoomed.dragging img { cursor: grabbing; }
  .zoom-badge {
    position: absolute; bottom: 10px; left: 10px; z-index: 5;
    background: rgba(10,14,20,0.78); backdrop-filter: blur(6px);
    color: #fff; font: 600 11px/1 ui-monospace, monospace;
    padding: 4px 8px; border-radius: 4px; border: 1px solid var(--border);
    pointer-events: none;
  }

  /* Thermal crosshair + per-pixel readout (Phase 3 radiometric) */
  .panel.thermal .therm-readout {
    position: absolute; pointer-events: none;
    inset: 0; z-index: 4;
  }
  .panel.thermal .therm-cursor {
    position: absolute; width: 14px; height: 14px;
    margin-left: -7px; margin-top: -7px;
    border: 2px solid #fff; border-radius: 50%;
    box-shadow: 0 0 0 1px rgba(0,0,0,0.6);
    pointer-events: none; transition: opacity 0.15s;
  }
  .panel.thermal .therm-cursor.hidden { opacity: 0; }
  .panel.thermal .therm-tip {
    position: absolute; transform: translate(-50%, -130%);
    background: rgba(10, 14, 20, 0.85); backdrop-filter: blur(6px);
    color: #fff; font: 600 12px/1.2 ui-monospace, monospace;
    padding: 4px 8px; border-radius: 4px; border: 1px solid var(--border);
    pointer-events: none; white-space: nowrap;
  }
  /* When the cursor is near the top of the panel, flip the tooltip
     to sit BELOW the cursor so it doesn't get clipped by the panel
     edge. JS toggles .below based on pointer Y. */
  .panel.thermal .therm-tip.below { transform: translate(-50%, 130%); }

  /* Temperature legend bar (under each thermal panel) */
  .panel-legend {
    margin-top: -6px; margin-bottom: 12px;
    display: flex; align-items: center; gap: 8px;
    font: 11px/1 ui-monospace, monospace; color: var(--muted);
  }
  .panel-legend .lbar {
    flex: 1; height: 12px; border-radius: 3px;
    background: linear-gradient(to right,
      #000000 0%, #500082 25%, #dc1e3c 50%, #ffc828 75%, #ffffff 100%);
    border: 1px solid var(--border);
  }
  .panel-legend .lmin, .panel-legend .lmax { min-width: 56px; text-align: center; color: var(--text); }
  .panel-legend .lmax { text-align: right; }

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
    gap: 12px; margin-bottom: 16px; flex-wrap: wrap;
  }
  .library-header h2 { margin: 0; font-size: 18px; font-weight: 600; }
  .library-header .meta { color: var(--muted); font-size: 13px; }
  .lib-toolbar {
    display: flex; gap: 16px; align-items: center; margin-bottom: 14px;
    flex-wrap: wrap; padding: 10px 12px;
    background: var(--surface); border: 1px solid var(--border); border-radius: 8px;
  }
  .lib-toolbar .group { display: flex; gap: 4px; align-items: center; }
  .lib-toolbar .group-label {
    font: 600 11px/1 system-ui; color: var(--muted);
    text-transform: uppercase; letter-spacing: 0.05em; margin-right: 6px;
  }
  .lib-toolbar .pill {
    background: var(--surface-2); border: 1px solid var(--border);
    color: var(--text); padding: 6px 12px; border-radius: 6px;
    cursor: pointer; font: 500 12px/1 system-ui;
  }
  .lib-toolbar .pill:hover { border-color: var(--accent); }
  .lib-toolbar .pill.active { background: var(--accent); color: #fff; border-color: var(--accent); }
  .lib-toolbar select {
    background: var(--surface-2); border: 1px solid var(--border);
    color: var(--text); padding: 6px 10px; border-radius: 6px;
    font: 500 12px/1 system-ui; cursor: pointer;
  }
  .session-grid {
    display: grid; gap: 14px;
    grid-template-columns: repeat(auto-fill, minmax(220px, 1fr));  /* default: medium */
  }
  .session-grid.size-small  { grid-template-columns: repeat(auto-fill, minmax(140px, 1fr)); gap: 10px; }
  .session-grid.size-large  { grid-template-columns: repeat(auto-fill, minmax(320px, 1fr)); gap: 18px; }
  .session-grid.list { grid-template-columns: 1fr; gap: 6px; }
  .session-grid.list .session-card {
    flex-direction: row; align-items: stretch;
  }
  .session-grid.list .session-card .thumb-wrap {
    width: 120px; height: 90px; aspect-ratio: auto; flex-shrink: 0;
  }
  .session-grid.list .session-card.both .thumb-wrap { width: 220px; }
  .session-grid.list .session-card .info { flex: 1; }

  /* Card modality variants */
  .session-card .thumb-wrap.both { display: flex; gap: 0; }
  .session-card .thumb-wrap.both .thumb { width: 50%; }
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
  .detail-header .back, .detail-header .dh-btn {
    background: var(--surface-2); border: 1px solid var(--border);
    color: var(--text); padding: 6px 12px; border-radius: 6px;
    cursor: pointer; font-size: 13px;
  }
  .detail-header .back:hover, .detail-header .dh-btn:hover { border-color: var(--accent); }
  .detail-header .dh-btn.danger { color: var(--err); }
  .detail-header .dh-btn.danger:hover { border-color: var(--err); background: rgba(255, 80, 80, 0.06); }
  .detail-header .dh-spacer { flex: 1; }
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
  .capture-tile .dl {
    position: absolute; top: 4px; right: 4px;
    background: rgba(10,14,20,0.78); backdrop-filter: blur(6px);
    color: white; border: 1px solid var(--border); border-radius: 4px;
    width: 24px; height: 24px;
    display: flex; align-items: center; justify-content: center;
    cursor: pointer; font: 600 12px/1 system-ui;
    opacity: 0; transition: opacity 0.15s;
  }
  .capture-tile:hover .dl { opacity: 1; }
  .capture-tile .dl:hover { border-color: var(--accent); }

  /* Download menu — small popover anchored to a button */
  .dl-menu {
    position: absolute; z-index: 350;
    background: var(--surface); border: 1px solid var(--border);
    border-radius: 6px; padding: 4px;
    display: flex; flex-direction: column; gap: 2px;
    box-shadow: 0 8px 24px rgba(0,0,0,0.6);
    font: 13px/1 system-ui; min-width: 180px;
  }
  .dl-menu .dl-item {
    background: transparent; border: none; color: var(--text);
    text-align: left; padding: 8px 12px; border-radius: 4px;
    cursor: pointer;
  }
  .dl-menu .dl-item:hover { background: var(--surface-2); }
  .dl-menu .dl-item:disabled { opacity: 0.4; cursor: not-allowed; }
  .dl-menu .dl-item .dl-sub { color: var(--muted); font-size: 11px; display: block; margin-top: 2px; }
  /* Lightbox download button */
  .lightbox .lb-dl {
    position: absolute; top: 16px; right: 72px;
    background: rgba(255,255,255,0.08); border: 1px solid transparent;
    color: white; padding: 6px 14px; border-radius: 6px;
    cursor: pointer; font: 600 12px/1 system-ui;
    text-transform: uppercase; letter-spacing: 0.05em;
  }
  .lightbox .lb-dl:hover { background: rgba(255,255,255,0.16); border-color: var(--border); }

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
  /* Vis / Thermal modality toggle in the lightbox */
  .lb-modality {
    position: absolute; top: 16px; left: 24px;
    display: flex; gap: 4px;
  }
  .lb-modality .mb {
    background: rgba(255,255,255,0.08); border: 1px solid transparent;
    color: white; padding: 6px 14px; border-radius: 6px;
    cursor: pointer; font: 600 12px/1 system-ui;
    text-transform: uppercase; letter-spacing: 0.05em;
  }
  .lb-modality .mb:hover { background: rgba(255,255,255,0.16); }
  .lb-modality .mb.active.vis   { background: #1a3a5a; border-color: var(--accent); }
  .lb-modality .mb.active.therm { background: #5a1a1a; border-color: var(--err); }
  .lb-modality .mb:disabled { opacity: 0.35; cursor: not-allowed; }

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
    <div class="modal-row">
      <label>Stop after</label>
      <div class="interval-grid" id="dur-grid"></div>
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
  <button class="lb-dl"    id="lb-dl"    aria-label="Download">⬇ Download</button>
  <div class="lb-modality" id="lb-modality">
    <button class="mb vis active" id="lb-vis" data-mode="vis">Visible</button>
    <button class="mb therm"      id="lb-therm" data-mode="therm">Thermal</button>
  </div>
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

  // Temperature unit (°F default per user preference). Persists.
  let tempUnit = (() => {
    try { return localStorage.getItem('gh_temp_unit') || 'F'; }
    catch { return 'F'; }
  })();
  let lastRadiometric = null;

  // Latest thermal raw frame fetched alongside the JPEG. Cached so the
  // hover handler can compute temps without a per-mouse-move fetch.
  // tempScaleX100: 1 means raw counts ARE centi-Kelvin; 10 means deci-K.
  let thermRaw = null;       // Uint16Array of length W*H, native (pre-rotation)
  let thermRawW = 0, thermRawH = 0, thermRawTs = 0;
  let tempScaleX100 = 1;

  function fToC(f) { return (f - 32) * 5 / 9; }
  function fmtTemp(tF) {
    if (tF == null || !isFinite(tF)) return '—';
    if (tempUnit === 'C') return fToC(tF).toFixed(1) + '°C';
    return tF.toFixed(1) + '°F';
  }
  function renderThermTempLabel(rad) {
    if (!fields.thermalTempLabel) return;
    if (!rad || !rad.haveTemps) {
      fields.thermalTempLabel.textContent = rad && rad.active === false
        ? 'radiometric off' : '—';
      if (fields.legendMin) fields.legendMin.textContent = '—';
      if (fields.legendMax) fields.legendMax.textContent = '—';
      return;
    }
    fields.thermalTempLabel.textContent =
      'min ' + fmtTemp(rad.minTempF) +
      '  ctr ' + fmtTemp(rad.centerTempF) +
      '  max ' + fmtTemp(rad.maxTempF);
    // Refresh legend bar labels.
    if (fields.legendMin) fields.legendMin.textContent = fmtTemp(rad.minTempF);
    if (fields.legendMax) fields.legendMax.textContent = fmtTemp(rad.maxTempF);
    // Cache scale factor for crosshair temp lookups.
    tempScaleX100 = (rad.tlinearResolution === 1) ? 1 : 10;
  }

  // Convert a single raw count → °F using the cached scale.
  function rawToF(raw) {
    const cK = raw * tempScaleX100;
    const C  = cK / 100 - 273.15;
    return C * 9 / 5 + 32;
  }

  // Periodic fetch of the latest thermal raw16 frame. Caches to
  // thermRaw / thermRawW / thermRawH so the hover handler is purely
  // synchronous (no per-mouse-move fetch). Only fires while the
  // thermal panel is visible (live view) and we haven't already got
  // a fresh frame for this preview cycle.
  async function refreshThermRaw() {
    if (currentView !== 'live' || !currentDeviceId) return;
    try {
      const r = await fetch(
        '/api/devices/' + encodeURIComponent(currentDeviceId) + '/last-thermal.raw16',
        { cache: 'no-store' }
      );
      if (!r.ok) return;
      const w = Number(r.headers.get('X-Frame-Width'))  || 160;
      const h = Number(r.headers.get('X-Frame-Height')) || 120;
      const buf = await r.arrayBuffer();
      if (buf.byteLength !== w * h * 2) return;
      thermRaw = new Uint16Array(buf);
      thermRawW = w; thermRawH = h; thermRawTs = Date.now();
    } catch {}
  }

  // Map a panel-relative pointer position to a raw-frame pixel index.
  // Honors object-fit:contain (letterboxing) and the current thermal
  // rotation so the cursor lands on the correct source pixel.
  function pointerToRawIdx(panel, ev) {
    if (!thermRaw) return null;
    const rect = panel.getBoundingClientRect();
    const px = ev.clientX - rect.left;
    const py = ev.clientY - rect.top;
    if (px < 0 || py < 0 || px > rect.width || py > rect.height) return null;

    // The displayed image is the firmware-rotated JPEG. After rotation
    // 1 or 3 the displayed dims are H × W (portrait). Compute fitted
    // size + offsets for object-fit: contain, then map back to raw.
    const r = currentSettings.thermRotation & 3;
    const dispW = (r & 1) ? thermRawH : thermRawW;
    const dispH = (r & 1) ? thermRawW : thermRawH;
    const scale = Math.min(rect.width / dispW, rect.height / dispH);
    const fittedW = dispW * scale;
    const fittedH = dispH * scale;
    const offX = (rect.width  - fittedW) / 2;
    const offY = (rect.height - fittedH) / 2;
    if (px < offX || px > offX + fittedW ||
        py < offY || py > offY + fittedH) return null;
    // Pixel coord in the displayed (rotated) image.
    const dx = Math.floor((px - offX) / scale);
    const dy = Math.floor((py - offY) / scale);
    // Inverse-rotate to native raw-frame coords.
    let rx, ry;
    switch (r) {
      case 0: rx = dx;                    ry = dy;                    break;
      case 1: rx = dy;                    ry = thermRawH - 1 - dx;    break;
      case 2: rx = thermRawW - 1 - dx;    ry = thermRawH - 1 - dy;    break;
      case 3: rx = thermRawW - 1 - dy;    ry = dx;                    break;
    }
    if (rx < 0 || ry < 0 || rx >= thermRawW || ry >= thermRawH) return null;
    return { idx: ry * thermRawW + rx, panelX: px, panelY: py };
  }

  // ───── Zoom + pan helper ─────
  //
  // Attaches mouse wheel zoom (anchored at cursor), drag-pan when
  // zoomed, and double-click reset to a container element + image.
  // State is stored on the container element via .__zoom for live
  // panels (img.src changes don't reset zoom — transform stays).
  function attachZoom(container, img, opts) {
    opts = opts || {};
    const minScale = opts.minScale ?? 1;
    const maxScale = opts.maxScale ?? 8;
    const onZoomChange = opts.onZoomChange || (() => {});
    const state = container.__zoom = { scale: 1, tx: 0, ty: 0 };

    function apply() {
      img.style.transform =
        'translate(' + state.tx + 'px,' + state.ty + 'px) scale(' + state.scale + ')';
      container.classList.toggle('zoomed', state.scale > 1.001);
      onZoomChange(state.scale);
    }
    function clamp() {
      // At scale ≈ 1 just snap to the centered baseline.
      if (state.scale <= 1.001) { state.tx = 0; state.ty = 0; return; }
      // Compute the un-transformed (object-fit:contain) layout, then
      // clamp tx/ty so the image edges can't pull past the container
      // edges after translate(tx,ty) scale(s).
      const rect  = container.getBoundingClientRect();
      const w     = img.naturalWidth  || img.offsetWidth  || rect.width;
      const h     = img.naturalHeight || img.offsetHeight || rect.height;
      const fit   = Math.min(rect.width / w, rect.height / h);
      const dispW = w * fit, dispH = h * fit;
      const baseX = (rect.width  - dispW) / 2;   // contain offset
      const baseY = (rect.height - dispH) / 2;
      // Image rect after transform:  left = tx + baseX*s,
      // right = tx + (baseX+dispW)*s.  Constraints: left ≤ 0,
      // right ≥ rect.width.
      const leftBound   = rect.width  - (baseX + dispW) * state.scale;
      const rightBound  = -baseX * state.scale;
      const topBound    = rect.height - (baseY + dispH) * state.scale;
      const bottomBound = -baseY * state.scale;
      if (state.tx > rightBound)  state.tx = rightBound;
      if (state.tx < leftBound)   state.tx = leftBound;
      if (state.ty > bottomBound) state.ty = bottomBound;
      if (state.ty < topBound)    state.ty = topBound;
    }

    function zoomAt(deltaY, cx, cy) {
      const rect = container.getBoundingClientRect();
      const px = cx - rect.left;
      const py = cy - rect.top;
      const ix = (px - state.tx) / state.scale;
      const iy = (py - state.ty) / state.scale;
      const factor = deltaY < 0 ? 1.18 : 1 / 1.18;
      const next = Math.max(minScale, Math.min(maxScale, state.scale * factor));
      if (next === state.scale) return;
      state.scale = next;
      state.tx = px - ix * state.scale;
      state.ty = py - iy * state.scale;
      clamp(); apply();
    }
    function reset() {
      state.scale = 1; state.tx = 0; state.ty = 0;
      apply();
    }

    container.addEventListener('wheel', (e) => {
      // Only zoom when ctrl/meta or always — for image panels, wheel
      // alone is fine; on pages without scroll context this is the
      // expected behavior. preventDefault stops page scroll.
      e.preventDefault();
      zoomAt(e.deltaY, e.clientX, e.clientY);
    }, { passive: false });

    let drag = null;
    container.addEventListener('mousedown', (e) => {
      if (state.scale <= 1.001) return;
      drag = { x: e.clientX, y: e.clientY, tx: state.tx, ty: state.ty };
      container.classList.add('dragging');
    });
    window.addEventListener('mousemove', (e) => {
      if (!drag) return;
      state.tx = drag.tx + (e.clientX - drag.x);
      state.ty = drag.ty + (e.clientY - drag.y);
      clamp(); apply();
    });
    window.addEventListener('mouseup', () => {
      if (drag) { drag = null; container.classList.remove('dragging'); }
    });
    container.addEventListener('dblclick', reset);

    container.__zoomReset = reset;
    apply();
  }

  // Per-panel control buttons + view-mode toggles.
  function mkFsButton(onClick) {
    return el('button', { class: 'pbtn', title: 'Fullscreen', 'aria-label': 'Fullscreen',
                           onclick: onClick }, '⛶');
  }
  function mkInfoButton(onClick) {
    return el('button', { class: 'pbtn', title: 'Hide chrome (clean image)',
                           'aria-label': 'Toggle chrome', onclick: onClick }, '⊟');
  }

  // Compact mode: hide the panel-bar above and the legend below so the
  // image stands alone. Useful in fullscreen where chrome is noise.
  // A corner toggle button (always visible in compact mode) restores
  // the chrome — otherwise the user could lose the controls forever.
  function toggleCompact(which) {
    const wrap = which === 'therm' ? fields.thermWrap : fields.visWrap;
    if (!wrap) return;
    wrap.classList.toggle('compact');
  }

  // Fullscreen via the browser's Fullscreen API. Works on desktop and
  // most mobile. The wrap element gets .fullscreen for our own CSS too,
  // because requestFullscreen styling alone leaves the rest of the
  // page visible behind it on some browsers.
  function toggleFullscreen(which) {
    const wrap = which === 'therm' ? fields.thermWrap : fields.visWrap;
    if (!wrap) return;
    const inFs = !!document.fullscreenElement;
    if (inFs) {
      document.exitFullscreen?.();
      return;
    }
    wrap.classList.add('fullscreen');
    const exit = () => {
      if (!document.fullscreenElement) {
        wrap.classList.remove('fullscreen');
        document.removeEventListener('fullscreenchange', exit);
      }
    };
    document.addEventListener('fullscreenchange', exit);
    (wrap.requestFullscreen?.() ?? Promise.reject('no fs api'))
      .catch(() => {
        // Some browsers (Safari iOS) don't support element fullscreen.
        // Fall back to our CSS-only fullscreen + Esc-to-exit.
        const onKey = (e) => {
          if (e.key === 'Escape') {
            wrap.classList.remove('fullscreen');
            document.removeEventListener('keydown', onKey);
          }
        };
        document.addEventListener('keydown', onKey);
      });
  }

  function setupThermPanelHover(panel) {
    const showCursor = (panelX, panelY, tF) => {
      fields.thermCursor.style.left = panelX + 'px';
      fields.thermCursor.style.top  = panelY + 'px';
      fields.thermCursor.classList.remove('hidden');
      fields.thermTip.style.display = '';
      fields.thermTip.style.left = panelX + 'px';
      fields.thermTip.style.top  = panelY + 'px';
      // Flip tooltip below cursor when near the top edge so it doesn't
      // get clipped by the panel boundary.
      fields.thermTip.classList.toggle('below', panelY < 40);
      fields.thermTip.textContent = fmtTemp(tF);
    };
    const hide = () => {
      fields.thermCursor.classList.add('hidden');
      fields.thermTip.style.display = 'none';
    };
    panel.addEventListener('mousemove', (e) => {
      // Hide the temp readout while the panel is zoomed/panned —
      // pointerToRawIdx assumes the unzoomed object-fit:contain
      // layout, so the lookup would be wrong for zoomed views.
      if (panel.__zoom && panel.__zoom.scale > 1.001) { hide(); return; }
      const hit = pointerToRawIdx(panel, e);
      if (!hit || !thermRaw) { hide(); return; }
      const raw = thermRaw[hit.idx];
      if (raw === 0) { hide(); return; }
      showCursor(hit.panelX, hit.panelY, rawToF(raw));
    });
    panel.addEventListener('mouseleave', hide);
    // Touch: tap-to-show, single-tap pin (auto-clears after 2 s).
    panel.addEventListener('touchstart', (e) => {
      const t = e.touches[0]; if (!t) return;
      const hit = pointerToRawIdx(panel, t);
      if (!hit || !thermRaw) return;
      const raw = thermRaw[hit.idx];
      if (raw === 0) return;
      showCursor(hit.panelX, hit.panelY, rawToF(raw));
      setTimeout(hide, 2000);
    }, { passive: true });
  }

  // Current device-side orientation. Updated from tick.settings.
  // Both modalities use the same rotation model: 0/1/2/3 → 0/90/180/270.
  let currentSettings = { visRotation: 0, thermRotation: 0 };

  function applyPanelOrientationCss() {
    const tp = fields.thermalPanel, vp = fields.visPanel;
    // Both panels swap to portrait aspect at 90/270 since the firmware
    // serves a JPEG with swapped dimensions in those modes.
    if (tp) {
      const r = currentSettings.thermRotation & 3;
      tp.classList.toggle('portrait', r === 1 || r === 3);
    }
    if (vp) {
      const r = currentSettings.visRotation & 3;
      vp.classList.toggle('portrait', r === 1 || r === 3);
    }
    if (fields.thermRotBtn) {
      fields.thermRotBtn.classList.toggle('active', currentSettings.thermRotation !== 0);
      fields.thermRotBtn.title = 'Rotate thermal — currently ' +
        (currentSettings.thermRotation * 90) + '°';
    }
    if (fields.visRotBtn) {
      fields.visRotBtn.classList.toggle('active', currentSettings.visRotation !== 0);
      fields.visRotBtn.title = 'Rotate visible — currently ' +
        (currentSettings.visRotation * 90) + '°';
    }
  }

  function applySettingPreview(patch) {
    Object.assign(currentSettings, patch);
    applyPanelOrientationCss();
  }

  async function sendSettings(patch) {
    if (!currentDeviceId) { toast('No device connected', 'err'); return; }
    try {
      await fetch('/api/devices/' + encodeURIComponent(currentDeviceId) + '/cmd', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          'Authorization': 'Bearer ' + authToken,
        },
        body: JSON.stringify({ cmd: 'settings.update', id: 'set-' + Date.now(), ...patch }),
      });
    } catch (e) { toast('Settings save failed: ' + (e?.message || e), 'err'); }
  }

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
  // Counter so brief device drops don't immediately blank the UI.
  // poll() bumps this when devices is empty; flips to "searching"
  // empty state only when sustained for many polls.
  let noDeviceTicks = 0;

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
  // 0 = run until manually stopped. Sent as maxDurationSec to the
  // firmware; the timelapse task self-stops when elapsed >= this.
  const TL_DURATIONS = [
    { label: 'No limit', sec: 0 },
    { label: '1 min',    sec: 60 },
    { label: '5 min',    sec: 300 },
    { label: '15 min',   sec: 900 },
    { label: '30 min',   sec: 1800 },
    { label: '1 hr',     sec: 3600 },
    { label: '2 hr',     sec: 7200 },
  ];
  let tlSelectedDuration = 0;

  function buildDurGrid() {
    const grid = document.getElementById('dur-grid');
    if (!grid) return;
    grid.innerHTML = '';
    TL_DURATIONS.forEach(opt => {
      const e = el('div', {
        class: 'iv' + (opt.sec === tlSelectedDuration ? ' active' : ''),
        onclick: () => { tlSelectedDuration = opt.sec; buildDurGrid(); },
      }, opt.label);
      grid.appendChild(e);
    });
  }

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
    buildDurGrid();
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
      { intervalSec: tlSelectedInterval, captureVis: tlCaptureVis, captureTherm: tlCaptureTherm,
        maxDurationSec: tlSelectedDuration },
      null, 'Start Timelapse');
  };

  // ───── Live view DOM ─────
  function buildLiveView() {
    fields.thermalImg = el('img', { id: 'therm-img', alt: 'Thermal preview' });
    fields.thermalEmpty = el('div', { class: 'panel-empty' }, 'no thermal frames yet');
    fields.thermalAge = el('span', { class: 'age' }, '—');
    fields.thermalTempLabel = el('span', { id: 'therm-temp', title: 'Click to toggle °F / °C' }, '');
    fields.thermalTempLabel.style.cursor = 'pointer';
    fields.thermalTempLabel.onclick = () => {
      tempUnit = (tempUnit === 'F') ? 'C' : 'F';
      try { localStorage.setItem('gh_temp_unit', tempUnit); } catch {}
      // Force a redraw on the next tick — done via the cached state.
      if (lastRadiometric) renderThermTempLabel(lastRadiometric);
    };
    fields.thermalMeta = el('div', { class: 'panel-meta' },
      el('span', { id: 'therm-fps' }, '—'),
      el('span', {}, '·'),
      fields.thermalTempLabel,
      fields.thermalAge);
    // Thermal rotate button: cycles 0→90→180→270→0 CW. Settings live
    // on the device (NVS-backed) so live preview AND recordings rotate
    // together. Label shows current degree value.
    fields.thermRotBtn = el('button', { class: 'pc rotate', title: 'Rotate thermal (cycles 0/90/180/270)',
      onclick: () => {
        const next = (currentSettings.thermRotation + 1) & 3;
        applySettingPreview({ thermRotation: next });
        sendSettings({ thermRotation: next });
      } }, '⟲');
    // Per-pixel temperature readout overlay (in-image — only thing
    // that needs to live ON the JPEG so it can follow the cursor).
    fields.thermCursor = el('div', { class: 'therm-cursor hidden' });
    fields.thermTip    = el('div', { class: 'therm-tip',  style: 'display:none' });
    const thermReadout = el('div', { class: 'therm-readout' },
      fields.thermCursor, fields.thermTip);
    fields.thermalPanel = el('div', { class: 'panel thermal' },
      thermReadout, fields.thermalEmpty);
    setupThermPanelHover(fields.thermalPanel);

    // Header bar above the image: title + meta + rotate/fullscreen/info btns.
    fields.thermFs   = mkFsButton(() => toggleFullscreen('therm'));
    fields.thermInfo = mkInfoButton(() => toggleCompact('therm'));
    const thermBar = el('div', { class: 'panel-bar thermal' },
      el('span', { class: 'ptitle' }, 'Thermal'),
      el('div', { class: 'pmeta' }, fields.thermalMeta),
      el('div', { class: 'pbtns' }, fields.thermRotBtn, fields.thermInfo, fields.thermFs));

    fields.legendMin = el('span', { class: 'lmin' }, '—');
    fields.legendMax = el('span', { class: 'lmax' }, '—');
    fields.legend = el('div', { class: 'panel-legend' },
      fields.legendMin, el('span', { class: 'lbar' }), fields.legendMax);

    const thermCornerToggle = el('button', { class: 'corner-toggle',
      title: 'Show chrome', 'aria-label': 'Show chrome',
      onclick: () => toggleCompact('therm') }, '⊞');
    fields.thermWrap = el('div', { class: 'panel-wrap therm-wrap' },
      thermBar, fields.thermalPanel, fields.legend, thermCornerToggle);

    fields.visImg = el('img', { id: 'vis-img', alt: 'Visible preview' });
    fields.visEmpty = el('div', { class: 'panel-empty' }, 'no visible preview yet');
    fields.visAge = el('span', { class: 'age' }, '—');
    fields.visMeta = el('div', { class: 'panel-meta' },
      el('span', { id: 'vis-res' }, '—'),
      el('span', {}, '·'),
      fields.visAge);
    fields.visRotBtn = el('button', { class: 'pbtn', title: 'Rotate visible (cycles 0/90/180/270)',
      onclick: () => {
        const next = (currentSettings.visRotation + 1) & 3;
        applySettingPreview({ visRotation: next });
        sendSettings({ visRotation: next });
      } }, '⟲');
    fields.visFs   = mkFsButton(() => toggleFullscreen('vis'));
    fields.visInfo = mkInfoButton(() => toggleCompact('vis'));
    fields.visPanel = el('div', { class: 'panel visible' }, fields.visEmpty);
    const visBar = el('div', { class: 'panel-bar' },
      el('span', { class: 'ptitle' }, 'Visible'),
      el('div', { class: 'pmeta' }, fields.visMeta),
      el('div', { class: 'pbtns' }, fields.visRotBtn, fields.visInfo, fields.visFs));
    const visCornerToggle = el('button', { class: 'corner-toggle',
      title: 'Show chrome', 'aria-label': 'Show chrome',
      onclick: () => toggleCompact('vis') }, '⊞');
    fields.visWrap = el('div', { class: 'panel-wrap vis-wrap' },
      visBar, fields.visPanel, visCornerToggle);

    // Update the thermal rotate button styling to match the new pbtn class.
    fields.thermRotBtn.className = 'pbtn';

    // Attach zoom to each Live View panel. Wheel-zoom around cursor,
    // drag to pan when zoomed, double-click to reset. Setup deferred
    // until images are appended (handled in updateLiveView).
    const ensureZoom = () => {
      if (!fields.thermalPanel.__zoom && fields.thermalImg.parentElement) {
        attachZoom(fields.thermalPanel, fields.thermalImg);
      }
      if (!fields.visPanel.__zoom && fields.visImg.parentElement) {
        attachZoom(fields.visPanel, fields.visImg);
      }
    };
    fields._ensureZoom = ensureZoom;

    const views = el('div', { class: 'views' }, fields.thermWrap, fields.visWrap);

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

    // (Legend lives inside the thermal wrap now; no longer rendered separately here.)
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

    // Sync orientation from device. Server is source of truth; user
    // taps just optimistically update the local CSS while the cmd
    // round-trips. Compares before assign so we don't churn DOM.
    const s = live.settings;
    if (s && ((s.visRotation | 0) !== currentSettings.visRotation ||
              (s.thermRotation | 0) !== currentSettings.thermRotation)) {
      currentSettings.visRotation   = (s.visRotation | 0) & 3;
      currentSettings.thermRotation = (s.thermRotation | 0) & 3;
      applyPanelOrientationCss();
    }

    // Radiometric temps. Cached so the °F/°C toggle handler can re-
    // render without waiting for the next tick.
    lastRadiometric = live.radiometric || null;
    renderThermTempLabel(lastRadiometric);

    if (therm && therm.frames > 0 && !fields.thermalImg.parentElement) {
      // Insert image as the first child so the .therm-readout overlay
      // renders on top of it.
      fields.thermalPanel.insertBefore(fields.thermalImg, fields.thermalPanel.firstChild);
      fields.thermalEmpty.remove();
    }
    if (vis && vis.ready && !fields.visImg.parentElement) {
      fields.visPanel.appendChild(fields.visImg);
      fields.visEmpty.remove();
    }
    // Now that images live inside their panels, wire the zoom helper.
    fields._ensureZoom?.();

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
    // Pull the raw thermal frame for crosshair temp lookups. Same
    // cadence as JPEG refresh so the per-pixel readout stays roughly
    // in sync with what the user sees.
    refreshThermRaw();
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
        // Don't blank a working live view the instant the device drops.
        // Wait until we've seen no device for ~10 polls (~20 s at 2 s
        // poll cadence) before flipping to the "searching" empty state.
        // This rides through brief WS reconnect cycles without
        // visible UI churn.
        noDeviceTicks = (noDeviceTicks || 0) + 1;
        if (noDeviceTicks >= 10) {
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
        }
        updateChips(null, null);
        return;
      }
      noDeviceTicks = 0;

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

  // Auth-fetch a file and trigger a browser download with the given
  // filename. Used for capture downloads (vis JPEG, therm JPEG,
  // raw16, sidecar JSON). Cleans up the temp blob URL.
  async function authDownload(url, filename) {
    try {
      const r = await fetch(url, {
        headers: { 'Authorization': 'Bearer ' + authToken },
        cache: 'no-store',
      });
      if (!r.ok) {
        toast('Download failed: HTTP ' + r.status, 'err');
        return;
      }
      const blob = await r.blob();
      const u = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = u; a.download = filename;
      document.body.appendChild(a); a.click();
      setTimeout(() => { URL.revokeObjectURL(u); a.remove(); }, 1000);
    } catch (e) {
      toast('Download error: ' + (e?.message || e), 'err');
    }
  }

  // Trigger many downloads sequentially with a small delay so the
  // browser doesn't rate-limit / drop. Toasts progress.
  async function bulkDownload(items) {
    let ok = 0, fail = 0;
    for (let i = 0; i < items.length; i++) {
      const it = items[i];
      try {
        await authDownload(it.url, it.filename);
        ok++;
      } catch { fail++; }
      // 200 ms gap is enough for Chrome/Firefox not to throttle.
      if (i < items.length - 1) {
        await new Promise((r) => setTimeout(r, 200));
      }
      if ((ok + fail) % 5 === 0 || (ok + fail) === items.length) {
        toast('Downloading… ' + (ok + fail) + '/' + items.length, 'ok');
      }
    }
  }

  // Show a small menu of file-download options anchored near a button.
  // When multiple captures are passed, prepends "All …" entries that
  // bulk-download each modality across every capture in the list.
  // Closes on outside click + Esc.
  function openDownloadMenu(anchor, captures, sessionId) {
    closeDownloadMenu();
    const baseUrl = (file) => '/api/devices/' + encodeURIComponent(currentDeviceId) +
                              '/sessions/' + encodeURIComponent(sessionId) +
                              '/file/' + file;

    const visAll   = captures.filter((c) => c.visOk).map((c) => ({
      url: baseUrl(c.visFile),                          filename: sessionId + '_' + c.visFile,
    }));
    const thermAll = captures.filter((c) => c.thermOk).map((c) => ({
      url: baseUrl(c.thermFile),                        filename: sessionId + '_' + c.thermFile,
    }));
    const rawAll   = captures.filter((c) => c.thermOk).map((c) => ({
      url: baseUrl(pad6(c.seq) + '_therm.raw16'),       filename: sessionId + '_' + pad6(c.seq) + '_therm.raw16',
    }));
    const sideAll  = captures.filter((c) => c.thermOk).map((c) => ({
      url: baseUrl(pad6(c.seq) + '_therm.json'),        filename: sessionId + '_' + pad6(c.seq) + '_therm.json',
    }));

    const items = [];
    // Session-level metadata always available.
    items.push({
      label: 'session.json', sub: 'session-level metadata',
      url: baseUrl('session.json'), filename: sessionId + '_session.json',
    });
    items.push({
      label: 'captures.jsonl', sub: 'per-capture log',
      url: baseUrl('captures.jsonl'), filename: sessionId + '_captures.jsonl',
    });

    if (captures.length > 1) {
      if (visAll.length)   items.push({ bulk: visAll,
        label: 'All visible JPEGs',  sub: visAll.length + ' files' });
      if (thermAll.length) items.push({ bulk: thermAll,
        label: 'All thermal JPEGs',  sub: thermAll.length + ' files' });
      if (rawAll.length)   items.push({ bulk: rawAll,
        label: 'All thermal raw16',  sub: rawAll.length + ' lossless frames' });
      if (sideAll.length)  items.push({ bulk: sideAll,
        label: 'All thermal sidecars', sub: sideAll.length + ' JSON files' });
    }

    for (const c of captures) {
      const tag = '#' + c.seq + ' ';
      if (c.visOk) items.push({
        label: tag + 'visible JPEG', sub: c.visFile,
        url: baseUrl(c.visFile), filename: sessionId + '_' + c.visFile,
      });
      if (c.thermOk) {
        items.push({
          label: tag + 'thermal JPEG', sub: c.thermFile,
          url: baseUrl(c.thermFile), filename: sessionId + '_' + c.thermFile,
        });
        items.push({
          label: tag + 'thermal raw16', sub: 'lossless 160×120 uint16',
          url: baseUrl(pad6(c.seq) + '_therm.raw16'),
          filename: sessionId + '_' + pad6(c.seq) + '_therm.raw16',
        });
        items.push({
          label: tag + 'thermal sidecar', sub: 'metadata JSON',
          url: baseUrl(pad6(c.seq) + '_therm.json'),
          filename: sessionId + '_' + pad6(c.seq) + '_therm.json',
        });
      }
    }
    if (items.length === 0) return;
    const menu = el('div', { class: 'dl-menu', id: 'dl-menu' });
    for (const it of items) {
      menu.appendChild(el('button', {
        class: 'dl-item',
        onclick: () => {
          closeDownloadMenu();
          if (it.bulk) bulkDownload(it.bulk);
          else         authDownload(it.url, it.filename);
        },
      }, it.label, el('span', { class: 'dl-sub' }, it.sub)));
    }
    document.body.appendChild(menu);
    // Position near anchor.
    const rect = anchor.getBoundingClientRect();
    menu.style.left = Math.max(8, Math.min(window.innerWidth - menu.offsetWidth - 8,
                                rect.left)) + 'px';
    menu.style.top  = Math.min(window.innerHeight - menu.offsetHeight - 8,
                                rect.bottom + 4) + 'px';
    setTimeout(() => {
      const onDocClick = (e) => {
        if (!menu.contains(e.target) && e.target !== anchor) {
          closeDownloadMenu();
          document.removeEventListener('click', onDocClick);
        }
      };
      const onKey = (e) => {
        if (e.key === 'Escape') {
          closeDownloadMenu();
          document.removeEventListener('keydown', onKey);
        }
      };
      document.addEventListener('click', onDocClick);
      document.addEventListener('keydown', onKey);
    }, 0);
  }
  function closeDownloadMenu() {
    document.getElementById('dl-menu')?.remove();
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

  // Library display preferences. Persisted in localStorage so the user's
  // last layout sticks across reloads.
  const libDefaults = { modality: 'vis', layout: 'medium', sort: 'newest' };
  function loadLibPrefs() {
    try {
      const s = JSON.parse(localStorage.getItem('gh_lib') || 'null');
      return { ...libDefaults, ...(s || {}) };
    } catch { return { ...libDefaults }; }
  }
  function saveLibPrefs() {
    try { localStorage.setItem('gh_lib', JSON.stringify(libPrefs)); } catch {}
  }
  let libPrefs = loadLibPrefs();
  let lastSessions = [];

  function buildLibraryView() {
    const v = el('div', { class: 'library-view' });
    const header = el('div', { class: 'library-header' },
      el('h2', {}, 'Sessions'),
      el('div', { class: 'meta', id: 'lib-meta' }, 'Loading…'));
    const toolbar = buildLibToolbar();
    const grid = el('div', { class: 'session-grid', id: 'session-grid' });
    applyGridLayoutClass(grid);
    v.appendChild(header);
    v.appendChild(toolbar);
    v.appendChild(grid);
    return v;
  }

  function applyGridLayoutClass(grid) {
    grid.classList.remove('size-small','size-large','list');
    if (libPrefs.layout === 'small') grid.classList.add('size-small');
    else if (libPrefs.layout === 'large') grid.classList.add('size-large');
    else if (libPrefs.layout === 'list') grid.classList.add('list');
  }

  function buildLibToolbar() {
    const tb = el('div', { class: 'lib-toolbar' });

    const showOpts = [['vis','Visible'], ['therm','Thermal'], ['both','Both']];
    const showGroup = el('div', { class: 'group' },
      el('span', { class: 'group-label' }, 'Show'));
    for (const [k, label] of showOpts) {
      const p = el('div', {
        class: 'pill' + (libPrefs.modality === k ? ' active' : ''),
        onclick: () => { libPrefs.modality = k; saveLibPrefs(); rerenderLibrary(); },
      }, label);
      showGroup.appendChild(p);
    }
    tb.appendChild(showGroup);

    const layoutOpts = [['small','S'], ['medium','M'], ['large','L'], ['list','List']];
    const layoutGroup = el('div', { class: 'group' },
      el('span', { class: 'group-label' }, 'Layout'));
    for (const [k, label] of layoutOpts) {
      const p = el('div', {
        class: 'pill' + (libPrefs.layout === k ? ' active' : ''),
        onclick: () => { libPrefs.layout = k; saveLibPrefs(); rerenderLibrary(); },
      }, label);
      layoutGroup.appendChild(p);
    }
    tb.appendChild(layoutGroup);

    const sortGroup = el('div', { class: 'group' },
      el('span', { class: 'group-label' }, 'Sort'));
    const sel = document.createElement('select');
    [
      ['newest', 'Newest first'],
      ['oldest', 'Oldest first'],
      ['most',   'Most captures'],
      ['fewest', 'Fewest captures'],
      ['longest','Longest duration'],
      ['shortest','Shortest duration'],
    ].forEach(([v, label]) => {
      const o = document.createElement('option');
      o.value = v; o.textContent = label;
      if (libPrefs.sort === v) o.selected = true;
      sel.appendChild(o);
    });
    sel.onchange = () => { libPrefs.sort = sel.value; saveLibPrefs(); rerenderLibrary(); };
    sortGroup.appendChild(sel);
    tb.appendChild(sortGroup);

    return tb;
  }

  function rerenderLibrary() {
    const grid = document.getElementById('session-grid');
    if (!grid) return;
    applyGridLayoutClass(grid);
    if (lastSessions.length === 0) return;
    const sorted = sortSessions(lastSessions, libPrefs.sort);
    grid.replaceChildren(...sorted.map(renderSessionCard));
  }

  function sortSessions(arr, key) {
    const copy = arr.slice();
    const num = (s, k, dflt) => (typeof s[k] === 'number' ? s[k] : dflt);
    const newest = (a,b) => num(b,'timestamp',0) - num(a,'timestamp',0);
    const cmps = {
      newest,
      oldest:   (a,b) => num(a,'timestamp',0) - num(b,'timestamp',0),
      most:     (a,b) => num(b,'captureCount',0) - num(a,'captureCount',0),
      fewest:   (a,b) => num(a,'captureCount',0) - num(b,'captureCount',0),
      longest:  (a,b) => num(b,'durationSec',0) - num(a,'durationSec',0),
      shortest: (a,b) => num(a,'durationSec',0) - num(b,'durationSec',0),
    };
    copy.sort(cmps[key] || newest);
    return copy;
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
      lastSessions = list;
      meta.textContent = (j.total ?? list.length) + ' sessions' +
                         (j.truncated ? ' (showing newest ' + j.listed + ')' : '');
      if (list.length === 0) {
        grid.replaceChildren(el('div', { class: 'empty-library' },
          el('h3', {}, 'No sessions yet'),
          el('div', {},
            'Press Capture or Start Timelapse on the Live View to record one.')));
        return;
      }
      const sorted = sortSessions(list, libPrefs.sort);
      grid.replaceChildren(...sorted.map(renderSessionCard));
    } catch (e) {
      meta.textContent = 'Network error';
    }
  }

  function renderSessionCard(s) {
    const sid = s.sessionId || 'session_?';
    const fileUrl = (name) => currentDeviceId
      ? '/api/devices/' + encodeURIComponent(currentDeviceId) +
        '/sessions/' + encodeURIComponent(sid) + '/file/' + name
      : '';
    const isTl = s.mode === 'timelapse';
    const incomplete = isTl && s.complete === false;
    const dur = (s.durationSec != null && s.durationSec > 0)
      ? fmtElapsed(s.durationSec * 1000) : '—';
    const ts = s.timestamp ? new Date(s.timestamp * 1000) : null;
    const tsTxt = ts ? ts.toLocaleString() : '';

    const modality = libPrefs.modality;
    const wantVis   = modality === 'vis'   || modality === 'both';
    const wantTherm = modality === 'therm' || modality === 'both';

    const card = el('div', {
      class: 'session-card' + (modality === 'both' ? ' both' : ''),
      onclick: () => setView('detail', sid),
    });
    const thumbWrap = el('div', { class: 'thumb-wrap' + (modality === 'both' ? ' both' : '') });

    function makeThumb(name) {
      const img = el('img', { class: 'thumb' });
      loadAuthImg(img, fileUrl(name), () => {
        // Fall back to a "no preview" placeholder for this slot.
        const ph = el('div', { class: 'thumb-empty', style: 'flex:1;display:flex;align-items:center;justify-content:center' }, 'no preview');
        img.replaceWith(ph);
      });
      return img;
    }

    if (s.captureVis !== false && wantVis)   thumbWrap.appendChild(makeThumb('000001_vis.jpg'));
    if (s.captureTherm !== false && wantTherm) thumbWrap.appendChild(makeThumb('000001_therm.jpg'));
    if (!thumbWrap.children.length) {
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
    const dlBtn = el('button', { class: 'dh-btn',
      title: 'Download files for any capture in this session',
      onclick: (e) => {
        if (detailCaptureList.length === 0) return;
        // Open the download menu over ALL captures so the user can grab
        // any individual file. Saves a click vs visiting each tile.
        openDownloadMenu(e.currentTarget, detailCaptureList, sid);
      } }, '⬇ Files');
    const delBtn = el('button', { class: 'dh-btn danger',
      title: 'Delete this session from the device SD',
      onclick: () => deleteSession(sid) }, '🗑 Delete');
    const header = el('div', { class: 'detail-header' },
      el('button', { class: 'back', onclick: () => setView('library') }, '← Library'),
      el('h2', {}, sid),
      el('div', { class: 'dh-spacer' }),
      dlBtn,
      delBtn);
    const meta = el('div', { class: 'detail-meta', id: 'detail-meta' });
    const grid = el('div', { class: 'capture-grid', id: 'capture-grid' });
    grid.appendChild(el('div', { class: 'empty-library' }, 'Loading…'));
    v.appendChild(header);
    v.appendChild(meta);
    v.appendChild(grid);
    return v;
  }

  // Confirms then issues DELETE /sessions/:sid. On success, navigates
  // back to Library and triggers a reload so the deleted card vanishes.
  async function deleteSession(sid) {
    if (!currentDeviceId) { toast('No device connected', 'err'); return; }
    // \\n becomes \n in the served JS — a bare \n in this template
    // literal would be interpreted as a real newline by the outer
    // backtick string and break the inner JavaScript string literal.
    if (!confirm('Delete ' + sid + ' from the device?\\nThis removes all of its files and cannot be undone.')) {
      return;
    }
    try {
      const r = await fetch(
        '/api/devices/' + encodeURIComponent(currentDeviceId) +
        '/sessions/' + encodeURIComponent(sid),
        { method: 'DELETE',
          headers: { 'Authorization': 'Bearer ' + authToken } }
      );
      if (!r.ok) {
        const err = await r.json().catch(() => ({}));
        toast('Delete failed: ' + (err.error || ('HTTP ' + r.status)), 'err');
        return;
      }
      toast(sid + ' deleted', 'ok');
      // Drop the cached entry so an immediate library nav doesn't
      // briefly re-show the dead session.
      lastSessions = lastSessions.filter((s) => s.sessionId !== sid);
      setView('library');
      // Force a refresh from device too, in case multiple sessions
      // were affected (shouldn't happen in v1, but cheap).
      loadLibrary();
    } catch (e) {
      toast('Network error during delete: ' + (e?.message || e), 'err');
    }
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
      const totalCaps = j.captureCount ?? '?';
      const returned  = j.returnedCount;
      const capsTxt = (j.truncated && returned != null)
        ? returned + ' of ' + totalCaps + ' (truncated)'
        : String(totalCaps);
      meta.replaceChildren(
        metaItem('Mode',      m.mode || '?'),
        metaItem('Captures',  capsTxt),
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
        // Per-tile download button: opens menu with this capture's
        // available files (vis/therm JPEG + thermal raw16 + sidecar).
        const dl = el('button', {
          class: 'dl', title: 'Download files for this capture',
          onclick: (e) => {
            e.stopPropagation();   // don't open lightbox
            openDownloadMenu(dl, [c], sid);
          },
        }, '⬇');
        tile.appendChild(dl);
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
  let lbModality = 'vis';   // 'vis' | 'therm', persists while lightbox is open

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
  function setLbModality(m) {
    lbModality = m;
    document.getElementById('lb-vis').classList.toggle('active', m === 'vis');
    document.getElementById('lb-therm').classList.toggle('active', m === 'therm');
    renderLightbox();
  }
  function renderLightbox() {
    const c = detailCaptureList[lightboxIdx];
    if (!c) return;
    // Reset zoom to 1x on every image swap so each capture starts fresh.
    document.getElementById('lightbox').__zoomReset?.();
    // Honor what's actually present for this capture. If the requested
    // modality wasn't recorded, fall back to the other; if neither, show
    // a placeholder + disable nav switches.
    const visBtn   = document.getElementById('lb-vis');
    const thermBtn = document.getElementById('lb-therm');
    visBtn.disabled   = !c.visOk;
    thermBtn.disabled = !c.thermOk;
    let mode = lbModality;
    if (mode === 'vis'   && !c.visOk   && c.thermOk) mode = 'therm';
    if (mode === 'therm' && !c.thermOk && c.visOk)   mode = 'vis';
    visBtn.classList.toggle('active', mode === 'vis');
    thermBtn.classList.toggle('active', mode === 'therm');

    const img = document.getElementById('lb-img');
    img.removeAttribute('src');
    const file = mode === 'therm' ? c.thermFile : c.visFile;
    if ((mode === 'vis' && c.visOk) || (mode === 'therm' && c.thermOk)) {
      loadAuthImg(img,
        '/api/devices/' + encodeURIComponent(currentDeviceId) +
        '/sessions/' + encodeURIComponent(c.sessionId) +
        '/file/' + file);
    }
    const info = document.getElementById('lb-info');
    info.replaceChildren(
      el('span', { class: 'seq' }, '#' + c.seq + ' / ' + detailCaptureList.length),
      el('span', {}, ' · ' + file),
      c.timestamp ? el('span', {}, ' · ' + new Date(c.timestamp * 1000).toLocaleString()) : null,
    );
    document.getElementById('lb-prev').disabled = lightboxIdx === 0;
    document.getElementById('lb-next').disabled = lightboxIdx === detailCaptureList.length - 1;
  }
  document.getElementById('lb-close').onclick = closeLightbox;
  document.getElementById('lb-prev').onclick = () => navLightbox(-1);
  document.getElementById('lb-next').onclick = () => navLightbox(1);
  document.getElementById('lb-vis').onclick   = () => setLbModality('vis');
  document.getElementById('lb-therm').onclick = () => setLbModality('therm');
  document.getElementById('lb-dl').onclick = (e) => {
    if (lightboxIdx < 0) return;
    const c = detailCaptureList[lightboxIdx];
    if (!c) return;
    openDownloadMenu(e.currentTarget, [c], c.sessionId);
  };
  // Lightbox zoom: wheel + drag + dblclick reset on the lb-img inside
  // the lightbox container. Reset on each navLightbox / setLbModality
  // so a fresh image starts at 1x (handled inside renderLightbox).
  attachZoom(document.getElementById('lightbox'),
             document.getElementById('lb-img'), { maxScale: 12 });
  document.addEventListener('keydown', (e) => {
    const lb = document.getElementById('lightbox');
    if (!lb.classList.contains('show')) return;
    if (e.key === 'Escape') closeLightbox();
    else if (e.key === 'ArrowLeft') navLightbox(-1);
    else if (e.key === 'ArrowRight') navLightbox(1);
    else if (e.key === 'v' || e.key === 'V') setLbModality('vis');
    else if (e.key === 't' || e.key === 'T') setLbModality('therm');
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
