# OEM Ignition Logger

## Mål
Reverse engineer Toyota 4E-FE OEM ECU via signalanalyse på NE (crank) og IGT (ignition).

## Hardware
- **Board**: ESP32-C5-WROOM-1 (esp32-c5-devkitc-1)

### Pin-oversigt (ESP32-C5)
| Pin    | Signal         | Type      | Beskyttelse           | Bemærkning        |
|--------|----------------|-----------|-----------------------|-------------------|
| GPIO6  | NE crank       | Digital   | 10k/18k deler (5V)    | Påkrævet          |
| GPIO5  | IGT ignition   | Digital   | 33k/10k deler (12V)   | Påkrævet          |
| GPIO10 | CAL knap       | Digital   | Intern pull-up        | Påkrævet          |
| GPIO2  | MAP sensor     | ADC1_CH2  | 10k/18k deler (5V)    | Valgfri, auto-detekteret |
| GPIO15 | Fuel injektor  | Digital   | 33k/10k deler (12V)   | Valgfri, auto-detekteret |
| GPIO23 | IAC ventil PWM | Digital   | 33k/10k deler (12V)   | Valgfri, auto-detekteret |
| GPIO3  | Knock sensor   | ADC1_CH3  | 33k/10k deler (12V)   | Valgfri, altid samplet   |
| GPIO21 | OLED SDA       | I2C       | Ingen                  | Valgfri, auto-detekteret |
| GPIO20 | OLED SCL       | I2C       | Ingen                  | Valgfri, auto-detekteret |

Undgå GPIO9-14 (SPI flash / USB DP/DM), GPIO11/12 (UART0 TX/RX), GPIO18/19 (native USB).

