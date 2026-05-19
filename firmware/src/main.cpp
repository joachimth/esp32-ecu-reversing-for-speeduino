#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <DNSServer.h>

// ─── Pins ────────────────────────────────────────────────────────────────────
#define PIN_NE   25
#define PIN_IGT  26
#define PIN_CAL  0
#define PIN_MAP  34
#define PIN_INJ  35
#define PIN_IAC  32

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

// ─── Config ──────────────────────────────────────────────────────────────────
float  calibOffset  = 215.0f;
String staSSID      = "";
String staPass      = "";
bool   staConnected = false;

// MAP sensor scaling (sensor voltage → kPa; divider compensated inside readMapKpa)
float mapVmin   = 0.5f;    // sensor V at min pressure
float mapVmax   = 4.5f;    // sensor V at max pressure
float mapKpaMin = 10.0f;   // kPa at Vmin
float mapKpaMax = 105.0f;  // kPa at Vmax

Preferences    prefs;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer      dnsServer;

// ─── LittleFS logging ────────────────────────────────────────────────────────
File        logFile;
static int  logCount  = 0;
static int  logBytes  = 0;
static bool logActive = false;

// ─── ISRs ────────────────────────────────────────────────────────────────────
void IRAM_ATTR crankISR()
{
    uint32_t now = micros(), dt = now - lastToothUs;
    if (toothPeriodUs > 0) {
        if (dt > toothPeriodUs * 1.7f) { toothCount = 0; synced = true; }
        else if (++toothCount >= 36)    toothCount = 0;
    }
    toothPeriodUs = dt; lastToothUs = now;
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
    if (!digitalRead(PIN_INJ)) injStartUs = now;
    else { injMs = (now - injStartUs) / 1000.0f; lastInjUs = now; }
}
void IRAM_ATTR iacISR()
{
    uint32_t now = micros();
    if (digitalRead(PIN_IAC)) {
        if (lastIacEdgeUs) iacPeriodUs = now - lastIacEdgeUs;
        iacRiseUs = now;
    } else { iacHighUs = now - iacRiseUs; }
    lastIacEdgeUs = now;
}

// ─── Sensor helpers ──────────────────────────────────────────────────────────
static float readMapKpa()
{
    float vGpio   = analogRead(PIN_MAP) * 3.3f / 4095.0f;
    float vSensor = vGpio * 1.5f;  // 10k/20k divider compensation
    float range   = mapVmax - mapVmin;
    if (range < 0.01f) return mapKpaMin;
    return mapKpaMin + (vSensor - mapVmin) / range * (mapKpaMax - mapKpaMin);
}
static bool isMapConnected() { int r=analogRead(PIN_MAP); return r>300&&r<3800; }
static bool isInjActive()    { return (micros()-lastInjUs)    < 2000000UL; }
static bool isIacActive()    { return lastIacEdgeUs&&(micros()-lastIacEdgeUs)<2000000UL; }
static float getIacDuty()    { return iacPeriodUs>0 ? iacHighUs/iacPeriodUs*100.0f : -1.0f; }

// ─── Core computation ────────────────────────────────────────────────────────
static void computeValues(float& rpm, float& adv, float& dwell,
                           int& tooth, float& frac, bool& sync)
{
    noInterrupts();
    tooth       = toothCount;
    uint32_t tp = toothPeriodUs;
    uint32_t dt = micros() - lastToothUs;
    dwell       = dwellMs; sync = synced;
    interrupts();
    if (tp==0 || dt/tp>3) { frac=0; adv=0; rpm=0; return; }
    frac = (float)dt/tp; if (frac>1.0f) frac=1.0f;
    adv  = calibOffset - (tooth*10.0f + frac*10.0f);
    rpm  = 60000000.0f / (tp*36.0f);
}

