# OEM Ignition Logger

## Mål
Reverse engineer Toyota 4E-FE OEM ECU via signalanalyse på NE (crank) og IGT (ignition).

## Hardware
- **Board**: ESP32 (esp32dev)

### Pin-oversigt
| Pin    | Signal         | Type      | Beskyttelse           | Bemærkning        |
|--------|----------------|-----------|-----------------------|-------------------|
| GPIO25 | NE crank       | Digital   | 10k/20k deler (5V)    | Påkrævet          |
| GPIO26 | IGT ignition   | Digital   | 33k/10k deler (12V)   | Påkrævet          |
| GPIO0  | CAL knap       | Digital   | Intern pull-up        | Påkrævet          |
| GPIO34 | MAP sensor     | ADC       | 10k/20k deler (5V)    | Valgfri, auto-detekteret |
| GPIO35 | Fuel injektor  | Digital   | 33k/10k deler (12V)   | Valgfri, auto-detekteret |
| GPIO32 | IAC ventil PWM | Digital   | 33k/10k deler (12V)   | Valgfri, auto-detekteret |

### Spændingsdelere
```
NE / MAP (5V):   SIG ---[10k]---+--- GPIO
                                |
                               [20k]
                                |
                               GND

IGT / INJ / IAC (12V): SIG ---[33k]---+--- GPIO
                                      |
                                     [10k]
                                      |
                                     GND
```

## Signallogik
- **NE**: Digital 36-2 crank trigger. Missing tooth = 0° reference (TDC).
- **IGT**: Ignition command. Rising = dwell start. Falling = spark / ignition event.
- **MAP**: Analog tryk-sensor. 0.5V=10kPa, 4.5V=105kPa (Bosch 1-bar approx).
- **INJ**: Fuel injector puls. Falling = start, Rising = slut. Pulsbredde = indsprøjtningsvarighed.
- **IAC**: PWM signal fra ECU. Duty cycle = ventil-åbning %.

## Auto-detektion af valgfrie sensorer
| Sensor | Betingelse for "aktiv" |
|--------|------------------------|
| MAP    | ADC-værdi i [300, 3800] (0.24V – 3.06V) |
| INJ    | Puls registreret inden for 2 sekunder |
| IAC    | Puls registreret inden for 2 sekunder |

Web-dashboardet viser kun sensor-kort når data er validt. Skjules automatisk ved frakobling.

## Formler
```
rawAngle   = tooth * 10 + (dt / toothPeriod) * 10
advance    = calibOffset - rawAngle        [° BTDC]
rpm        = 60_000_000 / (toothPeriodUs * 36)
dwell      = (igtFall_us - igtRise_us) / 1000   [ms]
map_kPa    = (v_sensor - 0.5) * 23.75 + 10      [kPa, Bosch 1-bar]
inj_ms     = (injRise_us - injFall_us) / 1000    [ms]
iac_pct    = iacHighUs / iacPeriodUs * 100       [%]
```

