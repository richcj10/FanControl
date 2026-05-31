/**
 * @file web_server.cpp
 * @brief REST API route implementation for the fan controller web interface.
 *
 * Uses ESPAsyncWebServer so HTTP handlers execute in a background task and
 * never block the main loop.  API routes are registered before the LittleFS
 * static handler so that paths like /api/status are matched first.
 *
 * All POST endpoints expect a JSON body delivered in a single TCP segment
 * (< ~1460 bytes).  The body callback is invoked per-chunk; since our
 * payloads are small this is always a single call.
 */

#include "web_server.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ETH.h>
#include "config.h"
#include "emc2305.h"

/**
 * @brief Send a JSON string response.
 * @param req  Incoming request.
 * @param json Serialised JSON string.
 * @param code HTTP status code (default 200).
 */
static void sendJson(AsyncWebServerRequest *req, const String &json, int code = 200) {
    req->send(code, "application/json", json);
}

/** @brief Convenience alias for the ESPAsyncWebServer body callback signature. */
using BodyCb = std::function<void(AsyncWebServerRequest*, uint8_t*, size_t)>;

/**
 * @brief Register a POST handler that receives the full request body.
 *
 * ESPAsyncWebServer delivers body data through a separate callback from the
 * request handler.  This helper wires both together so callers only need to
 * supply a single lambda that receives (request, data, length).
 *
 * @param server Reference to the AsyncWebServer.
 * @param path   URL path to match (exact, no wildcards).
 * @param cb     Callback invoked with the body bytes when the request arrives.
 */
static void onPost(AsyncWebServer &server, const char *path, BodyCb cb) {
    server.on(path, HTTP_POST,
        [](AsyncWebServerRequest *req) {},   // request handler (unused — body cb fires instead)
        nullptr,                              // upload handler (not needed)
        [cb](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
            cb(req, data, len);
        });
}

/**
 * @brief Register all REST API routes and start the HTTP server.
 *
 * Route registration order matters: AsyncWebServer matches the first
 * registered handler whose URI and method match the request.  All
 * /api/* routes are added before the LittleFS catch-all so they are
 * never shadowed by the static-file handler.
 *
 * @param server Reference to the AsyncWebServer instance (port 80, created in main.cpp).
 */
