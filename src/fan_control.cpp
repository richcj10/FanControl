/**
 * @file fan_control.cpp
 * @brief Mode-aware fan on/off and speed control for the EMC2305.
 *
 * In closed-loop mode (ENAG=1) the chip's PID owns FAN_DRIVE, so writing 0 to
 * it does nothing.  Stopping a fan therefore clears ENAG first and then writes
 * drive = 0.  Starting a closed-loop fan seeds the drive register, writes the
 * tach target, and only then sets ENAG so the PID starts from a sane point.
 */

#include "fan_control.h"
#include "config.h"
#include "emc2305.h"

#define DEFAULT_PWM 64

static bool valid(uint8_t n) { return n >= 1 && n <= NUM_FANS; }

/** Re-read the real mode after a failed write so g_fans doesn't lie. */
static bool fail(uint8_t n, const char *step) {
    Serial.printf("fan%u: I2C FAILED during %s\n", n, step);
    bool cl;
    if (emc2305ReadMode(n, cl)) g_fans[n - 1].closedLoop = cl;
    return false;
}

static uint16_t cappedTarget(uint8_t n, uint16_t rpm) {
    uint16_t mx = g_fanDefaults[n - 1].maxRpm;
    return (mx > 0 && rpm > mx) ? mx : rpm;
}

bool fanPowerOff(uint8_t n) {
    if (!valid(n)) return false;
    FanState &f = g_fans[n - 1];

    if (f.on) {
        f.savedValid      = true;
        f.savedClosedLoop = f.closedLoop;
        f.savedTargetRpm  = f.targetRpm;
        f.savedPwm        = f.pwm;
    }
    // Always touch the hardware, even if `on` was already false — it is cheap
    // and guarantees the fan really is stopped.
    if (!emc2305SetMode(n, false))  return fail(n, "off: clear ENAG");
    if (!emc2305SetFanPWM(n, 0))    return fail(n, "off: drive=0");

    f.closedLoop = false;
    f.pwm        = 0;
    f.on         = false;
    Serial.printf("fan%u: OFF (saved %s, target %u rpm, pwm %u)\n", n,
                  f.savedClosedLoop ? "closed_loop" : "pwm", f.savedTargetRpm, f.savedPwm);
    return true;
}

bool fanPowerOn(uint8_t n) {
    if (!valid(n)) return false;
    FanState         &f = g_fans[n - 1];
    const FanDefault &d = g_fanDefaults[n - 1];
    if (d.disabled) return false;
    if (f.on) return true;

    bool     closed;
    uint16_t target = 0;
    uint8_t  pwm;
    if (f.savedValid) {
        closed = f.savedClosedLoop;
        target = f.savedTargetRpm;
        pwm    = f.savedPwm;
    } else {
        closed = d.closedLoop;
        target = d.targetRpm;
        pwm    = d.pwm;
    }
    if (pwm == 0) pwm = d.pwm ? d.pwm : DEFAULT_PWM;
    if (closed && target == 0) closed = false;     // nothing to regulate to
    if (!closed && pwm == 0)   pwm = DEFAULT_PWM;

    if (closed) {
        target = cappedTarget(n, target);
        if (!emc2305SetFanPWM(n, pwm))        return fail(n, "on: seed drive");
        if (!emc2305SetTargetRpm(n, target))  return fail(n, "on: tach target");
        if (!emc2305SetMode(n, true))         return fail(n, "on: set ENAG");
        f.targetRpm = target;
    } else {
        if (!emc2305SetMode(n, false))        return fail(n, "on: clear ENAG");
        if (!emc2305SetFanPWM(n, pwm))        return fail(n, "on: drive");
    }

    f.closedLoop = closed;
    f.pwm        = pwm;
    f.on         = true;
    f.onSinceMs  = millis();
    f.savedValid = false;
    Serial.printf("fan%u: ON (%s, target %u rpm, pwm %u)\n", n,
                  closed ? "closed_loop" : "pwm", closed ? target : 0, pwm);
    return true;
}

bool fanSetPct(uint8_t n, int pct) {
    if (!valid(n)) return false;
    if (pct <= 0) return fanPowerOff(n);
    if (pct > 100) pct = 100;
    FanState &f = g_fans[n - 1];
    uint8_t pwm = (uint8_t)((pct * 255 + 50) / 100);

    if (f.closedLoop && !emc2305SetMode(n, false)) return fail(n, "pct: clear ENAG");
    f.closedLoop = false;
    if (!emc2305SetFanPWM(n, pwm)) return fail(n, "pct: drive");

    f.pwm = pwm;
    if (!f.on) f.onSinceMs = millis();
    f.on         = true;
    f.savedValid = false;      // an explicit speed supersedes the saved snapshot
    Serial.printf("fan%u: PWM %d%% (%u)\n", n, pct, pwm);
    return true;
}

bool fanSetRpm(uint8_t n, uint16_t rpm) {
    if (!valid(n) || rpm == 0) return false;
    FanState         &f = g_fans[n - 1];
    const FanDefault &d = g_fanDefaults[n - 1];
    if (d.disabled) return false;

    rpm = cappedTarget(n, rpm);
    // From a stopped fan, seed the drive register so the PID doesn't start at 0
    if (!f.on || f.pwm == 0) {
        uint8_t seed = d.pwm ? d.pwm : DEFAULT_PWM;
        if (!emc2305SetFanPWM(n, seed)) return fail(n, "rpm: seed drive");
        f.pwm = seed;
    }
    if (!emc2305SetTargetRpm(n, rpm)) return fail(n, "rpm: tach target");
    if (!f.closedLoop && !emc2305SetMode(n, true)) return fail(n, "rpm: set ENAG");

    f.closedLoop = true;
    f.targetRpm  = rpm;
    if (!f.on) f.onSinceMs = millis();
    f.on         = true;
    f.savedValid = false;
    Serial.printf("fan%u: closed-loop %u rpm\n", n, rpm);
    return true;
}
