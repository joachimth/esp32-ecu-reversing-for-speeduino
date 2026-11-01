# OEM Ignition Logger

Reverse engineer Toyota 4E-FE OEM ECU via ESP32-C5.
Måler tændingsvinkel (°BTDC ved gnisten), dwell, RPM, MAP-tryk, injektor-pulsbredde, tomgangsventil-duty og kølevands-/indsugningstemperatur, mens bilen kører på den originale ECU.

---

## Sådan kommer du i gang

> Følg disse 5 trin. Tag ca. 30–45 minutter inkl. lodning.

### 1. Saml hardwaren

Loggeren sidder **parallelt bag på OEM-ECU'ens stik** (backprobe-nåle eller T-kabel), mens bilen kører på den originale ECU. Den lytter kun og må ikke belaste signalerne.

![Ledningsdiagram 4E-FE → ESP32-C5](docs/wiring-4e-fe.svg)

Du skal bruge:
- 1× **Waveshare ESP32-C5-WIFI6-KIT** (ESP32-C5-WROOM-1 N16R4, USB-C), forsynet fra en **powerbank** (ikke bilens 12 V – undgår stelsløjfer)
- 1× **MAX9924** VR-konditionering (µMAX-10) på loggerprintet, forsynet fra boardets 5V-pin. Konfigureres i Mode A1: ZERO_EN og INT_THRS til 5V, EXT ikke forbundet, BIAS på en 1k/1k-deler (5V/2) med 100 nF + 10 µF. Ingen modstand mellem NE+ og NE−
- Modstande: **2× 10 kΩ + 2× 18 kΩ** (IGT, MAP), **2× 1 MΩ + 2× 2 MΩ** (THW, THA), **2× 33 kΩ + 2× 10 kΩ** (INJ, IAC), **1× 10 kΩ** (pull-up på MAX9924 OUT), **1× 330 Ω** (optokobler-LED)
- **2× 100 nF** (THW, THA ved ADC-pin) og **2× BAT54S** (Schottky) som clamp på INJ og IAC. 1N4148 har ca. 0,7 V spændingsfald og clamper først ved ca. 4 V, over ESP32'ens grænse på 3,6 V
- 1× **PC817 optokobler** (TE1–E1-bro til automatisk kalibrering) og 1× momentan trykknap (CAL)

```
NE (VR-sensor, ± AC):
  A21 NE+ ──► IN+ ┌─────────┐ OUT ──┬──────── GPIO8
  A9  NE− ──► IN− │ MAX9924 │       │
                  └─────────┘     [10k] → 3V3      (MAX9924 VCC = 5V, GND = E1)
  Mode A1: ZERO_EN (6) + INT_THRS (9) → 5V, EXT (8) NC, BIAS (4) = 1k/1k-deler fra 5V + 100 nF/10 µF.
  10k i serie på IN+ og IN−, 470 pF–1 nF mellem dem. Ingen modstand over NE+/NE− (belaster OEM-ECU'en).

IGT1 / PIM (5 V):         THW / THA (NTC, høj impedans):   INJ #10 / RSO (12 V + induktive spidser):
  ECU ──[10k]──┬── GPIO     ECU ──[1M]──┬──────┬── GPIO      ECU ──[33k]──┬────────┬── GPIO
             [18k]                    [2M]   [100n]                     [10k]   BAT54S → 3V3 og GND
               │                        │      │                          │
              GND                      GND    GND                        GND

TE1 (auto-kalibrering):
  GPIO26 ──[330]──► PC817 LED (A→K→GND)      PC817 transistor: C = A2 TE1, E = A24 E1
```

> **NE er en VR-sensor.** Den giver ± vekselspænding, der stiger med omdrejningstallet, og skal gennem en VR-konditionering. En ren modstandsdeler sender negativ spænding ind i GPIO'en. NE− må **ikke** forbindes til stel (det er ECU'ens reference).
>
> **IGT er 5 V.** En 33k/10k-deler giver kun ca. 1,2 V, og ESP'en ser aldrig HIGH. Brug 10k/18k.
>
> **THW/THA** er NTC'er med pull-up i OEM-ECU'en. Med 1 MΩ/2 MΩ belaster loggeren dem under 0,5 %, så OEM-ECU'ens egen måling ikke påvirkes. 100 nF ved ADC-pin'en er nødvendig, fordi kilden er højohmig.