### Spændingsdelere
```
NE / MAP (5V):   SIG ---[10k]---+--- GPIO
                                |
                               [18k]
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
- **Knock**: Analog resonanssignal (knock sensor). 64 ADC-læsninger → peak-to-peak amplitude 0–100%.
- **OLED**: SSD1306 128×64 display. Auto-detekteret via I2C scan på adresse 0x3C ved boot.

## Auto-detektion af valgfrie sensorer
| Sensor | Betingelse for "aktiv" |
|--------|------------------------|
| MAP    | ADC-værdi i [300, 3800] (0.24V – 3.06V) |
| INJ    | Puls registreret inden for 2 sekunder |
| IAC    | Puls registreret inden for 2 sekunder |
| Knock  | Altid samplet; niveau vises som 0–100% amplitude |
| OLED   | I2C scan 0x3C ved boot; display aktivt hvis fundet |

Web-dashboardet viser kun sensor-kort når data er validt. Skjules automatisk ved frakobling.

## Formler
```
rawAngle   = tooth * 10 + (dt / toothPeriod) * 10
advance    = calibOffset - rawAngle        [° BTDC]
rpm        = 60_000_000 / (toothPeriodUs * 36)
dwell      = (igtFall_us - igtRise_us) / 1000   [ms]
vGpio      = analogRead(PIN_MAP) * 3.3 / 4095
vSensor    = vGpio * 1.556                        [10k/18k divider kompensation: (10+18)/18]
map_kPa    = mapKpaMin + (vSensor - mapVmin) / (mapVmax - mapVmin) * (mapKpaMax - mapKpaMin)
inj_ms     = (injRise_us - injFall_us) / 1000    [ms]
iac_pct    = iacHighUs / iacPeriodUs * 100       [%]
knock_pct  = (adcMax - adcMin) * 100 / 4095      [0–100%, 64 samples]
```

MAP sensor standard-presets:
| Navn         | Vmin  | Vmax  | kPa min | kPa max |
|--------------|-------|-------|---------|---------|
| Bosch 1-bar  | 0.50V | 4.50V | 10 kPa  | 105 kPa |
| Bosch 2.5-bar| 0.50V | 4.50V | 20 kPa  | 250 kPa |
| GM 3-bar     | 0.50V | 4.50V | 10 kPa  | 300 kPa |
| MPX4115      | 0.20V | 4.80V | 15 kPa  | 115 kPa |

Tand-tæller nulstilles ved missing-tooth: dt > lastPeriod × 1.7.
Frac clampes til [0, 1] og nulstilles hvis motoren er stoppet (dt > period × 3).

## Kalibrering
Tryk GPIO10 kortvarigt ved idle (motor = 10° BTDC, standard Toyota):
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
| `/status`         | GET      | `{offset, synced, logActive, logBytes, fsFree, fsTotal, staConnected, staIP, uptime, freeHeap, apClients, rssi}` |
| `/cal`            | POST     | Auto-kalibrering (kræver sync, valgfri `angle=10`) → `{offset}`     |
| `/cal/set`        | POST     | Manuel offset (form: `offset=215`) → `{offset}`                     |
| `/wifi/config`    | GET      | `{ssid, connected, ip}` for STA-tilstand                            |
| `/wifi/config`    | POST     | Sæt STA credentials (form: `ssid=…&pass=…`) → gemmer i NVS         |
| `/wifi/clear`     | POST     | Ryd STA credentials og nulstil til AP-only                          |
| `/config`         | GET      | `{mapVmin, mapVmax, mapKpaMin, mapKpaMax}` MAP skalering            |
| `/config`         | POST     | Sæt MAP skalering (form: `mapVmin=…&mapVmax=…&mapKpaMin=…&mapKpaMax=…`) → gemt i NVS |
| `/labels`         | GET      | `{map, inj, iac}` sensor-navne                                      |
| `/labels`         | POST     | Sæt sensor-navne (form: `map=…&inj=…&iac=…`) → gemt i NVS          |
| `/knock`          | GET      | `{level, count, threshold}` knock sensor status                     |
| `/knock`          | POST     | Sæt tærskel (`threshold=30`) og/eller nulstil tæller (`reset=1`)    |
| `/log/start`      | POST     | Start LittleFS-logning (append til /ignlog.csv)                     |
| `/log/stop`       | POST     | Stop logning                                                        |
| `/log/clear`      | POST     | Slet /ignlog.csv og nulstil tæller                                  |
| `/log.csv`        | GET      | Download /ignlog.csv direkte fra LittleFS                           |
| `/update`         | GET      | OTA upload formular                                                 |
| `/update`         | POST     | Modtager firmware.bin, flasher + genstarter                         |

## WebSocket JSON-format
```json
{"r":875,"a":10.2,"d":3.14,"t":20,"f":51,"s":1,"sc":120,"m":98.5,"mv":1.234,"i":2.30,"c":45.0,"cf":14.2,"k":12,"kc":3,"kt":30,"lc":150,"la":1,"lb":30720}
```
| Felt | Beskrivelse                                    | -1 = ikke tilgængelig |
|------|------------------------------------------------|-----------------------|
| r    | RPM                                            | —                     |
| a    | Advance °BTDC                                  | —                     |
| d    | Dwell ms                                       | —                     |
| t    | Tand (0–35)                                    | —                     |
| f    | Fraktion × 100                                 | —                     |
| s    | Sync (0/1)                                     | —                     |
| sc   | Sync-tæller (missing-tooth events siden boot)  | —                     |
| m    | MAP kPa (beregnet via konfigureret kurve)      | -1                    |
| mv   | MAP sensor-spænding i V (GPIO × 1.556 divider) | —                     |
| i    | Injektor ms                                    | -1                    |
| c    | IAC duty %                                     | -1                    |
| cf   | IAC frekvens Hz                                | 0 = ikke aktiv        |
| k    | Knock amplitude 0–100% (peak-to-peak, 64 samp)| —                     |
| kc   | Knock-tæller (events over tærskel siden boot)  | —                     |
| kt   | Knock tærskel %                                | —                     |
| lc   | Log-linjer gemt i /ignlog.csv                  | —                     |
| la   | Log aktiv (0/1)                                | —                     |
| lb   | Log fil størrelse i bytes                      | —                     |

## CSV log-format
```
timestamp_ms,rpm,adv_btdc,dwell_ms,tooth,frac,sync,map_kpa,inj_ms,iac_pct
0,875,10.2,3.14,20,51,1,98.5,2.30,45.0
200,880,10.3,3.14,20,48,1,,2.31,
```
Tomme felter = sensor ikke tilsluttet.
**Logfil**: `/ignlog.csv` i LittleFS. Append-mode, overlever genstart. Auto-stop når <1 KB fri.
Flush-interval: hver 5. write. Hentes via GET `/log.csv`. Slettes via POST `/log/clear`.
Første linje i ny fil er metadata-kommentar:
```
# OEM Ignition Logger | offset=215.0 | MAP Bosch=0.50V–4.50V 10–105kPa
```

## Biblioteker
```
esphome/ESPAsyncWebServer-esphome @ ^3.3.0
esphome/AsyncTCP-esphome @ ^2.1.0
olikraus/U8g2 @ ^2.35.30
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

