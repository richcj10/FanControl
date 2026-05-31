/**
 * @file main.cpp
 * @brief Application entry point — hardware init, Ethernet events, and main loop.
 *
 * ## Hardware
 * - **MCU**: WT32-ETH01 (ESP32 + LAN8720 PHY on built-in EMAC)
 * - **Fan controller**: EMC2305 on I2C (SDA=33, SCL=32, addr=0x2C)
 * - **Ethernet clock**: 50 MHz oscillator input on GPIO0 (ETH_CLOCK_GPIO0_IN)
 *
 * ## Boot sequence
 * 1. Mount LittleFS (no format-on-fail — re-upload filesystem image if this fails).
 * 2. Load all settings from /settings.json via @ref configLoad().
 * 3. Initialise I2C and the EMC2305 fan controller.
 * 4. Apply per-fan boot defaults: PWM/mode, MIN_DRIVE, spin-up config.
 * 5. Enable EMC2305 watchdog and per-fan ramp-rate control.
 * 6. Register Ethernet event handler and start ETH driver.
 * 7. Register HTTP routes, web OTA (/update, if enabled), and start the server.
 * 8. Start ArduinoOTA push listener if enabled.
 *
 * ## Main loop timing
 * | Task                   | Interval |
 * |------------------------|----------|
 * | Tach + PWM poll        | 500 ms   |
 * | Alarm check + alert    | 1000 ms  |
 * | MQTT status publish    | 5000 ms  |
 * | MQTT keepalive         | every iteration |
 */

#include <Arduino.h>
#include <Wire.h>
#include <ETH.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoOTA.h>

#include "config.h"
#include "emc2305.h"
#include "web_server.h"
#include "mqtt_handler.h"

#define I2C_SDA       33
#define I2C_SCL       32
#define ETH_PHY_ADDR  1
#define ETH_PHY_POWER 16
#define ETH_PHY_MDC   23
#define ETH_PHY_MDIO  18
#define ETH_CLK_MODE  ETH_CLOCK_GPIO0_IN

static AsyncWebServer server(80);
static bool ethUp = false;

static void onEthEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_ETH_START:
            ETH.setHostname(g_config.hostname);
            break;

        case ARDUINO_EVENT_ETH_GOT_IP:
            ethUp = true;
            Serial.print("ETH IP: ");
            Serial.println(ETH.localIP());
            mqttSetup();
            break;

        case ARDUINO_EVENT_ETH_DISCONNECTED:
            ethUp = false;
            break;

        default:
            break;
    }
}

static void updateFanStatus() {
    for (int i = 0; i < NUM_FANS; i++) {
        uint16_t tach = 0;
        if (emc2305ReadFanTach(i + 1, tach)) {
            uint32_t raw = emc2305TachToRPM(tach);
            uint8_t ppr = g_fanDefaults[i].pulsesPerRev;
            g_fans[i].rpm = (uint16_t)(raw / (ppr > 0 ? ppr : 1));
        }
        emc2305ReadFanPWM(i + 1, g_fans[i].pwm);
    }
}

static void checkAlarms() {
    Emc2305FaultStatus fs;
    if (!emc2305ReadFaultStatus(fs)) return;

    for (int i = 0; i < NUM_FANS; i++) {
        // Fan marked disabled — clear all alarms silently and skip
        if (g_fanDefaults[i].disabled) {
            g_fans[i].alarmStall = g_fans[i].alarmSpinFail = g_fans[i].alarmDriveFail = false;
            g_fans[i].alarmRpmLow = g_fans[i].alarmRpmHigh = false;
            continue;
        }

        const uint8_t bit    = (uint8_t)(1 << i);
        bool          wasAny = g_fans[i].anyAlarm();

        g_fans[i].alarmStall      = (fs.stall     & bit) != 0;
        g_fans[i].alarmSpinFail   = (fs.spin      & bit) != 0;
        g_fans[i].alarmDriveFail  = (fs.driveFail & bit) != 0;

        const uint16_t lo = g_config.alarms[i].minRpm;
        const uint16_t hi = g_config.alarms[i].maxRpm;
        g_fans[i].alarmRpmLow  = (lo > 0 && g_fans[i].rpm > 0 && g_fans[i].rpm < lo);
        g_fans[i].alarmRpmHigh = (hi > 0 && g_fans[i].rpm > hi);

        if (g_fans[i].anyAlarm() != wasAny)
            mqttPublishAlert(i + 1, g_fans[i].anyAlarm());
    }
}

static void applyFanDefaults() {
    for (int i = 1; i <= NUM_FANS; i++) {
        const FanDefault &d = g_fanDefaults[i - 1];

        // Fan marked as not installed — park at 0 and move on
        if (d.disabled) {
            emc2305SetFanPWM(i, 0);
            g_fans[i - 1].pwm        = 0;
            g_fans[i - 1].closedLoop = false;
            continue;
        }

        // Write MIN_DRIVE register if non-zero
        if (d.minDrive > 0)
            emc2305WriteReg(EMC2305_FAN_BASE[i - 1] + EMC2305_OFF_MIN_DRIVE, d.minDrive);

        // Spin-up config: soft = chip default (0x19), hard = 65% drive / extended time (0xEB)
        emc2305WriteReg(EMC2305_FAN_BASE[i - 1] + EMC2305_OFF_SPINUP_CFG,
                        d.hardSpinup ? 0xEB : 0x19);

        if (d.closedLoop) {
            emc2305SetMode(i, true);
            emc2305SetTargetRpm(i, d.targetRpm);
            g_fans[i - 1].closedLoop = true;
            g_fans[i - 1].targetRpm  = d.targetRpm;
        } else {
            emc2305SetFanPWM(i, d.pwm);
            g_fans[i - 1].pwm        = d.pwm;
            g_fans[i - 1].closedLoop = false;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);  // let flash settle after esptool reset before mounting

    if (!LittleFS.begin(false)) {
        // Do NOT auto-format — that would wipe the uploaded web UI files.
        // If this fires, re-upload the filesystem image via PlatformIO.
        Serial.println("LittleFS mount FAILED — re-upload filesystem image");
    } else {
        Serial.println("LittleFS mounted OK");
    }

    configLoad();
    Serial.printf("Hostname: %s\n", g_config.hostname);

    Wire.begin(I2C_SDA, I2C_SCL);
    delay(50);

    Serial.print("EMC2305 init... ");
    if (!emc2305Init()) {
        Serial.println("FAILED — halting");
        while (true) delay(1000);
    }
    Serial.println("OK");

    applyFanDefaults();

    emc2305EnableWatchdog(true);
    for (int i = 1; i <= NUM_FANS; i++)
        emc2305EnableRamp(i, true);

    WiFi.onEvent(onEthEvent);
    ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_LAN8720, ETH_CLK_MODE);

    webServerSetup(server);
    Serial.printf("HTTP server started — http://%s.local\n", g_config.hostname);

    if (g_config.otaArduinoEnabled) {
        ArduinoOTA.setHostname(g_config.hostname);
        if (g_config.otaPassword[0] != '\0')
            ArduinoOTA.setPassword(g_config.otaPassword);
        ArduinoOTA.onStart([]() { Serial.println("ArduinoOTA: start"); });
        ArduinoOTA.onEnd([]()   { Serial.println("\nArduinoOTA: done"); });
        ArduinoOTA.onError([](ota_error_t e) { Serial.printf("ArduinoOTA error[%u]\n", e); });
        ArduinoOTA.begin();
        Serial.println("ArduinoOTA enabled");
    }
}

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

    if (g_config.otaArduinoEnabled)
        ArduinoOTA.handle();

    webServerLoop();
}
