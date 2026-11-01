# OEM Ignition Logger

## Mål
Reverse engineer Toyota 4E-FE OEM ECU via signalanalyse på NE (crank) og IGT (ignition), plus MAP, injektor, tomgangsventil og THW/THA-temperaturer.

## Hardware
- **Board**: Waveshare ESP32-C5-WIFI6-KIT (ESP32-C5-WROOM-1 **N16R4**: 16 MB flash, 4 MB PSRAM). PlatformIO-board: `esp32-c5-devkitc-1`.

### Pin-oversigt (ESP32-C5)
| Pin    | Signal         | Type      | Beskyttelse           | Bemærkning        |
|--------|----------------|-----------|-----------------------|-------------------|
| GPIO8  | NE crank       | Digital   | MAX9926 VR-modul, OUT + 10k pull-up til 3V3 | Påkrævet (ECU A21 NE+ / A9 NE−) |
| GPIO9  | IGT ignition   | Digital   | 10k/18k deler (5V)    | Påkrævet (ECU A6 IGT1) |
| GPIO10 | CAL knap       | Digital   | Intern pull-up        | Starter auto-kalibrering |
| GPIO4  | MAP sensor     | ADC1_CH3  | 10k/18k deler (5V)    | Valgfri, auto-detekteret (ECU A4 PIM) |
| GPIO3  | Kølevand (THW) | ADC1_CH2  | 1M/2M deler + 100 nF | Valgfri, auto-detekteret (ECU A3 THW) |
| GPIO5  | Indsugning (THA) | ADC1_CH4 | 1M/2M deler + 100 nF | Valgfri, auto-detekteret (ECU A5 THA) |
| GPIO24 | Fuel injektor  | Digital   | 33k/10k deler + BAT54S clamp (12V) | Valgfri, auto-detekteret (ECU A12 #10) |
| GPIO23 | IAC ventil PWM | Digital   | 33k/10k deler + BAT54S clamp (12V) | Valgfri, auto-detekteret (ECU A14 RSO) |
| GPIO26 | TE1-styring    | Udgang    | PC817 optokobler, 330 Ω | HIGH = TE1–E1 brokoblet (kun under auto-kalibrering) |
| GPIO0  | OLED SDA       | I2C       | Ingen                  | Deaktiveret |
| GPIO1  | OLED SCL       | I2C       | Ingen                  | Deaktiveret |
| –      | Knock          | –         | Ikke tilsluttet (`PIN_KNOCK = -1`) | Deler belaster OEM-ECU'ens piezo-signal |

Pins der **ikke** må bruges (kilder: ESP-IDF GPIO summary for ESP32-C5, ESP32-C5-datablad, arduino-esp32 `variants/esp32c5`, Waveshare-diagram ESP32-C5-WIFI6-KIT-NXRX):
- GPIO15 = SPICS1 (PSRAM chip select) – NC på moduler med PSRAM (N16R4/N16R8)
- GPIO16–22 = SPI flash/PSRAM
- GPIO13/14 = USB-JTAG. GPIO11/12 = UART0 TX/RX (CH343 → Serial)
- GPIO6 = Waveshare BAT_ADC (200k/100k-deler + 2× 100 nF – ødelægger digitale flanker)
- Strapping-pins GPIO2, 7, 25, 27, 28 må ikke drives udefra (GPIO27 = WS2812 RGB-LED, GPIO28 = BOOT-knap)
- ADC1 = GPIO1–6 (CH0–CH5)

### Tilslutning til Toyota 4E-FE (Corolla E11, ECU 89661-1A760)
Loggeren tappes **parallelt bag på OEM-ECU'ens stik**, mens bilen kører på original-ECU'en. Fuldt diagram: `docs/wiring-4e-fe.svg`.

```
NE (VR-sensor, ± AC – aldrig en ren modstandsdeler, NE− må ikke til stel):
  A21 NE+ → MAX9926 IN+,  A9 NE− → MAX9926 IN−,  OUT (open drain) → GPIO8 + 10k pull-up til 3V3
  MAX9926 VCC = boardets 5V-pin, GND = E1

IGT1 (A6) / PIM (A4), 5V:      SIG ---[10k]---+--- GPIO
                                              |
                                             [18k]
                                              |
                                             GND

THW (A3) / THA (A5), NTC:      SIG ---[1M]----+------+--- GPIO     (belastning < 0,5 % på OEM-målingen)
                                              |      |
                                            [2M]   [100n]
                                              |      |
                                             GND    GND

#10 (A12) / RSO (A14), 12V:    SIG ---[33k]---+---+--- GPIO
                                              |   |
                                             [10k] BAT54S → 3V3 / GND (clamp mod 40–80 V induktive spidser)
                                              |
                                             GND

TE1 (A2):  GPIO26 ---[330]--- PC817 LED --- GND ;  PC817 C = A2 TE1, E = A24 E1
```
- **Ledningsfarver (Haynes):** A21 NE+ hvid, A9 NE− skærmet, A6 IGT1 gul-grøn, A4 PIM lysegrøn-rød, A3 THW hvid, A5 THA gul-sort, A12 #10 sort, A14 RSO sort-blå, A2 TE1 blå-hvid, A24 E1 brun. NE-parrets farver er uenige mellem Haynes og autoelectric.ru (A21 sort / A9 hvid). Identificér på pin-position + kontrolmåling (tabel i README).
- **GND:** A24 E1. Forsyning fra powerbank på USB-C, ikke bilens 12 V.
- **Må ikke tilsluttes:** KNK1 (højimpedant piezo).
- IGT er 5 V: en 33k/10k-deler giver kun ~1,2 V og registreres aldrig som HIGH.

## Signallogik
- **NE**: VR-sensor på 36-2 triggerhjul, konditioneret af MAX9926. Første tand efter hullet = tand 0; offset til TDC findes ved kalibrering.
- **IGT**: Ignition command (5 V, IGT1 = cyl. 1+4, wasted spark). Rising = dwell start. Falling = spark. `igtISR()` gemmer tand + brøkdel (heltal, ×1000) i selve interruptet ved den faldende flanke; advance beregnes ud fra den vinkel – ikke ud fra krumtappens position, når loggen sampler.
- **MAP**: Analog tryk-sensor (Toyota PIM). Standard: 1.16V=20kPa, 3.71V=105kPa (Toyota 4E-FE preset, ~0.03 V/kPa).
- **INJ**: Fuel injector puls (gruppe #10 = cyl. 1+3). Falling = start, Rising = slut. Pulsbredde = indsprøjtningsvarighed.
- **IAC**: PWM signal fra ECU til 1-spolet rotary solenoid (RSO/RSD). Duty cycle = ventil-åbning %.
- **THW / THA**: NTC med OEM-ECU'ens pull-up til 5 V. `R = Rpu·V/(5−V)`, temperatur via Steinhart-Hart fitted til Toyotas spec. `Rpu` kalibreres med `/temp/cal` (standard 2200 Ω).
- **TE1**: GPIO26 HIGH → PC817 brokobler TE1–E1 → OEM-ECU'en låser tændingen på 10° BTDC. Kun aktiv under auto-kalibrering; altid LOW ved boot.
- **Knock**: Ikke tilsluttet på 4E-FE (`PIN_KNOCK = -1`, `k = -1` i WebSocket, kortet skjules).
- **OLED**: SSD1306 128×64 display på GPIO0/1. **Deaktiveret** (`oledOk = false`). Crashet skyldtes sandsynligvis, at SDA/SCL tidligere sad på flash-pins (GPIO21/20). Testes over USB før genaktivering.

## Auto-detektion af valgfrie sensorer
| Sensor | Betingelse for "aktiv" |
|--------|------------------------|
| MAP    | ADC-værdi i [300, 3800] (0.24V – 3.06V) |
| INJ    | Puls registreret inden for 2 sekunder |
| IAC    | Puls registreret inden for 2 sekunder |
| THW/THA | Sensor-spænding i [0,05 V, 4,90 V] (ellers `-99`) |
| Knock  | Deaktiveret (`PIN_KNOCK = -1`) |
| OLED   | **Deaktiveret** – `oledOk = false` hardkodet indtil test over USB |

Web-dashboardet viser kun sensor-kort når data er validt. Skjules automatisk ved frakobling.

## Formler
```
sparkAngle = sparkTooth * 10 + sparkFracQ / 100        [° efter tand 0, ved IGT faldende flanke]
             (sparkFracQ = (t_spark - t_tooth) * 1000 / toothPeriod, maks 1000 – 3000 i hullet efter tand 33)
advance    = wrap180(calibOffset - sparkAngle)          [° BTDC]
rpm        = 60_000_000 / (toothPeriodUs * 36)
dwell      = (igtFall_us - igtRise_us) / 1000           [ms]
vGpio      = analogRead(pin) * 3.3 / 4095
vSensor    = vGpio * 1.556                              [MAP 10k/18k: (10+18)/18]
vSensor    = vGpio * 1.5                                [THW/THA 1M/2M: (1+2)/2]
map_kPa    = mapKpaMin + (vSensor - mapVmin) / (mapVmax - mapVmin) * (mapKpaMax - mapKpaMin)
R_ntc      = Rpu * vSensor / (5 - vSensor)
T_ntc      = 1 / (A + B·ln R + C·ln³ R) - 273.15        A=1.219325e-3, B=2.785840e-4, C=3.760559e-8
inj_ms     = (injRise_us - injFall_us) / 1000           [ms]
iac_pct    = iacHighUs / iacPeriodUs * 100              [%]
```

MAP sensor standard-presets:
| Navn         | Vmin  | Vmax  | kPa min | kPa max |
|--------------|-------|-------|---------|---------|
| Toyota 4E-FE (standard) | 1.16V | 3.71V | 20 kPa | 105 kPa |
| Bosch 1-bar  | 0.50V | 4.50V | 10 kPa  | 105 kPa |
| Bosch 2.5-bar| 0.50V | 4.50V | 20 kPa  | 250 kPa |
| GM 3-bar     | 0.50V | 4.50V | 10 kPa  | 300 kPa |
| MPX4115      | 0.20V | 4.80V | 15 kPa  | 115 kPa |

Tand-tæller nulstilles ved missing-tooth: dt > lastPeriod × 1.7. Hullet varer 3 tandperioder (36-2), så `toothPeriodUs` gemmes som `dt / MISSING_GAP_PERIODS` ved hullet.
Frac clampes til [0, 1] og nulstilles hvis motoren er stoppet (dt > period × 4; hullet selv varer 3 perioder). For gnister i hullet efter tand 33 clampes brøken til [0, 3], så vinkler 330–360° ikke klippes.

## Kalibrering

### Tændingsvinkel (auto)
Startes med `POST /cal/auto` (dashboard) eller kort tryk på CAL-knappen (GPIO10). `autoCalStep()` kører hvert 50 ms:

| Tilstand (`ac`) | Betydning | Videre når |
|---|---|---|
| 1 | Venter på stabil tomgang (sync, 450–1300 o/min) | 3 s sammenhængende tomgang → TE1 HIGH |
| 2 | TE1 brokoblet, OEM-ECU låser på 10° | 3 s |
| 3 | Måler gnistvinkel | 3 s, ≥ 10 gnister, spredning ≤ 2° → `calibOffset = middel + calAngle` |
| 4 | OK | – |
| 5 | Fejl (`ae`): 1 ingen stabil tomgang inden 20 s, 2 omdrejninger forlod tomgang, 3 tændingen låste ikke, 4 for få gnister | – |

TE1 slippes altid ved OK, fejl, afbrydelse (`/cal/auto/stop`) og timeout. Gnistvinkler samles i en ringbuffer (48) fra `loop()` og midles cirkulært. `acDelta` = forskel i gnistvinkel før og efter TE1 (logges på serial).
Gemt i NVS: `"offset"` (standard 215) og `"cal_angle"` (standard 10).

`POST /cal` (manuel, TE1 brokoblet med hånden) bruger gnistvinklerne fra de sidste 2 s med samme krav.

### Temperatur
`POST /temp/cal` med `ambient=°C` (kold motor, stået natten over): `Rpu = R_ntc(ambient) · (5 − V) / V` for hver tilsluttet sensor. Gemt i NVS som `"clt_rpu"` og `"iat_rpu"` (standard 2200 Ω).

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
| `/cal`            | POST     | Manuel kalibrering, TE1 brokoblet med hånden (valgfri `angle=10`) → `{offset}` |
| `/cal/auto`       | POST     | Start auto-kalibrering via TE1-optokobler (valgfri `angle=10`)      |
| `/cal/auto/stop`  | POST     | Afbryd auto-kalibrering og slip TE1                                 |
| `/temp`           | GET      | `{clt, iat, cltV, iatV, cltRpu, iatRpu}`                            |
| `/temp/cal`       | POST     | Kalibrér pull-ups ved kendt temperatur (form: `ambient=15`)          |
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
| `/update`         | GET      | OTA upload formular (manuel)                                        |
| `/update`         | POST     | Modtager firmware.bin, flasher + genstarter (manuel)                |
| `/ota/status`     | GET      | `{state, pct, current, available, auto, error}` auto-OTA status     |
| `/ota/check`      | POST     | Tjek manifest.json for ny version nu (blokerende ~1-2 s)            |
| `/ota/update`     | POST     | Download + flash ny firmware fra GitHub releases (blokerende)        |
| `/ota/config`     | POST     | Sæt `auto=1/0` – aktivér/deaktivér automatisk OTA-check (NVS-gemt)  |

## WebSocket JSON-format
```json
{"r":875,"a":10.2,"d":3.14,"t":20,"f":51,"s":1,"sc":120,"m":35.0,"mv":1.600,"i":2.30,"c":45.0,"cf":14.2,"ct":88.5,"it":24.0,"cv":0.612,"iv":2.310,"k":-1,"kc":0,"kt":30,"ac":4,"ae":0,"ao":215.0,"te":0,"lc":150,"la":1,"lb":30720}
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
| ct   | Kølevand °C (THW)                              | -99                   |
| it   | Indsugningsluft °C (THA)                       | -99                   |
| cv   | THW sensor-spænding V                          | —                     |
| iv   | THA sensor-spænding V                          | —                     |
| ac   | Auto-kalibrering tilstand 0–5                  | —                     |
| ae   | Auto-kalibrering fejlkode 0–4                  | —                     |
| ao   | Aktuelt kalibrerings-offset °                  | —                     |
| te   | TE1-udgang (0 fri / 1 brokoblet)               | —                     |
| k    | Knock amplitude 0–100% (peak-to-peak, 64 samp)| -1 (ikke tilsluttet)  |
| kc   | Knock-tæller (events over tærskel siden boot)  | —                     |
| kt   | Knock tærskel %                                | —                     |
| lc   | Log-linjer gemt i /ignlog.csv                  | —                     |
| la   | Log aktiv (0/1)                                | —                     |
| lb   | Log fil størrelse i bytes                      | —                     |

## CSV log-format
```
timestamp_ms,rpm,adv_btdc,dwell_ms,tooth,frac,sync,map_kpa,inj_ms,iac_pct,clt_c,iat_c,te1
0,875,10.2,3.14,20,51,1,35.0,2.30,45.0,88.5,24.0,0
200,880,10.3,3.14,20,48,1,,2.31,,88.5,,0
```
Tomme felter = sensor ikke tilsluttet.
**Logfil**: `/ignlog.csv` i LittleFS. Append-mode, overlever genstart. Auto-stop når <1 KB fri.
Flush-interval: hver 5. write. Hentes via GET `/log.csv`. Slettes via POST `/log/clear`.
Første linje i ny fil er metadata-kommentar:
```
# OEM Ignition Logger | offset=215.0 | MAP MAP=1.16V–3.71V 20–105kPa
```

## Platform & Biblioteker

**Platform**: pioarduino-fork af espressif32 bruges fordi den officielle PlatformIO espressif32 (v7.x) mangler ESP32-C5 support. Konfigureret i `platformio.ini`:
```
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
board     = esp32-c5-devkitc-1
framework = arduino
```

**Biblioteker** (`lib_deps`):
```
mathieucarbou/ESPAsyncWebServer @ ^3.3.0
mathieucarbou/AsyncTCP @ ^3.3.0
olikraus/U8g2 @ ^2.35.30
```
mathieucarbou-forks bruges frem for me-no-dev / esphome-forks da de er aktivt vedligeholdt.
OTA via Arduino `Update`-biblioteket (del af framework). DNS via `DNSServer` (del af framework).

## Partition tabel (min_spiffs.csv)
| Navn    | Type | Offset     | Størrelse |
|---------|------|------------|-----------|
| nvs     | data | 0x9000     | 20 KB     |
| otadata | data | 0xe000     | 8 KB      |
| app0    | app  | 0x10000    | 1872 KB (0x1D4000) |
| app1    | app  | 0x1F0000   | 1872 KB (0x1D4000) |
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
  package.json          devDependencies: playwright (kør `npm install` før screenshot.js)

docs/
  index.html            web flasher (GitHub Pages, ESP Web Tools)
  wiring-4e-fe.svg      ledningsdiagram: Toyota 4E-FE ECU-stik → interface → ESP32-C5
  manifest.json         ESP Web Tools manifest → releases/latest

toyota-4e-fe/
  README.md             bil/ECU-fakta, adapter REV2-tjek, tune-noter, specifikationer
  Plan_maaling_af_OEM-kort.md   måleplan + omregning til VE-/tændingstabeller
  OEM-aflytning_ESP32-C5.html   ledningsdiagram + procedure (standalone side)
  ecu/ adapter/ speeduino/ dyno/ tools/   pinout, adapter-diagram/ODS, tune+logs, dyno, mlg→csv

.github/workflows/
  build.yml             CI: 4 jobs → build / deploy-pages / auto-release (main) / release (tag)
```

`build.yml` jobs (alle binaries bygges med PlatformIO env `esp32c5`):
| Job            | Trigger                     | Funktion                                                                 |
|----------------|-----------------------------|--------------------------------------------------------------------------|
| `build`        | push main, tag `v*`, manuel | Byg firmware + LittleFS, saml binaries, upload som artifact (90 dage)     |
| `deploy-pages` | efter `build`               | Bundl `docs/` + binaries → deploy til GitHub Pages (web flasher)          |
| `auto-release` | push til `main`             | Slet+genskab `latest` release med friske binaries (Octokit + ncipollo)    |
| `release`      | tag-push `v*`               | Navngivet release med auto-genereret changelog                           |

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

# Tag screenshots automatisk med Playwright (første gang: installer afhængigheder)
cd sim && npm install && npx playwright install chromium
node screenshot.js
# → gemmer: screenshot_top.png, screenshot_bottom.png, screenshot_mobile.png, screenshot_full.png
```

`sim/index.html` er identisk med `firmware/data/index.html` men med WebSocket erstattet af en
JavaScript-datagenerator der producerer realistiske motordata:
- RPM ~875 omdrejninger (varierer sinusformet)
- Advance ~10.2° BTDC, Dwell ~3.14 ms
- MAP ~35 kPa (tomgangsvakuum, Toyota 4E-FE skalering)
- Kølevand ~88 °C, indsugningsluft ~24 °C, auto-kalibrering "OK" (offset 215°)
- Injektor ~2.30 ms, IAC ~45% duty @ 14.2 Hz
- Knock skjult (`k = -1`, ikke tilsluttet på 4E-FE)

Simulatoren kører 120 ticks ved opstart så grafer og scatter-plot er udfyldt inden siden vises.
Alle UI-sektioner, konfigurationspaneler og knapper fungerer visuelt (API-kald er stubbe).
Nyttig til UI-udvikling, præsentation og screenshots uden bil eller hardware.

## Release procedure (web flash)
Hvert push til `main` kører `auto-release`-jobbet, der automatisk genopbygger
`latest`-releasen med friske binaries – web flasheren peger altid på `latest`.
En navngivet release med changelog laves ved tag-push:
```bash
git tag v1.0.0
git push origin v1.0.0
# GitHub Actions bygger og opretter navngivet release (job: release) med alle binaries
# Web flasher bruger fortsat "latest" (opdateret af auto-release ved hvert main-push)
```

## GitHub Pages (én gang)
Repo Settings → Pages → **Source: GitHub Actions** → Save.
GitHub Actions workflow'en deployer automatisk `docs/` ved hvert push til main.
Web flasher er live på:
`https://joachimth.github.io/esp32-ecu-reversing-for-speeduino/`

## Kendte begrænsninger / Bugs

| Problem | Detalje | Status |
|---------|---------|--------|
| ~~Advance/RPM forkert i tand 0~~ | ~~Efter hullet blev `toothPeriodUs` sat til hele hullets varighed (3 perioder), så frac/advance/RPM i tand 0 var op til ~6,7° forkerte.~~ | **Fikset** – `dt / MISSING_GAP_PERIODS` ved hullet |
| ~~IGT-deler forkert~~ | ~~Dokumentationen angav 33k/10k for IGT (5 V-signal → ~1,2 V, aldrig HIGH) og en ren deler på NE (VR-sensor).~~ | **Fikset** – 10k/18k på IGT, MAX9926 på NE |
| ~~Advance målt på forkert tidspunkt~~ | ~~Advance blev regnet ud fra krumtappens position, når loggen samplede (hver 200 ms), ikke ved gnisten. CAL gemte den tilfældige tand.~~ | **Fikset** – vinklen fanges i `igtISR()` ved faldende flanke |
| ~~Gnist i 36-2-hullet klippet~~ | ~~Brøken blev clampet til 1, så gnister mellem tand 33 og tand 0 (330–360°) blev registreret som 340°.~~ | **Fikset** – clamp til 3 efter tand 33 |
| ~~INJ på GPIO15~~ | ~~GPIO15 er SPICS1 (PSRAM CS) og ikke ført ud på N16R4-modulet.~~ | **Fikset** – flyttet til GPIO24 |
| ~~NE på GPIO6~~ | ~~På Waveshare-boardet har GPIO6 batterimålingens deler + 200 nF, som runder flankerne.~~ | **Fikset** – NE på GPIO8, IGT på GPIO9 |
| ~~MAP på strapping-pin~~ | ~~MAP sad på GPIO2 (strapping-pin); deleren drev pin'en ved boot.~~ | **Fikset** – flyttet til GPIO4 (ADC1_CH3) |
| ~~OLED på flash-pins~~ | ~~SDA/SCL på GPIO21/20, som er SPI flash på ESP32-C5.~~ | **Fikset** – GPIO0/1. OLED stadig deaktiveret indtil test |
| OLED deaktiveret | `Wire.begin()` crashede ESP32-C5. Oprindeligt tilskrevet legacy I2C-API i arduino-esp32 3.3.8, men SDA/SCL sad på flash-pins (GPIO21/20). `oledOk = false` hardkodet i `setup()`. | Afventer test over USB på GPIO0/1 |
| ~~IAC duty% forkert~~ | ~~`iacISR()` målte edge-to-edge (dvs. low-time), ikke rising-to-rising period. Duty% var kun korrekt ved 50% duty.~~ | **Fikset** – tilføjet `lastIacRiseUs` |
| ~~Sync-flag ryddes ikke~~ | ~~`synced` sættes aldrig `false` igen efter signaltab. Sync-kort viste "OK" og CAL-knap forblev aktiv ved RPM=0.~~ | **Fikset** – `sync=false` i stale-check |
| ~~CAL `delay(1000)`~~ | ~~Tryk på CAL-knap blokerede main loop i 1 sek (WebSocket, DNS, serial).~~ | **Fikset** – non-blocking debounce |

---

## TODO / Roadmap

### v1.x – Forbedringer af eksisterende
- [x] Live grafer i web UI (RPM, ADV, MAP over 60 sek – ren Canvas, ingen biblioteker)
- [x] Kalibrerings-knap i web UI + manuel offset-input (GET /status, POST /cal, POST /cal/set)
- [x] LittleFS-baseret logging (append til /ignlog.csv, overlever genstart, auto-stop ved fuld FS)
- [x] WiFi Station mode – forbind til eksisterende WiFi (AP+STA dual mode, NVS-gemt)
- [x] MAP-sensor konfiguration (custom kPa/V kurve, 5 presets (inkl. Toyota 4E-FE) + manuel, NVS-gemt, live V-visning)
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
- [x] Knock sensor (kode beholdt, men `PIN_KNOCK = -1` på 4E-FE – ikke tilsluttet)
- [~] OLED SSD1306 128×64 (GPIO0=SDA/GPIO1=SCL) – kode implementeret, men **deaktiveret**. Tidligere på GPIO21/20, som er SPI flash på C5 – sandsynlig årsag til crashet. Skal testes over USB før genaktivering
- [x] RPM progress bar (3-farvet grøn/amber/rød bar under RPM-tallet op til 7000)
- [x] Farvekodet Advance-værdi (blå/grøn/orange/amber/rød efter tændings-zone)
- [x] Optagelses-indikator (pulserende rød prik i header ved aktiv logning)
- [x] Knock-kort flash animation ved detektion
- [x] Intet-signal banner med pin-vejledning (vises ved RPM=0 + sync=false)
- [x] Flash-plads visuel statusbar (LittleFS fri/brugt)
- [x] CSV download-filnavn inkl. dato (ignlog_YYYY-MM-DD.csv)
- [x] Canvas resize handler (grafer tegner korrekt efter browser/mobil rotation)
- [x] OTA auto-update: `version.h` stamped af CI, check hvert 6. time via manifest.json, download+flash fra GitHub releases, dashboard banner + progress
- [x] ~~IAC stepper decodning~~ – ikke relevant: Corolla E11 4E-FE (1997+) bruger en 1-spolet rotary solenoid (RSO/RSD) med PWM, som allerede måles korrekt
- [x] Gnistvinkel fanget i IGT-interrupt (advance ved gnisten)
- [x] Auto-kalibrering via TE1-optokobler (GPIO26) med stabilitetstjek og automatisk frigivelse
- [x] THW/THA-temperaturer (højimpedant 1M/2M, Toyota NTC, pull-up-kalibrering)
- [ ] Valgfri INJ2-kanal til #20 (A25, cyl. 2+4) for at bekræfte, at grupperne er ens

### v2 – OLED standalone display
> **Status**: Kode implementeret, men OLED er p.t. deaktiveret (`oledOk = false`). Crashet skyldtes sandsynligvis, at SDA/SCL sad på flash-pins (GPIO21/20). Nu GPIO0/1 – testes over USB før genaktivering.
- [~] SSD1306 128x64 OLED via I2C (GPIO0=SDA, GPIO1=SCL) – kode klar, hardware ikke aktiv
- [~] Live gauge ved bilen uden PC/telefon:
  ```
  RPM: 875   ADV: 10.1°
  DWL: 3.0ms SYNC OK
  MAP: 98kPa INJ: 2.3ms
  [===========] T20
  ```
- [~] Tand-bar (0–360° position vist som horisontal bar nederst)
- [~] Knock indikator (lille bar øverst til højre når level > 5%)
- [ ] Kalibrerings-menu på knap

### v3 – Speeduino integration
- [ ] Send timing-offset til Speeduino via UART
- [ ] Bidirectional: modtag VE/timing tables fra Speeduino
- [ ] Auto-tuning forslag baseret på logged MAP + advance data
