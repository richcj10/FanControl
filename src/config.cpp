/**
 * @file config.cpp
 * @brief NVS persistence implementation for application configuration.
 *
 * Uses the Arduino ESP32 Preferences library (thin wrapper around NVS) to
 * store and retrieve the AppConfig struct.  A single "init" boolean key acts
 * as a sentinel so the first boot can detect an uninitialised partition and
 * apply factory defaults rather than reading garbage bytes.
 */

#include "config.h"
#include <Preferences.h>
#include <string.h>

/** @brief Global configuration instance, shared across all modules. */
AppConfig g_config;

/** @brief Live fan-state array, shared across all modules. */
FanState  g_fans[NUM_FANS];

/**
 * @brief Apply factory-default values to @ref g_config.
 *
 * Called on first boot when the NVS "init" sentinel is absent.
 * Credentials are left empty; the operator must configure them via the
 * web UI before MQTT will connect.
 */
static void configDefault() {
    strlcpy(g_config.mqttHost,   "192.168.1.1", sizeof(g_config.mqttHost));
    g_config.mqttPort    = 1883;
    g_config.mqttUser[0] = '\0';
    g_config.mqttPass[0] = '\0';
    strlcpy(g_config.mqttPrefix, "fancontrol",  sizeof(g_config.mqttPrefix));
    memset(g_config.alarms, 0, sizeof(g_config.alarms));
}

/**
 * @brief Load configuration from NVS into @ref g_config.
 *
 * Opens the "fanctrl" namespace in read-only mode.  If the "init" key is
 * missing (first boot or erased flash), defaults are applied and saved so
 * subsequent boots find a valid blob.
 */
void configLoad() {
    Preferences p;
    p.begin("fanctrl", true);
    if (!p.isKey("init")) {
        p.end();
        configDefault();
        configSave();
        return;
    }
    p.getBytes("cfg", &g_config, sizeof(g_config));
    p.end();
}

/**
 * @brief Persist @ref g_config to NVS.
 *
 * Opens the "fanctrl" namespace in read-write mode, sets the "init"
 * sentinel, and writes the full AppConfig struct as a raw byte blob.
 * The NVS partition must not be full; if it is, Preferences will silently
 * fail — check serial output if settings do not survive a reboot.
 */
void configSave() {
    Preferences p;
    p.begin("fanctrl", false);
    p.putBool("init", true);
    p.putBytes("cfg", &g_config, sizeof(g_config));
    p.end();
}
