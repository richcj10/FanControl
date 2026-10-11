/**
 * @file fan_control.h
 * @brief Mode-aware fan power / speed commands shared by MQTT and the web API.
 *
 * Every function updates @ref g_fans only after the hardware write succeeded.
 * On an I2C failure the mode is re-read from the chip so g_fans reflects what
 * the fan is actually doing, and the failure is logged.
 */

#pragma once
#include <stdint.h>

/**
 * @brief Stop a fan in either mode.
 *
 * Saves mode / target RPM / PWM (only if the fan was on), clears ENAG so the
 * PID releases the drive register, then writes drive = 0.
 */
bool fanPowerOff(uint8_t fanNum);

/**
 * @brief Turn a fan on, restoring the state saved by fanPowerOff().
 *
 * With nothing saved (e.g. after reboot) the configured defaults are used.
 * No-op if the fan is already on.
 */
bool fanPowerOn(uint8_t fanNum);

/** @brief Direct-PWM speed, 0–100 %.  0 is a power-off; >0 forces PWM mode. */
bool fanSetPct(uint8_t fanNum, int pct);

/** @brief Switch to closed-loop at @p rpm and turn the fan on. */
bool fanSetRpm(uint8_t fanNum, uint16_t rpm);
