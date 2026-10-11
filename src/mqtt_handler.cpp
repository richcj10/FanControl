/**
 * @file mqtt_handler.cpp
 * @brief MQTT client — flat-topic status, generic alerts, HA discovery, and fan commands.
 *
 * Publishes scalar topics (rpm, pwm%, alert, temp) rather than JSON blobs so
 * Home Assistant sensors need no value_template.  Discovery configs are pushed
 * on every broker reconnect so HA self-heals after a restart.
 *
 * Only fans with @c FanDefault::disabled == false are published or subscribed.
 */

#include "mqtt_handler.h"
#include <WiFi.h>
#include <ETH.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "config.h"
#include "emc2305.h"
#include "fan_control.h"

static WiFiClient   ethClient;
static PubSubClient mqtt(ethClient);
// Prefix the current subscriptions / retained topics were made under
static char         activePrefix[40] = "";
static volatile bool configDirty = false;
static uint32_t     lastAttemptMs  = UINT32_MAX;
static bool         wasConnected   = false;
static bool         availPublished = false;

// Human-readable PubSubClient state codes
static const char* mqttStateStr(int s) {
    switch (s) {
        case -4: return "TIMEOUT — broker did not respond";
        case -3: return "CONNECTION LOST";
        case -2: return "TCP FAILED — wrong host or port?";
        case -1: return "DISCONNECTED";
        case  0: return "CONNECTED";
        case  1: return "BAD PROTOCOL";
        case  2: return "BAD CLIENT ID";
        case  3: return "BROKER UNAVAILABLE";
        case  4: return "BAD CREDENTIALS — wrong user/pass?";
        case  5: return "UNAUTHORIZED";
        default: return "UNKNOWN";
    }
}

// ----------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------
static bool pub(const char *topic, const char *payload, bool retain = false) {
    bool ok = mqtt.publish(topic, payload, retain);
    if (!ok) Serial.printf("MQTT publish FAILED: %s\n", topic);
    return ok;
}

// Unique ID: hostname_XXYY_suffix
static void makeUid(char *buf, size_t len, const char *suffix) {
    uint8_t mac[6];
    ETH.macAddress(mac);
    snprintf(buf, len, "%s_%02X%02X_%s", g_config.hostname, mac[4], mac[5], suffix);
}

static void addDevice(JsonObject dev) {
    uint8_t mac[6];
    ETH.macAddress(mac);
    char uid[24];
    snprintf(uid, sizeof(uid), "%s_%02X%02X", g_config.hostname, mac[4], mac[5]);
    JsonArray ids = dev["ids"].to<JsonArray>();
    ids.add(uid);
    dev["name"] = g_config.hostname;
    dev["mdl"]  = "WT32-ETH01 + EMC2305";
    dev["mf"]   = "DIY";
}

// ----------------------------------------------------------------
// Command callback
// ----------------------------------------------------------------

