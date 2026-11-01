#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "version.h"

// ─── Pins (Waveshare ESP32-C5-WIFI6-KIT, ESP32-C5-WROOM-1 N16R4) ─────────────
// ESP32-C5 (ESP-IDF GPIO summary, ESP32-C5 datasheet, arduino-esp32 variant,
// Waveshare ESP32-C5-WIFI6-KIT-NXRX schematic):
//   ADC1: GPIO1–6 = ADC1_CH0–CH5 (reliable with WiFi active).
//   GPIO15 = SPICS1 (PSRAM chip select) – NC on modules with PSRAM (N16R4/N16R8).
//   GPIO16–22 = SPI flash/PSRAM – do not use.
//   GPIO13/14 = USB-JTAG (USB D-/D+).   GPIO11/12 = UART0 TX/RX (CH343, Serial).
//   Strapping pins: GPIO2, 7, 25, 27, 28 – keep external drivers off them.
//   Waveshare board: GPIO6 = BAT_ADC (200k/100k divider + 2×100nF) – do not use,
//   GPIO27 = WS2812 RGB LED, GPIO28 = BOOT button.
//
// Interface to the Toyota 4E-FE OEM ECU connector (tapped in parallel, listen only).
// See docs/wiring-4e-fe.svg.
//   NE    → GPIO8   ECU A21 NE+ / A9 NE- → MAX9924 VR conditioner (differential),
//                   open-drain OUT with 10k pull-up to 3V3. NE is a VR sensor
//                   (AC, amplitude grows with RPM) – never a plain resistor divider.
//   IGT   → GPIO9   ECU A6 IGT1, 5V logic → 10k/18k divider
//   INJ   → GPIO24  ECU A12 #10 (cyl 1+3), 12V low side → 33k/10k + BAT54S clamp
//   IAC   → GPIO23  ECU A14 RSO, 12V PWM low side → 33k/10k + BAT54S clamp
//   MAP   → GPIO4   ECU A4 PIM, 0-5V → 10k/18k divider, ADC1_CH3
//   CLT   → GPIO3   ECU A3 THW, 1M/2M divider + 100nF, ADC1_CH2 (high impedance:
//                   does not disturb the OEM ECU's own NTC reading)
//   IAT   → GPIO5   ECU A5 THA, 1M/2M divider + 100nF, ADC1_CH4
//   TE1   → GPIO26  output → PC817 optocoupler LED (330R). Transistor C=A2 TE1,
//                   E=A24 E1. HIGH = TE1 bridged → OEM ECU locks timing at 10° BTDC.
//   CAL   → GPIO10  push button to GND (internal pull-up) → starts auto-calibration
//   SDA   → GPIO0, SCL → GPIO1 (arduino-esp32 default I2C pins for C5)
//   KNOCK → not connected on 4E-FE (a divider loads the OEM piezo signal)
#define PIN_NE     8
#define PIN_IGT    9
#define PIN_INJ   24
#define PIN_IAC   23
#define PIN_MAP    4   // ADC1_CH3
#define PIN_CLT    3   // ADC1_CH2
#define PIN_IAT    5   // ADC1_CH4
#define PIN_TE1   26
#define PIN_CAL   10
#define PIN_KNOCK -1   // not connected
#define PIN_SDA    0
#define PIN_SCL    1

const char* AP_SSID = "IgnLogger";
const char* AP_PASS = "ignition1";

// ─── ISR state ───────────────────────────────────────────────────────────────
volatile uint32_t lastToothUs   = 0;
volatile uint32_t toothPeriodUs = 0;
volatile int      toothCount    = 0;
volatile bool     synced        = false;
volatile uint32_t syncCount     = 0;  // missing-tooth events since boot

volatile uint32_t dwellStartUs  = 0;
volatile uint32_t dwellUs       = 0;

// Spark event (IGT falling edge) captured as crank position: tooth + fraction.
// Integer math only inside ISRs (no soft-float in interrupt context).
volatile int      sparkTooth    = 0;
volatile uint32_t sparkFracQ    = 0;   // fraction of a tooth × 1000 (0–3000 in the gap)
volatile uint32_t sparkUs       = 0;
volatile uint32_t sparkSeq      = 0;

volatile uint32_t injStartUs    = 0;
volatile uint32_t injUs         = 0;
volatile uint32_t lastInjUs     = 0;

volatile uint32_t iacRiseUs     = 0;
volatile float    iacHighUs     = 1.0f;
volatile float    iacPeriodUs   = 0.0f;
volatile uint32_t lastIacEdgeUs = 0;
volatile uint32_t lastIacRiseUs = 0;  // rising-to-rising period tracking (separate from lastIacEdgeUs)

// ─── Config ──────────────────────────────────────────────────────────────────
float  calibOffset  = 215.0f;
String staSSID      = "";
String staPass      = "";
bool   staConnected = false;

// 5V input divider: R1=10k, R2=18k → Vout = Vin × 18/28 → compensate with 28/18
static constexpr float MAP_DIV_COMP = 28.0f / 18.0f;  // ≈ 1.5556

// MAP sensor scaling (sensor voltage → kPa; divider compensated inside readMapKpa)
// Default: Toyota 4E-FE vacuum sensor (PIM), approx. 0.03 V/kPa
// (Toyota spec: ~0.4 V drop per 13.3 kPa vacuum, 3.3–3.9 V at atmosphere).
// Starting point – verify engine-off reading against today's barometric pressure.
float mapVmin   = 1.16f;   // sensor V at min pressure
float mapVmax   = 3.71f;   // sensor V at max pressure
float mapKpaMin = 20.0f;   // kPa at Vmin
float mapKpaMax = 105.0f;  // kPa at Vmax

// 36-2 trigger wheel: the missing-tooth gap spans 3 tooth periods
static constexpr uint32_t MISSING_GAP_PERIODS = 3;
static constexpr int      LAST_TOOTH_BEFORE_GAP = 33;  // teeth 0..33, then the gap

// Coolant / intake air temperature (Toyota NTC on THW/THA, ECU pull-up to 5 V).
// High-impedance divider 1M/2M → Vout = Vin × 2/3 → compensate with 3/2.
// (Max realistic sensor voltage ~4.6 V → 3.07 V at the pin; open sensor 5 V → 3.33 V.)
static constexpr float TEMP_DIV_COMP = 3.0f / 2.0f;
// Steinhart-Hart fitted to Toyota THA/THW spec (Haynes: 16.0k@-20°C, 2.45k@20°C, 0.58k@60°C)
static constexpr float SH_A = 1.219325e-03f, SH_B = 2.785840e-04f, SH_C = 3.760559e-08f;
float cltRpu = 2200.0f;   // OEM ECU pull-up for THW (Ω) – calibrated via /temp/cal
float iatRpu = 2200.0f;   // OEM ECU pull-up for THA (Ω)

// Auto-calibration (TE1–E1 bridge via optocoupler)
float    calAngle    = 10.0f;   // known locked timing with TE1 bridged (° BTDC)
uint8_t  acState     = 0;       // 0 idle,1 waiting for idle,2 TE1 on/settling,3 measuring,4 OK,5 failed
uint8_t  acError     = 0;       // 1 no stable idle,2 rpm left idle window,3 timing not locked,4 too few sparks
uint32_t acStateMs   = 0;
uint32_t acStartMs   = 0;
float    acDelta     = 0.0f;    // change in spark angle when TE1 was bridged (°)