// ─── Logging helpers ─────────────────────────────────────────────────────────
static bool openLogFile()
{
    bool exists = LittleFS.exists("/ignlog.csv");
    logFile = LittleFS.open("/ignlog.csv", exists ? "a" : "w");
    if (!logFile) return false;
    if (!exists)
        logFile.println("timestamp_ms,rpm,adv_btdc,dwell_ms,tooth,frac,sync,"
                        "map_kpa,inj_ms,iac_pct");
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
    float mapKpa = isMapConnected() ? readMapKpa() : -1.0f;
    float injMsV = isInjActive()    ? injMs        : -1.0f;
    float iacPct = isIacActive()    ? getIacDuty() : -1.0f;

    char mS[10]="", iS[10]="", cS[10]="";
    if (mapKpa>=0) snprintf(mS,sizeof(mS),"%.1f",mapKpa);
    if (injMsV>=0) snprintf(iS,sizeof(iS),"%.2f",injMsV);
    if (iacPct>=0) snprintf(cS,sizeof(cS),"%.1f",iacPct);

    char row[128];
    snprintf(row, sizeof(row), "%lu,%.0f,%.1f,%.2f,%d,%d,%d,%s,%s,%s",
             (unsigned long)millis(), rpm, adv, dwell,
             tooth, (int)(frac*100), sync?1:0, mS, iS, cS);
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
    float mapKpa = isMapConnected() ? readMapKpa() : -1.0f;
    float injMsV = isInjActive()    ? injMs        : -1.0f;
    float iacPct = isIacActive()    ? getIacDuty() : -1.0f;
    float mapV = analogRead(PIN_MAP) * 3.3f / 4095.0f * 1.5f;  // sensor voltage
    char buf[220];
    snprintf(buf, sizeof(buf),
        "{\"r\":%.0f,\"a\":%.1f,\"d\":%.2f,\"t\":%d,\"f\":%d,\"s\":%d"
        ",\"m\":%.1f,\"mv\":%.3f,\"i\":%.2f,\"c\":%.1f,\"lc\":%d,\"la\":%d,\"lb\":%d}",
        rpm, adv, dwell, tooth, (int)(frac*100), sync?1:0,
        mapKpa, mapV, injMsV, iacPct, logCount, logActive?1:0, logBytes);
    ws.textAll(buf);
}

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);
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
    mapVmin     = prefs.getFloat("map_vmin",   0.5f);
    mapVmax     = prefs.getFloat("map_vmax",   4.5f);
    mapKpaMin   = prefs.getFloat("map_kmin",  10.0f);
    mapKpaMax   = prefs.getFloat("map_kmax", 105.0f);

    LittleFS.begin(true);

    // Restore log counters from existing file
    if (LittleFS.exists("/ignlog.csv")) {
        File f = LittleFS.open("/ignlog.csv", "r");
        logBytes = f.size();
        logCount = max(0, (logBytes - 90) / 52); // estimate
        f.close();
    }

    // WiFi – AP always on; STA if configured
    WiFi.mode(staSSID.length() > 0 ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    dnsServer.start(53, "*", WiFi.softAPIP());
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
    server.on("/cal", HTTP_POST, [](AsyncWebServerRequest* req) {
        noInterrupts(); bool s=synced; int tc=toothCount; interrupts();
        if (!s) { req->send(400,"application/json","{\"error\":\"Motor ikke synkroniseret\"}"); return; }
        calibOffset = tc*10.0f + 10.0f;
        prefs.putFloat("offset", calibOffset);
        char buf[40]; snprintf(buf,sizeof(buf),"{\"offset\":%.1f}",calibOffset);
        Serial.printf("CAL (web): offset=%.1f\n", calibOffset);
        req->send(200, "application/json", buf);
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
                "timestamp_ms,rpm,adv_btdc,dwell_ms,tooth,frac,sync,map_kpa,inj_ms,iac_pct\n");
    });

    // ── Static files (after all specific handlers) ────────────────────────────
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    server.onNotFound([](AsyncWebServerRequest* req){
        req->redirect("http://192.168.4.1/");
    });

    server.begin();
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop()
{
    dnsServer.processNextRequest();

    // WiFi STA reconnect
    static uint32_t lastWifiCheck = 0;
    if (staSSID.length() > 0 && millis() - lastWifiCheck > 15000) {
        lastWifiCheck = millis();
        bool was = staConnected;
        staConnected = (WiFi.status() == WL_CONNECTED);
        if (!staConnected) WiFi.begin(staSSID.c_str(), staPass.c_str());
        else if (!was) Serial.printf("STA: Forbundet. IP: %s\n", WiFi.localIP().toString().c_str());
    }

    static uint32_t lastPrint = 0, lastWs = 0;
    uint32_t now = millis();

    if (now - lastPrint >= 200) {
        lastPrint = now;
        float rpm, adv, dwell, frac; int tooth; bool sync;
        computeValues(rpm, adv, dwell, tooth, frac, sync);
        Serial.printf("%.0f,%.1f,%.2f,%d.%02d,%d\n",
                      rpm,adv,dwell,tooth,(int)(frac*100),sync?1:0);
        logSample(rpm, adv, dwell, tooth, frac, sync);
    }

    if (now - lastWs >= 200) {
        lastWs = now;
        pushToClients();
        ws.cleanupClients();
    }

    if (!digitalRead(PIN_CAL)) {
        noInterrupts(); int tc=toothCount; interrupts();
        calibOffset = tc*10.0f + 10.0f;
        prefs.putFloat("offset", calibOffset);
        Serial.printf("CAL: offset=%.1f\n", calibOffset);
        delay(1000);
    }
}
