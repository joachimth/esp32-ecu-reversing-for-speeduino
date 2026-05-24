# OEM Ignition Logger

Reverse engineer Toyota 4E-FE OEM ECU via ESP32-C5.
Måler advance (°BTDC), dwell, RPM, MAP-tryk, injektor-pulsbredde og knock ud fra NE og IGT signaler.

---

## Sådan kommer du i gang

> Følg disse 5 trin. Tag ca. 30 minutter inkl. lodning.

### 1. Saml hardwaren

Du skal bruge:
- 1× **ESP32-C5-WROOM-1 DevKitC** (USB-C)
- Modstande: **4× 33 kΩ**, **4× 10 kΩ**, **2× 18 kΩ** (1/4 W)
- 1× momentan trykknap (CAL)
- Ledning til bilens ECU-stik (NE + IGT minimum)

Tilslut signalerne med spændingsdelere så ECU'ens 5 V/12 V signaler bliver til 3.3 V som ESP32 kan tåle:

```
NE / MAP  (5V):                  IGT / INJ / IAC / Knock (12V):

  ECU ───[10kΩ]───┬─── GPIO        ECU ───[33kΩ]───┬─── GPIO
                  │                                 │
                [18kΩ]                            [10kΩ]
                  │                                 │
                 GND                               GND
```

**Påkrævet:**
| Pin    | Signal       | Spændingsdeler        |
|--------|--------------|-----------------------|
| GPIO6  | NE crank     | 10 k / 18 k (5 V)     |
| GPIO5  | IGT ignition | 33 k / 10 k (12 V)    |
| GPIO10 | CAL knap     | Direkte til GND (intern pull-up) |

**Valgfrit** (kan tilføjes senere – auto-detekteres):
| Pin    | Signal         | Spændingsdeler         |
|--------|----------------|------------------------|
| GPIO2  | MAP sensor     | 10 k / 18 k (5 V)      |
| GPIO15 | Fuel injektor  | 33 k / 10 k (12 V)     |
| GPIO23 | IAC ventil PWM | 33 k / 10 k (12 V)     |
| GPIO3  | Knock sensor   | 33 k / 10 k (12 V)     |
| GPIO21 | OLED SDA       | (ingen – I2C 3.3 V)    |
| GPIO20 | OLED SCL       | (ingen – I2C 3.3 V)    |

> **Tip:** Forbind altid ESP32 **GND** til bilens **chassis-jord** før noget andet.

### 2. Flash firmwaren

