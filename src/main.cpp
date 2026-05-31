/**
 * @file main.cpp
 * @brief Application entry point — hardware init, Ethernet events, and main loop.
 *
 * ## Hardware
 * - **MCU**: WT32-ETH01 (ESP32 + LAN8720 PHY on built-in EMAC)
 * - **Fan controller**: EMC2305 on I2C (SDA=33, SCL=32, addr=0x2C)
 * - **Ethernet clock**: 50 MHz oscillator output on GPIO0 (ETH_CLOCK_GPIO0_IN)
 *
 * ## Boot sequence
 * 1. Load persisted config from NVS (@ref configLoad).
 * 2. Mount LittleFS (web UI assets).
 * 3. Initialise I2C and the EMC2305 fan controller.
 * 4. Enable watchdog and per-fan ramp-rate control.
 * 5. Register Ethernet event handler and start the ETH driver.
 * 6. Register HTTP routes and start the web server.
 *
 * ## Main loop timing
 * | Task                   | Interval |
 * |------------------------|----------|
 * | Tach + PWM poll        | 500 ms   |
 * | Alarm check            | 1000 ms  |
 * | MQTT status publish    | 5000 ms  |
 * | MQTT keepalive (loop)  | every iteration |
 */

#include <Arduino.h>
#include <Wire.h>
#include <ETH.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>

#include "config.h"
#include "emc2305.h"
#include "web_server.h"
#include "mqtt_handler.h"

/** @brief I2C data pin (SDA) connected to EMC2305. */
#define I2C_SDA 33

/** @brief I2C clock pin (SCL) connected to EMC2305. */
#define I2C_SCL 32

/** @brief LAN8720 PHY I2C address on the WT32-ETH01 board. */
#define ETH_PHY_ADDR  1

/** @brief GPIO pin used to control LAN8720 power-enable. */
#define ETH_PHY_POWER 16

/** @brief RMII MDC clock pin (management data clock). */
#define ETH_PHY_MDC   23

/** @brief RMII MDIO data pin (management data I/O). */
#define ETH_PHY_MDIO  18

/**
 * @brief Ethernet reference clock mode.
 *
 * The WT32-ETH01 routes the LAN8720's 50 MHz output into ESP32 GPIO0,
 * so the ESP32 acts as a clock consumer (not generator).
 */
#define ETH_CLK_MODE  ETH_CLOCK_GPIO0_IN

/** @brief HTTP server instance on port 80. */
static AsyncWebServer server(80);

/** @brief Set to true once ARDUINO_EVENT_ETH_GOT_IP fires. */
static bool ethUp = false;

/**
 * @brief Ethernet and IP event handler.
 *
 * Called from the ESP-IDF event loop (not from loop()).  Only lightweight
 * operations (hostname assignment, flag update, mqttSetup) are performed
 * here to avoid blocking the network stack.
 *
 * @param event Network event identifier from the Arduino ETH library.
 */
static void onEthEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_ETH_START:
            ETH.setHostname("fancontrol");
            break;

        case ARDUINO_EVENT_ETH_GOT_IP:
            ethUp = true;
            Serial.print("ETH IP: ");
            Serial.println(ETH.localIP());
            mqttSetup();
            break;

        case ARDUINO_EVENT_ETH_DISCONNECTED:
        case ARDUINO_EVENT_ETH_LOST_IP:
            ethUp = false;
            break;

        default:
            break;
    }
}

/**
 * @brief Read the latest tach count and PWM register for every fan channel.
 *
 * Updates @ref g_fans[i].rpm and @ref g_fans[i].pwm in-place.  Called every
 * 500 ms from the main loop.  A failed tach read is silently skipped so
 * the previous RPM value is retained until the next successful read.
 */
static void updateFanStatus() {
    for (int i = 0; i < NUM_FANS; i++) {
        uint16_t tach = 0;
        if (emc2305ReadFanTach(i + 1, tach))
            g_fans[i].rpm = (uint16_t)emc2305TachToRPM(tach);
        emc2305ReadFanPWM(i + 1, g_fans[i].pwm);
    }
}