// Sensor labels (user-defined names shown in dashboard)
String lblMap = "MAP";
String lblInj = "Injektor";
String lblIac = "IAC";

Preferences    prefs;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer      dnsServer;

// ─── OLED (SSD1306 128×64, auto-detected via I2C scan) ───────────────────────
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);
bool oledOk = false;

// ─── Knock sensor (disabled when PIN_KNOCK < 0; peak-to-peak amplitude) ──────
static uint8_t  knockLevel = 0;    // 0–100 % peak-to-peak of last window
static uint32_t knockCount = 0;    // knock events above threshold since boot
static uint8_t  knockThresh = 30;  // threshold % (NVS-stored)

// ─── LittleFS logging ────────────────────────────────────────────────────────
File        logFile;
static int  logCount  = 0;
static int  logBytes  = 0;
static bool logActive = false;

// ─── OTA auto-update ─────────────────────────────────────────────────────────
// Manifest URL: stamped with version by CI, checked to detect new firmware.
// Firmware URL: GitHub releases "latest" tag – always points to newest build.
// Security note: setInsecure() skips TLS cert verification. Acceptable for
// a LAN-only embedded device where firmware authenticity is ensured by the
// GitHub release pipeline. For production with internet exposure, pin the
// GitHub root CA instead.
#define OTA_MANIFEST_URL \
    "https://joachimth.github.io/esp32-ecu-reversing-for-speeduino/manifest.json"
#define OTA_FIRMWARE_URL \
    "https://github.com/joachimth/esp32-ecu-reversing-for-speeduino/releases/latest/download/firmware.bin"
#define OTA_CHECK_INTERVAL_MS  (6UL * 3600000UL)  // 6 hours

// States: 0=idle, 1=checking, 2=available, 3=updating, 4=done, 5=error
static uint8_t  otaState       = 0;
static uint8_t  otaProgress    = 0;   // 0–100 %
static String   otaAvailVer    = "";
static String   otaError       = "";
static bool     otaAutoEnabled = true;  // can be toggled via /ota/config

// FreeRTOS task handle + command flag.
// loop() and HTTP handlers set otaTaskCmd and notify the task – they never
// call blocking OTA functions directly (that caused watchdog resets).
// 0 = idle, 1 = check for update, 2 = perform update
static TaskHandle_t     otaTaskHandle = NULL;
static volatile uint8_t otaTaskCmd   = 0;

// ─── ISRs ────────────────────────────────────────────────────────────────────
void IRAM_ATTR crankISR()
{
    uint32_t now = micros(), dt = now - lastToothUs;
    if (toothPeriodUs > 0) {
        if (dt > toothPeriodUs * 1.7f) {
            toothCount = 0; synced = true; syncCount = syncCount + 1;
            // The gap is 3 tooth periods long. Store one normal period so that
            // frac/advance/RPM in tooth 0 are not off by a factor of 3 (up to ~6.7°).
            dt /= MISSING_GAP_PERIODS;
        }
        else { int tc = toothCount + 1; toothCount = (tc >= 36) ? 0 : tc; }
    }
    toothPeriodUs = dt; lastToothUs = now;
}
void IRAM_ATTR igtISR()
{
    uint32_t now = micros();
    if (digitalRead(PIN_IGT)) { dwellStartUs = now; return; }
    // Falling edge = spark. Record dwell and the crank position at this instant.
    dwellUs = now - dwellStartUs;
    uint32_t tp = toothPeriodUs;
    if (!synced || tp == 0) return;
    int      tc   = toothCount;
    uint32_t q    = (uint32_t)((uint64_t)(now - lastToothUs) * 1000ULL / tp);
    uint32_t qMax = (tc == LAST_TOOTH_BEFORE_GAP) ? 1000UL * MISSING_GAP_PERIODS : 1000UL;
    if (q > qMax) q = qMax;
    sparkTooth = tc; sparkFracQ = q; sparkUs = now; sparkSeq = sparkSeq + 1;
}
void IRAM_ATTR injISR()
{
    uint32_t now = micros();
    if (!digitalRead(PIN_INJ)) injStartUs = now;
    else { injUs = now - injStartUs; lastInjUs = now; }
}
void IRAM_ATTR iacISR()
{
    uint32_t now = micros();
    if (digitalRead(PIN_IAC)) {
        // Rising edge: measure period rising-to-rising (not edge-to-edge)
        if (lastIacRiseUs) iacPeriodUs = now - lastIacRiseUs;
        lastIacRiseUs = now;
        iacRiseUs = now;
    } else {
        // Falling edge: high-time = time since rising edge
        iacHighUs = now - iacRiseUs;
    }
    lastIacEdgeUs = now;
}

// ─── Sensor helpers ──────────────────────────────────────────────────────────
static int readMapADC()
{
    // 8× oversampling reduces ESP32 ADC noise by ~3×
    uint32_t s = 0;
    for (int i=0; i<8; i++) s += analogRead(PIN_MAP);
    return (int)(s >> 3);
}
static float adcToMapKpa(int adc)
{
    float vSensor = adc * 3.3f / 4095.0f * MAP_DIV_COMP;
    float range   = mapVmax - mapVmin;
    if (range < 0.01f) return mapKpaMin;
    return mapKpaMin + (vSensor - mapVmin) / range * (mapKpaMax - mapKpaMin);
}
static float readMapKpa()    { return adcToMapKpa(readMapADC()); }
static bool isMapConnected() { int r=readMapADC(); return r>300&&r<3800; }
static bool isInjActive()    { return (micros()-lastInjUs)    < 2000000UL; }
static bool isIacActive()    { return lastIacEdgeUs&&(micros()-lastIacEdgeUs)<2000000UL; }
static float getIacDuty()    { return iacPeriodUs>0 ? iacHighUs/iacPeriodUs*100.0f : -1.0f; }
static float getIacFreqHz()  { return iacPeriodUs>0 ? 1000000.0f/iacPeriodUs : 0.0f; }

// ─── Temperatures (THW / THA) ─────────────────────────────────────────────────
static float ntcTempC(float r)
{
    float L = logf(r);
    return 1.0f / (SH_A + SH_B * L + SH_C * L * L * L) - 273.15f;
}
static float ntcResForTemp(float tC)
{
    // Inverse of ntcTempC by bisection on log(R); T falls as R rises
    float lo = logf(30.0f), hi = logf(300000.0f);
    for (int i = 0; i < 40; i++) {
        float mid = 0.5f * (lo + hi);
        if (ntcTempC(expf(mid)) > tC) lo = mid; else hi = mid;
    }
    return expf(0.5f * (lo + hi));
}
static float readSensorV(int pin)
{
    uint32_t s = 0;
    for (int i = 0; i < 8; i++) s += analogRead(pin);
    return (s >> 3) * 3.3f / 4095.0f * TEMP_DIV_COMP;
}
// Returns °C, or -99 when the input is not connected / out of range
static float readTempC(int pin, float rpu, float* vOut = nullptr)
{
    float v = readSensorV(pin);
    if (vOut) *vOut = v;
    if (v < 0.05f || v > 4.90f) return -99.0f;
    return ntcTempC(rpu * v / (5.0f - v));
}