Web Tools manifest-offsets (decimal) – ESP32-C5:
- bootloader.bin  → 8192   (0x2000, C5 bootloader starter her – ikke 0x1000 som ESP32)
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

sim/
  index.html            simuleret dashboard – ingen ESP32 nødvendig (åbn direkte i browser)
  screenshot.js         Playwright-script til automatiske screenshots af dashboardet

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

## Web UI simulation (uden ESP32)
```bash
# Åbn dashboardet direkte i browser (ingen server nødvendig)
open sim/index.html

# Tag screenshots automatisk med Playwright
node sim/screenshot.js
# → gemmer: screenshot_top.png, screenshot_bottom.png, screenshot_mobile.png, screenshot_full.png
```

`sim/index.html` er identisk med `firmware/data/index.html` men med WebSocket erstattet af en
JavaScript-datagenerator der producerer realistiske motordata:
- RPM ~875 omdrejninger (varierer sinusformet)
- Advance ~10.2° BTDC, Dwell ~3.14 ms
- MAP ~98.5 kPa (Bosch 1-bar skalering)
- Injektor ~2.30 ms, IAC ~45% duty @ 14.2 Hz
- Knock amplitude ~8% (under tærskel)

Simulatoren kører 120 ticks ved opstart så grafer og scatter-plot er udfyldt inden siden vises.
Alle UI-sektioner, konfigurationspaneler og knapper fungerer visuelt (API-kald er stubbe).
Nyttig til UI-udvikling, præsentation og screenshots uden bil eller hardware.

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
- [x] MAP-sensor konfiguration (custom kPa/V kurve, 4 presets + manuel, NVS-gemt, live V-visning)
- [x] Konfigurationspanel: sensor-labels (brugerdefinerede navne for MAP/INJ/IAC, NVS-gemt)
- [x] IAC frekvens-visning (Hz ved siden af duty %, hjælper identifikation af solenoid vs stepper)
- [x] Session min/max tracking på alle gauge-kort (reset-knap i header)
- [x] System diagnostik-panel (oppetid, fri heap, AP klienter, STA RSSI, flash-plads) @ 10s
- [x] MAP ADC 8× oversampling (reducerer ESP32 ADC-støj med ~3×)
- [x] Sync-tæller (sc) i WebSocket – viser missing-tooth events siden boot i tooth-sektionen
- [x] CSV metadata-kommentar i logfil-header (offset + MAP skalering)
- [x] Advance vs RPM scatter plot (live OEM tændingsmap, 2000 pt buffer, orange trendlinje)
- [x] MAP vs RPM scatter (toggle ADV/MAP, vises automatisk ved MAP-tilslutning)
- [x] Scatter CSV eksport (client-side download: rpm,adv_btdc,map_kpa)
- [x] Dwell i grader (°) beregnet fra dwell_ms × RPM, vist i dwell-kort)
- [x] Skærmvågen via Wake Lock API (forhindrer telefon i at slukke under monitorering)
- [x] Konfigurerbar kalibreringsvinkel (POST /cal angle=X, default 10° BTDC)
- [x] Knock sensor (GPIO33, peak-to-peak amplitude 64 ADC-reads, tærskel NVS-gemt, dashboard bar)
- [x] OLED SSD1306 128×64 (GPIO21/22 I2C, auto-detekteret, RPM/ADV/DWL/MAP/INJ/IAC + tand-bar)
- [ ] IAC stepper decodning (Toyota 4E-FE bruger 4-wire stepper, ikke simpel PWM)

### v2 – OLED standalone display
- [x] SSD1306 128x64 OLED via I2C (GPIO21=SDA, GPIO20=SCL) – auto-detekteret ved boot
- [x] Live gauge ved bilen uden PC/telefon:
  ```
  RPM: 875   ADV: 10.1°
  DWL: 3.0ms SYNC OK
  MAP: 98kPa INJ: 2.3ms
  [===========] T20
  ```
- [x] Tand-bar (0–360° position vist som horisontal bar nederst)
- [x] Knock indikator (lille bar øverst til højre når level > 5%)
- [ ] Kalibrerings-menu på knap

### v3 – Speeduino integration
- [ ] Send timing-offset til Speeduino via UART
- [ ] Bidirectional: modtag VE/timing tables fra Speeduino
- [ ] Auto-tuning forslag baseret på logged MAP + advance data
