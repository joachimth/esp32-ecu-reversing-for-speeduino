#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <DNSServer.h>

// ─── Pins ────────────────────────────────────────────────────────────────────
#define PIN_NE   25   // NE crank          10k/20k divider, 5V
#define PIN_IGT  26   // IGT ignition      33k/10k divider, 12V
#define PIN_CAL  0    // CAL button        active low, pull-up
#define PIN_MAP  34   // MAP sensor (ADC)  10k/20k divider, 5V  – optional
#define PIN_INJ  35   // Fuel injector     33k/10k divider, 12V – optional
#define PIN_IAC  32   // IAC valve PWM     33k/10k divider, 12V – optional

const char* AP_SSID = "IgnLogger";
const char* AP_PASS = "ignition1";

// ─── ISR state ───────────────────────────────────────────────────────────────
volatile uint32_t lastToothUs   = 0;
volatile uint32_t toothPeriodUs = 0;
volatile int      toothCount    = 0;
volatile bool     synced        = false;

volatile uint32_t dwellStartUs  = 0;
volatile float    dwellMs       = 0;

volatile uint32_t injStartUs    = 0;
volatile float    injMs         = 0;
volatile uint32_t lastInjUs     = 0;

volatile uint32_t iacRiseUs     = 0;
volatile float    iacHighUs     = 1.0f;
volatile float    iacPeriodUs   = 0.0f;
volatile uint32_t lastIacEdgeUs = 0;

float       calibOffset = 215.0f;
Preferences prefs;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer      dnsServer;

// ─── Circular log buffer ─────────────────────────────────────────────────────
struct LogEntry {
    uint32_t ts;
    float    rpm, adv, dwell;
    uint8_t  tooth, frac, sync;
    uint8_t  _pad;
    float    mapKpa;   // -1 = not connected
    float    injMs;    // -1 = not active
    float    iacPct;   // -1 = not active
};                     // 32 bytes × 1500 = 48 KB

static const int LOG_SIZE  = 1500;
static LogEntry  logBuf[LOG_SIZE];
static int       logIdx    = 0;     // next write position
static int       logCount  = 0;     // entries in buffer (max LOG_SIZE)
static bool      logActive = false;

// ─── ISRs ────────────────────────────────────────────────────────────────────
void IRAM_ATTR crankISR()
{
    uint32_t now = micros();
    uint32_t dt  = now - lastToothUs;
    if (toothPeriodUs > 0) {
        if (dt > toothPeriodUs * 1.7f) {
            toothCount = 0; synced = true;
        } else {
            if (++toothCount >= 36) toothCount = 0;
        }
    }
    toothPeriodUs = dt;
    lastToothUs   = now;
}

void IRAM_ATTR igtISR()
{
    uint32_t now = micros();
    if (digitalRead(PIN_IGT)) dwellStartUs = now;
    else                      dwellMs = (now - dwellStartUs) / 1000.0f;
}

void IRAM_ATTR injISR()
{
    uint32_t now = micros();
    if (!digitalRead(PIN_INJ)) { injStartUs = now; }          // FALLING = start
    else { injMs = (now - injStartUs) / 1000.0f; lastInjUs = now; }
}

void IRAM_ATTR iacISR()
{
    uint32_t now = micros();
    if (digitalRead(PIN_IAC)) {
        if (lastIacEdgeUs) iacPeriodUs = now - lastIacEdgeUs;
        iacRiseUs = now;
    } else {
        iacHighUs = now - iacRiseUs;
    }
    lastIacEdgeUs = now;
}

// ─── Sensor helpers ──────────────────────────────────────────────────────────
static float readMapKpa()
{
    float vGpio = analogRead(PIN_MAP) * 3.3f / 4095.0f;
    float vSens = vGpio * 1.5f;                           // 10k/20k → ×1.5
    return (vSens - 0.5f) * 23.75f + 10.0f;              // Bosch 1-bar approx
}

static bool isMapConnected()
{
    int r = analogRead(PIN_MAP);
    return r > 300 && r < 3800;
}

static bool isInjActive()   { return (micros() - lastInjUs)     < 2000000UL; }
static bool isIacActive()   { return lastIacEdgeUs && (micros() - lastIacEdgeUs) < 2000000UL; }
static float getIacDuty()   { return iacPeriodUs > 0 ? iacHighUs / iacPeriodUs * 100.0f : -1.0f; }