// ─── Spark angle ──────────────────────────────────────────────────────────────
// Crank angle of the last spark relative to tooth 0 (°), or -1 if none recently
static float lastSparkAngle(uint32_t* seqOut = nullptr)
{
    noInterrupts();
    int tc = sparkTooth; uint32_t q = sparkFracQ, su = sparkUs, seq = sparkSeq;
    interrupts();
    if (seqOut) *seqOut = seq;
    if (seq == 0 || micros() - su > 500000UL) return -1.0f;
    return tc * 10.0f + q / 100.0f;
}
static float wrap180(float a)
{
    while (a > 180.0f)   a -= 360.0f;
    while (a <= -180.0f) a += 360.0f;
    return a;
}

// Ring buffer of recent spark angles (filled from loop())
static float    spkBuf[48];
static uint32_t spkBufMs[48];
static uint8_t  spkHead = 0, spkCount = 0;
static void collectSpark()
{
    static uint32_t lastSeq = 0;
    uint32_t seq; float ang = lastSparkAngle(&seq);
    if (ang < 0 || seq == lastSeq) return;
    lastSeq = seq;
    spkBuf[spkHead] = ang; spkBufMs[spkHead] = millis();
    spkHead = (spkHead + 1) % 48; if (spkCount < 48) spkCount++;
}
// Circular mean/spread of sparks newer than sinceMs. Returns sample count.
static int sparkStats(uint32_t sinceMs, float& mean, float& spread)
{
    int n = 0; float ref = 0, sum = 0, mn = 1e9f, mx = -1e9f;
    for (int i = 0; i < spkCount; i++) {
        int k = (spkHead + 48 - 1 - i) % 48;
        if ((int32_t)(spkBufMs[k] - sinceMs) < 0) continue;
        if (n == 0) ref = spkBuf[k];
        float d = wrap180(spkBuf[k] - ref);
        sum += d; if (d < mn) mn = d; if (d > mx) mx = d; n++;
    }
    if (n == 0) return 0;
    mean = ref + sum / n; if (mean < 0) mean += 360.0f; if (mean >= 360.0f) mean -= 360.0f;
    spread = mx - mn;
    return n;
}

// ─── Knock sensor ─────────────────────────────────────────────────────────────
static void sampleKnock()
{
    // 64 raw reads → peak-to-peak amplitude as 0-100 %
    // analogRead on ESP32 ≈ 15-25 µs → total ≈ 1-1.6 ms
    if (PIN_KNOCK < 0) { knockLevel = 0; return; }
    int mn = 4095, mx = 0;
    for (int i = 0; i < 64; i++) {
        int r = analogRead(PIN_KNOCK);
        if (r < mn) mn = r;
        if (r > mx) mx = r;
    }
    knockLevel = (uint8_t)constrain((mx - mn) * 100 / 4095, 0, 100);
    if (knockLevel > knockThresh) knockCount++;
}

// ─── OLED display ─────────────────────────────────────────────────────────────
static void updateOled(float rpm, float adv, float dwell,
                        int tooth, float frac, bool sync,
                        float mapKpa, float injMsV, float iacPct)
{
    if (!oledOk) return;
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    char L[22];

    if (!sync && rpm < 10.0f) {
        u8g2.drawStr(22, 24, "NO SIGNAL");
        u8g2.setFont(u8g2_font_5x7_tf);
        u8g2.drawStr(10, 38, "Connect NE + IGT");
    } else if (!sync) {
        snprintf(L, sizeof(L), "RPM:%-5.0f  SYNC:--", rpm);
        u8g2.drawStr(0, 10, L);
        u8g2.drawStr(16, 30, "Searching...");
    } else {
        // Row 1: RPM + SYNC
        snprintf(L, sizeof(L), "RPM:%-5.0fSYNC:OK", rpm);
        u8g2.drawStr(0, 10, L);
        // Row 2: Advance + Dwell
        snprintf(L, sizeof(L), "ADV:%-4.1f  DWL:%.1f", adv, dwell);
        u8g2.drawStr(0, 22, L);
        // Row 3: MAP (if present)
        if (mapKpa >= 0) {
            snprintf(L, sizeof(L), "MAP:%.1f kPa", mapKpa);
            u8g2.drawStr(0, 34, L);
        }
        // Row 4: INJ + IAC (if present)
        if (injMsV >= 0 || iacPct >= 0) {
            char i2[10]="", c2[8]="";
            if (injMsV >= 0) snprintf(i2, sizeof(i2), "INJ:%.2f", injMsV);
            if (iacPct >= 0) snprintf(c2, sizeof(c2), "IAC:%.0f%%", iacPct);
            snprintf(L, sizeof(L), "%-10s%s", i2, c2);
            u8g2.drawStr(0, 46, L);
        }
        // Tooth bar
        int bw = constrain((int)((tooth * 10.0f + frac * 10.0f) / 360.0f * 126.0f), 0, 126);
        u8g2.drawFrame(0, 55, 128, 9);
        u8g2.drawBox(1, 56, bw, 7);
        u8g2.setFont(u8g2_font_5x7_tf);
        snprintf(L, sizeof(L), "T%d", tooth);
        u8g2.drawStr(115, 63, L);
        // Knock indicator (small bar top-right if active)
        if (knockLevel > 5) {
            int kw = constrain(knockLevel * 30 / 100, 0, 30);
            u8g2.drawFrame(98, 0, 30, 5);
            u8g2.drawBox(99, 1, kw, 3);
        }
    }
    u8g2.sendBuffer();
}

// ─── Core computation ────────────────────────────────────────────────────────
static void computeValues(float& rpm, float& adv, float& dwell,
                           int& tooth, float& frac, bool& sync)
{
    noInterrupts();
    tooth       = toothCount;
    uint32_t tp = toothPeriodUs;
    uint32_t dt = micros() - lastToothUs;
    dwell       = dwellUs / 1000.0f; sync = synced;
    interrupts();
    // Stale signal → clear. The 36-2 gap itself lasts 3 periods, so use 4 to avoid
    // false "stopped" samples taken just before the first tooth after the gap.
    if (tp==0 || dt/tp>4) { frac=0; adv=0; rpm=0; sync=false; return; }  // also clear sync when signal is stale
    frac = (float)dt/tp; if (frac>1.0f) frac=1.0f;
    // Advance = calibration offset minus the crank angle at the last spark
    // (IGT falling edge) – not the crank position at the time of this sample.
    float spk = lastSparkAngle();
    adv  = (spk >= 0) ? wrap180(calibOffset - spk) : 0.0f;
    rpm  = 60000000.0f / (tp*36.0f);
}

