# OEM Ignition Logger

Reverse engineer Toyota 4E-FE OEM ECU via ESP32.
Måler advance (°BTDC), dwell (ms) og RPM ud fra NE og IGT signaler.

## Web Flash

**[&#9889; Flash ESP32 her](https://joachimth.github.io/esp32-ecu-reversing-for-speeduino/)**

Kræver Chrome eller Edge. Klik "Install" og følg guiden.

## Dashboard

Efter flash – tilslut WiFi **IgnLogger** / `ignition1` og åbn:

```
http://192.168.4.1
```

OTA firmware update: `http://192.168.4.1/update`

## Hardware

| Pin    | Signal | Beskyttelse       |
|--------|--------|-------------------|
| GPIO25 | NE     | 10k/20k deler     |
| GPIO26 | IGT    | 33k/10k deler     |
| GPIO0  | CAL    | intern pull-up    |

## Build

```bash
cd firmware
pio run -t upload
pio run -t uploadfs
```

Se [CLAUDE.md](CLAUDE.md) for fuld dokumentation.
