/**
 * @file config.h
 * @brief Application configuration types and LittleFS JSON persistence interface.
 *
 * All settings live in /settings.json on LittleFS.
 * Call configLoad() once after LittleFS.begin().
 */

#pragma once
#include <Arduino.h>

#define NUM_FANS 5

struct FanAlarmConfig {
    uint16_t minRpm;
    uint16_t maxRpm;
};

struct AppConfig {
    char           hostname[32];       // mDNS / OTA / MQTT client hostname
    char           mqttHost[64];
    uint16_t       mqttPort;
    char           mqttUser[32];
    char           mqttPass[32];
    char           mqttPrefix[40];
    FanAlarmConfig alarms[NUM_FANS];
    bool           otaWebEnabled;
    bool           otaArduinoEnabled;
    char           otaPassword[32];
};

struct FanDefault {
    uint8_t  pwm;           // direct-PWM duty (0-255)
    bool     closedLoop;    // true = closed-loop RPM mode at boot
    uint16_t targetRpm;     // closed-loop setpoint
    uint16_t maxRpm;        // rated fan speed — gauge full-scale and closed-loop cap (0 = use 3000)
    uint8_t  minDrive;      // EMC2305 MIN_DRIVE register (0-255); 0 = chip default
    uint8_t  pulsesPerRev;  // tach pulses per revolution (1-4); corrects RPM if > 1
    bool     hardSpinup;    // true = 65% drive / extended time; false = chip default
    bool     disabled;      // fan not installed — boot at PWM=0, suppress all alarms
};

struct FanState {
    uint16_t rpm;
    uint8_t  pwm;
    bool     closedLoop;
    uint16_t targetRpm;
    bool     alarmStall;
    bool     alarmSpinFail;
    bool     alarmDriveFail;
    bool     alarmRpmLow;
    bool     alarmRpmHigh;

    bool anyAlarm() const {
        return alarmStall || alarmSpinFail || alarmDriveFail || alarmRpmLow || alarmRpmHigh;
    }
};

extern AppConfig  g_config;
extern FanState   g_fans[NUM_FANS];
extern FanDefault g_fanDefaults[NUM_FANS];
/** True once the EMC2305 has been successfully initialised. */
extern bool       g_emc2305Ok;

void configLoad();
void configSave();