/** Trim whitespace (and one layer of double quotes) in place. */
static char* trim(char *s) {
    while (*s && isspace((uint8_t)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((uint8_t)s[n - 1])) s[--n] = '\0';
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') { s[n - 1] = '\0'; s++; }
    return s;
}

/**
 * Parse a power payload: ON/OFF, on/off, 1/0, true/false (any case) or
 * {"state":"ON"} / {"state":"OFF"}.  Returns 1, 0, or -1 if unrecognised.
 */
static int parsePower(char *val) {
    char *s = trim(val);
    JsonDocument doc;
    if (*s == '{') {
        if (deserializeJson(doc, s)) return -1;
        const char *st = doc["state"] | (const char*)nullptr;
        if (!st) return -1;
        static char buf[16];
        strlcpy(buf, st, sizeof(buf));
        s = trim(buf);
    }
    if (!strcasecmp(s, "on")  || !strcmp(s, "1") || !strcasecmp(s, "true"))  return 1;
    if (!strcasecmp(s, "off") || !strcmp(s, "0") || !strcasecmp(s, "false")) return 0;
    return -1;
}

/** Parse a plain number payload; false if it isn't one. */
static bool parseNumber(char *val, float &out) {
    char *s = trim(val), *end = nullptr;
    out = strtof(s, &end);
    return end != s && *end == '\0';
}

static void applyPower(int fanNum, bool on) {
    bool ok = on ? fanPowerOn(fanNum) : fanPowerOff(fanNum);
    if (!ok) Serial.printf("MQTT: fan%d power %s FAILED\n", fanNum, on ? "ON" : "OFF");
    mqttPublishFanState(fanNum);   // always report what the fan is really doing
}

static void onMessage(char *topic, byte *payload, unsigned int len) {
    char val[64];
    unsigned int n = (len < sizeof(val) - 1) ? len : sizeof(val) - 1;
    memcpy(val, payload, n);
    val[n] = '\0';
    Serial.printf("MQTT rx: %s = '%s'\n", topic, val);

    // Match against the prefix we actually subscribed with (it may have just changed)
    size_t plen = strlen(activePrefix);
    if (strncmp(topic, activePrefix, plen) != 0 || topic[plen] != '/') {
        Serial.println("MQTT reject: topic not under active prefix");
        return;
    }
    const char *rest = topic + plen + 1;

    // ---- all fans ----
    if (strcmp(rest, "all/set/power") == 0) {
        int p = parsePower(val);
        if (p < 0) { Serial.printf("MQTT reject: bad power payload '%s'\n", val); return; }
        for (int i = 1; i <= NUM_FANS; i++) {
            if (g_fanDefaults[i - 1].disabled) continue;
            applyPower(i, p == 1);
        }
        return;
    }

    // ---- single fan ----
    int  fanNum = 0, used = 0;
    char cmd[16] = "";
    if (sscanf(rest, "fan%d/set/%15s%n", &fanNum, cmd, &used) != 2 || rest[used] != '\0') {
        Serial.printf("MQTT reject: unknown topic '%s'\n", rest);
        return;
    }
    if (fanNum < 1 || fanNum > NUM_FANS) {
        Serial.printf("MQTT reject: fan %d out of range\n", fanNum);
        return;
    }
    if (g_fanDefaults[fanNum - 1].disabled) {
        Serial.printf("MQTT reject: fan%d is disabled\n", fanNum);
        return;
    }

    if (strcmp(cmd, "power") == 0) {
        int p = parsePower(val);
        if (p < 0) { Serial.printf("MQTT reject: bad power payload '%s'\n", val); return; }
        applyPower(fanNum, p == 1);

    } else if (strcmp(cmd, "pct") == 0) {
        float f;
        if (!parseNumber(val, f)) { Serial.printf("MQTT reject: bad pct payload '%s'\n", val); return; }
        if (!fanSetPct(fanNum, (int)(f + 0.5f)))
            Serial.printf("MQTT: fan%d set pct FAILED\n", fanNum);
        mqttPublishFanState(fanNum);

    } else if (strcmp(cmd, "rpm") == 0) {
        float f;
        if (!parseNumber(val, f) || f < 1) { Serial.printf("MQTT reject: bad rpm payload '%s'\n", val); return; }
        if (f > 65535) f = 65535;
        if (!fanSetRpm(fanNum, (uint16_t)f))
            Serial.printf("MQTT: fan%d set rpm FAILED\n", fanNum);
        mqttPublishFanState(fanNum);

    } else {
        Serial.printf("MQTT reject: unknown command '%s'\n", cmd);
    }
}

// ----------------------------------------------------------------
// Setup / reconnect
// ----------------------------------------------------------------
void mqttSetup() {
    mqtt.setServer(g_config.mqttHost, g_config.mqttPort);
    mqtt.setBufferSize(1024);
    mqtt.setCallback(onMessage);
    // Force an immediate first connection attempt when mqttLoop() is first called
    lastAttemptMs = millis() - 10001;
}

static void mqttSubscribeCommands() {
    strlcpy(activePrefix, g_config.mqttPrefix, sizeof(activePrefix));
    char topic[80];
    // + must be a complete topic level — "fan+" is invalid per MQTT spec.
    // The "+" in set/power also matches "all", giving <prefix>/all/set/power.
    snprintf(topic, sizeof(topic), "%s/+/set/pct",   activePrefix);
    mqtt.subscribe(topic);
    snprintf(topic, sizeof(topic), "%s/+/set/power", activePrefix);
    mqtt.subscribe(topic);
    snprintf(topic, sizeof(topic), "%s/+/set/rpm",   activePrefix);
    mqtt.subscribe(topic);
    Serial.printf("MQTT subscribed: %s/+/set/{pct,power,rpm}\n", activePrefix);
}

void mqttApplyConfig() { configDirty = true; }

/** Runs in the main loop: drop the session so reconnect re-subscribes and re-announces. */
static void mqttHandleConfigChange() {
    if (!configDirty) return;
    configDirty = false;
    if (mqtt.connected()) {
        char t[80];
        snprintf(t, sizeof(t), "%s/availability", activePrefix);
        mqtt.publish(t, "offline", true);   // old prefix is going away
        mqtt.disconnect();
    }
    mqtt.setServer(g_config.mqttHost, g_config.mqttPort);
    lastAttemptMs = millis() - 10001;       // reconnect immediately
    Serial.println("MQTT config changed — reconnecting");
}

static void mqttReconnect() {
    if (mqtt.connected()) return;

    uint32_t elapsed = millis() - lastAttemptMs;
    if (elapsed < 10000) {
        // Print a countdown every 2 s so you can see it's alive and waiting
        static uint32_t lastCountMs = 0;
        if (millis() - lastCountMs >= 2000) {
            lastCountMs = millis();
            Serial.printf("MQTT retry in %us  (broker: %s:%u)\n",
                          (unsigned)((10000 - elapsed) / 1000),
                          g_config.mqttHost, g_config.mqttPort);
        }
        return;
    }

    lastAttemptMs = millis();

    uint8_t mac[6];
    ETH.macAddress(mac);
    char clientId[48];
    snprintf(clientId, sizeof(clientId), "%s-%02X%02X", g_config.hostname, mac[4], mac[5]);

    char lwt[80];
    snprintf(lwt, sizeof(lwt), "%s/availability", g_config.mqttPrefix);

    const char *user = g_config.mqttUser[0] ? g_config.mqttUser : nullptr;
    const char *pass = g_config.mqttPass[0] ? g_config.mqttPass : nullptr;

    Serial.println("========== MQTT connect attempt ==========");
    Serial.printf("  Broker  : %s:%u\n",  g_config.mqttHost, g_config.mqttPort);
    Serial.printf("  ClientID: %s\n",     clientId);
    Serial.printf("  User    : %s\n",     g_config.mqttUser[0] ? g_config.mqttUser : "(none)");
    Serial.printf("  Password: %s\n",     g_config.mqttPass[0] ? "***" : "(none)");
    Serial.printf("  Prefix  : %s\n",     g_config.mqttPrefix);
    Serial.printf("  LWT     : %s\n",     lwt);
    Serial.println("------------------------------------------");

    if (mqtt.connect(clientId, user, pass, lwt, 1, true, "offline")) {
        Serial.println("  Result  : CONNECTED OK");
        Serial.println("==========================================");
        availPublished = pub(lwt, "online", true);
        mqttSubscribeCommands();
        mqttPublishDiscovery();
        for (int i = 1; i <= NUM_FANS; i++) mqttPublishFanState(i);
    } else {
        int rc = mqtt.state();
        Serial.printf("  Result  : FAILED rc=%d — %s\n", rc, mqttStateStr(rc));
        Serial.println("==========================================");
        Serial.println("  >> Check broker host/port, credentials, and network reachability");
    }
}

void mqttLoop() {
    bool now = mqtt.connected();

    // Detect drop — print once when connection is lost
    if (wasConnected && !now) {
        Serial.printf("MQTT connection LOST: rc=%d — %s\n",
                      mqtt.state(), mqttStateStr(mqtt.state()));
        availPublished = false;
    }
    wasConnected = now;

    mqttHandleConfigChange();
    mqttReconnect();
    mqtt.loop();
}

bool mqttConnected() { return mqtt.connected(); }

// ----------------------------------------------------------------
// Status publish — flat scalar topics, enabled fans only
// ----------------------------------------------------------------
void mqttPublishStatus() {
    if (!mqtt.connected()) return;

    // Republish availability if the initial post-connect publish failed
    if (!availPublished) {
        char avty[80];
        snprintf(avty, sizeof(avty), "%s/availability", g_config.mqttPrefix);
        availPublished = pub(avty, "online", true);
    }

    char topic[80], payload[32];

    for (int i = 0; i < NUM_FANS; i++) {
        if (g_fanDefaults[i].disabled) continue;
        int n = i + 1;

        snprintf(topic, sizeof(topic), "%s/fan%d/rpm", g_config.mqttPrefix, n);
        snprintf(payload, sizeof(payload), "%u", g_fans[i].rpm);
        pub(topic, payload, true);

        snprintf(topic, sizeof(topic), "%s/fan%d/pwm", g_config.mqttPrefix, n);
        snprintf(payload, sizeof(payload), "%u", g_fans[i].pwm * 100 / 255);
        pub(topic, payload, true);

        snprintf(topic, sizeof(topic), "%s/fan%d/alert", g_config.mqttPrefix, n);
        pub(topic, g_fans[i].anyAlarm() ? "ON" : "OFF", true);

        mqttPublishFanState(n);
    }

    // ESP32 die temp — plain float string
    snprintf(topic, sizeof(topic), "%s/temp", g_config.mqttPrefix);
    snprintf(payload, sizeof(payload), "%.1f", (float)temperatureRead());
    pub(topic, payload, true);

    // EMC2305 chip health
    snprintf(topic, sizeof(topic), "%s/emc2305", g_config.mqttPrefix);
    pub(topic, g_emc2305Ok ? "OK" : "FAULT", true);
}

// ----------------------------------------------------------------
// Fan power state + attributes  (<prefix>/fanN/state, <prefix>/fanN/attrs)
// ----------------------------------------------------------------
void mqttPublishFanState(uint8_t fanNum) {
    if (!mqtt.connected()) return;
    if (fanNum < 1 || fanNum > NUM_FANS) return;
    if (g_fanDefaults[fanNum - 1].disabled) return;
    const FanState &f = g_fans[fanNum - 1];

    char topic[80];
    snprintf(topic, sizeof(topic), "%s/fan%d/state", g_config.mqttPrefix, fanNum);
    pub(topic, f.on ? "ON" : "OFF", true);

    JsonDocument doc;
    doc["power"]   = f.on ? "ON" : "OFF";
    doc["mode"]    = f.closedLoop ? "closed_loop" : "pwm";
    if (f.closedLoop) doc["target_rpm"] = f.targetRpm;
    else              doc["target_rpm"] = nullptr;
    doc["rpm"]     = f.rpm;
    doc["pwm_pct"] = f.pwm * 100 / 255;
    char payload[160];
    serializeJson(doc, payload, sizeof(payload));
    snprintf(topic, sizeof(topic), "%s/fan%d/attrs", g_config.mqttPrefix, fanNum);
    pub(topic, payload, true);
}

// ----------------------------------------------------------------
// Generic alert publish — fires on any fault state change
// ----------------------------------------------------------------
void mqttPublishAlert(uint8_t fanNum, bool alert) {
    if (!mqtt.connected()) return;
    if (fanNum < 1 || fanNum > NUM_FANS) return;
    if (g_fanDefaults[fanNum - 1].disabled) return;

    char topic[80];
    snprintf(topic, sizeof(topic), "%s/fan%d/alert", g_config.mqttPrefix, fanNum);
    pub(topic, alert ? "ON" : "OFF", true);
}

// ----------------------------------------------------------------
// EMC2305 chip-health publish — fires immediately on state change
// ----------------------------------------------------------------
void mqttPublishEmc2305(bool ok) {
    if (!mqtt.connected()) return;
    char topic[80];
    snprintf(topic, sizeof(topic), "%s/emc2305", g_config.mqttPrefix);
    pub(topic, ok ? "OK" : "FAULT", true);
}

// ----------------------------------------------------------------
// HA auto-discovery  (enabled fans only)
// Publishes are spread with a small delay between each fan group
// to avoid overflowing the lwIP TCP send buffer.
// ----------------------------------------------------------------
void mqttPublishDiscovery() {
    if (!mqtt.connected()) return;

    char avty[80];
    snprintf(avty, sizeof(avty), "%s/availability", g_config.mqttPrefix);

    for (int i = 0; i < NUM_FANS; i++) {
        if (g_fanDefaults[i].disabled) continue;

        int  n = i + 1;
        char uid[48], suffix[24], discTopic[100], payload[800];
        char rpmTopic[80], pwmTopic[80], alertTopic[80], pctCmd[80], powerCmd[80];
        char stateTopic[80], attrsTopic[80];
        snprintf(stateTopic, sizeof(stateTopic), "%s/fan%d/state", g_config.mqttPrefix, n);
        snprintf(attrsTopic, sizeof(attrsTopic), "%s/fan%d/attrs", g_config.mqttPrefix, n);
        snprintf(rpmTopic,   sizeof(rpmTopic),   "%s/fan%d/rpm",       g_config.mqttPrefix, n);
        snprintf(pwmTopic,   sizeof(pwmTopic),   "%s/fan%d/pwm",       g_config.mqttPrefix, n);
        snprintf(alertTopic, sizeof(alertTopic), "%s/fan%d/alert",     g_config.mqttPrefix, n);
        snprintf(pctCmd,     sizeof(pctCmd),     "%s/fan%d/set/pct",   g_config.mqttPrefix, n);
        snprintf(powerCmd,   sizeof(powerCmd),   "%s/fan%d/set/power", g_config.mqttPrefix, n);

        // -- RPM sensor --
        {
            JsonDocument doc;
            snprintf(suffix, sizeof(suffix), "fan%d_rpm", n);
            makeUid(uid, sizeof(uid), suffix);
            char name[48]; snprintf(name, sizeof(name), "%s Fan %d RPM", g_config.hostname, n);
            doc["name"]         = name;
            doc["uniq_id"]      = uid;
            doc["avty_t"]       = avty;
            doc["stat_t"]       = rpmTopic;
            doc["unit_of_meas"] = "RPM";
            doc["stat_cla"]     = "measurement";
            doc["ic"]           = "mdi:fan";
            addDevice(doc["dev"].to<JsonObject>());
            serializeJson(doc, payload, sizeof(payload));
            snprintf(discTopic, sizeof(discTopic), "homeassistant/sensor/%s/fan%d_rpm/config", g_config.hostname, n);
            pub(discTopic, payload, true);
        }

        // -- PWM % sensor --
        {
            JsonDocument doc;
            snprintf(suffix, sizeof(suffix), "fan%d_pwm", n);
            makeUid(uid, sizeof(uid), suffix);
            char name[48]; snprintf(name, sizeof(name), "%s Fan %d PWM", g_config.hostname, n);
            doc["name"]         = name;
            doc["uniq_id"]      = uid;
            doc["avty_t"]       = avty;
            doc["stat_t"]       = pwmTopic;
            doc["unit_of_meas"] = "%";
            doc["stat_cla"]     = "measurement";
            doc["ic"]           = "mdi:speedometer";
            addDevice(doc["dev"].to<JsonObject>());
            serializeJson(doc, payload, sizeof(payload));
            snprintf(discTopic, sizeof(discTopic), "homeassistant/sensor/%s/fan%d_pwm/config", g_config.hostname, n);
            pub(discTopic, payload, true);
        }

        // -- Alert binary sensor (any fault) --
        {
            JsonDocument doc;
            snprintf(suffix, sizeof(suffix), "fan%d_alert", n);
            makeUid(uid, sizeof(uid), suffix);
            char name[48]; snprintf(name, sizeof(name), "%s Fan %d Alert", g_config.hostname, n);
            doc["name"]    = name;
            doc["uniq_id"] = uid;
            doc["avty_t"]  = avty;
            doc["dev_cla"] = "problem";
            doc["stat_t"]  = alertTopic;
            doc["pl_on"]   = "ON";
            doc["pl_off"]  = "OFF";
            addDevice(doc["dev"].to<JsonObject>());
            serializeJson(doc, payload, sizeof(payload));
            snprintf(discTopic, sizeof(discTopic), "homeassistant/binary_sensor/%s/fan%d_alert/config", g_config.hostname, n);
            pub(discTopic, payload, true);
        }

        // -- Fan control entity --
        {
            JsonDocument doc;
            snprintf(suffix, sizeof(suffix), "fan%d", n);
            makeUid(uid, sizeof(uid), suffix);
            char name[48]; snprintf(name, sizeof(name), "%s Fan %d", g_config.hostname, n);
            doc["name"]         = name;
            doc["uniq_id"]      = uid;
            doc["avty_t"]       = avty;
            doc["cmd_t"]        = powerCmd;
            doc["pl_on"]        = "ON";
            doc["pl_off"]       = "OFF";
            doc["stat_t"]       = stateTopic;     // real power state, ON/OFF
            doc["json_attr_t"]  = attrsTopic;     // mode, target_rpm, rpm, pwm_pct
            doc["pct_cmd_t"]    = pctCmd;
            doc["pct_stat_t"]   = pwmTopic;
            doc["spd_rng_min"]  = 1;
            doc["spd_rng_max"]  = 100;
            addDevice(doc["dev"].to<JsonObject>());
            size_t need = serializeJson(doc, payload, sizeof(payload));
            if (need >= sizeof(payload)) Serial.printf("MQTT discovery fan%d payload TRUNCATED (%u)\n", n, (unsigned)need);
            Serial.printf("MQTT discovery fan%d: %s\n", n, payload);
            snprintf(discTopic, sizeof(discTopic), "homeassistant/fan/%s/fan%d/config", g_config.hostname, n);
            pub(discTopic, payload, true);
        }

        // Yield after each fan's 4 publishes so lwIP can drain the TCP send buffer
        delay(20);
    }

    // -- ESP32 temperature sensor --
    {
        char uid[48], payload[600], discTopic[100], tempTopic[80];
        snprintf(tempTopic, sizeof(tempTopic), "%s/temp", g_config.mqttPrefix);
        makeUid(uid, sizeof(uid), "esp_temp");

        JsonDocument doc;
        char name[48]; snprintf(name, sizeof(name), "%s ESP32 Temp", g_config.hostname);
        doc["name"]         = name;
        doc["uniq_id"]      = uid;
        doc["avty_t"]       = avty;
        doc["dev_cla"]      = "temperature";
        doc["stat_t"]       = tempTopic;
        doc["unit_of_meas"] = "°C";
        doc["stat_cla"]     = "measurement";
        addDevice(doc["dev"].to<JsonObject>());
        serializeJson(doc, payload, sizeof(payload));
        snprintf(discTopic, sizeof(discTopic), "homeassistant/sensor/%s/esp_temp/config", g_config.hostname);
        pub(discTopic, payload, true);
    }

    // -- EMC2305 chip health binary sensor --
    {
        char uid[48], payload[600], discTopic[100], emcTopic[80];
        snprintf(emcTopic, sizeof(emcTopic), "%s/emc2305", g_config.mqttPrefix);
        makeUid(uid, sizeof(uid), "emc2305");

        JsonDocument doc;
        char name[48]; snprintf(name, sizeof(name), "%s EMC2305", g_config.hostname);
        doc["name"]    = name;
        doc["uniq_id"] = uid;
        doc["avty_t"]  = avty;
        doc["dev_cla"] = "problem";
        doc["stat_t"]  = emcTopic;
        doc["pl_on"]   = "FAULT";
        doc["pl_off"]  = "OK";
        addDevice(doc["dev"].to<JsonObject>());
        serializeJson(doc, payload, sizeof(payload));
        snprintf(discTopic, sizeof(discTopic), "homeassistant/binary_sensor/%s/emc2305/config", g_config.hostname);
        pub(discTopic, payload, true);
    }

    Serial.println("MQTT discovery published");
}
