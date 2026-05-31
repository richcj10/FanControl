/**
 * @file mqtt_handler.h
 * @brief MQTT publish interface for fan status and alarm events.
 *
 * Uses PubSubClient over a WiFiClient, which on the ESP32 routes through
 * lwIP regardless of whether the active interface is Wi-Fi or Ethernet.
 *
 * ## Topic schema
 * All topics are prefixed with @ref AppConfig::mqttPrefix (default "fancontrol").
 *
 * | Topic                              | Payload           | Retained |
 * |------------------------------------|-------------------|----------|
 * | `<prefix>/fanN/status`             | JSON object       | No       |
 * | `<prefix>/fanN/alarm/stall`        | "1" or "0"        | Yes      |
 * | `<prefix>/fanN/alarm/spin_fail`    | "1" or "0"        | Yes      |
 * | `<prefix>/fanN/alarm/drive_fail`   | "1" or "0"        | Yes      |
 *
 * Alarm messages are published with the retained flag so Home Assistant
 * receives the last known state immediately on (re)subscription.
 *
 * @note Call @ref mqttSetup() once the Ethernet link is up and
 *       @ref g_config has been loaded.  Call @ref mqttLoop() every iteration
 *       of the Arduino loop() to maintain the connection and process
 *       incoming keepalives.
 */

#pragma once
#include <stdint.h>

/**
 * @brief Configure the MQTT client from the current @ref g_config.
 *
 * Must be called after the Ethernet link is established and
 * @ref configLoad() has populated g_config.  Re-call this function
 * after saving new MQTT credentials to apply them.
 */
void mqttSetup();

/**
 * @brief Maintain the MQTT connection and process keepalives.
 *
 * Calls the PubSubClient loop (required for keepalive packets) and
 * attempts a reconnect if the client has disconnected.  Reconnect
 * attempts are rate-limited to once per 10 seconds to prevent
 * flooding the broker during an outage.
 *
 * Must be called from the main Arduino loop().
 */
void mqttLoop();

/**
 * @brief Publish the current RPM, PWM, and alarm state for all fans.
 *
 * Serialises each @ref FanState to a JSON object and publishes it to
 * @c <prefix>/fanN/status.  No-op if the client is not connected.
 * Intended to be called on a fixed interval (every 5 s in main.cpp).
 */
void mqttPublishStatus();

/**
 * @brief Publish a single alarm state change for one fan channel.
 *
 * Published with the MQTT retained flag so Home Assistant sees the
 * last state on reconnect.  No-op if the client is not connected.
 *
 * @param fanNum  Fan number, 1–5.
 * @param type    Alarm type string, e.g. "stall", "spin_fail", "drive_fail".
 * @param active  true if the alarm just activated, false if it cleared.
 */
void mqttPublishAlarm(uint8_t fanNum, const char *type, bool active);