Tand-tæller nulstilles ved missing-tooth: dt > lastPeriod × 1.7.
Frac clampes til [0, 1] og nulstilles hvis motoren er stoppet (dt > period × 3).

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
```
- **TOOTH** = `toothCount.frac×100` (f.eks. 20.51 = tand 20, 51% inde i perioden)
- **SYNC** = 1 når missing tooth er fundet, 0 ellers

## WiFi (Access Point + Station + Captive Portal)
| Parameter    | Værdi                     |
|--------------|---------------------------|
| AP SSID      | IgnLogger                 |
| AP Password  | ignition1                 |
| AP IP        | 192.168.4.1               |
| Dashboard    | http://192.168.4.1        |
| OTA          | http://192.168.4.1/update |

Ved tilslutning til AP vises captive portal automatisk på iPhone, Android og Windows.
DNS-server omdirigerer alle forespørgsler til 192.168.4.1.

**WiFi Station mode** (valgfri): SSID+adgangskode gemmes i NVS (nøgler `sta_ssid`, `sta_pass`).
ESP32 kører WIFI_AP_STA – AP er altid aktiv, STA forbinder til eksisterende netværk.
Auto-reconnect hvert 15. sekund hvis konfigureret men frakoblet.

## Web Endpoints
| URL               | Metode   | Funktion                                                            |
|-------------------|----------|---------------------------------------------------------------------|
| `/`               | GET      | Live dashboard (LittleFS)                                           |
| `/ws`             | WS       | WebSocket → JSON @ 5 Hz                                             |
| `/status`         | GET      | `{offset, synced, logActive, logBytes, fsFree, fsTotal, staConnected, staIP}` |
| `/cal`            | POST     | Auto-kalibrering (kræver sync) → `{offset}`                         |
| `/cal/set`        | POST     | Manuel offset (form: `offset=215`) → `{offset}`                     |
| `/wifi/config`    | GET      | `{ssid, connected, ip}` for STA-tilstand                            |
| `/wifi/config`    | POST     | Sæt STA credentials (form: `ssid=…&pass=…`) → gemmer i NVS         |
| `/wifi/clear`     | POST     | Ryd STA credentials og nulstil til AP-only                          |
| `/log/start`      | POST     | Start LittleFS-logning (append til /ignlog.csv)                     |
| `/log/stop`       | POST     | Stop logning                                                        |
| `/log/clear`      | POST     | Slet /ignlog.csv og nulstil tæller                                  |
| `/log.csv`        | GET      | Download /ignlog.csv direkte fra LittleFS                           |
| `/update`         | GET      | OTA upload formular                                                 |
| `/update`         | POST     | Modtager firmware.bin, flasher + genstarter                         |

## WebSocket JSON-format
```json
{"r":875,"a":10.2,"d":3.14,"t":20,"f":51,"s":1,"m":98.5,"i":2.30,"c":45.0,"lc":150,"la":1,"lb":30720}
```
| Felt | Beskrivelse                         | -1 = ikke tilgængelig |
|------|-------------------------------------|-----------------------|
| r    | RPM                                 | —                     |
| a    | Advance °BTDC                       | —                     |
| d    | Dwell ms                            | —                     |
| t    | Tand (0–35)                         | —                     |
| f    | Fraktion × 100                      | —                     |
| s    | Sync (0/1)                          | —                     |
| m    | MAP kPa                             | -1                    |
| i    | Injektor ms                         | -1                    |
| c    | IAC duty %                          | -1                    |
| lc   | Log-linjer gemt i /ignlog.csv       | —                     |
| la   | Log aktiv (0/1)                     | —                     |
| lb   | Log fil størrelse i bytes           | —                     |

## CSV log-format
```
timestamp_ms,rpm,adv_btdc,dwell_ms,tooth,frac,sync,map_kpa,inj_ms,iac_pct
0,875,10.2,3.14,20,51,1,98.5,2.30,45.0
200,880,10.3,3.14,20,48,1,,2.31,
```
Tomme felter = sensor ikke tilsluttet.
**Logfil**: `/ignlog.csv` i LittleFS. Append-mode, overlever genstart. Auto-stop når <1 KB fri.
Flush-interval: hver 5. write. Hentes via GET `/log.csv`. Slettes via POST `/log/clear`.

## Biblioteker
```
esphome/ESPAsyncWebServer-esphome @ ^3.3.0
esphome/AsyncTCP-esphome @ ^2.1.0
```
Esphome-forks bruges frem for me-no-dev da de er aktivt vedligeholdt og kompatible med
nyere ESP-IDF. OTA via Arduino `Update`-biblioteket (del af framework). DNS via `DNSServer`
(del af framework).

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
  src/main.cpp          ISR + WiFi AP + captive portal + WebSocket + OTA + logging
  data/index.html       web dashboard (LittleFS, ~5 KB)
  platformio.ini

docs/
  index.html            web flasher (GitHub Pages, ESP Web Tools)
  manifest.json         ESP Web Tools manifest → releases/latest

.github/workflows/
  build.yml             CI: 3 jobs → build / deploy-pages / release (på tag)
```

## Build & Flash (lokal)
```bash
cd firmware && pio run          # byg firmware
pio run -t buildfs              # byg LittleFS image
pio run -t upload               # flash firmware via USB
pio run -t uploadfs             # flash web UI via USB
pio device monitor              # serial monitor 115200 baud
```

## Release procedure (web flash)
```bash
git tag v1.0.0
git push origin v1.0.0
# GitHub Actions bygger og opretter release med alle binaries
# Web flasher bruger automatisk "latest" release
```

## GitHub Pages (én gang)
Repo Settings → Pages → **Source: GitHub Actions** → Save.
GitHub Actions workflow'en deployer automatisk `docs/` ved hvert push til main.
Web flasher er live på:
`https://joachimth.github.io/esp32-ecu-reversing-for-speeduino/`

---

## TODO / Roadmap

### v1.x – Forbedringer af eksisterende
- [x] Live grafer i web UI (RPM, ADV, MAP over 60 sek – ren Canvas, ingen biblioteker)
- [x] Kalibrerings-knap i web UI + manuel offset-input (GET /status, POST /cal, POST /cal/set)
- [x] LittleFS-baseret logging (append til /ignlog.csv, overlever genstart, auto-stop ved fuld FS)
- [x] WiFi Station mode – forbind til eksisterende WiFi (AP+STA dual mode, NVS-gemt)
- [ ] Konfigurationspanel: MAP-skalering, sensor-labels
- [ ] MAP-sensor konfiguration (custom kPa/V kurve)
- [ ] IAC stepper decodning (Toyota 4E-FE bruger 4-wire stepper, ikke simpel PWM)
- [ ] Knock sensor (analog, spektralanalyse på ADC)

### v2 – OLED standalone display
- [ ] SSD1306 128x64 OLED via I2C (GPIO21=SDA, GPIO22=SCL)
- [ ] Live gauge ved bilen uden PC/telefon:
  ```
  RPM: 875   ADV: 10.1°
  DWL: 3.0ms SYNC OK
  MAP: 98kPa INJ: 2.3ms
  ```
- [ ] Roterende tand-visning eller advance bar-gauge
- [ ] Kalibrerings-menu på knap

### v3 – Speeduino integration
- [ ] Send timing-offset til Speeduino via UART
- [ ] Bidirectional: modtag VE/timing tables fra Speeduino
- [ ] Auto-tuning forslag baseret på logged MAP + advance data