/**
 * @brief Check EMC2305 fault registers and update per-fan alarm flags.
 *
 * Reads all four fault-status registers in a single burst, then for each
 * fan channel:
 * - Maps hardware fault bits to the corresponding @ref FanState alarm fields.
 * - Applies software RPM-range alarms against @ref AppConfig::alarms thresholds.
 * - Publishes any alarm state *changes* immediately via MQTT so Home
 *   Assistant receives transitions without waiting for the 5-second
 *   periodic status publish.
 *
 * Called every 1000 ms from the main loop.
 */
static void checkAlarms() {
    Emc2305FaultStatus fs;
    if (!emc2305ReadFaultStatus(fs)) return;

    for (int i = 0; i < NUM_FANS; i++) {
        const uint8_t bit = (uint8_t)(1 << i);

        bool wasStall = g_fans[i].alarmStall;
        bool wasSpin  = g_fans[i].alarmSpinFail;
        bool wasDrive = g_fans[i].alarmDriveFail;

        g_fans[i].alarmStall      = (fs.stall     & bit) != 0;
        g_fans[i].alarmSpinFail   = (fs.spin      & bit) != 0;
        g_fans[i].alarmDriveFail  = (fs.driveFail & bit) != 0;

        const uint16_t lo = g_config.alarms[i].minRpm;
        const uint16_t hi = g_config.alarms[i].maxRpm;
        g_fans[i].alarmRpmLow  = (lo > 0 && g_fans[i].rpm > 0 && g_fans[i].rpm < lo);
        g_fans[i].alarmRpmHigh = (hi > 0 && g_fans[i].rpm > hi);

        if (g_fans[i].alarmStall     != wasStall)  mqttPublishAlarm(i+1, "stall",      g_fans[i].alarmStall);
        if (g_fans[i].alarmSpinFail  != wasSpin)   mqttPublishAlarm(i+1, "spin_fail",  g_fans[i].alarmSpinFail);
        if (g_fans[i].alarmDriveFail != wasDrive)  mqttPublishAlarm(i+1, "drive_fail", g_fans[i].alarmDriveFail);
    }
}

/**
 * @brief Arduino setup — runs once on power-on or reset.
 *
 * Halts with a serial error message (infinite delay loop) if the EMC2305
 * fails to initialise, as there is no meaningful operation without the
 * fan controller.  LittleFS mount failure is non-fatal — the web UI will
 * be unavailable but fan control and MQTT still operate.
 */
void setup() {
    Serial.begin(115200);
    delay(100);

    configLoad();

    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed — upload filesystem image via PlatformIO");
    } else {
        Serial.println("LittleFS mounted OK");
    }

    Wire.begin(I2C_SDA, I2C_SCL);
    delay(50);

    Serial.print("EMC2305 init... ");
    if (!emc2305Init()) {
        Serial.println("FAILED — halting");
        while (true) delay(1000);
    }
    Serial.println("OK");

    emc2305EnableWatchdog(true);
    for (int i = 1; i <= NUM_FANS; i++)
        emc2305EnableRamp(i, true);

    WiFi.onEvent(onEthEvent);
    ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_LAN8720, ETH_CLK_MODE);

    webServerSetup(server);
    Serial.println("HTTP server started on port 80");
}

/**
 * @brief Arduino main loop — non-blocking polling and MQTT maintenance.
 *
 * All timing is managed with millis() deltas so the loop never blocks.
 * MQTT operations only run while @ref ethUp is true to avoid PubSubClient
 * attempting TCP connects before the network interface is ready.
 */
void loop() {
    static uint32_t lastTach    = 0;
    static uint32_t lastAlarm   = 0;
    static uint32_t lastMqttPub = 0;

    if (millis() - lastTach >= 500) {
        lastTach = millis();
        updateFanStatus();
    }

    if (millis() - lastAlarm >= 1000) {
        lastAlarm = millis();
        checkAlarms();
    }

    if (ethUp) {
        mqttLoop();
        if (millis() - lastMqttPub >= 5000) {
            lastMqttPub = millis();
            mqttPublishStatus();
        }
    }
}
