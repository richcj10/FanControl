/**
 * @file mqtt_handler.h
 * @brief MQTT publish interface — status, alerts, HA auto-discovery, and fan commands.
 *
 * Only fans not marked @c disabled in @ref FanDefault are published.
 *
 * ## Published topics  (all retained)
 * | Topic                           | Payload            | Notes                     |
 * |---------------------------------|--------------------|---------------------------|
 * | `<prefix>/availability`         | `online`/`offline` | LWT; set offline on drop  |
 * | `<prefix>/fanN/rpm`             | e.g. `"1200"`      | RPM integer string        |
 * | `<prefix>/fanN/pwm`             | `"0"`–`"100"`      | Duty cycle as %           |
 * | `<prefix>/fanN/alert`           | `ON` / `OFF`       | Any fault active          |
 * | `<prefix>/temp`                 | e.g. `"45.2"`      | ESP32 die temp in °C      |
 *
 * ## Subscribed topics  (incoming commands)
 * | Topic                           | Payload            | Action                    |
 * |---------------------------------|--------------------|---------------------------|
 * | `<prefix>/+/set/pct`            | `"0"`–`"100"`      | Set fan speed %           |
 * | `<prefix>/+/set/power`          | `ON` / `OFF`       | Fan on (restore default) or off |
 *
 * ## HA auto-discovery  (retained, published on every connect)
 * | Component       | Path                                     |
 * |-----------------|------------------------------------------|
 * | `sensor`        | `homeassistant/sensor/<host>/fanN_rpm`   |
 * | `sensor`        | `homeassistant/sensor/<host>/fanN_pwm`   |
 * | `binary_sensor` | `homeassistant/binary_sensor/<host>/fanN_alert` |
 * | `fan`           | `homeassistant/fan/<host>/fanN`          |
 * | `sensor`        | `homeassistant/sensor/<host>/esp_temp`   |
 */

#pragma once
#include <stdint.h>

void mqttSetup();
void mqttLoop();
bool mqttConnected();

/** Publish rpm, pwm%, and alert state for all enabled fans + ESP32 temp. */
void mqttPublishStatus();

/** Publish a single fan's generic alert state (any fault → ON). */
void mqttPublishAlert(uint8_t fanNum, bool alert);

/** Publish HA MQTT auto-discovery config for all enabled fans. */
void mqttPublishDiscovery();
