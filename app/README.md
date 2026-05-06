# Grasshopper frontend

Phase 2 skeleton. Vite + Preact + TypeScript. Three-tab shell that
connects to the device's LAN WebSocket and dumps the snapshot.

## Dev

```bash
cd app
bun install      # or npm install
bun run dev      # localhost:5173

# point at a real device:
VITE_WS_URL=ws://192.168.1.50/ws bun run dev
```

## Build

```bash
bun run build
# emits dist/
```

Phase 7 will:
- Add `vite-plugin-singlefile` to bundle into one HTML
- Add a postbuild step to gzip → `firmware/main/assets/app.html.gz`
- Wire LittleFS partition → `esp_http_server` to serve it
- Fill in the Live / Capture / Library tabs for real

## Types

The frontend imports `@proto` which resolves to
`../proto/generated/types.ts` via Vite + tsconfig path aliases. Edit
`proto/schema.json` and run `python3 proto/codegen.py` from the repo
root to regenerate.