// ─── Core computation ────────────────────────────────────────────────────────
static void computeValues(float& rpm, float& adv, float& dwell,
                           int& tooth, float& frac, bool& sync)
{
    noInterrupts();
    tooth       = toothCount;
    uint32_t tp = toothPeriodUs;
    uint32_t dt = micros() - lastToothUs;
    dwell       = dwellMs;
    sync        = synced;
    interrupts();

    if (tp == 0 || dt / tp > 3) { frac = 0; adv = 0; rpm = 0; return; }
    frac = (float)dt / tp;
    if (frac > 1.0f) frac = 1.0f;
    adv = calibOffset - (tooth * 10.0f + frac * 10.0f);
    rpm = 60000000.0f / (tp * 36.0f);
}

// ─── Logging ─────────────────────────────────────────────────────────────────
static void logSample(float rpm, float adv, float dwell,
                       int tooth, float frac, bool sync)
{
    if (!logActive) return;
    LogEntry& e = logBuf[logIdx];
    e.ts      = millis();
    e.rpm     = rpm;  e.adv = adv;  e.dwell = dwell;
    e.tooth   = (uint8_t)tooth;
    e.frac    = (uint8_t)(frac * 100);
    e.sync    = sync ? 1 : 0;
    e._pad    = 0;
    e.mapKpa  = isMapConnected() ? readMapKpa() : -1.0f;
    e.injMs   = isInjActive()    ? injMs        : -1.0f;
    e.iacPct  = isIacActive()    ? getIacDuty() : -1.0f;
    logIdx    = (logIdx + 1) % LOG_SIZE;
    if (logCount < LOG_SIZE) logCount++;
}

