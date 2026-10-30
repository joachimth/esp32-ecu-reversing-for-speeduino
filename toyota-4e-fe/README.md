# Toyota Corolla E11 1.3 4E-FE → Speeduino

Projektdata for bilen, som loggeren i dette repo skal aflytte. Målet er at måle original-ECU'ens brændstof- og tændingskort og bruge dem som startpunkt for en Speeduino-tune.

Status 30-10-2026: Adapter REV2 er gennemgået og korrekt på papiret. ESP32-loggeren er klar. De originale kort er endnu ikke målt.

## Mappestruktur

```
toyota-4e-fe/
├── README.md                          denne fil
├── OEM-aflytning_ESP32-C5.html        ledningsdiagram, procedure og komponentliste (åbn i browser)
├── Plan_maaling_af_OEM-kort.md        plan for måling og omregning til VE-/tændingstabeller
├── ecu/
│   ├── ECU_pinout_4E-FE.csv           verificeret pinout A1–A26 / B1–B16 med farver og Speeduino-pins
│   └── fotos/                         label og printets loddeside
├── adapter/
│   ├── SCH_SpeeduinoAdaptor_Toyota_REV2_2026-10-27.pdf
│   ├── Adapter_pinout_4E-FE_Speeduino.ods   REV2-tjek, multimeter-tjek, korrekt pinout
│   └── Adapter_mapping_REV1_ORIGINAL_fejl-i-A-stik.ods
├── speeduino/
│   ├── CurrentTune_2026-05-20.msq
│   ├── tabelgeneratorer/              TunerStudio IAT- og O2-tabeller
│   └── REV1_adapter_logs_IKKE_GYLDIGE/  april-logs fra REV1 (mlg + konverteret csv)
├── dyno/                              to dyno-kørsler
└── tools/mlg_til_csv.py               konverterer TunerStudio .mlg (v2) til CSV
```

Ikke med i repoet (ophavsret): Haynes-manual, Legion-Avtodata 4E-FE/5E-FE-manual (RU), Toyota 4A-FE repair manual, autoelectric.ru-diagrammer og fremmede ECU-bins. Værdier herfra er citeret med kildeangivelse.

## 1. Bilen og den originale ECU

| Felt | Værdi |
|---|---|
| ECU | Toyota 89661-1A760, Fujitsu Ten 211000-5200, "E3", 4E-FE, uden CAN |
| Bil | Corolla E11 (AE/EE111), EU, 1997+, manuel gearkasse |
| Belastning | Speed-density med vakuumsensor (PIM) |
| Trigger | Kun NE: krumtap-VR med **36-2** (NE-oscillogram, RU-manual s. 58). Ingen knastsensor |
| Indsprøjtning | 2 grupper: **#10 = cyl. 1+3**, **#20 = cyl. 2+4** |
| Tænding | Wasted spark, **IGT1 = cyl. 1+4**, **IGT2 = cyl. 2+3**, IGF-tilbagemelding |
| Basistænding | **8–12° BTDC ved tomgang med TE1–E1 brokoblet** (RU-manual s. 8–9, DIS-2) |
| Øvrigt | Bankesensor, 1 smalbånds-lambda m. varme, 1-spolet tomgangsventil (RSO/RSD), immobiliser |

Pinouten er verificeret mod tre uafhængige kilder: Haynes diagram 39, autoelectric.ru A35 og Legion-Avtodata-stikbilledet for "Corolla 110 eksport 1997, MT".

## 2. Adapter (SpeeduinoAdaptor_Toyota REV2)

