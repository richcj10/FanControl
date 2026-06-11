/**
 * @file config.cpp
 * @brief LittleFS JSON persistence for all application settings (/settings.json).
 */

#include "config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <string.h>

AppConfig  g_config;
FanState   g_fans[NUM_FANS];
FanDefault g_fanDefaults[NUM_FANS];

#define SETTINGS_FILE "/settings.json"

static void applyDefaults() {
    strlcpy(g_config.hostname,   "fancontrol",  sizeof(g_config.hostname));
    strlcpy(g_config.mqttHost,   "192.168.1.1", sizeof(g_config.mqttHost));
    g_config.mqttPort           = 1883;
    g_config.mqttUser[0]        = '\0';
    g_config.mqttPass[0]        = '\0';
    strlcpy(g_config.mqttPrefix, "fancontrol",  sizeof(g_config.mqttPrefix));
    memset(g_config.alarms, 0, sizeof(g_config.alarms));
    g_config.otaWebEnabled      = true;
    g_config.otaArduinoEnabled  = true;
    g_config.otaPassword[0]     = '\0';
    for (int i = 0; i < NUM_FANS; i++)
        g_fanDefaults[i] = {64, false, 1200, 3000, 0, 1, false, false};
}

void configLoad() {
    applyDefaults();

    File f = LittleFS.open(SETTINGS_FILE, "r");
    if (!f) return;

    JsonDocument doc;
    if (deserializeJson(doc, f)) { f.close(); return; }
    f.close();

    if (doc["hostname"].is<const char*>())
        strlcpy(g_config.hostname, doc["hostname"] | "fancontrol", sizeof(g_config.hostname));

    JsonObject mqtt = doc["mqtt"];
    if (mqtt) {
        strlcpy(g_config.mqttHost,   mqtt["host"]   | "192.168.1.1", sizeof(g_config.mqttHost));
        g_config.mqttPort           = mqtt["port"]   | 1883;
        strlcpy(g_config.mqttUser,   mqtt["user"]   | "", sizeof(g_config.mqttUser));
        strlcpy(g_config.mqttPass,   mqtt["pass"]   | "", sizeof(g_config.mqttPass));
        strlcpy(g_config.mqttPrefix, mqtt["prefix"] | "fancontrol", sizeof(g_config.mqttPrefix));
    }

    JsonArray alarms = doc["alarms"].as<JsonArray>();
    for (int i = 0; i < NUM_FANS && i < (int)alarms.size(); i++) {
        g_config.alarms[i].minRpm = alarms[i]["minRpm"] | 0;
        g_config.alarms[i].maxRpm = alarms[i]["maxRpm"] | 0;
    }

    JsonObject ota = doc["ota"];
    if (ota) {
        g_config.otaWebEnabled     = ota["webEnabled"]     | true;
        g_config.otaArduinoEnabled = ota["arduinoEnabled"] | true;
        strlcpy(g_config.otaPassword, ota["password"] | "", sizeof(g_config.otaPassword));
    }

    JsonArray fans = doc["fans"].as<JsonArray>();
    for (int i = 0; i < NUM_FANS && i < (int)fans.size(); i++) {
        g_fanDefaults[i].pwm          = fans[i]["pwm"]          | 64;
        const char* mode              = fans[i]["mode"]          | "pwm";
        g_fanDefaults[i].closedLoop   = (strcmp(mode, "rpm") == 0);
        g_fanDefaults[i].targetRpm    = fans[i]["targetRpm"]    | 1200;
        g_fanDefaults[i].minDrive     = fans[i]["minDrive"]     | 0;
        g_fanDefaults[i].pulsesPerRev = fans[i]["pulsesPerRev"] | 1;
        if (g_fanDefaults[i].pulsesPerRev < 1) g_fanDefaults[i].pulsesPerRev = 1;
        g_fanDefaults[i].maxRpm       = fans[i]["maxRpm"]       | 3000;
        if (g_fanDefaults[i].maxRpm < 100) g_fanDefaults[i].maxRpm = 3000;
        g_fanDefaults[i].hardSpinup   = fans[i]["hardSpinup"]   | false;
        g_fanDefaults[i].disabled     = fans[i]["disabled"]     | false;
    }
}

void configSave() {
    JsonDocument doc;

    doc["hostname"] = g_config.hostname;

    JsonObject mqtt    = doc["mqtt"].to<JsonObject>();
    mqtt["host"]       = g_config.mqttHost;
    mqtt["port"]       = g_config.mqttPort;
    mqtt["user"]       = g_config.mqttUser;
    mqtt["pass"]       = g_config.mqttPass;
    mqtt["prefix"]     = g_config.mqttPrefix;

    JsonArray alarms = doc["alarms"].to<JsonArray>();
    for (int i = 0; i < NUM_FANS; i++) {
        JsonObject a  = alarms.add<JsonObject>();
        a["minRpm"]   = g_config.alarms[i].minRpm;
        a["maxRpm"]   = g_config.alarms[i].maxRpm;
    }

    JsonObject ota         = doc["ota"].to<JsonObject>();
    ota["webEnabled"]      = g_config.otaWebEnabled;
    ota["arduinoEnabled"]  = g_config.otaArduinoEnabled;
    ota["password"]        = g_config.otaPassword;

    JsonArray fans = doc["fans"].to<JsonArray>();
    for (int i = 0; i < NUM_FANS; i++) {
        JsonObject o        = fans.add<JsonObject>();
        o["pwm"]            = g_fanDefaults[i].pwm;
        o["mode"]           = g_fanDefaults[i].disabled    ? "off"
                              : g_fanDefaults[i].closedLoop ? "rpm" : "pwm";
        o["disabled"]       = g_fanDefaults[i].disabled;
        o["targetRpm"]      = g_fanDefaults[i].targetRpm;
        o["maxRpm"]         = g_fanDefaults[i].maxRpm;
        o["minDrive"]       = g_fanDefaults[i].minDrive;
        o["pulsesPerRev"]   = g_fanDefaults[i].pulsesPerRev;
        o["hardSpinup"]     = g_fanDefaults[i].hardSpinup;
    }

    File f = LittleFS.open(SETTINGS_FILE, "w");
    if (!f) return;
    serializeJson(doc, f);
    f.close();
}
