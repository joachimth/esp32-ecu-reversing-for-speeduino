# OEM Ignition Logger

Reverse engineer Toyota 4E-FE OEM ECU via ESP32-C5.
Måler advance (°BTDC), dwell, RPM, MAP-tryk, injektor-pulsbredde og knock ud fra NE og IGT signaler.

## Web Flash

**[⚡ Flash ESP32 her](https://joachimth.github.io/esp32-ecu-reversing-for-speeduino/)**

Kræver Chrome eller Edge med Web Serial API (USB-kabel til ESP32).

## Live Dashboard

Tilslut WiFi og åbn dashboardet – vises automatisk som captive portal:

| Parameter  | Værdi |
|------------|-------|
| WiFi SSID  | `IgnLogger` |
| Password   | `ignition1` |
| Dashboard  | http://192.168.4.1 |
| OTA update | http://192.168.4.1/update |

### Funktioner
- **RPM, Advance °BTDC, Dwell ms, Sync** – altid synlige
- **MAP kPa, Injektor ms, IAC %** – vises automatisk ved tilslutning af sensorer
- **Knock sensor** – peak-to-peak amplitude, konfigurerbar tærskel
- **Live grafer** – RPM, ADV, MAP over 60 sek; Advance vs RPM scatter plot
- **Rå datalogning** – Start/Stop, download som `.csv` (LittleFS, append, overlever genstart)
- **OTA firmware update** – ingen USB nødvendig efter første flash

## Hardware (ESP32-C5-WROOM-1)

| Pin    | Signal     | Type     | Spændingsdeler       |
|--------|------------|----------|----------------------|
| GPIO6  | NE crank   | Digital  | 10k/18k (5V→3.3V)   |
| GPIO5  | IGT ignition | Digital | 33k/10k (12V→3.3V)  |
| GPIO10 | CAL knap   | Digital  | Intern pull-up       |
| GPIO2  | MAP sensor | ADC1_CH2 | 10k/18k (5V→3.3V)   |
| GPIO15 | Injektor   | Digital  | 33k/10k (12V→3.3V)  |
| GPIO23 | IAC ventil | Digital  | 33k/10k (12V→3.3V)  |
| GPIO3  | Knock      | ADC1_CH3 | 33k/10k (12V→3.3V)  |
| GPIO21 | OLED SDA   | I2C      | –                    |
| GPIO20 | OLED SCL   | I2C      | –                    |

```
NE / MAP (5V):      SIG ---[10k]---+--- GPIO
                                   |
                                  [18k]
                                   |
                                  GND

IGT / INJ / IAC / Knock (12V):
                    SIG ---[33k]---+--- GPIO
                                   |
                                  [10k]
                                   |
                                  GND
```

Undgå GPIO9–14 (SPI flash), GPIO18/19 (USB D+/D-), GPIO11 (UART0 TX / Serial).

## Kalibrering

1. Start motoren, lad den varme op til idle
2. Tryk kortvarigt på GPIO10 (CAL knap)
3. Offset (standard Toyota: 10° BTDC) gemmes i NVS og bevares efter genstart

## Serial Monitor

115200 baud – CSV output:
```
RPM,ADV,DWELL,TOOTH,SYNC
875,10.2,3.14,20.51,1
```

## Web UI simulation (uden ESP32)

Forhåndsvis og screenshot dashboardet uden hardware via simulatoren i `sim/`:

<p align="center">
  <img src="sim/screenshot_top.png" width="48%" alt="Dashboard top – RPM, Advance, Dwell, MAP, Injektor, IAC">
  <img src="sim/screenshot_mobile.png" width="28%" alt="Dashboard mobil">
</p>

<p align="center">
  <img src="sim/screenshot_bottom.png" width="48%" alt="Dashboard bund – grafer og scatter">
</p>

```bash
# Åbn direkte i browser
open sim/index.html

# Eller tag screenshots med Playwright (kræver Node.js + Playwright installeret)
node sim/screenshot.js
```

`sim/index.html` er en kopi af dashboardet hvor WebSocket-forbindelsen er erstattet af en
JavaScript-generator der producerer realistiske motordata (RPM ~875, advance ~10°, MAP ~98 kPa,
injektor, IAC, knock). Alle sektioner og grafer er fuldt funktionelle.

## Byg lokalt

```bash
cd firmware
pio run               # byg firmware
pio run -t buildfs    # byg LittleFS image
pio run -t upload     # flash firmware via USB
pio run -t uploadfs   # flash web dashboard via USB
pio device monitor    # serial monitor 115200 baud
```

**Platform**: pioarduino-fork af espressif32 kræves (officiel platform mangler ESP32-C5 support).
Se `firmware/platformio.ini` for detaljer.

## Første opsætning (én gang)

### 1. GitHub Pages
`Settings → Pages → Source: GitHub Actions → Save`

### 2. Første release
```bash
git tag v1.0.0
git push origin v1.0.0
```
GitHub Actions bygger automatisk og uploader binaries til releasen.
Web flasheren peger på `releases/latest` og virker herefter.

Se [CLAUDE.md](CLAUDE.md) for fuld teknisk dokumentation, API-reference og roadmap.
