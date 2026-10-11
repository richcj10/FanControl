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
 * | `<prefix>/emc2305`              | `OK` / `FAULT`     | Fan IC reachable on I2C   |
 *
 * ## Subscribed topics  (incoming commands)
 * | Topic                           | Payload            | Action                    |
 * |---------------------------------|--------------------|---------------------------|
 * | `<prefix>/fanN/set/power`       | ON/OFF/on/off/1/0/true/false or `{"state":"ON"}` | Stop (any mode) / restore saved state |
 * | `<prefix>/all/set/power`        | same              | Same, for every enabled fan |
 * | `<prefix>/fanN/set/pct`         | `"0"`–`"100"`      | Direct PWM % (forces PWM mode; 0 = off) |
 * | `<prefix>/fanN/set/rpm`         | RPM integer        | Closed-loop at target, fan on |
 *
 * Also published (retained): `<prefix>/fanN/state` (`ON`/`OFF`) and
 * `<prefix>/fanN/attrs` (JSON: power, mode, target_rpm, rpm, pwm_pct).
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

/** Publish a fan's real power state and attributes — call after any change. */
void mqttPublishFanState(uint8_t fanNum);

/** Config (host/port/prefix) changed: reconnect and re-subscribe on the main loop. */
void mqttApplyConfig();

/** Publish a single fan's generic alert state (any fault → ON). */
void mqttPublishAlert(uint8_t fanNum, bool alert);

/** Publish EMC2305 chip health immediately — call on state change. */
void mqttPublishEmc2305(bool ok);

/** Publish HA MQTT auto-discovery config for all enabled fans. */
void mqttPublishDiscovery();
