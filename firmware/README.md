# Grasshopper firmware (ESP-IDF v5.x)

Phase 2 skeleton. Boots, joins Wi-Fi STA, opens an outbound WSS to the
[debug relay](../relay/), and emits a Hello + Init then a Tick every
1.5 s with static state. No Lepton, no camera, no SD writer yet —
those layer in over phases 3–6.

## Build

```bash
. $HOME/esp/esp-idf/export.sh

cd firmware
idf.py set-target esp32s3
idf.py menuconfig    # Grasshopper → Wi-Fi: set SSID + password
                     # Grasshopper → Debug relay: token, URL (defaults are fine)

idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

If `/dev/ttyACM0` doesn't enumerate (some GOOUUU boards don't), try
`/dev/ttyUSB0` (the CH340 UART bridge); `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`
in `sdkconfig.defaults` may need to flip back to UART.

## Components

```
main/                 boot, app_main, periodic Tick
components/
  proto/              generated from proto/schema.json (do not hand-edit)
  net_wifi/           STA only (AP fallback comes in phase 4)
  net_relay/          outbound WSS to the debug relay
```

Phase 3 adds `hal_lepton` (recycled from Fox), phase 4 the camera +
storage + OLED, phase 5 the state machine + capture, phase 6 timelapse,
phase 7 the LAN HTTP/WS server for the frontend, phase 8 OTA.

## Verifying the round-trip

After flashing, watch the [relay dashboard](https://project-grasshopper-production.up.railway.app/)
— the device should appear with `online` pill within ~10 s of getting
an IP.

Or via curl:

```bash
curl https://project-grasshopper-production.up.railway.app/api/devices
```
