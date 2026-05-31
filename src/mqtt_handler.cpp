/**
 * @file mqtt_handler.cpp
 * @brief MQTT client implementation using PubSubClient over Ethernet.
 *
 * WiFiClient is used as the TCP transport because on the ESP32, both the
 * Wi-Fi and Ethernet interfaces share the same lwIP network stack.
 * WiFiClient therefore works transparently over an Ethernet (ETH.h) link
 * without any additional adapter layer.
 *
 * The client ID is fixed as "fancontrol-esp32".  If multiple units share a
 * broker, each must have a unique client ID — consider appending the MAC
 * address suffix if deploying more than one device.
 */

#include "mqtt_handler.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "config.h"

/** @brief TCP transport; routes through lwIP over either Wi-Fi or Ethernet. */
static WiFiClient   ethClient;

/** @brief MQTT client bound to the shared TCP transport. */
static PubSubClient mqtt(ethClient);

/** @brief Timestamp of the last broker connection attempt (ms since boot). */
static uint32_t lastAttemptMs = 0;

/**
 * @brief Configure the MQTT client from g_config and set the packet buffer size.
 *
 * Does not immediately connect — the first connection attempt happens the
 * next time @ref mqttLoop() is called and finds the client disconnected.
 */
void mqttSetup() {
    mqtt.setServer(g_config.mqttHost, g_config.mqttPort);
    mqtt.setBufferSize(512);
}

/**
 * @brief Attempt to connect to the broker if disconnected.
 *
 * Rate-limited to one attempt every 10 seconds.  Uses anonymous access
 * if mqttUser is empty, otherwise passes credentials from g_config.
 */
static void mqttReconnect() {
    if (mqtt.connected() || millis() - lastAttemptMs < 10000) return;
    lastAttemptMs = millis();
    const char *user = g_config.mqttUser[0] ? g_config.mqttUser : nullptr;
    const char *pass = g_config.mqttPass[0] ? g_config.mqttPass : nullptr;
    mqtt.connect("fancontrol-esp32", user, pass);
}

/**
 * @brief Service the MQTT connection — reconnect if needed, send keepalives.
 *
 * Must be called every iteration of loop().  PubSubClient::loop() sends
 * PINGREQ packets to keep the broker connection alive within the keepalive
 * window (default 15 s).
 */
void mqttLoop() {
    mqttReconnect();
    mqtt.loop();
}

/**
 * @brief Publish the full status JSON for every fan channel.
 *
 * Iterates over @ref g_fans and publishes one JSON message per fan to
 * @c <prefix>/fanN/status.  Messages are not retained so stale data is
 * not replayed to new subscribers after a reboot.
 */
void mqttPublishStatus() {
    if (!mqtt.connected()) return;
    char topic[80];
    char payload[200];
    for (int i = 0; i < NUM_FANS; i++) {
        JsonDocument doc;
        doc["rpm"]        = g_fans[i].rpm;
        doc["pwm"]        = g_fans[i].pwm;
        doc["closedLoop"] = g_fans[i].closedLoop;
        doc["stall"]      = g_fans[i].alarmStall;
        doc["spinFail"]   = g_fans[i].alarmSpinFail;
        doc["driveFail"]  = g_fans[i].alarmDriveFail;
        doc["rpmLow"]     = g_fans[i].alarmRpmLow;
        doc["rpmHigh"]    = g_fans[i].alarmRpmHigh;
        serializeJson(doc, payload, sizeof(payload));
        snprintf(topic, sizeof(topic), "%s/fan%d/status", g_config.mqttPrefix, i + 1);
        mqtt.publish(topic, payload);
    }
}

/**
 * @brief Publish a retained alarm state change for one fan.
 *
 * Published with @c retain=true so Home Assistant receives the last known
 * alarm state immediately when it subscribes (e.g. after an HA restart).
 * Payload is "1" (active) or "0" (cleared).
 *
 * @param fanNum  1–5.
 * @param type    Short alarm identifier: "stall", "spin_fail", or "drive_fail".
 * @param active  true if alarm just fired, false if it just cleared.
 */
void mqttPublishAlarm(uint8_t fanNum, const char *type, bool active) {
    if (!mqtt.connected()) return;
    char topic[80];
    snprintf(topic, sizeof(topic), "%s/fan%d/alarm/%s", g_config.mqttPrefix, fanNum, type);
    mqtt.publish(topic, active ? "1" : "0", true);
}