// ─── Auto-calibration ─────────────────────────────────────────────────────────
// 1) wait for 3 s stable idle  2) bridge TE1–E1 (optocoupler) and wait 3 s
// 3) measure the spark angle for 3 s – must be stable – then offset = angle + calAngle.
// TE1 is always released on success, failure, abort or timeout.
// Works the same with a manual TE1 switch: bridge it before starting.
static void stopAutoCal(uint8_t st, uint8_t err)
{
    digitalWrite(PIN_TE1, LOW);
    acState = st; acError = err; acStateMs = millis();
    Serial.printf("AUTOCAL: state=%d err=%d offset=%.1f delta=%.1f\n", st, err, calibOffset, acDelta);
}
static void startAutoCal(float angle)
{
    calAngle = constrain(angle, -10.0f, 60.0f);
    digitalWrite(PIN_TE1, LOW);
    acState = 1; acError = 0; acDelta = 0.0f;
    acStartMs = acStateMs = millis();
}
static void autoCalStep()
{
    if (acState < 1 || acState > 3) return;
    uint32_t now = millis();
    float rpm, adv, dwell, frac; int tooth; bool sync;
    computeValues(rpm, adv, dwell, tooth, frac, sync);
    bool idle = sync && rpm > 450.0f && rpm < 1300.0f;
    static float before = -1.0f;
    switch (acState) {
    case 1:   // wait for stable idle
        if (!idle) { acStateMs = now; if (now - acStartMs > 20000UL) stopAutoCal(5, 1); break; }
        if (now - acStateMs >= 3000UL) {
            float m, sp;
            before = (sparkStats(now - 2000UL, m, sp) >= 5) ? m : -1.0f;
            digitalWrite(PIN_TE1, HIGH);
            acState = 2; acStateMs = now;
        }
        break;
    case 2:   // TE1 bridged – let the OEM ECU settle on fixed timing
        if (!idle) { stopAutoCal(5, 2); break; }
        if (now - acStateMs >= 3000UL) { acState = 3; acStateMs = now; }
        break;
    case 3: { // measure
        if (!idle) { stopAutoCal(5, 2); break; }
        if (now - acStateMs < 3000UL) break;
        float mean, spread;
        int n = sparkStats(acStateMs, mean, spread);
        if (n < 10)        { stopAutoCal(5, 4); break; }
        if (spread > 2.0f) { stopAutoCal(5, 3); break; }
        acDelta = (before >= 0) ? wrap180(before - mean) : 0.0f;
        calibOffset = mean + calAngle;
        if (calibOffset >= 360.0f) calibOffset -= 360.0f;
        prefs.putFloat("offset", calibOffset);
        prefs.putFloat("cal_angle", calAngle);
        stopAutoCal(4, 0);
        break; }
    }
}

// ─── Logging helpers ─────────────────────────────────────────────────────────
static bool openLogFile()
{
    bool exists = LittleFS.exists("/ignlog.csv");
    logFile = LittleFS.open("/ignlog.csv", exists ? "a" : "w");
    if (!logFile) return false;
    if (!exists) {
        char hdr[192];
        snprintf(hdr, sizeof(hdr),
            "# OEM Ignition Logger | offset=%.1f | MAP %s=%.2fV–%.2fV %.0f–%.0fkPa",
            calibOffset, lblMap.c_str(), mapVmin, mapVmax, mapKpaMin, mapKpaMax);
        logFile.println(hdr);
        logFile.println("timestamp_ms,rpm,adv_btdc,dwell_ms,tooth,frac,sync,"
                        "map_kpa,inj_ms,iac_pct,clt_c,iat_c,te1");
    }
    return true;
}

static void logSample(float rpm, float adv, float dwell,
                       int tooth, float frac, bool sync)
{
    if (!logActive || !logFile) return;
    if (LittleFS.totalBytes() - LittleFS.usedBytes() < 1024) {
        logActive = false; logFile.flush(); logFile.close();
        Serial.println("LOG: FS fuldt – logning stoppet");
        return;
    }
    int   mAdc   = readMapADC();
    float mapKpa = (mAdc>300&&mAdc<3800) ? adcToMapKpa(mAdc) : -1.0f;
    float injMsV = isInjActive() ? injUs/1000.0f : -1.0f;
    float iacPct = isIacActive() ? getIacDuty() : -1.0f;

    float cltC   = readTempC(PIN_CLT, cltRpu);
    float iatC   = readTempC(PIN_IAT, iatRpu);

    char mS[10]="", iS[10]="", cS[10]="", tS[10]="", aS[10]="";
    if (mapKpa>=0) snprintf(mS,sizeof(mS),"%.1f",mapKpa);
    if (injMsV>=0) snprintf(iS,sizeof(iS),"%.2f",injMsV);
    if (iacPct>=0) snprintf(cS,sizeof(cS),"%.1f",iacPct);
    if (cltC>-90)  snprintf(tS,sizeof(tS),"%.1f",cltC);
    if (iatC>-90)  snprintf(aS,sizeof(aS),"%.1f",iatC);

    char row[160];
    snprintf(row, sizeof(row), "%lu,%.0f,%.1f,%.2f,%d,%d,%d,%s,%s,%s,%s,%s,%d",
             (unsigned long)millis(), rpm, adv, dwell,
             tooth, (int)(frac*100), sync?1:0, mS, iS, cS, tS, aS,
             digitalRead(PIN_TE1) ? 1 : 0);
    int n = logFile.println(row);
    if (n > 0) { logCount++; logBytes += n; }
    static uint8_t fc = 0;
    if (++fc >= 5) { logFile.flush(); fc = 0; }
}

// ─── WebSocket broadcast ─────────────────────────────────────────────────────
static void pushToClients()
{
    float rpm, adv, dwell, frac; int tooth; bool sync;
    computeValues(rpm, adv, dwell, tooth, frac, sync);
    int   mapAdc  = readMapADC();
    bool  mapOk   = (mapAdc > 300 && mapAdc < 3800);
    float mapKpa  = mapOk ? adcToMapKpa(mapAdc) : -1.0f;
    float mapV    = mapAdc * 3.3f / 4095.0f * MAP_DIV_COMP;
    float injMsV  = isInjActive()    ? injUs/1000.0f   : -1.0f;
    float iacPct  = isIacActive()    ? getIacDuty()    : -1.0f;
    float iacFreq = isIacActive()    ? getIacFreqHz()  :  0.0f;
    float cltV, iatV;
    float cltC    = readTempC(PIN_CLT, cltRpu, &cltV);
    float iatC    = readTempC(PIN_IAT, iatRpu, &iatV);
    char buf[420];
    noInterrupts(); uint32_t sc = syncCount; interrupts();
    snprintf(buf, sizeof(buf),
        "{\"r\":%.0f,\"a\":%.1f,\"d\":%.2f,\"t\":%d,\"f\":%d,\"s\":%d,\"sc\":%lu"
        ",\"m\":%.1f,\"mv\":%.3f,\"i\":%.2f,\"c\":%.1f,\"cf\":%.1f"
        ",\"ct\":%.1f,\"it\":%.1f,\"cv\":%.3f,\"iv\":%.3f"
        ",\"k\":%d,\"kc\":%lu,\"kt\":%d"
        ",\"ac\":%d,\"ae\":%d,\"ao\":%.1f,\"te\":%d"
        ",\"lc\":%d,\"la\":%d,\"lb\":%d}",
        rpm, adv, dwell, tooth, (int)(frac*100), sync?1:0, (unsigned long)sc,
        mapKpa, mapV, injMsV, iacPct, iacFreq,
        cltC, iatC, cltV, iatV,
        PIN_KNOCK < 0 ? -1 : (int)knockLevel, (unsigned long)knockCount, (int)knockThresh,
        (int)acState, (int)acError, calibOffset, digitalRead(PIN_TE1) ? 1 : 0,
        logCount, logActive?1:0, logBytes);
    ws.textAll(buf);
}