**Påkrævet:**
| Pin    | Signal       | ECU-pin (4E-FE) | Interface                         |
|--------|--------------|-----------------|-----------------------------------|
| GPIO8  | NE crank     | A21 NE+ / A9 NE− | MAX9924, OUT + 10k pull-up til 3V3 |
| GPIO9  | IGT ignition | A6 IGT1 (cyl. 1+4) | 10 k / 18 k (5 V)               |
| GPIO10 | CAL knap     | –               | Direkte til GND (intern pull-up)  |
| GND    | Fælles stel  | A24 E1          | Direkte                           |

**Valgfrit** (kan tilføjes senere – auto-detekteres):
| Pin    | Signal         | ECU-pin (4E-FE)       | Interface                    |
|--------|----------------|-----------------------|------------------------------|
| GPIO4  | MAP sensor     | A4 PIM                | 10 k / 18 k (5 V)            |
| GPIO3  | Kølevand (CLT) | A3 THW                | 1 M / 2 M + 100 nF         |
| GPIO5  | Indsugning (IAT) | A5 THA              | 1 M / 2 M + 100 nF         |
| GPIO24 | Fuel injektor  | A12 #10 (cyl. 1+3)    | 33 k / 10 k + BAT54S clamp   |
| GPIO23 | IAC ventil PWM | A14 RSO               | 33 k / 10 k + BAT54S clamp   |
| GPIO26 | TE1-styring    | A2 TE1 / A24 E1       | PC817 optokobler, 330 Ω      |
| GPIO0  | OLED SDA       | –                     | (ingen – I2C 3.3 V)          |
| GPIO1  | OLED SCL       | –                     | (ingen – I2C 3.3 V)          |

Knock (KNK1) tilsluttes ikke: piezo-sensoren er højimpedant, og en deler dæmper det signal, OEM-ECU'en bruger til bankestyring.

**Ledningsfarver og kontrolmåling (ECU-tilgangsledninger)**