1. Tilslut ESP32-C5 via USB-C til din computer
2. Åbn **[⚡ Web Flasher](https://joachimth.github.io/esp32-ecu-reversing-for-speeduino/)** i Chrome eller Edge
3. Tryk **CONNECT** → vælg serial port → tryk **INSTALL**

Ingen drivere eller PlatformIO nødvendig.

### 3. Forbind til WiFi

ESP32 starter sit eget WiFi-netværk:

| | |
|--------------|-----------------|
| **SSID**     | `IgnLogger`     |
| **Password** | `ignition1`     |
| **URL**      | http://192.168.4.1 |

Telefon/PC åbner dashboardet automatisk via captive portal. Ellers tast adressen manuelt.

### 4. Kalibrér ved idle

1. Start motoren, lad den varme op til stabil tomgang
2. **Tryk kortvarigt på CAL-knappen** (eller tryk **Kalibrér** på dashboardet)
3. Offset gemmes i flashen og bevares efter genstart

> Standard er 10° BTDC (Toyota idle). Det kan ændres via `/cal/set` eller dashboardet hvis dit setup er anderledes.

### 5. Brug dashboardet

På dashboardet ser du i realtid:
- **RPM, Advance, Dwell, Sync** – altid synlige
- **MAP, Injektor, IAC, Knock** – vises automatisk når sensoren er forbundet
- **Live grafer** – RPM/ADV/MAP over 60 sek + Advance vs RPM scatter (din OEM tændingsmap)

For at gemme data til analyse:
1. Tryk **Start log** → kør motoren igennem dit ønskede driftspunkt
2. Tryk **Stop log** → tryk **Download CSV**
3. Åbn filen i Excel / Speeduino TunerStudio

Firmware-opdateringer kan installeres trådløst via http://192.168.4.1/update (OTA).

---

## Pin-oversigt (komplet)

| Pin    | Signal         | Type     | Beskyttelse           | Status                   |
|--------|----------------|----------|-----------------------|--------------------------|
| GPIO6  | NE crank       | Digital  | 10 k / 18 k (5 V)     | Påkrævet                 |
| GPIO5  | IGT ignition   | Digital  | 33 k / 10 k (12 V)    | Påkrævet                 |
| GPIO10 | CAL knap       | Digital  | Intern pull-up        | Påkrævet                 |
| GPIO2  | MAP sensor     | ADC1_CH2 | 10 k / 18 k (5 V)     | Valgfri, auto-detekteret |
| GPIO15 | Fuel injektor  | Digital  | 33 k / 10 k (12 V)    | Valgfri, auto-detekteret |
| GPIO23 | IAC ventil PWM | Digital  | 33 k / 10 k (12 V)    | Valgfri, auto-detekteret |
| GPIO3  | Knock sensor   | ADC1_CH3 | 33 k / 10 k (12 V)    | Valgfri, altid samplet   |
| GPIO21 | OLED SDA       | I2C      | –                     | Reserveret (OLED p.t. deaktiveret) |
| GPIO20 | OLED SCL       | I2C      | –                     | Reserveret (OLED p.t. deaktiveret) |

**Reserverede pins der ikke må bruges:**
- GPIO12–14 → intern SPI flash i modulet
- GPIO18/19 → native USB D+/D-
- GPIO11 → UART0 TX (Serial monitor)

> OLED display er kodet men deaktiveret pga. en I2C HAL-bug i arduino-esp32 3.3.8 på ESP32-C5. Aktiveres automatisk når upstream-fixet kommer.

---

## Signaloversigt

- **NE**: Digital 36-2 crank trigger. Missing tooth = 0° reference (TDC).
- **IGT**: Tændingsordre. Rising = dwell start. Falling = gnist / ignition event.
- **MAP**: Analog tryk-sensor. 0.5 V = 10 kPa, 4.5 V = 105 kPa (Bosch 1-bar standard).
- **INJ**: Fuel injektor puls. Pulsbredde = indsprøjtningsvarighed.
- **IAC**: PWM signal fra ECU. Duty cycle = ventil-åbning %.
- **Knock**: Analog resonanssignal. Peak-to-peak amplitude over 64 ADC-samples.

## Serial Monitor (115 200 baud)

CSV output på USB:
```
RPM,ADV,DWELL,TOOTH,SYNC
875,10.2,3.14,20.51,1
```

## Web UI simulator (uden ESP32)

Forhåndsvis dashboardet uden hardware:

<p align="center">
  <img src="sim/screenshot_top.png" width="48%" alt="Dashboard top – RPM, Advance, Dwell, MAP, Injektor, IAC">
  <img src="sim/screenshot_mobile.png" width="28%" alt="Dashboard mobil">
</p>

<p align="center">
  <img src="sim/screenshot_bottom.png" width="48%" alt="Dashboard bund – grafer og scatter">
</p>

```bash
# Åbn direkte i browser (ingen server nødvendig)
open sim/index.html

# Tag screenshots via Playwright
node sim/screenshot.js
```

`sim/index.html` er identisk med dashboardet, men WebSocket er erstattet af en JavaScript-generator
der producerer realistiske motordata (RPM ~875, advance ~10°, MAP ~98 kPa, injektor, IAC, knock).

## Byg lokalt (alternativt til Web Flasher)

```bash
cd firmware
pio run               # byg firmware
pio run -t buildfs    # byg LittleFS image
pio run -t upload     # flash firmware via USB
pio run -t uploadfs   # flash web dashboard via USB
pio device monitor    # serial monitor 115200 baud
```

**Platform**: pioarduino-fork af espressif32 kræves (officiel platform mangler ESP32-C5 support).
Se [`firmware/platformio.ini`](firmware/platformio.ini) for detaljer.

## Fejlsøgning

| Symptom | Mulig årsag | Løsning |
|---------|-------------|---------|
| RPM = 0, Sync = 0 | NE signal når ikke ESP32 | Tjek spændingsdeler på GPIO6 + GND fælles med ECU |
| RPM OK, Sync = 0 | Missing tooth ikke detekteret | Kør motoren over 500 RPM nogle sek. |
| Advance ser forkert ud | Offset ikke kalibreret | Tryk CAL ved idle (motoren 10° BTDC) |
| MAP-kort vises ikke | Sensor ikke aktiv | Sensor skal levere 0.24–3.06 V på GPIO2 |
| Kan ikke finde IgnLogger WiFi | ESP32 ikke booted | Tjek USB-strøm, kig efter "AP ready" på serial |
| Browser viser ikke captive portal | Manuel tilgang | Tast http://192.168.4.1 direkte |

## Første opsætning af repo (én gang for vedligeholdere)

### GitHub Pages
`Settings → Pages → Source: GitHub Actions → Save`

### Første release
```bash
git tag v1.0.0
git push origin v1.0.0
```
GitHub Actions bygger automatisk og uploader binaries. Web flasheren peger på `releases/latest`.

---

Se [`CLAUDE.md`](CLAUDE.md) for fuld teknisk dokumentation, API-reference, WebSocket-format, CSV-skema og roadmap.