// ─── OTA helpers ─────────────────────────────────────────────────────────────
static void otaBroadcast()
{
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"ota\":%d,\"ota_pct\":%d,\"ota_ver\":\"%s\"}",
        (int)otaState, (int)otaProgress, otaAvailVer.c_str());
    ws.textAll(buf);
}

// Returns true if a newer version is available. Blocking but fast (~1-2 s).
static bool otaCheckAvailable()
{
    if (!staConnected) return false;
    otaState = 1; otaError = ""; otaBroadcast();

    WiFiClientSecure cli;
    cli.setInsecure();       // see security note above
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(8000);

    if (!http.begin(cli, OTA_MANIFEST_URL)) {
        otaState = 5; otaError = "connect"; otaBroadcast(); return false;
    }
    int code = http.GET();
    String body = (code == 200) ? http.getString() : "";
    http.end();

    if (code != 200) {
        otaState = 5; otaError = "HTTP " + String(code); otaBroadcast(); return false;
    }

    // Minimal JSON parse: find "version":"xxx"
    int vIdx = body.indexOf("\"version\"");
    if (vIdx < 0) { otaState = 0; otaBroadcast(); return false; }
    int q1 = body.indexOf('"', vIdx + 9);
    int q2 = (q1 >= 0) ? body.indexOf('"', q1 + 1) : -1;
    if (q1 < 0 || q2 < 0) { otaState = 0; otaBroadcast(); return false; }
    String remoteVer = body.substring(q1 + 1, q2);

    if (remoteVer.isEmpty() || remoteVer == FIRMWARE_VERSION) {
        otaState = 0; otaBroadcast(); return false;   // already up to date
    }

    otaAvailVer = remoteVer;
    otaState = 2; otaBroadcast();
    Serial.printf("OTA: update available %s → %s\n", FIRMWARE_VERSION, remoteVer.c_str());
    return true;
}

// Downloads firmware.bin from GitHub releases and flashes it. Blocking.
// Notifies via WebSocket before starting so dashboard can show progress.
static void otaPerformUpdate()
{
    if (!staConnected) { otaState = 5; otaError = "no wifi"; otaBroadcast(); return; }

    // Notify all clients: update is starting, device will reboot
    otaState = 3; otaProgress = 0; otaBroadcast();
    ws.cleanupClients();
    delay(300);  // let WS frame reach clients before we monopolise the CPU

    Serial.printf("OTA: downloading %s\n", OTA_FIRMWARE_URL);

    WiFiClientSecure cli;
    cli.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(60000);   // up to 60 s for binary download on slow hotspot

    if (!http.begin(cli, OTA_FIRMWARE_URL)) {
        otaState = 5; otaError = "connect"; otaBroadcast(); return;
    }
    int code = http.GET();
    if (code != 200) {
        http.end();
        otaState = 5; otaError = "HTTP " + String(code); otaBroadcast(); return;
    }

    int totalLen = http.getSize();  // -1 if chunked
    WiFiClient* stream = http.getStreamPtr();

    if (!Update.begin(totalLen > 0 ? (size_t)totalLen : UPDATE_SIZE_UNKNOWN)) {
        http.end();
        otaState = 5; otaError = "begin: " + String(Update.errorString());
        otaBroadcast(); return;
    }

    uint8_t  buf[1024];
    int      written   = 0;
    uint8_t  lastPct10 = 0;

    while (http.connected()) {
        size_t avail = stream->available();
        if (avail) {
            int n = stream->readBytes(buf, min((size_t)sizeof(buf), avail));
            if (n > 0 && Update.write(buf, (size_t)n) != (size_t)n) {
                Update.abort(); http.end();
                otaState = 5; otaError = "write error"; otaBroadcast(); return;
            }
            written += n;
            if (totalLen > 0) {
                otaProgress = (uint8_t)(written * 100 / totalLen);
                uint8_t p10 = otaProgress / 10;
                if (p10 != lastPct10) { lastPct10 = p10; otaBroadcast(); }
            }
        } else {
            if (totalLen > 0 && written >= totalLen) break;
            delay(1);
        }
    }
    http.end();

    if (!Update.end(true)) {
        otaState = 5; otaError = Update.errorString(); otaBroadcast(); return;
    }

    otaState = 4; otaProgress = 100; otaBroadcast();
    Serial.printf("OTA: success (%d bytes) – rebooting in 2 s\n", written);
    delay(2000);
    ESP.restart();
}