void webServerSetup(AsyncWebServer &server) {

    // ----------------------------------------------------------------
    // GET /api/status
    // Returns current IP, RPM, PWM, mode, and alarm flags for all fans.
    // ----------------------------------------------------------------
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument doc;
        doc["ip"] = ETH.localIP().toString();
        JsonArray fans = doc["fans"].to<JsonArray>();
        for (int i = 0; i < NUM_FANS; i++) {
            JsonObject f = fans.add<JsonObject>();
            f["rpm"]        = g_fans[i].rpm;
            f["pwm"]        = g_fans[i].pwm;
            f["closedLoop"] = g_fans[i].closedLoop;
            f["targetRpm"]  = g_fans[i].targetRpm;
            f["stall"]      = g_fans[i].alarmStall;
            f["spinFail"]   = g_fans[i].alarmSpinFail;
            f["driveFail"]  = g_fans[i].alarmDriveFail;
            f["rpmLow"]     = g_fans[i].alarmRpmLow;
            f["rpmHigh"]    = g_fans[i].alarmRpmHigh;
        }
        String out;
        serializeJson(doc, out);
        sendJson(req, out);
    });

    // ----------------------------------------------------------------
    // GET /api/config
    // Returns persisted MQTT settings and per-fan alarm thresholds.
    // ----------------------------------------------------------------
    server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument doc;
        doc["mqttHost"]   = g_config.mqttHost;
        doc["mqttPort"]   = g_config.mqttPort;
        doc["mqttUser"]   = g_config.mqttUser;
        doc["mqttPrefix"] = g_config.mqttPrefix;
        JsonArray alarms = doc["alarms"].to<JsonArray>();
        for (int i = 0; i < NUM_FANS; i++) {
            JsonObject a = alarms.add<JsonObject>();
            a["minRpm"] = g_config.alarms[i].minRpm;
            a["maxRpm"] = g_config.alarms[i].maxRpm;
        }
        String out;
        serializeJson(doc, out);
        sendJson(req, out);
    });

    // ----------------------------------------------------------------
    // POST /api/fan/pwm  —  body: { "fan": 1-5, "pwm": 0-255 }
    // Sets the FAN_DRIVE register directly (direct-PWM mode only).
    // ----------------------------------------------------------------
    onPost(server, "/api/fan/pwm", [](AsyncWebServerRequest *req, uint8_t *data, size_t len) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) { req->send(400); return; }
        uint8_t fan = doc["fan"] | 0;
        uint8_t pwm = doc["pwm"] | 0;
        bool ok = emc2305SetFanPWM(fan, pwm);
        if (ok && fan >= 1 && fan <= NUM_FANS) g_fans[fan - 1].pwm = pwm;
        sendJson(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
    });

    // ----------------------------------------------------------------
    // POST /api/fan/mode  —  body: { "fan": 1-5, "mode": "pwm"|"rpm" }
    // Switches the fan between direct-PWM and closed-loop RPM control.
    // ----------------------------------------------------------------
    onPost(server, "/api/fan/mode", [](AsyncWebServerRequest *req, uint8_t *data, size_t len) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) { req->send(400); return; }
        uint8_t fan  = doc["fan"] | 0;
        bool   isRpm = String((const char*)(doc["mode"] | "pwm")) == "rpm";
        bool ok = emc2305SetMode(fan, isRpm);
        if (ok && fan >= 1 && fan <= NUM_FANS) g_fans[fan - 1].closedLoop = isRpm;
        sendJson(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
    });

    // ----------------------------------------------------------------
    // POST /api/fan/target  —  body: { "fan": 1-5, "rpm": 200-5000 }
    // Writes the closed-loop RPM setpoint to the TACH_TARGET registers.
    // The fan must already be in closed-loop mode.
    // ----------------------------------------------------------------
    onPost(server, "/api/fan/target", [](AsyncWebServerRequest *req, uint8_t *data, size_t len) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) { req->send(400); return; }
        uint8_t  fan = doc["fan"] | 0;
        uint16_t rpm = doc["rpm"] | 0;
        bool ok = emc2305SetTargetRpm(fan, rpm);
        if (ok && fan >= 1 && fan <= NUM_FANS) g_fans[fan - 1].targetRpm = rpm;
        sendJson(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
    });

    // ----------------------------------------------------------------
    // POST /api/config/mqtt
    // Updates MQTT broker credentials in g_config and persists to NVS.
    // The MQTT client must be manually reconnected (handled by mqttLoop).
    // ----------------------------------------------------------------
    onPost(server, "/api/config/mqtt", [](AsyncWebServerRequest *req, uint8_t *data, size_t len) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) { req->send(400); return; }
        strlcpy(g_config.mqttHost,   doc["mqttHost"]   | "", sizeof(g_config.mqttHost));
        strlcpy(g_config.mqttUser,   doc["mqttUser"]   | "", sizeof(g_config.mqttUser));
        strlcpy(g_config.mqttPass,   doc["mqttPass"]   | "", sizeof(g_config.mqttPass));
        strlcpy(g_config.mqttPrefix, doc["mqttPrefix"] | "fancontrol", sizeof(g_config.mqttPrefix));
        g_config.mqttPort = doc["mqttPort"] | 1883;
        configSave();
        sendJson(req, "{\"ok\":true}");
    });

    // ----------------------------------------------------------------
    // POST /api/config/alarms
    // Body: { "alarms": [ {"minRpm": N, "maxRpm": N}, … ] }  (5 elements)
    // Updates per-fan RPM alarm thresholds and persists to NVS.
    // ----------------------------------------------------------------
    onPost(server, "/api/config/alarms", [](AsyncWebServerRequest *req, uint8_t *data, size_t len) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) { req->send(400); return; }
        JsonArray arr = doc["alarms"].as<JsonArray>();
        for (int i = 0; i < NUM_FANS && i < (int)arr.size(); i++) {
            g_config.alarms[i].minRpm = arr[i]["minRpm"] | 0;
            g_config.alarms[i].maxRpm = arr[i]["maxRpm"] | 0;
        }
        configSave();
        sendJson(req, "{\"ok\":true}");
    });

    // ----------------------------------------------------------------
    // Static files from LittleFS — registered LAST so API routes win.
    // Serves index.html, style.css, app.js from the /data directory.
    // ----------------------------------------------------------------
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    server.onNotFound([](AsyncWebServerRequest *req) {
        req->send(404, "text/plain", "Not found");
    });

    server.begin();
}