Farver fra Haynes diagram 39, sammenholdt med autoelectric.ru. Identificér altid ledningen på **pin-position** og bekræft med **kontrolmålingen**. Farven er kun en ekstra kontrol: der er fx flere sorte ledninger i stikket (A10 OX, A12 #10, B1 TAC, B9 +B). Stikbilleder er tegnet fra ECU'ens side, så rækkefølgen er spejlvendt, når du arbejder bagfra på ledningsnettets stik.

| ECU-pin | Signal | Farve (Haynes) | autoelectric | Kontrolmåling før splejsning |
|---|---|---|---|---|
| A21 | NE+ | hvid (W) | sort (sw) | **Kilderne er uenige.** ECU frakoblet: 985–1600 Ω mellem A21 og A9 |
| A9 | NE− | skærmet | hvid (ws) | Se A21. Splejs uden at bryde skærmen |
| A6 | IGT1 | gul-grøn (Y-G) | ge-gn | Tomgang: lav DC-middelværdi, under ca. 1 V (korte pulser) |
| A4 | PIM | lysegrøn-rød (LG-R) | hgn-rt | Tænding til, motor stoppet: 3,3–3,9 V. Falder ved tomgang |
| A3 | THW | hvid (W) | ws | Varm motor: 0,2–1,0 V mod E2/E1 |
| A5 | THA | gul-sort (Y-B) | ge-sw | Tænding til, ca. 20 °C: 0,5–3,4 V |
| A12 | #10 injektor 1+3 | sort (B) | sw | Tænding til, motor stoppet: ca. 12 V |
| A14 | RSO tomgangsventil | sort-blå (B-L) | sw-bl | Motor i gang: skiftende middelværdi mellem 0 og 12 V |
| A2 | TE1 | blå-hvid (L-W) | bl-ws | Tænding til: 9–14 V mod stel |
| A24 | E1 stel | brun (BR) | br | 0 V og under 1 Ω mod chassis |

Splejs uden at skære ledningen over (afisoleret "vindue" med lodning eller T-tap), så original-ECU'en kører uforstyrret. Byttes NE+ og NE− på MAX9924, trigger den på den modsatte flanke. Det fjernes af kalibreringen, men giver det ustabil sync, så byt de to.

> **Tip:** Forbind altid ESP32 **GND** til ECU'ens **E1** før noget andet.

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

### 4. Kalibrér

**Tændingsvinkel (automatisk):**
1. Start motoren, og lad den køre, til køleventilatoren har slået til
2. Tryk **Auto-kalibrér** på dashboardet eller kort på **CAL-knappen**
3. Loggeren venter på 3 s stabil tomgang, brokobler TE1–E1 via optokobleren (OEM-ECU'en låser tændingen på 10° BTDC), venter 3 s, måler gnistvinklen i 3 s og slipper TE1 igen
4. Status vises på dashboardet. Offset gemmes i flashen og bevares efter genstart

Kalibreringen afbrydes og TE1 slippes, hvis omdrejningerne forlader tomgang (450–1300 o/min), hvis tændingen ikke låser (spredning over 2°), eller efter 20 s uden stabil tomgang. Uden optokobler: slå en TE1–E1-kontakt til med hånden, før du starter.

**Temperatur (én gang, kold motor):** Lad bilen stå natten over, så kølevand og indsugningsluft har omgivelsestemperatur. Tænding til, motor stoppet. Indtast temperaturen under **Temperatur-kalibrering** og tryk **Kalibrér**. Loggeren beregner OEM-ECU'ens pull-up-modstande ud fra Toyotas NTC-kurve (standard 2,2 kΩ).

**MAP:** Standard-skaleringen er preset'en **Toyota 4E-FE** (1,16 V = 20 kPa, 3,71 V = 105 kPa, ca. 0,03 V/kPa). Det er et startpunkt: med tænding til og motoren stoppet skal MAP vise dagens lufttryk (ca. 100 kPa), og ved varm tomgang 21–48 kPa.

### 5. Brug dashboardet

På dashboardet ser du i realtid:
- **RPM, Advance, Dwell, Sync** – altid synlige
- **MAP, Injektor, IAC, Kølevand, Indsugningsluft** – vises automatisk, når sensoren er forbundet
- **Live grafer** – RPM/ADV/MAP over 60 sek + Advance vs RPM scatter (din OEM tændingsmap)

For at gemme data til analyse:
1. Tryk **Start log** → kør motoren igennem dit ønskede driftspunkt
2. Tryk **Stop log** → tryk **Download CSV**
3. Åbn filen i Excel / Speeduino TunerStudio

**Firmware-opdatering (OTA):** Enheden checker automatisk for nye versioner hver 6. time når den er på WiFi.
Når en opdatering er klar vises et banner i dashboardet - tryk **Installer nu** og enheden opdaterer og genstarter selv (~30 sek).
Manuel upload er også muligt via http://192.168.4.1/update.
Auto-opdatering kan slås fra under Diagnostik i dashboardet.

---

## Pin-oversigt (komplet)

Board: **Waveshare ESP32-C5-WIFI6-KIT** (ESP32-C5-WROOM-1 N16R4). Kilder: ESP-IDF GPIO-oversigt for ESP32-C5, ESP32-C5-datablad, arduino-esp32 `variants/esp32c5` og Waveshares diagram ESP32-C5-WIFI6-KIT-NXRX.

| Pin    | Signal         | Type     | ECU-pin (4E-FE)    | Interface                    | Status                   |
|--------|----------------|----------|--------------------|------------------------------|--------------------------|
| GPIO8  | NE crank       | Digital  | A21 / A9           | MAX9924 + 10k pull-up         | Påkrævet               |
| GPIO9  | IGT ignition   | Digital  | A6 IGT1            | 10 k / 18 k (5 V)            | Påkrævet                 |
| GPIO10 | CAL knap       | Digital  | –                  | Intern pull-up               | Påkrævet                 |
| GPIO4  | MAP sensor     | ADC1_CH3 | A4 PIM             | 10 k / 18 k (5 V)            | Valgfri, auto-detekteret |
| GPIO3  | Kølevand (THW) | ADC1_CH2 | A3 THW             | 1 M / 2 M + 100 nF         | Valgfri, auto-detekteret |
| GPIO5  | Indsugning (THA) | ADC1_CH4 | A5 THA           | 1 M / 2 M + 100 nF         | Valgfri, auto-detekteret |
| GPIO24 | Fuel injektor  | Digital  | A12 #10            | 33 k / 10 k + BAT54S         | Valgfri, auto-detekteret |
| GPIO23 | IAC ventil PWM | Digital  | A14 RSO            | 33 k / 10 k + BAT54S         | Valgfri, auto-detekteret |
| GPIO26 | TE1-styring    | Udgang   | A2 TE1 → A24 E1    | PC817 + 330 Ω                | Valgfri (auto-kalibrering) |
| GPIO0  | OLED SDA       | I2C      | –                  | –                            | Reserveret (OLED p.t. deaktiveret) |
| GPIO1  | OLED SCL       | I2C      | –                  | –                            | Reserveret (OLED p.t. deaktiveret) |

**Pins der ikke må bruges:**
- GPIO15 → SPICS1 (PSRAM chip select). Ikke ført ud på moduler med PSRAM (N16R4/N16R8)
- GPIO16–22 → SPI flash/PSRAM i modulet
- GPIO13/14 → USB-JTAG (USB D−/D+)
- GPIO11/12 → UART0 TX/RX (CH343 USB-seriel)
- GPIO6 → Waveshare-boardets batterimåling (BAT_ADC: 200 kΩ/100 kΩ-deler + 2× 100 nF)
- Strapping-pins: GPIO2, GPIO7, GPIO25, GPIO27, GPIO28 (GPIO27 = RGB-LED, GPIO28 = BOOT-knap). Ingen eksterne signaler her

> OLED display er kodet men deaktiveret. Crashet skyldtes sandsynligvis, at SDA/SCL sad på GPIO21/20, som er flash-pins på ESP32-C5. Pins er nu GPIO0/1, og OLED'en aktiveres, når det er testet over USB.

---

## Toyota 4E-FE projektdata

Mappen [`toyota-4e-fe/`](toyota-4e-fe/) indeholder bilens verificerede ECU-pinout, Speeduino-adapterens diagram og tjek (REV2), Speeduino-tunen, dyno-kørsler, måleplanen for OEM-kortene og et værktøj til at konvertere TunerStudio-logs til CSV.

## Signaloversigt

- **NE**: VR-sensor på et 36-2 triggerhjul, konditioneret af MAX9924 til en digital puls. Første tand efter hullet = tand 0. Offset til TDC findes ved kalibrering.
- **IGT**: Tændingsordre (5 V). Rising = dwell start. Falling = gnist. Krumtappens vinkel registreres i selve IGT-interruptet ved den faldende flanke, så advance er vinklen ved gnisten. IGT1 tænder cyl. 1+4 (wasted spark), én gang pr. omdrejning.
- **MAP**: Analog tryk-sensor (Toyota PIM). Standard-preset: 1,16 V = 20 kPa, 3,71 V = 105 kPa.
- **INJ**: Fuel injektor puls (gruppe #10 = cyl. 1+3). Pulsbredde = indsprøjtningsvarighed.
- **IAC**: PWM signal fra ECU til den 1-spolede rotary solenoid (RSO/RSD). Duty cycle = ventil-åbning %.
- **THW / THA**: NTC'er (Toyota-kurve, Steinhart-Hart) med OEM-ECU'ens pull-up til 5 V. Temperaturen regnes ud fra spændingen og den kalibrerede pull-up.
- **TE1**: Når GPIO26 er HIGH, brokobler optokobleren TE1–E1, og OEM-ECU'en låser tændingen på 10° BTDC. Bruges kun under auto-kalibrering.

## Serial Monitor (115 200 baud)

CSV output på USB:
```
RPM,ADV,DWELL,TOOTH,SYNC
875,10.2,3.14,20.51,1
```

## Web UI simulator (uden ESP32)

Forhåndsvis dashboardet uden hardware - åbn direkte i browseren:

```bash
open sim/index.html
```

<p align="center">
  <img src="sim/screenshot_top.png" width="48%" alt="Dashboard top – RPM, Advance, Dwell, MAP, Injektor, IAC">
  <img src="sim/screenshot_mobile.png" width="28%" alt="Dashboard mobil">
</p>

Screenshots opdateres automatisk ved hvert CI-build. Se [`SCREENSHOTS.md`](SCREENSHOTS.md) for detaljer og manuel procedure.

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
| RPM = 0, Sync = 0 | NE signal når ikke ESP32 | Tjek MAX9924 (5 V forsyning, IN+/IN− på A21/A9, pull-up på OUT til GPIO8) + GND fælles med ECU |
| RPM OK, Sync = 0 | Missing tooth ikke detekteret | Kør motoren over 500 RPM nogle sek. |
| Advance ser forkert ud | Offset ikke kalibreret | Kør Auto-kalibrér ved varm tomgang |
| Auto-kalibrering: "tændingen låste ikke" | TE1 bliver ikke brokoblet | Tjek optokobleren (C på A2, E på A24, LED via 330 Ω fra GPIO26) |
| Temperatur vises ikke | Under 0,05 V eller over 4,9 V | Tjek 1M/2M-deleren og 100 nF på GPIO3/GPIO5 |
| IGT/dwell = 0 | IGT når ikke HIGH-niveau | IGT er 5 V – brug 10k/18k, ikke 33k/10k |
| MAP-kort vises ikke | Sensor ikke aktiv | Sensor skal levere 0.24–3.06 V på GPIO4 |
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
