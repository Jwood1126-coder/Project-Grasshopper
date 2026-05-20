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
  /* Info / sleeping-by-design — distinct from "live" green and the
     amber "warn" so the user sees DEEP_SLEEP_CAPTURE / WAKE_RADIO at a
     glance without misreading them as a fault. Cyan, calm. */
  .chip.info  { color: #6cc7ff; border-color: #1e3a4a; background: #0e1a22; }
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

  /* Deep-sleep variant — moonlit color so it reads "device offline most of
     the time" rather than the green "live and capturing" banner. */
  .tl-banner.ds {
    background: linear-gradient(180deg, #1a2438 0%, #0e1422 100%);
    border-color: #3a4a78;
  }
  .tl-banner.ds .icon { background: #6b8acc; color: #001022; animation: none; }
  .tl-banner.ds .info .title { color: #aac6ff; }
  .tl-banner.ds .info .meta  { color: #88a4d8; }
  /* Offline-echo: localStorage-derived, not from device telemetry. Dim
     it further so the user reads "we're guessing based on what you
     armed" rather than "we're observing what's happening." */
  .tl-banner.ds.offline-echo {
    background: linear-gradient(180deg, #14182a 0%, #0a0e18 100%);
    border-color: #2a3458;
    border-style: dashed;
  }
  .tl-banner.ds.offline-echo .icon { background: #4a5878; color: #14182a; }
  .tl-banner.ds.offline-echo .info .title { color: #88a0c8; }
  .tl-banner.ds.offline-echo .info .meta  { color: #6a7a9a; }
  .tl-banner.ds.offline-echo .stop-btn {
    background: #444;
    color: #ccc;
  }
  .tl-banner.ds.offline-echo .stop-btn:hover:not(:disabled) {
    background: #555;
  }

  /* Modal hint: small explanatory text under a control. */
  .hint {
    font: 12px/1.4 -apple-system, sans-serif;
    color: #999; margin-top: 6px;
  }

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
    /* Don't stretch in the views grid — keep content top-aligned so
       both modality bars line up at the top of the row regardless
       of their panel heights. */
    align-self: start;
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
    /* Hard single-line + shrink-to-fit so the bar can't wrap when
       the temp readout grows. Min-width: 0 lets the flex item shrink
       below its content's intrinsic width. */
    white-space: nowrap; min-width: 0; flex: 1 1 0;
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
     no CSS transform here — that would double-rotate. The panel
     adapts its aspect-ratio to portrait when either modality is at
     90°/270°, and lets width derive from height (instead of always
     filling the column) so the panel hugs the JPEG with no
     letterbox bars on the sides. */
  .panel.portrait {
    aspect-ratio: 3/4;
    width: auto;
    max-width: 100%;
  }

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
  .fw-row {
    display: flex; gap: 8px; align-items: center; margin-top: 8px;
    padding: 10px 12px; background: var(--panel-bg);
    border: 1px solid var(--border); border-radius: 8px;
    flex-wrap: wrap;
  }
  .fw-row label { font-size: 12px; color: var(--muted); flex-shrink: 0; }
  .fw-row input[type=file] {
    flex: 1; min-width: 200px; color: var(--text);
    font: 12px ui-sans-serif;
  }
  .fw-row .btn { height: 36px; padding: 0 14px; }
  .fw-status {
    margin-top: 6px; padding: 6px 12px; font: 12px ui-monospace, monospace;
    color: var(--muted); min-height: 18px;
  }
  .fw-status.err { color: var(--err); }
  .fw-status.ok  { color: var(--ok); }

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
  /* Older-firmware compat banner. Calmer than the security banner —
     amber/warn rather than red, since the device still works, just
     with some metadata bugs the dashboard has already paved over. */
  .compat-banner {
    background: #2a2418; border: 1px solid var(--warn); border-radius: 8px;
    padding: 10px 14px; margin-bottom: 16px; color: #f0d9a0;
    font-size: 13px; line-height: 1.5;
  }
  .compat-banner strong { color: var(--warn); }
  .compat-banner code {
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
  /* Abandoned: distinct from "incomplete" (which is a transient state)
     and from "recording" (which is also incomplete=true but live). Muted
     gray-blue so abandoned sessions look obviously inert. The card
     itself dims so the eye skips past them at scan speed. */
  .session-card .badge.abandoned {
    background: #1f2530; color: #6b7a8a;
    text-transform: none; letter-spacing: 0;  /* show the reason readably */
    font-family: ui-monospace, monospace;
  }
  .session-card.abandoned { opacity: 0.6; }
  .session-card.abandoned:hover { opacity: 1; }
  /* Recording: red accent border + pulsing dot in the badge. Replaces
     the "incomplete" tag while the session is actively writing — same
     state, but framed as live progress instead of a fault. */
  .session-card.recording {
    border-color: var(--err);
    box-shadow: 0 0 0 1px var(--err) inset;
  }
  .session-card .badge.recording {
    background: var(--err); color: #fff;
    animation: rec-pulse 1.4s ease-in-out infinite;
  }
  @keyframes rec-pulse {
    0%, 100% { opacity: 1; }
    50%      { opacity: 0.55; }
  }

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
  /* Loading skeleton + missing marker for detail-view tiles. Without
     these, a tile awaiting its blob is indistinguishable from a 404
     tile — both render as a blank black box. Skeleton has a subtle
     shimmer; missing marker shows the failing file name so the
     operator can match it to the journal. */
  .capture-tile .tile-skeleton {
    position: absolute; inset: 0;
    display: flex; flex-direction: column; align-items: center; justify-content: center;
    gap: 8px; color: #4a5566;
    background: linear-gradient(90deg, #0a0a0a, #14181f, #0a0a0a);
    background-size: 200% 100%;
    animation: tile-shimmer 1.6s ease-in-out infinite;
  }
  @keyframes tile-shimmer {
    0%   { background-position: 100% 0; }
    100% { background-position: -100% 0; }
  }
  .capture-tile .tile-spinner {
    width: 18px; height: 18px; border-radius: 50%;
    border: 2px solid #2a3140; border-top-color: var(--accent);
    animation: tile-spin 0.9s linear infinite;
  }
  @keyframes tile-spin { to { transform: rotate(360deg); } }
  .capture-tile .tile-loading-label {
    font: 10px/1 ui-monospace, monospace; letter-spacing: 0.05em;
  }
  .capture-tile .tile-missing {
    position: absolute; inset: 0;
    display: flex; flex-direction: column; align-items: center; justify-content: center;
    gap: 4px; padding: 8px; text-align: center;
    color: var(--muted);
  }
  .capture-tile .tile-missing-label {
    font: 600 11px/1.3 system-ui; color: var(--err);
  }
  .capture-tile .tile-missing-file {
    font: 10px/1.3 ui-monospace, monospace; color: var(--muted);
    word-break: break-all;
  }
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

  /* Lightbox zoom buttons (sit below the close + download row) */
  .lightbox .lb-zoom {
    position: absolute; top: 60px; right: 16px;
    display: flex; flex-direction: column; gap: 4px;
  }
  .lightbox .lb-zb {
    background: rgba(255,255,255,0.08); border: 1px solid transparent;
    color: white; width: 38px; height: 38px; border-radius: 6px;
    display: flex; align-items: center; justify-content: center;
    cursor: pointer; font: 600 16px/1 system-ui;
  }
  .lightbox .lb-zb:hover { background: rgba(255,255,255,0.18); border-color: var(--border); }

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
      <label>Mode</label>
      <div class="toggle-row">
        <div class="tog on" id="mode-live" data-mode="live">Live</div>
        <div class="tog"    id="mode-ds"   data-mode="ds">Low Power</div>
      </div>
      <div class="hint" id="mode-hint">Device stays on between captures.</div>
    </div>
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
    <div class="modal-row" id="row-dur">
      <label>Stop after</label>
      <div class="interval-grid" id="dur-grid"></div>
    </div>
    <div class="modal-row" id="row-ds-max" style="display:none">
      <label>Captures <span style="color:#ff7a8c">*</span></label>
      <input type="number" id="ds-max-captures" min="1" max="999" value="12" style="width:80px;padding:6px;border-radius:6px;border:1px solid #444;background:#222;color:#eee" />
      <div class="hint">Required. Hard cap on the session length — the only stop guarantee when wake-radio is off.</div>
    </div>
    <div class="modal-row" id="row-ds-wifi" style="display:none">
      <label>Wake radio</label>
      <div class="toggle-row">
        <div class="tog" id="ds-wakewifi">Bring up Wi-Fi between captures</div>
      </div>
      <div class="hint">Lets the dashboard see in-progress sessions and accept Stop. Probably acceptable at 10-min+ intervals — verify with soak data before relying on it.</div>
    </div>
    <div class="modal-row" id="row-ds-window" style="display:none">
      <label>Window</label>
      <input type="number" id="ds-window-sec" min="5" max="60" value="15" style="width:80px;padding:6px;border-radius:6px;border:1px solid #444;background:#222;color:#eee" /> seconds
    </div>
    <div class="modal-row" id="row-ds-every" style="display:none">
      <label>Every Nth wake</label>
      <input type="number" id="ds-wifi-every" min="1" max="100" value="1" style="width:80px;padding:6px;border-radius:6px;border:1px solid #444;background:#222;color:#eee" />
      <div class="hint">1 = every wake (radio cost every cycle). Higher = lower battery cost, longer stop latency. Currently the firmware honors 1 only — the field is captured + persisted now so the schema is stable when periodic wake lands.</div>
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
  <div class="lb-zoom" id="lb-zoom">
    <button class="lb-zb" id="lb-zin"  aria-label="Zoom in">+</button>
    <button class="lb-zb" id="lb-zout" aria-label="Zoom out">−</button>
    <button class="lb-zb" id="lb-zrst" aria-label="Reset zoom">1×</button>
  </div>
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
      // Per-frame TLinear scale — overrides the tick-cached value so
      // hover temps stay correct even if AUTO_RESOLUTION flipped scale
      // since our last tick (codex #3).
      const resHdr = r.headers.get('X-Tlinear-Resolution');
      if (resHdr != null) {
        tempScaleX100 = (Number(resHdr) === 1) ? 1 : 10;
      }
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

    // Anchored zoom: keeps the (cx,cy) viewport point fixed under the
    // cursor as scale changes. Used by the +/- buttons (anchored at
    // panel center) and by ctrl-wheel (anchored at cursor).
    function zoomAt(direction, cx, cy) {
      const rect = container.getBoundingClientRect();
      const px = cx - rect.left;
      const py = cy - rect.top;
      const ix = (px - state.tx) / state.scale;
      const iy = (py - state.ty) / state.scale;
      const factor = direction > 0 ? 1.25 : 1 / 1.25;
      const next = Math.max(minScale, Math.min(maxScale, state.scale * factor));
      if (next === state.scale) return;
      state.scale = next;
      state.tx = px - ix * state.scale;
      state.ty = py - iy * state.scale;
      clamp(); apply();
    }
    function zoomCenter(direction) {
      const rect = container.getBoundingClientRect();
      zoomAt(direction, rect.left + rect.width / 2, rect.top + rect.height / 2);
    }
    function reset() {
      state.scale = 1; state.tx = 0; state.ty = 0;
      apply();
    }

    // ctrl/meta + wheel still zooms (matches browser convention for
    // page zoom and other image viewers). Plain wheel is left alone
    // so the user can scroll the dashboard with the cursor over a
    // panel.
    container.addEventListener('wheel', (e) => {
      if (!e.ctrlKey && !e.metaKey) return;
      e.preventDefault();
      zoomAt(e.deltaY < 0 ? +1 : -1, e.clientX, e.clientY);
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
    container.__zoomIn    = () => zoomCenter(+1);
    container.__zoomOut   = () => zoomCenter(-1);
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
  // Zoom controls: + / − / reset. wired to attachZoom's exposed
  // helpers on the panel.
  function mkZoomButtons(getContainer) {
    return [
      el('button', { class: 'pbtn', title: 'Zoom in', 'aria-label': 'Zoom in',
        onclick: () => getContainer()?.__zoomIn?.() }, '+'),
      el('button', { class: 'pbtn', title: 'Zoom out', 'aria-label': 'Zoom out',
        onclick: () => getContainer()?.__zoomOut?.() }, '−'),
      el('button', { class: 'pbtn', title: 'Reset zoom (or double-click image)',
        'aria-label': 'Reset zoom',
        onclick: () => getContainer()?.__zoomReset?.() }, '1×'),
    ];
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

  // Last-known system phase block — updated whenever a poll() sees one
  // in state.system / state.tick.system / state.init.system. Survives
  // across polls so the offline branch can distinguish "DEEP_SLEEP_CAPTURE
  // — sleeping by design" from "unknown — unexpectedly offline".
  // Cleared only when the device truly goes silent for a long time
  // (the noDeviceTicks >= 10 path).
  // Shape: { phase: string, phaseEnteredMs: number, observedAt: number }
  // observedAt is dashboard wall-clock at the time we cached it; lets
  // the chip show approximate "in this phase for X" even when the
  // device is offline.
  let lastSeenSystem = null;

  // Sliding-window record of connect / disconnect signals. Each entry
  // is the dashboard wall-clock ts when we observed the event in the
  // event stream. updateChips reads the size of this buffer (filtered
  // to the last CHURN_WINDOW_MS) and surfaces an "Unstable link" chip
  // when the count exceeds CHURN_THRESHOLD. Bounded so the array
  // can't grow unbounded over a long page lifetime.
  const churnEvents = [];   // array of timestamps (ms)
  const CHURN_WINDOW_MS = 5 * 60 * 1000;   // 5 minutes
  const CHURN_THRESHOLD = 6;               // 6 connect+disconnect events in 5 min
  const CHURN_MAX_TRACK = 50;              // prune ring at this size

  // ───── Timelapse settings modal ─────
  // Live mode allows fast intervals (task-loop captures); deep-sleep
  // mode requires ≥60s because Lepton boot eats ~10–18s of awake time
  // per wake. Modal filters which buttons render based on mode.
  const TL_INTERVALS = [
    { label: '5s',     sec: 5,    liveOnly: true  },
    { label: '10s',    sec: 10,   liveOnly: true  },
    { label: '30s',    sec: 30,   liveOnly: true  },
    { label: '1 min',  sec: 60   },
    { label: '5 min',  sec: 300  },
    { label: '15 min', sec: 900  },
    { label: '30 min', sec: 1800 },
    { label: '1 hr',   sec: 3600 },
  ];
  let tlMode = 'live';                 // 'live' | 'ds'
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
    const opts = tlMode === 'ds'
      ? TL_INTERVALS.filter(o => !o.liveOnly)
      : TL_INTERVALS;
    // If mode just switched and the selection is no longer valid (e.g.
    // we were on 5s and switched to ds), bump up to the smallest legal
    // option so confirm doesn't silently send a value the firmware will
    // reject.
    if (!opts.find(o => o.sec === tlSelectedInterval)) {
      tlSelectedInterval = opts[0]?.sec ?? 60;
    }
    opts.forEach(opt => {
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

  function applyModeUi() {
    document.getElementById('mode-live').classList.toggle('on', tlMode === 'live');
    document.getElementById('mode-ds').classList.toggle('on', tlMode === 'ds');
    const ds = tlMode === 'ds';
    document.getElementById('row-dur').style.display       = ds ? 'none' : '';
    document.getElementById('row-ds-max').style.display    = ds ? '' : 'none';
    document.getElementById('row-ds-wifi').style.display   = ds ? '' : 'none';
    const wakeWifi = document.getElementById('ds-wakewifi').classList.contains('on');
    document.getElementById('row-ds-window').style.display = (ds && wakeWifi) ? '' : 'none';
    document.getElementById('row-ds-every').style.display  = (ds && wakeWifi) ? '' : 'none';
    document.getElementById('mode-hint').textContent = ds
      ? 'Device deep-sleeps between captures. Capture journal lands on SD; dashboard sees the session only during optional wake-Wi-Fi windows.'
      : 'Device stays on between captures.';
    buildIvGrid();    // re-render interval grid with mode-appropriate options
  }

  function setupModalToggles() {
    const v = document.getElementById('tog-vis');
    const t = document.getElementById('tog-therm');
    v.classList.toggle('on', tlCaptureVis);
    t.classList.toggle('on', tlCaptureTherm);
    v.onclick = () => { tlCaptureVis = !tlCaptureVis; v.classList.toggle('on', tlCaptureVis); };
    t.onclick = () => { tlCaptureTherm = !tlCaptureTherm; t.classList.toggle('on', tlCaptureTherm); };

    document.getElementById('mode-live').onclick = () => { tlMode = 'live'; applyModeUi(); };
    document.getElementById('mode-ds').onclick   = () => { tlMode = 'ds';   applyModeUi(); };
    const wf = document.getElementById('ds-wakewifi');
    wf.onclick = () => {
      wf.classList.toggle('on');
      // window + every-Nth rows track the wakeWifi toggle
      const show = (tlMode === 'ds' && wf.classList.contains('on')) ? '' : 'none';
      document.getElementById('row-ds-window').style.display = show;
      document.getElementById('row-ds-every').style.display  = show;
    };
  }

  function openTlModal() {
    buildIvGrid();
    buildDurGrid();
    setupModalToggles();
    applyModeUi();   // apply current mode visibility BEFORE showing
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
    if (tlMode === 'ds') {
      const maxCapsRaw = document.getElementById('ds-max-captures').value;
      const maxCaps = parseInt(maxCapsRaw, 10);
      if (!isFinite(maxCaps) || maxCaps < 1) {
        toast('Captures is required (≥ 1) — it is the only stop guarantee in low-power mode', 'err');
        document.getElementById('ds-max-captures').focus();
        return;
      }
      const wakeWifi = document.getElementById('ds-wakewifi').classList.contains('on');
      const wakeWindowSec = wakeWifi
        ? (parseInt(document.getElementById('ds-window-sec').value, 10) || 15)
        : 0;
      const wakeWifiEvery = wakeWifi
        ? (parseInt(document.getElementById('ds-wifi-every').value, 10) || 1)
        : 1;

      // Hard confirm before arming DS with wake-Wi-Fi OFF. Device
      // goes radio-dark for the FULL session and the dashboard can
      // only see it again after the last capture completes (or by
      // resetting the device manually). Multiple users have armed
      // without realizing this is the contract — caused several
      // "device went offline and didn't come back" panics.
      if (!wakeWifi) {
        const totalSec = tlSelectedInterval * maxCaps;
        const totalMin = Math.ceil(totalSec / 60);
        const msg = 'Arm deep-sleep timelapse with wake-Wi-Fi OFF?\\n\\n' +
          maxCaps + ' captures × ' + tlSelectedInterval + 's interval ' +
          '(~' + totalMin + ' min total)\\n\\n' +
          '• Device radio is OFF between captures\\n' +
          '• Dashboard will show "Sleeping by design" until session completes\\n' +
          '• NO REMOTE STOP — only way to halt is to reset the device\\n' +
          '• Captures land on SD; visible in Library when session ends';
        if (!confirm(msg)) return;
      }
      closeTlModal();
      // Persist what we just armed so the dashboard can keep showing
      // an "armed but device-dark" banner while the device sleeps. The
      // device sets / clears its own DS state independently; we treat
      // localStorage as a hint and clear it when the device comes back
      // online reporting DS_INACTIVE (= session complete or aborted).
      try {
        localStorage.setItem('gh_ds_armed', JSON.stringify({
          armedAt: Date.now(),
          intervalSec: tlSelectedInterval,
          maxCaptures: maxCaps,
          captureVis: tlCaptureVis,
          captureTherm: tlCaptureTherm,
          wakeWifi, wakeWindowSec, wakeWifiEvery,
        }));
      } catch {}
      sendCmd('timelapse.start',
        { intervalSec: tlSelectedInterval, captureVis: tlCaptureVis, captureTherm: tlCaptureTherm,
          deepSleep: true, maxCaptures: maxCaps,
          wakeWifi, wakeWindowSec, wakeWifiEvery },
        null, 'Start Deep-Sleep Timelapse');
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

    // Header bar above the image: title + meta + zoom/rotate/info/fullscreen.
    fields.thermFs   = mkFsButton(() => toggleFullscreen('therm'));
    fields.thermInfo = mkInfoButton(() => toggleCompact('therm'));
    const thermZoomBtns = mkZoomButtons(() => fields.thermalPanel);
    const thermBar = el('div', { class: 'panel-bar thermal' },
      el('span', { class: 'ptitle' }, 'Thermal'),
      el('div', { class: 'pmeta' }, fields.thermalMeta),
      el('div', { class: 'pbtns' },
         ...thermZoomBtns, fields.thermRotBtn, fields.thermInfo, fields.thermFs));

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
    const visZoomBtns = mkZoomButtons(() => fields.visPanel);
    const visBar = el('div', { class: 'panel-bar' },
      el('span', { class: 'ptitle' }, 'Visible'),
      el('div', { class: 'pmeta' }, fields.visMeta),
      el('div', { class: 'pbtns' },
         ...visZoomBtns, fields.visRotBtn, fields.visInfo, fields.visFs));
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

    // ─── OTA firmware update ───
    fields.fwInput  = el('input', { type: 'file', accept: '.bin,application/octet-stream',
                                     id: 'fw-file' });
    fields.fwStatus = el('div', { class: 'fw-status', id: 'fw-status' }, '');
    fields.fwBtn    = el('button', { class: 'btn', onclick: () => uploadAndUpdateFirmware() },
      el('span', { class: 'icon' }, '⬆'),
      el('span', { class: 'btn-label' }, 'Update Firmware'));
    const fwRow = el('div', { class: 'fw-row' },
      el('label', {}, 'Firmware:'), fields.fwInput, fields.fwBtn);

    const diagnostics = el('details', {},
      el('summary', {}, 'Diagnostics & device state'),
      el('div', { class: 'body' }, fields.statBlock, tokenRow, fwRow, fields.fwStatus));

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

    // Active timelapse banner + Start/Stop button toggle. Deep-sleep
    // sessions take precedence (the dashboard only sees them during
    // wake-Wi-Fi windows, so when we DO see one, surface it loudly).
    // Pass device.online too so the offline-echo branch can decide
    // whether to clear stale localStorage hints.
    updateTlBanner(tl, live.deepSleep, !!(d && d.online));

    // Action-button gating. A button is "actionable" iff the device
    // is online AND no current state makes the cmd unsafe / nonsense.
    // We don't rely on the device's own preconditions because the
    // user round-trip is slow + the toast lands several seconds
    // after the click; better to grey out at the source.
    //
    // Rules (conservative — easy to relax later if needed):
    //   Capture:  device online AND no active TL/DS AND phase ∈ {LIVE}
    //   Start TL: device online AND no active TL/DS AND phase ∈ {LIVE}
    //   FFC:      device online AND phase != OTA
    //   Reboot:   device online (always — user override)
    //
    // The phase check uses tick.system.phase (canonical) with a
    // fallback to top-level system.phase from the relay cache.
    const online   = !!(d && d.online);
    const phase    = live.system?.phase || lastSeenSystem?.phase || 'UNKNOWN';
    const tlBusy   = !!(tl?.active) || !!(live.deepSleep?.active);
    const phaseOk  = phase === 'LIVE';
    const otaBusy  = phase === 'OTA';
    setBtnEnabled(fields.captureBtn, online && !tlBusy && phaseOk,
                   online ? (tlBusy ? 'Timelapse running' : (phaseOk ? '' : 'Device busy (' + phase + ')')) : 'Device offline');
    setBtnEnabled(fields.tlBtn,      online && !tlBusy && phaseOk,
                   online ? (tlBusy ? 'Timelapse already running' : (phaseOk ? '' : 'Device busy (' + phase + ')')) : 'Device offline');
    setBtnEnabled(fields.ffcBtn,     online && !otaBusy,
                   online ? (otaBusy ? 'Updating firmware' : '') : 'Device offline');
    setBtnEnabled(fields.rebootBtn,  online,
                   online ? '' : 'Device offline');
    setBtnEnabled(fields.fwBtn,      online && !otaBusy,
                   online ? (otaBusy ? 'OTA already in progress' : '') : 'Device offline');

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

  // Read the dashboard-side echo of an armed deep-sleep session.
  // Lives in localStorage; cleared when the device returns online
  // reporting DS_INACTIVE (= session complete or aborted).
  function loadDsArmed() {
    try {
      const raw = localStorage.getItem('gh_ds_armed');
      return raw ? JSON.parse(raw) : null;
    } catch { return null; }
  }
  function clearDsArmed() {
    try { localStorage.removeItem('gh_ds_armed'); } catch {}
  }

  function updateTlBanner(tl, ds, deviceOnline) {
    const slot = fields.tlBannerSlot;
    if (!slot) return;
    const dsActive = !!(ds && ds.active);
    const tlActive = !!(tl && tl.active);
    const armed = loadDsArmed();
    // If device is online AND not in DS_ACTIVE AND we have a stale
    // local echo, the session must have completed (or never armed).
    // Clear so we stop showing a phantom banner.
    if (armed && deviceOnline && !dsActive) {
      clearDsArmed();
    }
    const showOfflineEcho = !!(armed && !deviceOnline && !dsActive);
    const anyActive = dsActive || tlActive || showOfflineEcho;

    // Toggle Start button label/visibility — both modes use the same button.
    if (fields.tlBtn) {
      const lbl = fields.tlBtn.querySelector('.btn-label');
      if (lbl) {
        lbl.textContent = dsActive
          ? 'Deep-sleep timelapse running…'
          : tlActive
            ? 'Timelapse running…'
            : showOfflineEcho
              ? 'Deep-sleep session armed (device dark)…'
              : 'Start Timelapse…';
      }
      fields.tlBtn.disabled = anyActive;
      fields.tlBtn.style.opacity = anyActive ? '0.55' : '';
    }

    if (!anyActive) {
      slot.replaceChildren();
      return;
    }

    // Deep-sleep banner (takes precedence). The session is mostly
    // invisible to the dashboard — we see it only during wake-Wi-Fi
    // windows. When we do see it, render the count + max so the user
    // knows where in the schedule we are. timelapse.stop is routed
    // device-side to the DS scheduler when DS_ACTIVE.
    if (dsActive) {
      const stopBtn = el('button', {
        class: 'stop-btn',
        id: 'tl-stop',
        onclick: () => sendCmd('timelapse.stop', {}, stopBtn, 'Stop'),
      }, '■  Stop');
      const banner = el('div', { class: 'tl-banner ds' },
        el('div', { class: 'icon' }, '◐'),
        el('div', { class: 'info' },
          el('div', { class: 'title' },
            'Deep-sleep timelapse · ' + ds.sessionId),
          el('div', { class: 'meta' },
            'next capture seq ' + ds.nextSeq + ' / ' + ds.maxCaptures +
            ' · device sleeps between captures · this view appears only during wake windows')
        ),
        stopBtn);
      slot.replaceChildren(banner);
      return;
    }

    // Offline echo: device is dark but we know it was armed for a
    // deep-sleep session. Render a banner with computed-from-localStorage
    // estimates so the user isn't staring at "no device" wondering if
    // anything's happening. Cleared automatically when the device comes
    // back online reporting DS_INACTIVE.
    if (showOfflineEcho) {
      const a = armed;
      const intervalMs = (a.intervalSec || 0) * 1000;
      const elapsedMs = Date.now() - (a.armedAt || Date.now());
      // First capture fires immediately, subsequent captures land at
      // armedAt + (k-1)*interval + ~18s wake. Estimate the most-recent
      // committed capture by integer division.
      const estDoneRaw = intervalMs > 0 ? Math.floor(elapsedMs / intervalMs) + 1 : 1;
      const estDone = Math.max(0, Math.min(estDoneRaw, a.maxCaptures || 0));
      const totalDurMs = (a.maxCaptures || 0) * intervalMs;
      const finishAt = (a.armedAt || 0) + totalDurMs;
      const finishStr = new Date(finishAt).toLocaleString();
      const sessionDone = estDone >= (a.maxCaptures || 0);
      const stopHint = a.wakeWifi
        ? 'Stop will queue until next wake-radio window (~every '
          + ((a.intervalSec || 0) * (a.wakeWifiEvery || 1)) + 's)'
        : 'No remote stop — session will run until ' + (a.maxCaptures || '?') + ' captures complete';

      const cancelBtn = el('button', {
        class: 'stop-btn',
        title: 'Forget this echo — does not affect the device',
        onclick: () => {
          if (confirm('Forget this offline echo?\\n\\nThis only clears the dashboard hint. The device will continue its session until completion.')) {
            clearDsArmed();
            updateTlBanner(tl, ds, deviceOnline);
          }
        },
      }, '✕ Forget echo');

      const banner = el('div', { class: 'tl-banner ds offline-echo' },
        el('div', { class: 'icon' }, '◌'),
        el('div', { class: 'info' },
          el('div', { class: 'title' },
            sessionDone
              ? 'Deep-sleep session expected complete (device hasn\\'t reconnected yet)'
              : 'Deep-sleep session armed · device dark'),
          el('div', { class: 'meta' },
            'estimated capture ' + estDone + ' / ' + (a.maxCaptures || '?') +
            ' · interval ' + (a.intervalSec || '?') + 's' +
            ' · armed ' + fmtElapsed(elapsedMs) + ' ago'),
          el('div', { class: 'meta' },
            'expected finish: ' + finishStr),
          el('div', { class: 'meta' }, stopHint)),
        cancelBtn);
      slot.replaceChildren(banner);
      return;
    }

    // Live timelapse banner (unchanged behavior).
    const elapsed = Date.now() - (tl.startedMs || Date.now());
    const stopBtn = el('button', {
      class: 'stop-btn',
      id: 'tl-stop',
      onclick: () => sendCmd('timelapse.stop', {}, stopBtn, 'Stop'),
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

  // Map a wire-protocol phase name to a chip CSS class. Unknown values
  // get the default (no extra class) gray look — downgrade-safe: an old
  // firmware that never carries the system block doesn't break the UI.
  function phaseChipClass(phase) {
    switch (phase) {
      case 'LIVE':                return 'chip ok';
      case 'CAPTURE':
      case 'DEEP_SLEEP_CAPTURE':
      case 'WAKE_RADIO':          return 'chip info';
      case 'OTA':
      case 'BOOT':
      case 'RECOVERY':            return 'chip warn';
      case 'UNKNOWN':
      default:                    return 'chip';
    }
  }

  // Pull the canonical {phase, phaseEnteredMs} from a poll response.
  // Prefers the relay's top-level state.system (commit 3 firmware/relay
  // path); falls back to the splice_extras blocks inside tick / init for
  // older relay deployments that haven't redeployed yet. Returns null if
  // nothing is present.
  function readSystemBlock(state) {
    if (!state || typeof state !== 'object') return null;
    const candidates = [state.system, state.tick?.system, state.init?.system];
    for (const c of candidates) {
      if (c && typeof c.phase === 'string' &&
          typeof c.phaseEnteredMs === 'number') {
        return { phase: c.phase, phaseEnteredMs: c.phaseEnteredMs };
      }
    }
    return null;
  }

  function fmtPhaseDuration(ms) {
    if (!isFinite(ms) || ms < 0) return '';
    if (ms < 1000) return ms + ' ms';
    const s = Math.floor(ms / 1000);
    if (s < 60) return s + 's';
    return Math.floor(s / 60) + 'm ' + (s % 60) + 's';
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

    // Unstable-link chip. Render whenever churnEvents has more than
    // CHURN_THRESHOLD entries within the last CHURN_WINDOW_MS. We
    // count instead of filtering in-place so churnEvents stays small.
    // Hover tooltip lists the exact count + window for context.
    {
      const now = Date.now();
      const cutoff = now - CHURN_WINDOW_MS;
      let recent = 0;
      for (let i = churnEvents.length - 1; i >= 0; i--) {
        if (churnEvents[i] >= cutoff) recent++;
        else break;
      }
      if (recent >= CHURN_THRESHOLD) {
        const mins = Math.round(CHURN_WINDOW_MS / 60000);
        chips.appendChild(el('div', {
          class: 'chip err',
          title: recent + ' connect/disconnect events in the last ' + mins +
                 ' min — link is flapping (iPhone hotspot drops, RSSI margin, ' +
                 'WS-mutex contention on old firmware, etc).',
        }, 'Unstable link'));
      }
    }

    if (d && d.online) {
      // Three states:
      //   "Acquiring"  — frame counter has advanced within the last 10s
      //   "Stalled"    — frame counter was once nonzero but hasn't moved
      //                  in >10s (Lepton wedged mid-stream)
      //   "Warming up" — frame counter still 0 (boot/CCI/first-frame
      //                  pipeline, ~10-15s after a fresh boot — NOT
      //                  the same failure mode as Stalled, even though
      //                  the old chip showed them identically)
      const acqStale = lastThermFramesTs === 0 || (Date.now() - lastThermFramesTs) > 10000;
      const acqOk = lastThermFrames > 0 && !acqStale;
      let chipClass, chipText, chipTitle;
      if (acqOk) {
        chipClass = 'chip ok';
        chipText  = 'Acquiring';
        chipTitle = 'Acquisition: thermal frame counter increasing';
      } else if (lastThermFrames > 0) {
        chipClass = 'chip err';
        chipText  = 'Stalled';
        chipTitle = 'Lepton produced frames but counter hasn\\'t moved in >10s. ' +
                    'Try reseating the breakout or wait for auto-recovery.';
      } else {
        chipClass = 'chip warn';
        chipText  = 'Warming up';
        chipTitle = 'Lepton hasn\\'t delivered its first frame yet (typical 10-15s ' +
                    'after a fresh boot — CCI configure + first VoSPI sync).';
      }
      chips.appendChild(el('div', { class: chipClass, title: chipTitle }, chipText));
    }

    // Phase chip. Read from state if present; otherwise from the cached
    // lastSeenSystem (covers the offline branch where we don't have a
    // fresh state object). Compute "in phase for X" against tick.uptimeMs
    // when available, else against dashboard wall-clock since we cached
    // the block. Approximate is fine — the value is for human triage.
    const sys = readSystemBlock(state) || lastSeenSystem;
    if (sys) {
      let inPhaseMs = 0;
      const tickUp = state?.tick?.uptimeMs;
      if (typeof tickUp === 'number' && tickUp >= sys.phaseEnteredMs) {
        inPhaseMs = tickUp - sys.phaseEnteredMs;
      } else if (lastSeenSystem && lastSeenSystem.observedAt) {
        // Wall-clock fallback — represents the SUM of dashboard-time
        // since we last saw the device + whatever device-time was
        // already on the clock. Coarse but useful for "stuck > 30s".
        inPhaseMs = Date.now() - lastSeenSystem.observedAt;
      }
      const isStale = (!d || !d.online) ? true : false;
      chips.appendChild(el('div', {
        class: phaseChipClass(sys.phase) + (isStale ? ' stale' : ''),
        title: 'System phase' + (isStale ? ' (last known)' : '') +
               (inPhaseMs > 0 ? ' · in this phase for ' + fmtPhaseDuration(inPhaseMs) : ''),
      }, sys.phase));
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

  // Enable/disable + visually grey out a button based on whether the
  // underlying action is currently safe. Takes a "whyDisabled" string
  // that gets shown as the title attribute so the user can hover and
  // see exactly why something's greyed out (e.g. "Timelapse already
  // running", "Device offline"). Does NOT touch buttons currently in
  // setBtnPending state — the spinner takes precedence over gating.
  function setBtnEnabled(button, enabled, whyDisabled) {
    if (!button) return;
    if (button.classList.contains('pending')) return;   // mid-cmd; leave alone
    button.disabled = !enabled;
    button.style.opacity = enabled ? '' : '0.45';
    button.style.cursor = enabled ? '' : 'not-allowed';
    if (!enabled && whyDisabled) {
      button.title = whyDisabled;
    } else if (enabled) {
      button.removeAttribute('title');
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
          // Self-heal phantom-banner state. If the user clicked Stop
          // and the device says "no active timelapse" (or similar),
          // our banner was wrong — clear the localStorage echo AND
          // null the cached tick state so the next poll redraws from
          // fresh state instead of the stale cache that produced the
          // banner. Common cause: relay merged ticks (pre-eb8abc2)
          // and a prior DS session's deepSleep:{active:true} block
          // survived into the post-completion ticks.
          if (pending.type === 'timelapse.stop' && e.ok === false &&
              typeof e.msg === 'string' &&
              /no active timelapse/i.test(e.msg)) {
            clearDsArmed();
            // Force the next poll to rebuild the banner from current
            // state (it'll be empty/correct since the device truly
            // has no active TL).
            updateTlBanner(null, null, true);
          }
        }
        // Surface OTA progress / completion in the firmware status
        // row regardless of pendingCmds — multiple cmd.result events
        // with the same id stream during a single update.
        if (e.kind === 'cmd.result' && e.cmd === 'firmware.update') {
          let pct = '';
          if (e.data && typeof e.data === 'object' && typeof e.data.pct === 'number') {
            pct = ' (' + e.data.pct + '%)';
          }
          setFwStatus((e.ok ? '' : '✗ ') + (e.msg || '') + pct,
                       e.ok ? '' : 'err');
        }
        // Phase transitions — quietly cache the latest known phase from
        // the event stream too. Tick-derived state.system updates
        // are still the primary source; this just lets the chip flip
        // ~1 s sooner during a transition burst (e.g. LIVE→CAPTURE→LIVE
        // inside a single tick interval that the chip would otherwise
        // miss entirely).
        if (e.kind === 'phase' && typeof e.to === 'string' &&
            typeof e.uptimeMs === 'number') {
          // The transition event carries to + uptimeMs; phaseEnteredMs
          // for the NEW phase is the uptime at transition.
          lastSeenSystem = {
            phase: e.to,
            phaseEnteredMs: e.uptimeMs,
            observedAt: Date.now(),
          };
        }
        // Stuck-phase warnings — surface as a toast so the operator sees
        // them in real time, not just buried in the events ring.
        if (e.kind === 'phase.stuck' && typeof e.phase === 'string') {
          const inMs = (typeof e.inPhaseMs === 'number') ? e.inPhaseMs : 0;
          toast('⚠ phase ' + e.phase + ' stuck for ' + fmtPhaseDuration(inMs),
                'err');
        }
        // Track connect / disconnect events for the churn detector.
        // Each event is one transition; CHURN_THRESHOLD is set against
        // the SUM of both directions over CHURN_WINDOW_MS.
        if (e.kind === 'connected' || e.kind === 'disconnected') {
          churnEvents.push(e.ts || Date.now());
          if (churnEvents.length > CHURN_MAX_TRACK) {
            churnEvents.splice(0, churnEvents.length - CHURN_MAX_TRACK);
          }
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
          // Branch the empty state on last-known phase. If the device
          // went silent while in DEEP_SLEEP_CAPTURE, this is a designed
          // sleep — show the user the right story instead of "searching
          // for your device" (which implies something's wrong). After
          // ~10 minutes of cached state we drop the cache and fall back
          // to the generic "looking for" message; sleeping that long
          // without a wake-Wi-Fi window is genuinely unusual.
          const sleepingByDesign =
            lastSeenSystem &&
            lastSeenSystem.phase === 'DEEP_SLEEP_CAPTURE' &&
            (Date.now() - (lastSeenSystem.observedAt || 0)) < 10 * 60 * 1000;
          if (currentView === 'live' && !content.querySelector('.empty')) {
            content.innerHTML = '';
            if (sleepingByDesign) {
              content.appendChild(el('div', { class: 'empty' },
                el('div', { class: 'pulse' }),
                el('h2', {}, 'Sleeping by design'),
                el('p', {},
                  'Device entered DEEP_SLEEP_CAPTURE. Radio is off between captures; ' +
                  'the dashboard will reconnect during the next wake-Wi-Fi window or when the session completes.')
              ));
            } else {
              content.appendChild(el('div', { class: 'empty' },
                el('div', { class: 'pulse' }),
                el('h2', {}, 'Looking for your device…'),
                el('p', {}, 'Power on a Grasshopper unit and connect it to Wi-Fi. It should appear here within a few seconds.')
              ));
            }
          }
        }
        // Pass the cached system block through so the chip stays
        // visible during the offline window even before noDeviceTicks
        // reaches the empty-state threshold.
        updateChips(null, lastSeenSystem ? { system: lastSeenSystem } : null);
        // Grey out any action buttons that are still in the DOM from
        // the previous render — the user shouldn't be clicking Capture
        // / Start TL while we wait for the empty-state threshold. The
        // buttons get re-enabled the moment a device returns.
        const why = 'Device offline';
        setBtnEnabled(fields.captureBtn, false, why);
        setBtnEnabled(fields.tlBtn,      false, why);
        setBtnEnabled(fields.ffcBtn,     false, why);
        setBtnEnabled(fields.rebootBtn,  false, why);
        setBtnEnabled(fields.fwBtn,      false, why);
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
        // Cache the latest known phase so the offline branch can show
        // the right empty-state message + the chip stays correct.
        const sys = readSystemBlock(state);
        if (sys) lastSeenSystem = { ...sys, observedAt: Date.now() };
        updateActiveTl(live);
        updateChips(d, state);
        maybeRefreshLibrary();
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
      const live = state?.tick ?? state?.init;
      // Cache phase for the offline branch.
      const sys = readSystemBlock(state);
      if (sys) lastSeenSystem = { ...sys, observedAt: Date.now() };
      updateActiveTl(live);
      updateLiveView(d, state);
      updateChips(d, state);
      maybeRenderCompatBanner(d);
    } catch (e) {
      console.warn('poll error', e);
    }
  }

  // Pull the in-progress timelapse session id (and capture count) from the
  // latest tick/init. Side-effect: when the count moves, mark the library
  // dirty so the next maybeRefreshLibrary tick re-fetches the list and the
  // recording card's thumbnail/captures field stays current.
  function updateActiveTl(live) {
    const tl = live?.timelapse;
    const sid = (tl && tl.active && tl.sessionId) ? tl.sessionId : null;
    const count = (tl && typeof tl.captureCount === 'number') ? tl.captureCount : 0;
    if (sid !== activeTlSessionId) {
      activeTlSessionId = sid;
      // Force a refresh on TL start/stop transitions so the recording
      // card appears (or its incomplete badge clears) without delay.
      lastLibraryReloadMs = 0;
    } else if (sid && count !== activeTlCaptureCount) {
      // New capture committed during this session — refresh the card.
      lastLibraryReloadMs = 0;
    }
    activeTlCaptureCount = count;
  }

  // Library auto-refresh. Cheap when nothing changed: sessions.list is
  // a single device cmd that returns inline JSON. Cadence:
  //   - 8 s baseline while view='library' and a device is online
  //   - immediate when activeTlSessionId/captureCount transitions
  //     (lastLibraryReloadMs reset to 0 by updateActiveTl)
  // Detail view doesn't need it — captures.jsonl is loaded on open and
  // an in-progress session's later captures aren't visible anyway until
  // the session is closed (the detail view caches the snapshot).
  function maybeRefreshLibrary() {
    if (currentView !== 'library' || !currentDeviceId) return;
    const now = Date.now();
    if (now - lastLibraryReloadMs < 8000) return;
    lastLibraryReloadMs = now;
    loadLibrary();
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
  // Page-lifetime cache of (url → 404) so the fallback-thumbnail
  // probe chain doesn't keep re-hitting the device's sessions worker
  // for files we already know don't exist. Each Library re-render
  // (e.g. auto-refresh, layout toggle) would otherwise re-probe
  // every missing-thermal-seq-1 entry. Cleared only on page reload.
  const missingUrls = new Set();

  // In-flight fetch dedup. Without this, 5 simultaneous tiles all
  // wanting the same missing thermal would each fire their own
  // fetch — observed in the relay event log as 13+ identical
  // 404s within 1 second. With this, the first call starts the
  // fetch and every subsequent call for the same URL awaits the
  // same Promise. Cleared when the fetch resolves either way.
  const inflightFetches = new Map();   // url -> Promise<Response | null>

  // blob URL to <img>. Returns the blob URL (also tracked for cleanup).
  // Falls back to onerror handler if the fetch fails.
  async function loadAuthImg(img, url, onErr) {
    // Skip the round-trip entirely if we already know this URL is
    // a 404. The onErr handler still fires synchronously so the
    // fallback chain advances exactly as it would on a fresh probe.
    if (missingUrls.has(url)) {
      if (onErr) onErr(404);
      return null;
    }
    try {
      // Dedupe: if another tile is already fetching this URL, share
      // its Response. Each consumer still parses + creates its own
      // blob URL from the cloned response — Response bodies are
      // single-use so we clone() per awaiter.
      let pending = inflightFetches.get(url);
      if (!pending) {
        pending = fetch(url, {
          headers: { 'Authorization': 'Bearer ' + authToken },
          cache: 'no-store',
        }).finally(() => inflightFetches.delete(url));
        inflightFetches.set(url, pending);
      }
      const baseResponse = await pending;
      const r = baseResponse.clone();
      if (!r.ok) {
        // 404 = file genuinely missing on device. Cache so we don't
        // re-probe. Other status codes (502/503/etc) might be
        // transient — don't cache those, they'd suppress retries.
        if (r.status === 404) missingUrls.add(url);
        if (onErr) onErr(r.status);
        return null;
      }
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
  const libDefaults = { modality: 'vis', layout: 'medium', sort: 'newest',
                        // Hide abandoned sessions by default — the SD card
                        // accumulates a long tail of recovery-touched orphans
                        // (timestamp=0, aborted=true) over time, and they
                        // crowd out real recordings. Toggle pill in the
                        // toolbar lets users show them when they want to
                        // clean up. Stored per-user in localStorage with the
                        // rest of libPrefs.
                        showAbandoned: false };
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
  // sessionId of an in-progress timelapse, surfaced on the matching
  // library card with a "● recording" badge and dropping the redundant
  // "incomplete" tag (incomplete=true is by definition the recording state).
  // Updated from poll() each tick.
  let activeTlSessionId = null;
  // captureCount for the active TL on the previous tick — used to
  // trigger a library re-fetch when a new capture commits, so the
  // recording card shows the fresh count + thumbnail without waiting
  // on the slower wall-clock interval.
  let activeTlCaptureCount = 0;
  let lastLibraryReloadMs = 0;

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

    // Show-abandoned toggle. Default OFF — the typical user wants real
    // recordings, not the legacy/recovery clutter. Click to flip; the
    // pill text updates to make the current state obvious.
    const showGroupAb = el('div', { class: 'group' },
      el('span', { class: 'group-label' }, 'Abandoned'));
    const abPill = el('div', {
      class: 'pill' + (libPrefs.showAbandoned ? ' active' : ''),
      onclick: () => {
        libPrefs.showAbandoned = !libPrefs.showAbandoned;
        saveLibPrefs();
        // Re-fetch nothing — we have the full list cached in lastSessions.
        // Just rerender with the new filter.
        rerenderLibrary();
      },
      title: 'Show sessions marked aborted/interrupted (recovery orphans, ' +
             'cold-boot-cleared DS sessions). Off by default — they accumulate ' +
             'on the SD over time and crowd out real recordings.',
    }, libPrefs.showAbandoned ? 'shown' : 'hidden');
    showGroupAb.appendChild(abPill);
    tb.appendChild(showGroupAb);

    return tb;
  }

  // Apply abandoned-filter before sort. Used by both initial load and
  // the auto-refresh path — filtering inside the relay would be cleaner
  // but we want the toolbar toggle to flip instantly without a network
  // round trip. Counts are also reported in the meta line.
  function visibleSessions(list) {
    if (libPrefs.showAbandoned) return list;
    return list.filter((s) => s && s.aborted !== true);
  }

  function rerenderLibrary() {
    const grid = document.getElementById('session-grid');
    if (!grid) return;
    applyGridLayoutClass(grid);
    if (lastSessions.length === 0) return;
    const visible = visibleSessions(lastSessions);
    const sorted = sortSessions(visible, libPrefs.sort);
    grid.replaceChildren(...sorted.map(renderSessionCard));
    // Refresh meta line with the filtered count vs total.
    const meta = document.getElementById('lib-meta');
    if (meta) {
      const hidden = lastSessions.length - visible.length;
      meta.textContent = visible.length + ' sessions' +
        (hidden > 0 ? ' (' + hidden + ' abandoned hidden)' : '');
    }
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

  // Cheap fingerprint of the session list: mtime-ish bits per row that
  // would warrant a re-render. Lets auto-refresh avoid replaceChildren
  // (and the 100-thumbnail re-fetch storm that follows) when nothing
  // has actually changed since the last poll.
  function sessionsFingerprint(list) {
    let s = '';
    for (const x of list) {
      s += (x.sessionId || '?') + ':' +
           (x.captureCount ?? '?') + ':' +
           (x.durationSec ?? '?') + ':' +
           (x.complete === false ? 'i' : 'c') +
           // Include aborted so a recovery sweep that stamps the
           // marker triggers a re-render (the abandoned-filter pill
           // would otherwise read stale data).
           (x.aborted === true ? 'A' : '_') + ';';
    }
    return s;
  }
  let lastSessionsFp = '';

  async function loadLibrary() {
    const meta = document.getElementById('lib-meta');
    const grid = document.getElementById('session-grid');
    if (!grid) return;
    if (!currentDeviceId) {
      meta.textContent = 'Waiting for device…';
      return;
    }
    // Stamp here too — setView() calls loadLibrary directly without
    // going through maybeRefreshLibrary, and we want the next 8 s
    // throttle window to start now so we don't double-fetch.
    lastLibraryReloadMs = Date.now();
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
      // Skip re-render if nothing observable changed since last load.
      // Avoids destroying live <img> elements on every 8s auto-refresh,
      // which would otherwise re-issue every thumbnail fetch (cheap when
      // cached, but the 404-fallback chain still hits the device).
      const fp = sessionsFingerprint(list);
      if (fp === lastSessionsFp && grid.children.length > 0) return;
      lastSessionsFp = fp;
      const visible = visibleSessions(list);
      const sorted = sortSessions(visible, libPrefs.sort);
      grid.replaceChildren(...sorted.map(renderSessionCard));
      // Refresh meta with the filtered count.
      const hidden = list.length - visible.length;
      meta.textContent = visible.length + ' sessions' +
        (hidden > 0 ? ' (' + hidden + ' abandoned hidden)' : '') +
        (j.truncated ? ' · ' + j.total + ' total' : '');
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
    const isRecording = isTl && activeTlSessionId && activeTlSessionId === sid;
    const dur = (s.durationSec != null && s.durationSec > 0)
      ? fmtElapsed(s.durationSec * 1000) : '—';
    const ts = s.timestamp ? new Date(s.timestamp * 1000) : null;
    const tsTxt = ts ? ts.toLocaleString() : '';

    const modality = libPrefs.modality;
    const wantVis   = modality === 'vis'   || modality === 'both';
    const wantTherm = modality === 'therm' || modality === 'both';

    const card = el('div', {
      class: 'session-card' + (modality === 'both' ? ' both' : '') +
             (isRecording ? ' recording' : '') +
             (s.aborted === true ? ' abandoned' : ''),
      onclick: () => setView('detail', sid),
    });
    const thumbWrap = el('div', { class: 'thumb-wrap' + (modality === 'both' ? ' both' : '') });

    // Try the primary thumbnail; on 404 walk the fallback list. This
    // matters for thermal: seq=1 is often missing because the Lepton
    // hadn't produced its first frame when the first capture committed
    // (~10-15 s warmup post-CCI). For thermal we therefore fall back
    // to the latest committed seq, where the sensor is always running.
    function makeThumb(candidates) {
      const img = el('img', { class: 'thumb' });
      let i = 0;
      const tryNext = () => {
        if (i >= candidates.length) {
          const ph = el('div', { class: 'thumb-empty',
            style: 'flex:1;display:flex;align-items:center;justify-content:center' },
            'no preview');
          if (img.parentElement) img.replaceWith(ph);
          return;
        }
        const name = candidates[i++];
        loadAuthImg(img, fileUrl(name), tryNext);
      };
      tryNext();
      return img;
    }

    const last = (typeof s.captureCount === 'number' && s.captureCount > 1)
                 ? pad6(s.captureCount) : null;
    const visCandidates   = ['000001_vis.jpg'].concat(last ? [last + '_vis.jpg'] : []);
    const thermCandidates = ['000001_therm.jpg'].concat(last ? [last + '_therm.jpg'] : []);

    if (s.captureVis !== false && wantVis)   thumbWrap.appendChild(makeThumb(visCandidates));
    if (s.captureTherm !== false && wantTherm) thumbWrap.appendChild(makeThumb(thermCandidates));
    if (!thumbWrap.children.length) {
      thumbWrap.appendChild(el('div', { class: 'thumb-empty' }, 'no preview'));
    }
    // Abandoned = explicitly marked aborted by either the cold-boot
    // wipe path or the recovery sweep. Trumps "incomplete" — once
    // a session is abandoned it isn't going to resume, and showing
    // both badges would just confuse. Reasons are wire-protocol
    // strings from the firmware (session_store.c). Kept as raw text
    // so future reasons surface without a UI update.
    const abandoned   = s.aborted === true;
    const abortReason = abandoned && typeof s.abortedReason === 'string'
                        ? s.abortedReason : null;

    const info = el('div', { class: 'info' },
      el('div', { class: 'title' }, sid),
      el('div', { class: 'row' }, (s.captureCount ?? '?') + ' captures · ' +
        (isTl ? ('every ' + (s.intervalSec ?? '?') + 's · ' + dur) : 'single')),
      tsTxt && el('div', { class: 'row' }, tsTxt),
      el('div', { class: 'badges' },
        isRecording && el('span', { class: 'badge recording' }, '● recording'),
        el('span', { class: 'badge ' + (isTl ? 'tl' : 'single') },
           isTl ? 'timelapse' : 'single'),
        s.captureVis && s.captureTherm
          ? el('span', { class: 'badge' }, 'vis+therm')
          : (s.captureVis ? el('span', { class: 'badge' }, 'vis')
                          : el('span', { class: 'badge' }, 'therm')),
        // Aborted/abandoned trumps "incomplete" (no point showing both).
        abandoned
          ? el('span', { class: 'badge abandoned',
                         title: abortReason ? 'reason: ' + abortReason : 'aborted' },
              abortReason || 'abandoned')
          : (incomplete && !isRecording &&
             el('span', { class: 'badge incomplete' }, 'incomplete'))));
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

  // ─── OTA: upload .bin then fire firmware.update on the device ───
  async function uploadAndUpdateFirmware() {
    if (!currentDeviceId) { setFwStatus('No device connected', 'err'); return; }
    const file = fields.fwInput?.files?.[0];
    if (!file) { setFwStatus('Pick a grasshopper.bin file first', 'err'); return; }
    if (file.size < 32 * 1024 || file.size > 4 * 1024 * 1024) {
      setFwStatus('Bad file size: ' + file.size + ' B', 'err'); return;
    }
    // Hard confirm — OTA WILL reboot the device. Any active TL is
    // interrupted, sensors re-init, ~30-60 s of downtime. The pending
    // image starts in verify-pending state; if it crashes the
    // bootloader rolls back, but the operator should still opt in
    // explicitly. Show the size + name so they see what they're
    // about to flash.
    const sizeKb = (file.size / 1024 | 0);
    const msg = 'Flash ' + file.name + ' (' + sizeKb + ' KB) to the device?\\n\\n' +
      '• Device will reboot after download\\n' +
      '• Any active timelapse / capture will be interrupted\\n' +
      '• Live preview drops for ~30-60 s while the new image boots\\n' +
      '• Bootloader auto-rolls-back if the new image crashes early';
    if (!confirm(msg)) {
      setFwStatus('Cancelled', '');
      return;
    }
    setFwStatus('Uploading ' + sizeKb + ' KB to relay…');
    try {
      const upR = await fetch(
        '/api/devices/' + encodeURIComponent(currentDeviceId) + '/firmware',
        { method: 'POST',
          headers: {
            'Authorization': 'Bearer ' + authToken,
            'Content-Type': 'application/octet-stream',
          },
          body: file });
      if (!upR.ok) {
        const err = await upR.json().catch(() => ({}));
        setFwStatus('Upload failed: ' + (err.error || ('HTTP ' + upR.status)), 'err');
        return;
      }
      const upJ = await upR.json();
      setFwStatus('Uploaded (sha ' + upJ.sha256.slice(0, 12) + '…). Telling device to fetch…');
      // Now trigger the device-side OTA. URL is the public per-device
      // endpoint we just populated. The device fetches via HTTPS and
      // emits cmd.result events with progress; we render those below.
      const url = location.origin + '/firmware/' +
                  encodeURIComponent(currentDeviceId) + '/latest.bin';
      await fetch(
        '/api/devices/' + encodeURIComponent(currentDeviceId) + '/cmd',
        { method: 'POST',
          headers: {
            'Content-Type': 'application/json',
            'Authorization': 'Bearer ' + authToken,
          },
          body: JSON.stringify({ cmd: 'firmware.update', id: 'fwup-' + Date.now(), url }) });
      setFwStatus('Update started — watch progress below…');
    } catch (e) {
      setFwStatus('Network error: ' + (e?.message || e), 'err');
    }
  }
  function setFwStatus(text, kind) {
    if (!fields.fwStatus) return;
    fields.fwStatus.textContent = text;
    fields.fwStatus.className = 'fw-status ' + (kind || '');
  }

  // Confirms then issues DELETE /sessions/:sid. On success, navigates
  // back to Library and triggers a reload so the deleted card vanishes.
  async function deleteSession(sid) {
    if (!currentDeviceId) { toast('No device connected', 'err'); return; }
    // The doubled backslash is intentional: any backslash-n inside
    // this template literal would otherwise become a real newline in
    // the served JS and split the inner string literal in two.
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
        // Detail tile honors libPrefs.modality so a therm-only or
        // therm-preferred session doesn't render rows of "no vis"
        // placeholders. modality='both' picks vis when present
        // (vis is generally more recognizable as a thumbnail),
        // therm otherwise.
        const wantTherm = libPrefs.modality === 'therm';
        const useTherm  = (wantTherm && c.thermOk) ||
                          (libPrefs.modality === 'both' && !c.visOk && c.thermOk) ||
                          (!c.visOk && c.thermOk);
        const useVis    = !useTherm && c.visOk;
        const fileName  = useTherm ? c.thermFile : (useVis ? c.visFile : null);
        if (fileName) {
          // Show a loading skeleton until the blob lands. Without
          // this the tile is just a blank box, which the user can't
          // distinguish from "404 / file missing." On 404, swap the
          // skeleton for a clear "missing" marker. The fileName is
          // shown so the operator can match it to the journal if
          // they need to dig in.
          const skeleton = el('div', { class: 'tile-skeleton' },
            el('div', { class: 'tile-spinner' }),
            el('div', { class: 'tile-loading-label' }, 'loading…'));
          tile.appendChild(skeleton);
          const img = el('img', { style: 'display:none' });
          tile.appendChild(img);
          loadAuthImg(img,
            '/api/devices/' + encodeURIComponent(currentDeviceId) +
            '/sessions/' + encodeURIComponent(sid) +
            '/file/' + fileName,
            (status) => {
              // 404 or other fetch failure — swap the skeleton for a
              // missing-file marker. Includes the seq + filename so
              // the operator knows exactly what didn't come back.
              if (skeleton.parentElement) skeleton.remove();
              if (img.parentElement) img.remove();
              const what = useTherm ? 'thermal' : 'visible';
              tile.appendChild(el('div', { class: 'thumb-empty tile-missing' },
                el('div', { class: 'tile-missing-label' },
                  status === 404 ? '✗ ' + what + ' missing' : '✗ ' + what + ' err ' + status),
                el('div', { class: 'tile-missing-file' }, fileName)));
            });
          // Reveal the image once it loads (replace the skeleton).
          img.onload = () => {
            if (skeleton.parentElement) skeleton.remove();
            img.style.display = '';
          };
        } else {
          tile.appendChild(el('div', { class: 'thumb-empty',
            style: 'display:flex;align-items:center;justify-content:center;height:100%;color:var(--muted);font-size:11px' },
            wantTherm ? 'no therm' : 'no vis'));
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
  // Lightbox zoom: explicit +/− buttons + ctrl-wheel + drag + dblclick.
  // Reset on each image swap (handled inside renderLightbox).
  const lightbox = document.getElementById('lightbox');
  attachZoom(lightbox, document.getElementById('lb-img'), { maxScale: 12 });
  document.getElementById('lb-zin').onclick  = () => lightbox.__zoomIn?.();
  document.getElementById('lb-zout').onclick = () => lightbox.__zoomOut?.();
  document.getElementById('lb-zrst').onclick = () => lightbox.__zoomReset?.();
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

  // Minimum firmware version the deployed dashboard expects. Bump
  // when a UI feature starts depending on metadata the firmware writes
  // (e.g. aborted markers in session.json, deepSleep:{active:false}
  // tick fields, etc). Devices below this version still work, but
  // some UI affordances will be silently incomplete — Library cleanup,
  // sleeping-by-design detection, abandoned filtering, etc.
  //
  // Format: "MAJOR.MINOR.PATCH" — ignores anything after a dash.
  // Default-labeled firmware ("0.1.0-dev") is treated as below
  // minimum on purpose: it's almost always an unflagged local build
  // and the operator should know to set GRASSHOPPER_FW_VERSION.
  const MIN_FW_VERSION = '0.4.2';

  function parseFwVersion(v) {
    if (typeof v !== 'string') return null;
    const m = v.match(/^(\d+)\.(\d+)\.(\d+)/);
    if (!m) return null;
    return { major: +m[1], minor: +m[2], patch: +m[3] };
  }
  function fwBelowMin(deviceFw) {
    const dv = parseFwVersion(deviceFw);
    const mv = parseFwVersion(MIN_FW_VERSION);
    if (!dv || !mv) return false;   // unparseable → don't nag
    if (dv.major !== mv.major) return dv.major < mv.major;
    if (dv.minor !== mv.minor) return dv.minor < mv.minor;
    return dv.patch < mv.patch;
  }

  // Track so we only render once per page lifetime. Banner doesn't
  // self-dismiss when the device upgrades mid-session because that
  // would require monitoring fwVersion changes; the user can just
  // reload after flashing.
  let compatBannerRendered = false;

  function maybeRenderCompatBanner(d) {
    if (compatBannerRendered) return;
    if (!d || typeof d.fwVersion !== 'string') return;
    if (!fwBelowMin(d.fwVersion)) return;
    const slot = document.getElementById('banner-slot');
    if (!slot) return;
    compatBannerRendered = true;
    slot.appendChild(el('div', { class: 'compat-banner' },
      el('strong', {}, '⚠ Device firmware below dashboard expectations. '),
      'Device reports ',
      el('code', {}, d.fwVersion),
      '; dashboard expects ',
      el('code', {}, '>= ' + MIN_FW_VERSION),
      '. The device still works, but some metadata fields are missing: ',
      'abandoned-session filtering, recovery markers, the ',
      el('code', {}, 'deepSleep:{active:false}'),
      ' tick block, and the canonical ',
      'aborted-reason taxonomy may all be silently incomplete. ',
      'Flash ',
      el('code', {}, 'firmware/build/grasshopper.bin'),
      ' (or OTA via Update Firmware below) to clear this banner.'
    ));
  }

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