Diagrammet er tjekket pin for pin (fane "Adapter REV2 tjek" i ODS'en). **Alle 21 forbindelser er korrekte.** J2 101–126 = Toyota A1–A26, og J2 1–16 = B1–B16. IDC-symbolet "JT REV2" bruger fysisk IDC-nummerering (ulige/lige rækker), og den oversætter rigtigt til Speeduinos v0.4-numre (fx fysisk 1+2 = wiki-pin 1+40 = INJ1).

Bemærkninger:
- **A20 (CF) er ikke forbundet.** Net-et FAN_OUT findes på IDC 29 (Proto 2), men går ikke til J2 pin 120. 4E-FE har en termokontakt til ventilatoren, så ventilatoren kører uden det.
- Tre udgange sidder på andre MOSFET'er end board-default og skal sættes op i TunerStudio:
  - Benzinpumpe (FC) på Boost (Arduino D7). `fuelPumpPin` står pt. på "Board Default".
  - Lambdavarme (HT) på VVT (D4). Kræver programmerbar udgang, og VVT skal være slået fra.
  - Advarselslampe (W) på Idle 2 (D6). Kræver programmerbar udgang, og `iacChannels` skal blive på 1.
- E2 (sensorstel) går på fælles GND. Det virker, men et separat sensorstel giver mindre støj i en senere revision.
- `Adapter_mapping_REV1_ORIGINAL_fejl-i-A-stik.ods` bygger på en ældre 4E-FE-pinout (RSC/IDL-varianten) og er forskudt fra A7. REV2 følger det ikke.
- Fanen "Multimeter IDC-side" måler hele kæden (ledningsnet → adapter → IDC) fra Speeduino-enden, inden REV2 tages i brug.

## 3. Speeduino-tune og logs

- Firmware 2025.01.6, v0.4-board, 36-2, wasted spark, paired injection, MAP-algoritme.
- Der er endnu ingen logs med REV2-adapteren. April-loggene er fra REV1 og er arkiveret som ugyldige.
- Tune-punkter at rette:
  - `inj4CylPairing` = "1+4 & 2+3", men ledningsnettet parrer 1+3 / 2+4. Vælg "1+3 & 2+4", hvis firmwaren har valget.
  - `iacAlgorithm` = On/Off, men ventilen er en PWM rotary solenoid → "PWM Open loop", 1 kanal.
  - `TrigAng` = -90° er ikke verificeret. Den rigtige værdi kommer fra ESP32-loggerens kalibrering (TE1–E1) eller stroboskop.
  - `egoType` = Wide Band, men O2-tabellen er genereret fra `nb.inc` (smalbånd). Det skal passe sammen.
  - Tændingskortet har 48° ved lav belastning og op til 33° ved 100 kPa. Det er aggressivt for en motor uden knock-styring. OEM-intervallet ved 2000 o/min er 13–44°.

## 4. Måling af de originale kort

ESP32-loggeren i dette repo tappes parallelt bag på OEM-stikket. Se `OEM-aflytning_ESP32-C5.html`, `../docs/wiring-4e-fe.svg` og tabellen med ledningsfarver i hoved-README'en. Planen for omregning til VE- og tændingstabeller står i `Plan_maaling_af_OEM-kort.md`.

## 5. Kendte specifikationer (sanity-check)

| Parameter | Værdi | Kilde |
|---|---|---|
| Brændstoftryk | 2,81–2,87 bar (vakuumslange af), 2,27–2,55 bar (på) | Haynes 4A |
| Injektor | 13,4–14,2 Ω, høj-impedans | Haynes 4A |
| Injektionstid | 1,0–3,0 ms (tomgang, 2000, 3000 o/min) | RU s. 59 |
| Tænding | 8–12° (TE1–E1), 5–23° tomgang, 13–44° @2000, 17–44° @3000 (4E-FE 1997) | RU s. 59 |
| MAP | 680–780 mmHg (90–104 kPa) tænding til; 160–360 mmHg (21–48 kPa) tomgang | RU s. 59 |
| Tomgang | 650 o/min (MT) / 700 (AT) | Haynes 4A |
| Tomgangsventil | 17,0–24,5 Ω; PWM, 2,8 ms puls (A/C fra) / 3,5 ms (A/C til) | Haynes / RU s. 59 |
| Krumtapsensor | 985–1600 Ω kold / 1265–1890 Ω varm | Haynes 4A |
| IAT | 13,6–18,4 kΩ @ -20 °C, 2,21–2,69 kΩ @ 20 °C, 0,49–0,67 kΩ @ 60 °C | Haynes 4A |
| Toyota vakuumsensor (4A-FE-reference) | ca. 0,4 V fald pr. 13,3 kPa vakuum (≈ 3 V/100 kPa) | 4A-FE manual |
| Tændingsrækkefølge | 1-3-4-2 | Haynes |

## 6. Dyno

`dyno/`: 95,1 hk / 125,5 Nm (stop ca. 6150 o/min) og 90,9 hk / 121,2 Nm (til ca. 7150 o/min).
