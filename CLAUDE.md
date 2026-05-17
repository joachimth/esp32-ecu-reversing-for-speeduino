# OEM Ignition Logger

## Mål
Reverse engineer Toyota 4E-FE OEM ECU via signalanalyse på NE (crank) og IGT (ignition).

## Hardware
- **Board**: ESP32 (esp32dev)
- **GPIO25** = NE signal, 5V via 10k/20k spændingsdeler
- **GPIO26** = IGT signal, 5-12V via 33k/10k spændingsdeler (12V → ~2.8V)
- **GPIO0**  = CAL knap (active low, idle kalibrering)

### Spændingsdelere
```
NE (5V):   NE ---[10k]---+--- GPIO25
                         |
                        [20k]
                         |
                        GND

IGT (12V): IGT ---[33k]---+--- GPIO26
                          |
                         [10k]
                          |
                         GND
```

## Signallogik
- **NE**: Digital 36-2 crank trigger. Missing tooth = 0° reference (TDC).
- **IGT**: Ignition command. Rising = dwell start. Falling = spark / ignition event.

## Formler
```
rawAngle   = tooth * 10 + (dt / toothPeriod) * 10
advance    = calibOffset - rawAngle        [° BTDC]
rpm        = 60_000_000 / (toothPeriodUs * 36)
dwell      = (igtFall_us - igtRise_us) / 1000   [ms]
```

## Kalibrering
Tryk GPIO0 kortvarigt ved idle (motor = 10° BTDC, standard Toyota):
```
calibOffset = toothCount * 10 + 10
```
Gemt i NVS (Preferences) under nøglen "offset".

## Serial output (115200 baud)
```
RPM,ADV,DWELL,TOOTH,SYNC
875,10.2,3.14,20.51,1
```

## WiFi
| Parameter | Værdi |
|-----------|-------|
| SSID      | IgnLogger |
| Password  | ignition1 |
| IP        | 192.168.4.1 |
| Dashboard | http://192.168.4.1 |
| OTA       | http://192.168.4.1/update |

## Projekt struktur
```
firmware/
  src/main.cpp          firmwaren
  data/index.html       web dashboard (LittleFS)
  platformio.ini

docs/
  index.html            web flasher (GitHub Pages)
  manifest.json         ESP Web Tools manifest

.github/workflows/
  build.yml             CI: byg + release på tag
```

## Build & Flash
```bash
# Build lokalt
cd firmware && pio run

# Build filesystem
pio run -t buildfs

# Upload via USB
pio run -t upload && pio run -t uploadfs

# Web flash
# Åbn GitHub Pages URL → klik "Install"
```

## Release procedure
```bash
git tag v1.0.0
git push origin v1.0.0
# GitHub Actions bygger og opretter release med binaries
# Web flasher bruger automatisk "latest" release
```

## GitHub Pages
Aktivér i repo Settings → Pages → Deploy from branch → main → /docs

## v2 TODO (OLED standalone display)
- SSD1306 128x64 OLED via I2C (GPIO21=SDA, GPIO22=SCL)
- Live gauge direkte på skærm ved bilen:
  ```
  RPM: 875
  ADV: 10.1°
  DWL: 3.0 ms
  SYNC OK
  ```
- Ingen PC/telefon nødvendig
- Overvej: roterende tand-visning eller advance bar-gauge
