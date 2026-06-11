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

static WiFiClient   ethClient;
static PubSubClient mqtt(ethClient);
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
static void onMessage(char *topic, byte *payload, unsigned int len) {
    const char *rest = topic + strlen(g_config.mqttPrefix) + 1;
    int  fanNum = 0;
    char cmd[16] = "";
    if (sscanf(rest, "fan%d/set/%15s", &fanNum, cmd) != 2) return;
    if (fanNum < 1 || fanNum > NUM_FANS) return;
    if (g_fanDefaults[fanNum - 1].disabled) return;

    char val[16] = "";
    unsigned int n = (len < sizeof(val) - 1) ? len : sizeof(val) - 1;
    memcpy(val, payload, n);
    val[n] = '\0';

    if (strcmp(cmd, "pct") == 0) {
        int pct = constrain(atoi(val), 0, 100);
        uint8_t pwm = (uint8_t)((pct * 255 + 50) / 100);
        if (emc2305SetFanPWM(fanNum, pwm)) g_fans[fanNum - 1].pwm = pwm;

    } else if (strcmp(cmd, "power") == 0) {
        if (strcmp(val, "OFF") == 0) {
            if (emc2305SetFanPWM(fanNum, 0)) g_fans[fanNum - 1].pwm = 0;
        } else if (strcmp(val, "ON") == 0) {
            uint8_t pwm = g_fanDefaults[fanNum - 1].pwm ? g_fanDefaults[fanNum - 1].pwm : 64;
            if (emc2305SetFanPWM(fanNum, pwm)) g_fans[fanNum - 1].pwm = pwm;
        }
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
    char topic[80];
    // + must be a complete topic level — "fan+" is invalid per MQTT spec
    snprintf(topic, sizeof(topic), "%s/+/set/pct",   g_config.mqttPrefix);
    mqtt.subscribe(topic);
    snprintf(topic, sizeof(topic), "%s/+/set/power", g_config.mqttPrefix);
    mqtt.subscribe(topic);
    Serial.printf("MQTT subscribed: %s/+/set/{pct,power}\n", g_config.mqttPrefix);
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
        char statusTopic[80];
        snprintf(statusTopic, sizeof(statusTopic), "%s/fan%d/status", g_config.mqttPrefix, n);

        char uid[48], suffix[24], discTopic[100], payload[700];
        char rpmTopic[80], pwmTopic[80], alertTopic[80], pctCmd[80], powerCmd[80];
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
            doc["stat_t"]       = pwmTopic;
            doc["stat_val_tpl"] = "{{ 'ON' if value|int > 0 else 'OFF' }}";
            doc["pct_cmd_t"]    = pctCmd;
            doc["pct_stat_t"]   = pwmTopic;
            doc["spd_rng_min"]  = 1;
            doc["spd_rng_max"]  = 100;
            addDevice(doc["dev"].to<JsonObject>());
            serializeJson(doc, payload, sizeof(payload));
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