// ─── WebSocket broadcast ─────────────────────────────────────────────────────
static void pushToClients()
{
    float rpm, adv, dwell, frac;
    int   tooth; bool sync;
    computeValues(rpm, adv, dwell, tooth, frac, sync);

    float mapKpa = isMapConnected() ? readMapKpa() : -1.0f;
    float injMsV = isInjActive()    ? injMs        : -1.0f;
    float iacPct = isIacActive()    ? getIacDuty() : -1.0f;

    char buf[160];
    snprintf(buf, sizeof(buf),
        "{\"r\":%.0f,\"a\":%.1f,\"d\":%.2f,\"t\":%d,\"f\":%d,\"s\":%d"
        ",\"m\":%.1f,\"i\":%.2f,\"c\":%.1f,\"lc\":%d,\"la\":%d}",
        rpm, adv, dwell, tooth, (int)(frac * 100), sync ? 1 : 0,
        mapKpa, injMsV, iacPct, logCount, logActive ? 1 : 0);
    ws.textAll(buf);
}

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);

    pinMode(PIN_NE,  INPUT);
    pinMode(PIN_IGT, INPUT);
    pinMode(PIN_CAL, INPUT_PULLUP);
    pinMode(PIN_INJ, INPUT);
    pinMode(PIN_IAC, INPUT);

    attachInterrupt(digitalPinToInterrupt(PIN_NE),  crankISR, RISING);
    attachInterrupt(digitalPinToInterrupt(PIN_IGT), igtISR,   CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_INJ), injISR,   CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_IAC), iacISR,   CHANGE);

    prefs.begin("ign", false);
    calibOffset = prefs.getFloat("offset", 215.0f);

    LittleFS.begin(true);

    WiFi.softAP(AP_SSID, AP_PASS);
    dnsServer.start(53, "*", WiFi.softAPIP());
    Serial.printf("AP: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

    // WebSocket
    ws.onEvent([](AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType,
                  void*, uint8_t*, size_t) {});
    server.addHandler(&ws);

    // ── Captive portal ───────────────────────────────────────────────────────
    auto redir = [](AsyncWebServerRequest* r) { r->redirect("http://192.168.4.1/"); };
    server.on("/generate_204",               HTTP_GET, redir);
    server.on("/hotspot-detect.html",        HTTP_GET, redir);
    server.on("/library/test/success.html",  HTTP_GET, redir);
    server.on("/connectivity-check.html",    HTTP_GET, redir);
    server.on("/redirect",                   HTTP_GET, redir);
    server.on("/canonical.html",             HTTP_GET, redir);
    server.on("/ncsi.txt",                   HTTP_GET, redir);

    // ── Static files ─────────────────────────────────────────────────────────
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    // ── OTA ──────────────────────────────────────────────────────────────────
    server.on("/update", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html",
            "<!DOCTYPE html><html><head><meta charset=UTF-8><title>OTA</title>"
            "<style>body{font-family:sans-serif;background:#111;color:#ccc;padding:40px}"
            "h2{color:#ff8c00}input[type=file]{color:#ccc;margin:16px 0;display:block}"
            "input[type=submit]{background:#ff8c00;color:#000;border:0;padding:10px 24px;"
            "border-radius:6px;cursor:pointer}</style></head><body>"
            "<h2>Firmware OTA</h2>"
            "<form method=POST action=/update enctype=multipart/form-data>"
            "<input type=file name=f accept=.bin>"
            "<input type=submit value='Flash'></form>"
            "<p style='margin-top:32px'><a href=/ style='color:#666'>&larr; Dashboard</a>"
            "</p></body></html>");
    });
    server.on("/update", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            bool ok = !Update.hasError();
            auto r  = req->beginResponse(200, "text/plain", ok ? "OK" : "FAIL");
            r->addHeader("Connection", "close");
            req->send(r);
            if (ok) { delay(500); ESP.restart(); }
        },
        [](AsyncWebServerRequest*, const String&, size_t idx,
           uint8_t* data, size_t len, bool final) {
            if (!idx)                       Update.begin(UPDATE_SIZE_UNKNOWN);
            if (Update.write(data,len)!=len) Update.abort();
            if (final)                      Update.end(true);
        }
    );

    // ── Log API ──────────────────────────────────────────────────────────────
    server.on("/log/start", HTTP_POST, [](AsyncWebServerRequest* req) {
        logActive = true;
        req->send(200, "text/plain", "OK");
    });
    server.on("/log/stop", HTTP_POST, [](AsyncWebServerRequest* req) {
        logActive = false;
        req->send(200, "text/plain", "OK");
    });
    server.on("/log/clear", HTTP_POST, [](AsyncWebServerRequest* req) {
        logActive = false; logIdx = 0; logCount = 0;
        req->send(200, "text/plain", "OK");
    });
    server.on("/log.csv", HTTP_GET, [](AsyncWebServerRequest* req) {
        AsyncResponseStream* resp = req->beginResponseStream("text/csv");
        resp->addHeader("Content-Disposition",
                        "attachment; filename=\"ignlog.csv\"");
        resp->print("timestamp_ms,rpm,adv_btdc,dwell_ms,tooth,frac,sync,"
                    "map_kpa,inj_ms,iac_pct\n");
        int n     = min(logCount, LOG_SIZE);
        int start = (logCount >= LOG_SIZE) ? logIdx : 0;
        for (int i = 0; i < n; i++) {
            const LogEntry& e = logBuf[(start + i) % LOG_SIZE];
            char mStr[12]="", iStr[12]="", cStr[12]="";
            if (e.mapKpa >= 0) snprintf(mStr, sizeof(mStr), "%.1f", e.mapKpa);
            if (e.injMs  >= 0) snprintf(iStr, sizeof(iStr), "%.2f", e.injMs);
            if (e.iacPct >= 0) snprintf(cStr, sizeof(cStr), "%.1f", e.iacPct);
            char row[128];
            snprintf(row, sizeof(row), "%lu,%.0f,%.1f,%.2f,%u,%u,%u,%s,%s,%s\n",
                     (unsigned long)e.ts, e.rpm, e.adv, e.dwell,
                     e.tooth, e.frac, (unsigned)e.sync,
                     mStr, iStr, cStr);
            resp->print(row);
        }
        req->send(resp);
    });

    // ── Catch-all captive portal ──────────────────────────────────────────────
    server.onNotFound([](AsyncWebServerRequest* req) {
        req->redirect("http://192.168.4.1/");
    });

    server.begin();
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop()
{
    dnsServer.processNextRequest();

    static uint32_t lastPrint = 0;
    static uint32_t lastWs    = 0;
    uint32_t now = millis();

    if (now - lastPrint >= 200) {
        lastPrint = now;
        float rpm, adv, dwell, frac; int tooth; bool sync;
        computeValues(rpm, adv, dwell, tooth, frac, sync);
        Serial.printf("%.0f,%.1f,%.2f,%d.%02d,%d\n",
                      rpm, adv, dwell, tooth, (int)(frac*100), sync?1:0);
        logSample(rpm, adv, dwell, tooth, frac, sync);
    }

    if (now - lastWs >= 200) {
        lastWs = now;
        pushToClients();
        ws.cleanupClients();
    }

    if (!digitalRead(PIN_CAL)) {
        noInterrupts(); int tc = toothCount; interrupts();
        calibOffset = tc * 10.0f + 10.0f;
        prefs.putFloat("offset", calibOffset);
        Serial.printf("CAL: offset=%.1f\n", calibOffset);
        delay(1000);
    }
}
