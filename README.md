# Project Grasshopper

Commercial rewrite of [Scout Fox](https://github.com/Jwood1126-coder/thermal-scout-fox).
ESP32-S3 + FLIR Lepton 3.1R + OV2640/OV5640 dual-camera (visible + thermal) field unit.

## Layout

| Path        | What                                                      |
|-------------|-----------------------------------------------------------|
| `firmware/` | ESP-IDF v5.x project — the device firmware                |
| `app/`      | Vite + Preact + TS frontend (gzipped + flashed to LittleFS) |
| `proto/`    | Shared schema (typed JSON) + C/TS codegen                 |
| `relay/`    | Bun + Hono debug sidecar deployed to Railway (opt-in, dev only) |
| `docs/`     | Architecture, hardware, protocol notes                    |

The relay is a debug telescope, not a product feature. Grasshopper itself is fully self-contained and works without any cloud service.

## Status

Phase 0 — repo seeded so Railway can deploy the relay. Architecture doc + firmware skeleton next.
