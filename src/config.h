/**
 * @file config.h
 * @brief Application configuration types and NVS persistence interface.
 *
 * Defines the structures that hold persisted settings (MQTT credentials,
 * per-fan alarm thresholds) and the live fan state that is shared between
 * the EMC2305 driver, web server, and MQTT handler.
 *
 * Global instances @ref g_config and @ref g_fans are defined in config.cpp
 * and accessed by all modules via these extern declarations.
 */

#pragma once
#include <Arduino.h>

/** @brief Number of fan channels supported by the EMC2305. */
#define NUM_FANS 5

/**
 * @brief Per-fan RPM alarm threshold pair.
 *
 * A value of 0 disables the corresponding threshold check.
 */
struct FanAlarmConfig {
    uint16_t minRpm; /**< Lower RPM limit; alarm if RPM drops below this while running. */
    uint16_t maxRpm; /**< Upper RPM limit; alarm if RPM exceeds this value. */
};

/**
 * @brief Complete application configuration stored in NVS.
 *
 * Written to the "fanctrl" NVS namespace as a raw byte blob by
 * @ref configSave() and restored by @ref configLoad(). Changing the
 * struct layout without bumping a version key will corrupt existing NVS data.
 */
struct AppConfig {
    char          mqttHost[64];       /**< MQTT broker hostname or dotted-decimal IP. */
    uint16_t      mqttPort;           /**< MQTT broker port (default 1883). */
    char          mqttUser[32];       /**< MQTT username; empty string for anonymous. */
    char          mqttPass[32];       /**< MQTT password; empty string for anonymous. */
    char          mqttPrefix[40];     /**< Topic prefix, e.g. "fancontrol". */
    FanAlarmConfig alarms[NUM_FANS];  /**< Per-fan alarm thresholds, indexed 0–4. */
};

/**
 * @brief Live runtime state for a single fan channel.
 *
 * Updated every 500 ms by the tach poll in main.cpp and read by the
 * web server and MQTT publisher. Not persisted to NVS.
 */
struct FanState {
    uint16_t rpm;          /**< Last measured speed in RPM (0 = stopped/unknown). */
    uint8_t  pwm;          /**< Current FAN_DRIVE register value (0–255). */
    bool     closedLoop;   /**< true = EMC2305 hardware RPM control (ENAG=1). */
    uint16_t targetRpm;    /**< Closed-loop RPM setpoint (only valid when closedLoop=true). */
    bool     alarmStall;   /**< Fan stall detected by EMC2305 hardware (reg 0x25). */
    bool     alarmSpinFail;/**< Spin-up failure detected by EMC2305 hardware (reg 0x26). */
    bool     alarmDriveFail;/**< Drive-fail: chip cannot reach target (reg 0x27). */
    bool     alarmRpmLow;  /**< Software alarm: RPM below @ref FanAlarmConfig::minRpm. */
    bool     alarmRpmHigh; /**< Software alarm: RPM above @ref FanAlarmConfig::maxRpm. */

    /**
     * @brief Returns true if any alarm flag is set.
     * @return true if at least one alarm is active.
     */
    bool anyAlarm() const {
        return alarmStall || alarmSpinFail || alarmDriveFail || alarmRpmLow || alarmRpmHigh;
    }
};

/** @brief Global persisted configuration. Modified by web API, saved to NVS. */
extern AppConfig g_config;

/** @brief Live state for all fan channels, indexed 0–(NUM_FANS-1). */
extern FanState  g_fans[NUM_FANS];

/**
 * @brief Load configuration from NVS into @ref g_config.
 *
 * If no configuration has been saved yet, applies factory defaults and
 * immediately calls @ref configSave() so the key exists on next boot.
 */
void configLoad();

/**
 * @brief Persist @ref g_config to NVS.
 *
 * Writes the full AppConfig struct as a byte blob under the "fanctrl"
 * NVS namespace.  Safe to call from any task that has completed
 * @ref configLoad() first.
 */
void configSave();
