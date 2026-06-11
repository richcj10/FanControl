/**
 * @file web_server.cpp
 * @brief REST API routes, ElegantOTA web-upload, and static file serving.
 */

#include "web_server.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ETH.h>
#include <Update.h>
#include "config.h"
#include "emc2305.h"
#include "mqtt_handler.h"

extern void applyFanDefaults();

static void sendJson(AsyncWebServerRequest *req, const String &json, int code = 200) {
    req->send(code, "application/json", json);
}

using BodyCb = std::function<void(AsyncWebServerRequest*, uint8_t*, size_t)>;

static void onPost(AsyncWebServer &server, const char *path, BodyCb cb) {
    server.on(path, HTTP_POST,
        [](AsyncWebServerRequest *req) {},
        nullptr,
        [cb](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
            cb(req, data, len);
        });
}

void webServerSetup(AsyncWebServer &server) {

    // ----------------------------------------------------------------
    // GET /api/status
    // ----------------------------------------------------------------
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument doc;
        doc["ip"]        = ETH.localIP().toString();
        doc["espTempC"]  = (float)temperatureRead();
        doc["mqttOnline"]= mqttConnected();
        doc["emc2305Ok"] = g_emc2305Ok;
        JsonArray fans  = doc["fans"].to<JsonArray>();
        for (int i = 0; i < NUM_FANS; i++) {
            JsonObject f = fans.add<JsonObject>();
            f["rpm"]        = g_fans[i].rpm;
            f["pwm"]        = g_fans[i].pwm;
            f["closedLoop"] = g_fans[i].closedLoop;
            f["targetRpm"]  = g_fans[i].targetRpm;
            f["disabled"]   = g_fanDefaults[i].disabled;
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
    // GET /api/config  (general + MQTT + alarms + OTA flags)
    // ----------------------------------------------------------------
    server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument doc;
        doc["hostname"]          = g_config.hostname;
        doc["mqttHost"]          = g_config.mqttHost;
        doc["mqttPort"]          = g_config.mqttPort;
        doc["mqttUser"]          = g_config.mqttUser;
        doc["mqttPrefix"]        = g_config.mqttPrefix;
        doc["otaWebEnabled"]     = g_config.otaWebEnabled;
        doc["otaArduinoEnabled"] = g_config.otaArduinoEnabled;
        doc["otaPasswordSet"]    = (g_config.otaPassword[0] != '\0');
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
    // POST /api/config/general  —  { "hostname": "…" }
    // Reboot required for hostname change to take effect on ETH/mDNS/OTA.
    // ----------------------------------------------------------------
    onPost(server, "/api/config/general", [](AsyncWebServerRequest *req, uint8_t *data, size_t len) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) { req->send(400); return; }
        if (!doc["hostname"].isNull()) {
            strlcpy(g_config.hostname, doc["hostname"] | "fancontrol", sizeof(g_config.hostname));
            // Sanitise: only alphanumeric + hyphen
            for (char *p = g_config.hostname; *p; p++) {
                if (!isalnum((uint8_t)*p) && *p != '-') *p = '-';
            }
        }
        configSave();
        sendJson(req, "{\"ok\":true}");
    });

    // ----------------------------------------------------------------
    // POST /api/fan/pwm
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
    // POST /api/fan/mode
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
    // POST /api/fan/target
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
    // POST /api/config/ota  —  { otaWebEnabled, otaArduinoEnabled, otaPassword }
    // Changes take effect after reboot (OTA is initialised once at startup).
    // ----------------------------------------------------------------
    onPost(server, "/api/config/ota", [](AsyncWebServerRequest *req, uint8_t *data, size_t len) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) { req->send(400); return; }
        if (!doc["otaWebEnabled"].isNull())
            g_config.otaWebEnabled = doc["otaWebEnabled"].as<bool>();
        if (!doc["otaArduinoEnabled"].isNull())
            g_config.otaArduinoEnabled = doc["otaArduinoEnabled"].as<bool>();
        if (!doc["otaPassword"].isNull())
            strlcpy(g_config.otaPassword, doc["otaPassword"] | "", sizeof(g_config.otaPassword));
        configSave();
        sendJson(req, "{\"ok\":true}");
    });

    // ----------------------------------------------------------------
    // GET /api/fan/defaults
    // ----------------------------------------------------------------
    server.on("/api/fan/defaults", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument doc;
        JsonArray arr = doc["fans"].to<JsonArray>();
        for (int i = 0; i < NUM_FANS; i++) {
            JsonObject o       = arr.add<JsonObject>();
            o["pwm"]           = g_fanDefaults[i].pwm;
            o["mode"]          = g_fanDefaults[i].disabled    ? "off"
                                 : g_fanDefaults[i].closedLoop ? "rpm" : "pwm";
            o["targetRpm"]     = g_fanDefaults[i].targetRpm;
            o["maxRpm"]        = g_fanDefaults[i].maxRpm;
            o["minDrive"]      = g_fanDefaults[i].minDrive;
            o["pulsesPerRev"]  = g_fanDefaults[i].pulsesPerRev;
            o["hardSpinup"]    = g_fanDefaults[i].hardSpinup;
        }
        String out;
        serializeJson(doc, out);
        sendJson(req, out);
    });

    // ----------------------------------------------------------------
    // POST /api/fan/defaults  —  { "fans": [ {mode, pwm, targetRpm}, … ] }
    // ----------------------------------------------------------------
    onPost(server, "/api/fan/defaults", [](AsyncWebServerRequest *req, uint8_t *data, size_t len) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) { req->send(400); return; }
        JsonArray arr = doc["fans"].as<JsonArray>();
        for (int i = 0; i < NUM_FANS && i < (int)arr.size(); i++) {
            const char* mode             = arr[i]["mode"]          | "pwm";
            g_fanDefaults[i].disabled    = (strcmp(mode, "off") == 0);
            g_fanDefaults[i].closedLoop  = (strcmp(mode, "rpm") == 0);
            g_fanDefaults[i].pwm         = arr[i]["pwm"]           | g_fanDefaults[i].pwm;
            g_fanDefaults[i].targetRpm   = arr[i]["targetRpm"]     | g_fanDefaults[i].targetRpm;
            uint16_t mx = arr[i]["maxRpm"] | g_fanDefaults[i].maxRpm;
            g_fanDefaults[i].maxRpm      = (mx >= 100) ? mx : g_fanDefaults[i].maxRpm;
            g_fanDefaults[i].minDrive    = arr[i]["minDrive"]      | g_fanDefaults[i].minDrive;
            uint8_t ppr = arr[i]["pulsesPerRev"] | g_fanDefaults[i].pulsesPerRev;
            g_fanDefaults[i].pulsesPerRev = (ppr >= 1) ? ppr : 1;
            g_fanDefaults[i].hardSpinup  = arr[i]["hardSpinup"]    | g_fanDefaults[i].hardSpinup;
        }
        configSave();
        if (g_emc2305Ok) applyFanDefaults();
        sendJson(req, "{\"ok\":true}");
    });

    // ----------------------------------------------------------------
    // POST /api/settings/upload  —  accepts a full settings.json body
    // Merges all recognised keys then writes /settings.json.
    // ----------------------------------------------------------------
    onPost(server, "/api/settings/upload", [](AsyncWebServerRequest *req, uint8_t *data, size_t len) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) { req->send(400); return; }

        JsonObject mqtt = doc["mqtt"];
        if (mqtt) {
            strlcpy(g_config.mqttHost,   mqtt["host"]   | g_config.mqttHost,   sizeof(g_config.mqttHost));
            g_config.mqttPort           = mqtt["port"]   | g_config.mqttPort;
            strlcpy(g_config.mqttUser,   mqtt["user"]   | g_config.mqttUser,   sizeof(g_config.mqttUser));
            strlcpy(g_config.mqttPass,   mqtt["pass"]   | g_config.mqttPass,   sizeof(g_config.mqttPass));
            strlcpy(g_config.mqttPrefix, mqtt["prefix"] | g_config.mqttPrefix, sizeof(g_config.mqttPrefix));
        }

        JsonArray alarms = doc["alarms"].as<JsonArray>();
        for (int i = 0; i < NUM_FANS && i < (int)alarms.size(); i++) {
            g_config.alarms[i].minRpm = alarms[i]["minRpm"] | g_config.alarms[i].minRpm;
            g_config.alarms[i].maxRpm = alarms[i]["maxRpm"] | g_config.alarms[i].maxRpm;
        }

        JsonObject ota = doc["ota"];
        if (ota) {
            g_config.otaWebEnabled     = ota["webEnabled"]     | g_config.otaWebEnabled;
            g_config.otaArduinoEnabled = ota["arduinoEnabled"] | g_config.otaArduinoEnabled;
            strlcpy(g_config.otaPassword, ota["password"] | g_config.otaPassword, sizeof(g_config.otaPassword));
        }

        JsonArray fans = doc["fans"].as<JsonArray>();
        for (int i = 0; i < NUM_FANS && i < (int)fans.size(); i++) {
            g_fanDefaults[i].pwm        = fans[i]["pwm"]       | g_fanDefaults[i].pwm;
            const char* mode            = fans[i]["mode"]       | "pwm";
            g_fanDefaults[i].closedLoop = (strcmp(mode, "rpm") == 0);
            g_fanDefaults[i].targetRpm  = fans[i]["targetRpm"] | g_fanDefaults[i].targetRpm;
            uint16_t mx = fans[i]["maxRpm"] | g_fanDefaults[i].maxRpm;
            g_fanDefaults[i].maxRpm     = (mx >= 100) ? mx : g_fanDefaults[i].maxRpm;
        }

        configSave();
        sendJson(req, "{\"ok\":true}");
        // OTA changes require a reboot — client should call /api/reboot
    });

    // ----------------------------------------------------------------
    // POST /api/reboot
    // ----------------------------------------------------------------
    server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *req) {
        sendJson(req, "{\"ok\":true}");
        delay(200);
        ESP.restart();
    });

    // ----------------------------------------------------------------
    // Web OTA  —  GET /update serves upload page, POST /update flashes
    // ----------------------------------------------------------------
    if (g_config.otaWebEnabled) {
        // Upload page — minimal dark-themed form, no LittleFS dependency
        server.on("/update", HTTP_GET, [](AsyncWebServerRequest *req) {
            req->send(200, "text/html",
                "<!DOCTYPE html><html><head>"
                "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>OTA Update</title>"
                "<style>"
                "body{background:#080c14;color:#dde6f2;font-family:system-ui,sans-serif;"
                "display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}"
                ".box{background:#0d1424;border:1px solid #1c304e;border-radius:14px;padding:32px;"
                "max-width:420px;width:90%;text-align:center}"
                "h2{color:#00d4ff;margin:0 0 24px;font-size:1rem;letter-spacing:.12em;text-transform:uppercase}"
                "input[type=file]{display:block;width:100%;background:#131d32;border:1px dashed #2a4878;"
                "border-radius:8px;padding:20px;color:#dde6f2;cursor:pointer;margin-bottom:20px;box-sizing:border-box}"
                "button{background:linear-gradient(135deg,#005fa0,#0090cc);color:#fff;border:none;"
                "border-radius:8px;padding:10px 28px;font-size:.85rem;font-weight:700;cursor:pointer;width:100%}"
                "button:disabled{opacity:.45;cursor:not-allowed}"
                "#st{margin-top:16px;font-family:monospace;font-size:.78rem;color:#4a6480;min-height:18px}"
                "a{color:#00d4ff;font-size:.72rem;display:block;margin-top:18px;text-decoration:none}"
                "</style></head><body>"
                "<div class='box'>"
                "<h2>Firmware Update</h2>"
                "<form id='f'>"
                "<input type='file' id='fw' accept='.bin' required>"
                "<button type='submit' id='btn'>Flash Firmware</button>"
                "</form>"
                "<div id='st'></div>"
                "<a href='/'>&#8592; Dashboard</a>"
                "</div>"
                "<script>"
                "document.getElementById('f').onsubmit=function(e){"
                "e.preventDefault();"
                "var fi=document.getElementById('fw').files[0];if(!fi)return;"
                "var btn=document.getElementById('btn'),st=document.getElementById('st');"
                "btn.disabled=true;st.textContent='Uploading… 0%';st.style.color='';"
                "var fd=new FormData();fd.append('firmware',fi);"
                "var x=new XMLHttpRequest();"
                "x.upload.onprogress=function(e){if(e.lengthComputable)"
                "st.textContent='Uploading… '+Math.round(e.loaded/e.total*100)+'%';};"
                "x.onload=function(){if(x.status===200){st.style.color='#00e676';"
                "st.textContent='Done — rebooting…';}else{st.style.color='#ff4444';"
                "st.textContent='Error: '+x.responseText;btn.disabled=false;}};"
                "x.onerror=function(){st.style.color='#ffab40';"
                "st.textContent='Connection lost — device may be rebooting';};"
                "x.open('POST','/update');x.send(fd);};"
                "</script></body></html>"
            );
        });

        server.on("/update", HTTP_POST,
            [](AsyncWebServerRequest *req) {
                bool ok = !Update.hasError();
                AsyncWebServerResponse *resp =
                    req->beginResponse(200, "text/plain", ok ? "OK" : Update.errorString());
                resp->addHeader("Connection", "close");
                req->send(resp);
                if (ok) { delay(200); ESP.restart(); }
            },
            [](AsyncWebServerRequest *req, String /*filename*/, size_t index,
               uint8_t *data, size_t len, bool final) {
                if (index == 0) {
                    Serial.printf("OTA start: %u bytes free\n", ESP.getFreeSketchSpace());
                    if (!Update.begin(UPDATE_SIZE_UNKNOWN))
                        Update.printError(Serial);
                }
                if (!Update.hasError() && Update.write(data, len) != len)
                    Update.printError(Serial);
                if (final) {
                    if (Update.end(true))
                        Serial.printf("OTA done: %u bytes\n", index + len);
                    else
                        Update.printError(Serial);
                }
            }
        );
        Serial.println("Web OTA enabled at /update");
    }

    // ----------------------------------------------------------------
    // Static files from LittleFS — registered LAST so API routes win.
    // ----------------------------------------------------------------
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    server.onNotFound([](AsyncWebServerRequest *req) {
        req->send(404, "text/plain", "Not found");
    });

    server.begin();
}

void webServerLoop() {
    // Reserved for future per-loop web server tasks.
}