// FreeRTOS task that owns all blocking OTA work.
// Sleeps until loop() or an HTTP handler signals it via xTaskNotifyGive().
// Removing this task from the Task WDT means long TLS/download operations
// won't cause resets – the main loop stays healthy throughout.
static void otaTask(void* /*arg*/)
{
    // Note: this task is not registered with the Task WDT (we never called
    // esp_task_wdt_add) so it is not monitored by it. That is intentional –
    // TLS handshakes and firmware downloads can legitimately take 30-60 s.
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // sleep until signalled
        uint8_t cmd = otaTaskCmd;
        otaTaskCmd = 0;
        if (cmd == 1) {
            otaCheckAvailable();
            // If auto-update is enabled and check found new firmware, install immediately
            if (otaAutoEnabled && otaState == 2) otaPerformUpdate();
        } else if (cmd == 2) otaPerformUpdate();
    }
}

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);
    // Brief delay lets ESP-IDF MSPI timing calibration (which runs concurrently
    // with early setup() on ESP32-C5) finish before the first flash/NVS access.
    delay(500);
    Serial.println("RPM,ADV,DWELL,TOOTH,SYNC");
    pinMode(PIN_TE1, OUTPUT); digitalWrite(PIN_TE1, LOW);   // never bridge TE1 at boot
    pinMode(PIN_NE,  INPUT); pinMode(PIN_IGT, INPUT);
    pinMode(PIN_CAL, INPUT_PULLUP);
    pinMode(PIN_INJ, INPUT); pinMode(PIN_IAC, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_NE),  crankISR, RISING);
    attachInterrupt(digitalPinToInterrupt(PIN_IGT), igtISR,   CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_INJ), injISR,   CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_IAC), iacISR,   CHANGE);

    prefs.begin("ign", false);
    calibOffset = prefs.getFloat("offset",    215.0f);
    staSSID     = prefs.getString("sta_ssid", "");
    staPass     = prefs.getString("sta_pass", "");
    mapVmin     = prefs.getFloat("map_vmin",   1.16f);
    mapVmax     = prefs.getFloat("map_vmax",   3.71f);
    mapKpaMin   = prefs.getFloat("map_kmin",  20.0f);
    mapKpaMax   = prefs.getFloat("map_kmax", 105.0f);
    lblMap      = prefs.getString("lbl_map", "MAP");
    lblInj      = prefs.getString("lbl_inj", "Injektor");
    lblIac      = prefs.getString("lbl_iac", "IAC");
    knockThresh    = (uint8_t)prefs.getUInt("knock_thr", 30);
    cltRpu         = prefs.getFloat("clt_rpu", 2200.0f);
    iatRpu         = prefs.getFloat("iat_rpu", 2200.0f);
    calAngle       = prefs.getFloat("cal_angle", 10.0f);
    otaAutoEnabled = prefs.getBool("ota_auto", true);

    // OLED auto-detect disabled: Wire.begin() crashes on ESP32-C5 eco2 because
    // arduino-esp32 3.3.8 uses the legacy i2c_driver_install() API which accesses
    // I2C peripheral registers at addresses that don't match C5 eco2 hardware.
    // NOTE: the OLED was previously wired to GPIO21/20, which are SPI flash pins on
    // the ESP32-C5 – that alone crashes the chip. Pins are now GPIO0/1. Test the
    // OLED over USB before re-enabling it here (a boot crash would block OTA).
    // TODO: re-enable once verified on hardware.
    oledOk = false;

    LittleFS.begin(true);

    // Restore log counters from existing file
    if (LittleFS.exists("/ignlog.csv")) {
        File f = LittleFS.open("/ignlog.csv", "r");
        logBytes = f.size();
        logCount = max(0, (logBytes - 90) / 52); // estimate
        f.close();
    }

    // WiFi – AP always on; STA if configured.
    // esp_wifi_set_band_mode removed: let Arduino WiFi stack manage init
    // internally. On ESP32-C5 AP defaults to 2.4 GHz which is what we need.
    WiFi.setHostname("ignlogger");   // replaces default esp32xxxxxx on the network
    WiFi.mode(staSSID.length() > 0 ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    dnsServer.start(53, "*", WiFi.softAPIP());
    if (MDNS.begin("ignlogger")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("mDNS: http://ignlogger.local");
    }
    Serial.printf("AP: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
    if (staSSID.length() > 0) {
        WiFi.begin(staSSID.c_str(), staPass.c_str());
        Serial.printf("STA: forbinder til %s...\n", staSSID.c_str());
    }

    ws.onEvent([](AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType,
                  void*, uint8_t*, size_t) {});
    server.addHandler(&ws);

    // ── Captive portal ───────────────────────────────────────────────────────
    auto redir = [](AsyncWebServerRequest* r){ r->redirect("http://192.168.4.1/"); };
    server.on("/generate_204",              HTTP_GET, redir);
    server.on("/hotspot-detect.html",       HTTP_GET, redir);
    server.on("/library/test/success.html", HTTP_GET, redir);
    server.on("/connectivity-check.html",   HTTP_GET, redir);
    server.on("/redirect",                  HTTP_GET, redir);
    server.on("/canonical.html",            HTTP_GET, redir);
    server.on("/ncsi.txt",                  HTTP_GET, redir);

    // ── Status ───────────────────────────────────────────────────────────────
    server.on("/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        noInterrupts(); bool s = synced; interrupts();
        char buf[384];
        snprintf(buf, sizeof(buf),
            "{\"offset\":%.1f,\"synced\":%d,\"logActive\":%d,\"logBytes\":%d"
            ",\"fsFree\":%u,\"fsTotal\":%u,\"staConnected\":%d,\"staIP\":\"%s\""
            ",\"uptime\":%lu,\"freeHeap\":%u,\"apClients\":%d,\"rssi\":%d}",
            calibOffset, s?1:0, logActive?1:0, logBytes,
            (unsigned)(LittleFS.totalBytes()-LittleFS.usedBytes()),
            (unsigned)LittleFS.totalBytes(),
            staConnected?1:0,
            staConnected ? WiFi.localIP().toString().c_str() : "",
            millis()/1000UL,
            (unsigned)ESP.getFreeHeap(),
            (int)WiFi.softAPgetStationNum(),
            staConnected ? WiFi.RSSI() : 0);
        req->send(200, "application/json", buf);
    });

    // ── Calibration ──────────────────────────────────────────────────────────
    // Manual calibration: TE1 already bridged by hand → use the spark angle
    // of the last 2 s (must be stable).
    server.on("/cal", HTTP_POST, [](AsyncWebServerRequest* req) {
        noInterrupts(); bool s=synced; interrupts();
        if (!s) { req->send(400,"application/json","{\"error\":\"Motor ikke synkroniseret\"}"); return; }
        float angle = req->hasParam("angle",true) ? req->getParam("angle",true)->value().toFloat() : calAngle;
        angle = constrain(angle, -10.0f, 60.0f);
        float mean, spread;
        int n = sparkStats(millis() - 2000UL, mean, spread);
        if (n < 8)        { req->send(400,"application/json","{\"error\":\"For få gnister – er IGT tilsluttet?\"}"); return; }
        if (spread > 2.0f){ req->send(400,"application/json","{\"error\":\"Ustabil tænding – er TE1 brokoblet?\"}"); return; }
        calibOffset = mean + angle; if (calibOffset >= 360.0f) calibOffset -= 360.0f;
        prefs.putFloat("offset", calibOffset);
        char buf[40]; snprintf(buf,sizeof(buf),"{\"offset\":%.1f}",calibOffset);
        Serial.printf("CAL (web): offset=%.1f angle=%.1f\n", calibOffset, angle);
        req->send(200, "application/json", buf);
    });
    // Automatic calibration via TE1 optocoupler (see autoCalStep)
    server.on("/cal/auto", HTTP_POST, [](AsyncWebServerRequest* req) {
        float angle = req->hasParam("angle",true) ? req->getParam("angle",true)->value().toFloat() : calAngle;
        startAutoCal(angle);
        req->send(200, "application/json", "{\"ok\":true}");
    });
    server.on("/cal/auto/stop", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (acState >= 1 && acState <= 3) stopAutoCal(0, 0);
        req->send(200, "application/json", "{\"ok\":true}");
    });
    server.on("/cal/set", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("offset",true)) { req->send(400,"application/json","{\"error\":\"missing\"}"); return; }
        float val = req->getParam("offset",true)->value().toFloat();
        if (val<1.0f||val>359.0f) { req->send(400,"application/json","{\"error\":\"range\"}"); return; }
        calibOffset = val; prefs.putFloat("offset", calibOffset);
        char buf[40]; snprintf(buf,sizeof(buf),"{\"offset\":%.1f}",calibOffset);
        Serial.printf("CAL (set): offset=%.1f\n", calibOffset);
        req->send(200, "application/json", buf);
    });

    // ── WiFi Station ─────────────────────────────────────────────────────────
    server.on("/wifi/config", HTTP_GET, [](AsyncWebServerRequest* req) {
        char buf[160];
        snprintf(buf, sizeof(buf),
            "{\"ssid\":\"%s\",\"connected\":%d,\"ip\":\"%s\"}",
            staSSID.c_str(), staConnected?1:0,
            staConnected ? WiFi.localIP().toString().c_str() : "");
        req->send(200, "application/json", buf);
    });
    server.on("/wifi/config", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("ssid",true)) { req->send(400,"application/json","{\"error\":\"missing ssid\"}"); return; }
        staSSID = req->getParam("ssid",true)->value();
        staPass = req->hasParam("pass",true) ? req->getParam("pass",true)->value() : "";
        prefs.putString("sta_ssid", staSSID);
        prefs.putString("sta_pass", staPass);
        req->send(200, "application/json", "{\"ok\":true}");
        delay(500); ESP.restart();
    });
    server.on("/wifi/clear", HTTP_POST, [](AsyncWebServerRequest* req) {
        staSSID=""; staPass="";
        prefs.remove("sta_ssid"); prefs.remove("sta_pass");
        req->send(200, "application/json", "{\"ok\":true}");
        delay(500); ESP.restart();
    });

    // ── Sensor labels ────────────────────────────────────────────────────────
    server.on("/labels", HTTP_GET, [](AsyncWebServerRequest* req) {
        char buf[160];
        snprintf(buf, sizeof(buf),
            "{\"map\":\"%s\",\"inj\":\"%s\",\"iac\":\"%s\"}",
            lblMap.c_str(), lblInj.c_str(), lblIac.c_str());
        req->send(200, "application/json", buf);
    });
    server.on("/labels", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (req->hasParam("map",true)) { lblMap=req->getParam("map",true)->value(); prefs.putString("lbl_map",lblMap); }
        if (req->hasParam("inj",true)) { lblInj=req->getParam("inj",true)->value(); prefs.putString("lbl_inj",lblInj); }
        if (req->hasParam("iac",true)) { lblIac=req->getParam("iac",true)->value(); prefs.putString("lbl_iac",lblIac); }
        char buf[160];
        snprintf(buf, sizeof(buf),
            "{\"map\":\"%s\",\"inj\":\"%s\",\"iac\":\"%s\"}",
            lblMap.c_str(), lblInj.c_str(), lblIac.c_str());
        req->send(200, "application/json", buf);
    });

    // ── Sensor config ────────────────────────────────────────────────────────
    server.on("/config", HTTP_GET, [](AsyncWebServerRequest* req) {
        char buf[128];
        snprintf(buf, sizeof(buf),
            "{\"mapVmin\":%.2f,\"mapVmax\":%.2f,\"mapKpaMin\":%.1f,\"mapKpaMax\":%.1f}",
            mapVmin, mapVmax, mapKpaMin, mapKpaMax);
        req->send(200, "application/json", buf);
    });
    server.on("/config", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (req->hasParam("mapVmin",  true)) mapVmin   = req->getParam("mapVmin",  true)->value().toFloat();
        if (req->hasParam("mapVmax",  true)) mapVmax   = req->getParam("mapVmax",  true)->value().toFloat();
        if (req->hasParam("mapKpaMin",true)) mapKpaMin = req->getParam("mapKpaMin",true)->value().toFloat();
        if (req->hasParam("mapKpaMax",true)) mapKpaMax = req->getParam("mapKpaMax",true)->value().toFloat();
        mapVmin   = constrain(mapVmin,   0.0f, 5.0f);
        mapVmax   = constrain(mapVmax,   0.0f, 5.0f);
        mapKpaMin = constrain(mapKpaMin, 0.0f, 400.0f);
        mapKpaMax = constrain(mapKpaMax, 0.0f, 400.0f);
        prefs.putFloat("map_vmin", mapVmin);
        prefs.putFloat("map_vmax", mapVmax);
        prefs.putFloat("map_kmin", mapKpaMin);
        prefs.putFloat("map_kmax", mapKpaMax);
        char buf[128];
        snprintf(buf, sizeof(buf),
            "{\"mapVmin\":%.2f,\"mapVmax\":%.2f,\"mapKpaMin\":%.1f,\"mapKpaMax\":%.1f}",
            mapVmin, mapVmax, mapKpaMin, mapKpaMax);
        req->send(200, "application/json", buf);
    });

    // ── OTA ──────────────────────────────────────────────────────────────────
    server.on("/update", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200,"text/html",
            "<!DOCTYPE html><html><head><meta charset=UTF-8><title>OTA</title>"
            "<style>body{font-family:sans-serif;background:#111;color:#ccc;padding:40px}"
            "h2{color:#ff8c00}input[type=file]{color:#ccc;margin:16px 0;display:block}"
            "input[type=submit]{background:#ff8c00;color:#000;border:0;padding:10px 24px;"
            "border-radius:6px;cursor:pointer}</style></head><body>"
            "<h2>Firmware OTA</h2>"
            "<form method=POST action=/update enctype=multipart/form-data>"
            "<input type=file name=f accept=.bin>"
            "<input type=submit value=Flash></form>"
            "<p style='margin-top:32px'><a href=/ style='color:#666'>&larr; Dashboard</a>"
            "</p></body></html>");
    });
    server.on("/update", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            bool ok=!Update.hasError();
            auto r=req->beginResponse(200,"text/plain",ok?"OK":"FAIL");
            r->addHeader("Connection","close"); req->send(r);
            if (ok) { delay(500); ESP.restart(); }
        },
        [](AsyncWebServerRequest*,const String&,size_t idx,uint8_t* data,size_t len,bool final){
            if (!idx)                        Update.begin(UPDATE_SIZE_UNKNOWN);
            if (Update.write(data,len)!=len) Update.abort();
            if (final)                       Update.end(true);
        }
    );

    // ── Log API ──────────────────────────────────────────────────────────────
    server.on("/log/start", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!logFile && !openLogFile()) { req->send(500,"text/plain","FS error"); return; }
        logActive = true;
        req->send(200, "text/plain", "OK");
    });
    server.on("/log/stop", HTTP_POST, [](AsyncWebServerRequest* req) {
        logActive = false;
        if (logFile) { logFile.flush(); logFile.close(); }
        req->send(200, "text/plain", "OK");
    });
    server.on("/log/clear", HTTP_POST, [](AsyncWebServerRequest* req) {
        logActive = false;
        if (logFile) logFile.close();
        if (LittleFS.exists("/ignlog.csv")) LittleFS.remove("/ignlog.csv");
        logCount=0; logBytes=0;
        req->send(200, "text/plain", "OK");
    });
    server.on("/log.csv", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (LittleFS.exists("/ignlog.csv"))
            req->send(LittleFS, "/ignlog.csv", "text/csv", true);
        else
            req->send(200,"text/csv",
                "timestamp_ms,rpm,adv_btdc,dwell_ms,tooth,frac,sync,map_kpa,inj_ms,iac_pct,clt_c,iat_c,te1\n");
    });

    // ── Temperatures ─────────────────────────────────────────────────────────
    auto tempJson = [](char* buf, size_t n) {
        float cv, iv;
        float ct = readTempC(PIN_CLT, cltRpu, &cv);
        float it = readTempC(PIN_IAT, iatRpu, &iv);
        snprintf(buf, n,
            "{\"clt\":%.1f,\"iat\":%.1f,\"cltV\":%.3f,\"iatV\":%.3f,\"cltRpu\":%.0f,\"iatRpu\":%.0f}",
            ct, it, cv, iv, cltRpu, iatRpu);
    };
    server.on("/temp", HTTP_GET, [tempJson](AsyncWebServerRequest* req) {
        char buf[160]; tempJson(buf, sizeof(buf));
        req->send(200, "application/json", buf);
    });
    // Cold-soaked engine: both sensors at ambient → solve the OEM pull-up resistors
    server.on("/temp/cal", HTTP_POST, [tempJson](AsyncWebServerRequest* req) {
        if (!req->hasParam("ambient", true)) { req->send(400,"application/json","{\"error\":\"missing ambient\"}"); return; }
        float amb = constrain(req->getParam("ambient", true)->value().toFloat(), -30.0f, 45.0f);
        float rNtc = ntcResForTemp(amb);
        float v;
        v = readSensorV(PIN_CLT);
        if (v > 0.05f && v < 4.90f) { cltRpu = constrain(rNtc * (5.0f - v) / v, 300.0f, 20000.0f); prefs.putFloat("clt_rpu", cltRpu); }
        v = readSensorV(PIN_IAT);
        if (v > 0.05f && v < 4.90f) { iatRpu = constrain(rNtc * (5.0f - v) / v, 300.0f, 20000.0f); prefs.putFloat("iat_rpu", iatRpu); }
        char buf[160]; tempJson(buf, sizeof(buf));
        req->send(200, "application/json", buf);
    });

    // ── Knock sensor ─────────────────────────────────────────────────────────
    server.on("/knock", HTTP_GET, [](AsyncWebServerRequest* req) {
        char buf[80];
        snprintf(buf, sizeof(buf),
            "{\"level\":%d,\"count\":%lu,\"threshold\":%d}",
            (int)knockLevel, (unsigned long)knockCount, (int)knockThresh);
        req->send(200, "application/json", buf);
    });
    server.on("/knock", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (req->hasParam("threshold", true)) {
            int t = req->getParam("threshold", true)->value().toInt();
            knockThresh = (uint8_t)constrain(t, 1, 99);
            prefs.putUInt("knock_thr", knockThresh);
        }
        if (req->hasParam("reset", true)) knockCount = 0;
        char buf[80];
        snprintf(buf, sizeof(buf),
            "{\"level\":%d,\"count\":%lu,\"threshold\":%d}",
            (int)knockLevel, (unsigned long)knockCount, (int)knockThresh);
        req->send(200, "application/json", buf);
    });

    // ── OTA auto-update endpoints ─────────────────────────────────────────────
    server.on("/ota/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        char buf[192];
        snprintf(buf, sizeof(buf),
            "{\"state\":%d,\"pct\":%d,\"current\":\"%s\","
            "\"available\":\"%s\",\"auto\":%d,\"error\":\"%s\"}",
            (int)otaState, (int)otaProgress, FIRMWARE_VERSION,
            otaAvailVer.c_str(), otaAutoEnabled ? 1 : 0, otaError.c_str());
        req->send(200, "application/json", buf);
    });
    server.on("/ota/check", HTTP_POST, [](AsyncWebServerRequest* req) {
        req->send(200, "text/plain", "OK");
        // Signal OTA task – never block the async handler context
        otaTaskCmd = 1;
        if (otaTaskHandle) xTaskNotifyGive(otaTaskHandle);
    });
    server.on("/ota/update", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (otaState == 3) { req->send(409, "text/plain", "already updating"); return; }
        req->send(200, "text/plain", "OK");
        // Signal OTA task – never block the async handler context
        otaTaskCmd = 2;
        if (otaTaskHandle) xTaskNotifyGive(otaTaskHandle);
    });
    server.on("/ota/config", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (req->hasParam("auto", true)) {
            otaAutoEnabled = req->getParam("auto", true)->value() != "0";
            prefs.putBool("ota_auto", otaAutoEnabled);
        }
        char buf[48];
        snprintf(buf, sizeof(buf), "{\"auto\":%d}", otaAutoEnabled ? 1 : 0);
        req->send(200, "application/json", buf);
    });

    // ── Static files (after all specific handlers) ────────────────────────────
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    server.onNotFound([](AsyncWebServerRequest* req){
        req->redirect("http://192.168.4.1/");
    });

    // OTA task: 20 KB stack (TLS/mbedTLS needs headroom), priority 1 (below loop=1? no – same is fine)
    xTaskCreate(otaTask, "ota", 20480, NULL, 1, &otaTaskHandle);

    server.begin();
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop()
{
    dnsServer.processNextRequest();

    // WiFi STA reconnect
    static uint32_t lastWifiCheck = 0;
    static bool     staPrevConnected = false;
    if (staSSID.length() > 0 && millis() - lastWifiCheck > 15000) {
        lastWifiCheck = millis();
        bool was = staConnected;
        staConnected = (WiFi.status() == WL_CONNECTED);
        if (!staConnected) WiFi.begin(staSSID.c_str(), staPass.c_str());
        else if (!was) {
            Serial.printf("STA: Forbundet. IP: %s\n", WiFi.localIP().toString().c_str());
            staPrevConnected = false;  // reset so first-connect check fires below
        }
    }

    // OTA auto-check: once on first STA connect, then every 6 hours
    static uint32_t lastOtaCheck = 0;
    if (otaAutoEnabled && staConnected && otaState == 0) {
        bool firstConnect = !staPrevConnected;
        staPrevConnected = true;
        bool timeToCheck = (lastOtaCheck == 0 && firstConnect) ||
                           (millis() - lastOtaCheck > OTA_CHECK_INTERVAL_MS);
        if (timeToCheck) {
            lastOtaCheck = millis();
            // Signal OTA task – never block loop() directly
            otaTaskCmd = 1;
            if (otaTaskHandle) xTaskNotifyGive(otaTaskHandle);
        }
    }

    collectSpark();
    static uint32_t lastAc = 0;
    if (millis() - lastAc >= 50) { lastAc = millis(); autoCalStep(); }

    static uint32_t lastPrint = 0, lastWs = 0;
    uint32_t now = millis();

    if (now - lastPrint >= 200) {
        lastPrint = now;
        float rpm, adv, dwell, frac; int tooth; bool sync;
        computeValues(rpm, adv, dwell, tooth, frac, sync);
        sampleKnock();
        Serial.printf("%.0f,%.1f,%.2f,%d.%02d,%d\n",
                      rpm,adv,dwell,tooth,(int)(frac*100),sync?1:0);
        logSample(rpm, adv, dwell, tooth, frac, sync);
        int   oAdc    = readMapADC();
        float oMapKpa = (oAdc > 300 && oAdc < 3800) ? adcToMapKpa(oAdc) : -1.0f;
        float oInj    = isInjActive() ? injUs/1000.0f : -1.0f;
        float oIac    = isIacActive() ? getIacDuty() : -1.0f;
        updateOled(rpm, adv, dwell, tooth, frac, sync, oMapKpa, oInj, oIac);
    }

    if (now - lastWs >= 200) {
        lastWs = now;
        pushToClients();
        ws.cleanupClients();
    }

    // Non-blocking CAL button debounce (50 ms debounce, max 3 s hold)
    // Old code used delay(1000) which blocked WebSocket, DNS, and serial for 1 second.
    {
        static uint32_t calPressedAt = 0;
        bool calDown = !digitalRead(PIN_CAL);
        if (calDown && !calPressedAt) {
            calPressedAt = millis();
        } else if (!calDown && calPressedAt) {
            uint32_t held = millis() - calPressedAt;
            if (held > 50 && held < 3000) {
                // Short press: start auto-calibration (TE1 bridged by the optocoupler,
                // or by hand with a switch – the routine works either way)
                startAutoCal(calAngle);
                Serial.println("CAL: auto-calibration started");
            }
            calPressedAt = 0;
        }
    }
}
