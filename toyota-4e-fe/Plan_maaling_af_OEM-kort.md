# Plan: Mål den originale ECU's kort og overfør dem til Speeduino

Fabrikskortene findes ikke i nogen af filerne, og ROM'en er ikke udlæst. Der er to veje til at ramme dem. Vej A virker uanset hvilken chip ECU'en har.

## Vej A: Mål original-ECU'en mens bilen kører (anbefalet)

Kør bilen på **original-ECU'en** og aflyt signalerne parallelt bag på OEM-stikket. REV2-adapteren erstatter ECU'en og bruges ikke her.

### Hardware: ESP32-C5-loggeren i dette repo
- NE (A21/A9) via MAX9926, IGT1 (A6), PIM (A4), #10 (A12) og RSO (A14). Se `OEM-aflytning_ESP32-C5.html` og `../docs/wiring-4e-fe.svg`.
- Loggeren beregner RPM, advance, dwell, MAP, injektor-ms og IAC-duty direkte og logger dem til CSV (`/log.csv`).
- **THW/THA og KNK1 må ikke tilsluttes**, fordi de belaster OEM-ECU'ens egne målinger. Mål kun med varm motor (ventilatoren har koblet ind).
- Alternativ til detaljerede bølgeformer: en 8-kanals logikanalysator (FX2 + PulseView) på IGT1, IGT2, #10 og #20 med samme beskyttelse som loggeren.

### Kalibrering af tændingsvinkel
1. Varm motor, tomgang, **S1 slået til (TE1–E1 brokoblet)** → OEM-ECU'en giver fast 10° BTDC (8–12°).
2. Tryk CAL på loggeren. Offset'et (tand × 10 + 10) er vinklen fra 36-2-hullet til TDC og giver også den rigtige `TrigAng` til Speeduino.
3. Slå S1 fra og optag de normale driftspunkter.

### Hvad der optages
- Stationære punkter: tomgang, og 1500–6000 o/min i trin på 500 × gaspositioner fra let til fuld belastning. Bjergkørsel eller bremsning med håndbremsen giver belastning, ellers en dyno. Hvert punkt holdes 3–5 s.
- Fuldgasstræk i 3. gear (giver 100 kPa-rækken).
- Et koldstart-forløb (giver warmup-berigelse, men kun som PW-forhold, da THW ikke kan logges parallelt).
- Loggeren måler kun #10 (cyl. 1+3). Det er nok, fordi grupperne har samme pulsbredde.

### Omregning (script skrives, når der er data)
- **Brændstof:** OEM-pulsbredde − dødtid (målt eller ca. 1,0 ms @ 14 V) → effektiv tid pr. cyklus pr. cylinder (tjek om grupperne skyder 1× eller 2× pr. 720°).
  VE_Speeduino = PW_eff / (reqFuel × MAP/100 × korrektioner). reqFuel holdes som i tunen (12,2 ms), så VE-tabellen passer direkte.
  Bemærk: Ved let belastning trimmer OEM'en via lambda. Det giver ±5–10 % støj, så der tages middelværdi pr. celle.
- **Tænding:** Vinkel mellem IGT-faldende flanke og TDC (via offset fra trin 2) → `advTable1[MAP][RPM]`.
- **Dwell:** IGT-pulsbredde → `dwellrun` og spændingskorrektion.
- **Tomgangsventil:** RSO-duty ved tomgang og A/C → `iacOLPWMVal`.
- Resultatet: VE- og tændingstabel i msq-format til at indlæse i TunerStudio, plus en rapport over, hvilke celler der er målt og hvilke der er interpoleret.

## Vej B: Udlæs ROM'en

Det kræver skarpe fotos af **komponentsiden** af printet (MCU, evt. EPROM/flash, EEPROM). Med en ekstern EPROM er det en almindelig programmer-opgave. Ligger koden i maske-ROM i MCU'en, er det et dyrt sidespor. Selv med et dump skal kortene findes og skaleres i en ukendt Fujitsu Ten-kode, så Vej A er hurtigere til et Speeduino-startpunkt.

## Før noget af dette
Find tappunkterne på pin-position og bekræft hver ledning med kontrolmålingen i hoved-README'en, før du splejser. Inden Speeduino-drift skal REV2-adapteren verificeres med multimeter-tjekket fra IDC-siden (fane "Multimeter IDC-side" i `adapter/Adapter_pinout_4E-FE_Speeduino.ods`).
