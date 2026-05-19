# OEM Ignition Logger

Reverse engineer Toyota 4E-FE OEM ECU via ESP32.
Måler advance (°BTDC), dwell (ms) og RPM ud fra NE (36-2 crank) og IGT (ignition) signaler.

## Web Flash

**[⚡ Flash ESP32 her](https://joachimth.github.io/esp32-ecu-reversing-for-speeduino/)**

Kræver Chrome eller Edge med Web Serial API (USB-kabel til ESP32).
Flasher firmware + web-dashboard i én omgang.

## Live Dashboard

Efter flash – tilslut WiFi og åbn dashboardet:

| Parameter | Værdi |
|-----------|-------|
| WiFi SSID | `IgnLogger` |
| Password  | `ignition1` |
| Dashboard | http://192.168.4.1 |
| OTA update | http://192.168.4.1/update |

Dashboardet viser RPM, Advance °BTDC, Dwell ms og Sync-status via WebSocket (5 Hz).

## Hardware

```
GPIO25 = NE signal    NE ---[10k]---+--- GPIO25
                                    |
                                   [20k]
                                    |
                                   GND

GPIO26 = IGT signal   IGT ---[33k]---+--- GPIO26
                                     |
                                    [10k]
                                     |
                                    GND

GPIO0  = CAL knap     Tryk kortvarigt ved idle → gemmer kalibrering
```

## Kalibrering

Ved første brug ved idle (10° BTDC er Toyota-standard):
1. Start motoren, lad den varme op til idle
2. Tryk kortvarigt på GPIO0 (CAL knap)
3. Offset gemmes i NVS – bevares efter genstart

## Serial Monitor

Tilslut til 115200 baud for CSV-output:

```
RPM,ADV,DWELL,TOOTH,SYNC
875,10.2,3.14,20.51,1
1500,18.1,2.90,19.20,1
```

## Første opsætning (én gang)

### 1. GitHub Pages
`Settings → Pages → Deploy from branch → main → /docs → Save`

### 2. Første release
```bash
git tag v1.0.0
git push origin v1.0.0
```
GitHub Actions bygger automatisk og uploader binaries til releasen.
Web flasheren peger på `releases/latest` og virker herefter.

## Byg lokalt

```bash
cd firmware
pio run -t upload    # flash firmware
pio run -t uploadfs  # flash web dashboard
```

Se [CLAUDE.md](CLAUDE.md) for fuld teknisk dokumentation.
