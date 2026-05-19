# OEM Ignition Logger

## Mål
Reverse engineer Toyota 4E-FE OEM ECU via signalanalyse på NE (crank) og IGT (ignition).

## Hardware
- **Board**: ESP32 (esp32dev)
- **GPIO25** = NE signal, 5V via 10k/20k spændingsdeler
- **GPIO26** = IGT signal, 5-12V via 33k/10k spændingsdeler (12V → ~2.8V)
- **GPIO0**  = CAL knap (active low, idle kalibrering, intern pull-up)

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

Tand-tæller (tooth) nulstilles ved missing-tooth detektion: dt > lastPeriod × 1.7.
Fratældelen (frac) clampes til [0, 1] og nulstilles hvis motoren er stoppet (dt > period × 3).

## Kalibrering
Tryk GPIO0 kortvarigt ved idle (motor = 10° BTDC, standard Toyota):
```
calibOffset = toothCount * 10 + 10
```
Gemt i NVS (Preferences) under nøglen `"offset"`. Standardværdi: 215.

## Serial output (115200 baud)
```
RPM,ADV,DWELL,TOOTH,SYNC
875,10.2,3.14,20.51,1
1500,18.1,2.90,19.20,1
2500,31.4,2.60,17.85,1
```
- **TOOTH** = `toothCount.frac×100` (f.eks. 20.51 = tand 20, 51% inde i perioden)
- **SYNC** = 1 når missing tooth er fundet, 0 ellers

## WiFi (Access Point)
| Parameter | Værdi                     |
|-----------|---------------------------|
| SSID      | IgnLogger                 |
| Password  | ignition1                 |
| IP        | 192.168.4.1               |
| Dashboard | http://192.168.4.1        |
| OTA       | http://192.168.4.1/update |

## Web Endpoints
| URL        | Metode   | Funktion                              |
|------------|----------|---------------------------------------|
| `/`        | GET      | Live dashboard (serveret fra LittleFS)|
| `/ws`      | WS       | WebSocket → live CSV data, 5 Hz       |
| `/update`  | GET      | OTA upload formular                   |
| `/update`  | POST     | Modtager firmware.bin, flasher + genstarter |

WebSocket CSV-format: `RPM,ADV,DWELL,TOOTH.FRAC,SYNC`

## Biblioteker
```
esphome/ESPAsyncWebServer-esphome @ ^3.3.0
esphome/AsyncTCP-esphome @ ^2.1.0
```
Esphome-forks bruges frem for me-no-dev originalen da de er aktivt vedligeholdt og
kompatible med nyere ESP-IDF versioner. OTA håndteres direkte via Arduino `Update`-biblioteket
(del af framework) – ingen ekstra dependency.

## Partition tabel (min_spiffs.csv)
| Navn    | Type | Offset     | Størrelse |
|---------|------|------------|-----------|
| nvs     | data | 0x9000     | 20 KB     |
| otadata | data | 0xe000     | 8 KB      |
| app0    | app  | 0x10000    | 1875 KB   |
| app1    | app  | 0x1F0000   | 1875 KB   |
| spiffs  | data | 0x3D0000   | 192 KB    |

Web Tools manifest-offsets (decimal):
- bootloader.bin  → 4096
- partitions.bin  → 32768
- boot_app0.bin   → 57344
- firmware.bin    → 65536
- littlefs.bin    → 3997696

## Projekt struktur
```
firmware/
  src/main.cpp          ISR + WiFi AP + WebSocket + OTA
  data/index.html       web dashboard (LittleFS, ~3 KB)
  platformio.ini

docs/
  index.html            web flasher (GitHub Pages, ESP Web Tools)
  manifest.json         ESP Web Tools manifest → releases/latest

.github/workflows/
  build.yml             CI: 3 jobs → build / deploy-pages / release (på tag)
```

## Build & Flash (lokal)
```bash
# Byg firmware
cd firmware && pio run

# Byg LittleFS image
pio run -t buildfs

# Upload via USB (firmware + filesystem)
pio run -t upload && pio run -t uploadfs

# Serial monitor
pio device monitor
```

## Release procedure (web flash)
```bash
git tag v1.0.0
git push origin v1.0.0
# GitHub Actions bygger og opretter release med binaries
# Web flasher bruger automatisk "latest" release
```

## GitHub Pages (én gang)
Repo Settings → Pages → **Source: GitHub Actions** → Save.
GitHub Actions workflow'en deployer automatisk `docs/` ved hvert push til main.
Web flasher er herefter live på:
`https://joachimth.github.io/esp32-ecu-reversing-for-speeduino/`

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
