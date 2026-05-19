#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Update.h>

#define PIN_NE  25
#define PIN_IGT 26
#define PIN_CAL 0

volatile uint32_t lastToothUs   = 0;
volatile uint32_t toothPeriodUs = 0;
volatile int      toothCount    = 0;
volatile uint32_t lastEdgeUs    = 0;
volatile bool     synced        = false;
volatile uint32_t dwellStartUs  = 0;
volatile float    dwellMs       = 0;

float       calibOffset = 215.0f;
Preferences prefs;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

const char* AP_SSID = "IgnLogger";
const char* AP_PASS = "ignition1";

void IRAM_ATTR crankISR()
{
    uint32_t now = micros();
    uint32_t dt  = now - lastToothUs;
    if (toothPeriodUs > 0)
    {
        if (dt > (toothPeriodUs * 1.7f))
        {
            toothCount = 0;
            synced     = true;
        }
        else
        {
            toothCount++;
            if (toothCount >= 36)
                toothCount = 0;
        }
    }
    toothPeriodUs = dt;
    lastToothUs   = now;
}

void IRAM_ATTR igtISR()
{
    uint32_t now   = micros();
    bool     state = digitalRead(PIN_IGT);
    if (state)
        dwellStartUs = now;
    else
    {
        dwellMs    = (now - dwellStartUs) / 1000.0f;
        lastEdgeUs = now;
    }
}

static void computeValues(float& rpm, float& adv, float& dwell, int& tooth, float& frac, bool& sync)
{
    noInterrupts();
    tooth       = toothCount;
    uint32_t tp = toothPeriodUs;
    uint32_t dt = micros() - lastToothUs;
    dwell       = dwellMs;
    sync        = synced;
    interrupts();

    // No pulse for >3 tooth periods → engine stopped
    if (tp == 0 || dt / tp > 3) {
        frac = 0.0f; adv = 0.0f; rpm = 0.0f;
        return;
    }

    frac = (float)dt / tp;
    if (frac > 1.0f) frac = 1.0f;
    float raw = tooth * 10.0f + frac * 10.0f;
    adv  = calibOffset - raw;
    rpm  = 60000000.0f / (tp * 36.0f);
}

static void pushToClients()
{
    float rpm, adv, dwell, frac;
    int   tooth;
    bool  sync;
    computeValues(rpm, adv, dwell, tooth, frac, sync);

    char buf[64];
    snprintf(buf, sizeof(buf), "%.0f,%.1f,%.2f,%d.%02d,%d",
             rpm, adv, dwell, tooth, (int)(frac * 100), sync ? 1 : 0);
    ws.textAll(buf);
}

void setup()
{
    Serial.begin(115200);

    pinMode(PIN_NE,  INPUT);
    pinMode(PIN_IGT, INPUT);
    pinMode(PIN_CAL, INPUT_PULLUP);

    attachInterrupt(digitalPinToInterrupt(PIN_NE),  crankISR, RISING);
    attachInterrupt(digitalPinToInterrupt(PIN_IGT), igtISR,   CHANGE);

    prefs.begin("ign", false);
    calibOffset = prefs.getFloat("offset", 215.0f);

    LittleFS.begin(true);

    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("WiFi AP: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
    Serial.println("Web UI: http://192.168.4.1");
    Serial.println("OTA:    http://192.168.4.1/update");

    ws.onEvent([](AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType,
                  void*, uint8_t*, size_t) {});
    server.addHandler(&ws);

    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    // OTA page
    server.on("/update", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html",
            "<!DOCTYPE html><html><head><meta charset=UTF-8>"
            "<title>OTA Update</title>"
            "<style>body{font-family:sans-serif;background:#111;color:#ccc;padding:40px;}"
            "h2{color:#ff8c00;}input[type=file]{color:#ccc;margin:16px 0;display:block;}"
            "input[type=submit]{background:#ff8c00;color:#000;border:0;padding:10px 24px;"
            "border-radius:6px;cursor:pointer;font-size:1rem;}"
            "#msg{margin-top:20px;color:#4caf50;}</style></head><body>"
            "<h2>Firmware OTA Update</h2>"
            "<form method=POST action=/update enctype=multipart/form-data>"
            "<input type=file name=f accept=.bin>"
            "<input type=submit value='Flash firmware'></form>"
            "<div id=msg></div>"
            "<p style='margin-top:40px;font-size:.8rem;color:#555'>"
            "<a href=/ style='color:#666'>&#8592; Tilbage til dashboard</a></p>"
            "</body></html>");
    });

    server.on("/update", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            bool ok = !Update.hasError();
            AsyncWebServerResponse* resp =
                req->beginResponse(200, "text/plain", ok ? "OK" : "FAIL");
            resp->addHeader("Connection", "close");
            req->send(resp);
            if (ok) {
                delay(500);
                ESP.restart();
            }
        },
        [](AsyncWebServerRequest*, const String&, size_t index,
           uint8_t* data, size_t len, bool final) {
            if (!index)
                Update.begin(UPDATE_SIZE_UNKNOWN);
            if (Update.write(data, len) != len)
                Update.abort();
            if (final)
                Update.end(true);
        }
    );

    server.begin();
}

void loop()
{
    static uint32_t lastPrint = 0;
    static uint32_t lastWs    = 0;

    uint32_t now = millis();

    if (now - lastPrint >= 200)
    {
        lastPrint = now;

        float rpm, adv, dwell, frac;
        int   tooth;
        bool  sync;
        computeValues(rpm, adv, dwell, tooth, frac, sync);

        Serial.printf("%.0f,%.1f,%.2f,%d.%02d,%d\n",
                      rpm, adv, dwell, tooth, (int)(frac * 100), sync ? 1 : 0);
    }

    if (now - lastWs >= 200)
    {
        lastWs = now;
        pushToClients();
        ws.cleanupClients();
    }

    if (!digitalRead(PIN_CAL))
    {
        noInterrupts();
        int tc = toothCount;
        interrupts();
        calibOffset = tc * 10.0f + 10.0f;
        prefs.putFloat("offset", calibOffset);
        Serial.printf("CAL: offset=%.1f\n", calibOffset);
        delay(1000);
    }
}
