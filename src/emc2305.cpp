/**
 * @file emc2305.cpp
 * @brief EMC2305 5-channel PWM fan controller driver implementation.
 *
 * All communication is over the I2C bus initialised by the caller via
 * Wire.begin() before @ref emc2305Init() is called.
 *
 * Tachometer formula reference:
 *   The EMC2305 counts tach edge pairs over a fixed 32,768 Hz clock.
 *   For a standard 2-PPR fan: RPM = (32768 * 60) / (2 * tachCount)
 *   Simplified: RPM = 3,932,160 / tachCount
 */

#include "emc2305.h"
#include <Wire.h>

/**
 * @brief Per-fan I2C base addresses for registers 0x00–0x0F.
 *
 * Indexed 0–4 for fans 1–5.  Add an @c EMC2305_OFF_* offset to obtain
 * the absolute register address.
 */
const uint8_t EMC2305_FAN_BASE[5] = { 0x30, 0x40, 0x50, 0x60, 0x70 };

/**
 * @brief Write a single byte to an EMC2305 register over I2C.
 * @param reg   Register address.
 * @param value Byte to write.
 * @return true if Wire.endTransmission() returned 0 (no error).
 */
bool emc2305WriteReg(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(EMC2305_I2C_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

/**
 * @brief Read a single byte from an EMC2305 register over I2C.
 *
 * Uses a repeated-start (false argument to endTransmission) so the
 * register pointer write and the read are a single atomic I2C transaction.
 *
 * @param reg    Register address.
 * @param[out] value  Populated with the register byte on success.
 * @return true if both the write and read phases succeeded.
 */
bool emc2305ReadReg(uint8_t reg, uint8_t &value) {
    Wire.beginTransmission(EMC2305_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(EMC2305_I2C_ADDR, (uint8_t)1) != 1) return false;
    value = Wire.read();
    return true;
}

/**
 * @brief Initialise the EMC2305 for 4-wire fan operation.
 *
 * Sequence:
 * 1. Unlock the software-lock register.
 * 2. CONFIG: set DIS_TO=1 (disable SMBus timeout for standard I2C).
 * 3. PWM_POLARITY: normal polarity on all channels.
 * 4. PWM_OUTPUT_CFG: push-pull drive on all five channels (0x1F).
 * 5. PWM base frequency: 26 kHz on fans 1–5.
 * 6. Per-fan: divide=1, direct-PWM mode (ENAG=0, RANGE=00), ramp off,
 *    spin-up defaults, drive = 0 (fans stopped).
 *
 * @return true if every I2C write succeeds; false on the first failure.
 */
bool emc2305Init() {
    if (!emc2305WriteReg(EMC2305_REG_SW_LOCK,        0x00)) return false;
    if (!emc2305WriteReg(EMC2305_REG_CONFIG,         0x40)) return false;
    if (!emc2305WriteReg(EMC2305_REG_PWM_POLARITY,   0x00)) return false;
    if (!emc2305WriteReg(EMC2305_REG_PWM_OUTPUT_CFG, 0x1F)) return false;
    if (!emc2305WriteReg(EMC2305_REG_PWM_BASEF45,    0x00)) return false;
    if (!emc2305WriteReg(EMC2305_REG_PWM_BASEF123,   0x00)) return false;

    for (int i = 0; i < 5; i++) {
        uint8_t base = EMC2305_FAN_BASE[i];
        if (!emc2305WriteReg(base + EMC2305_OFF_PWM_DIVIDE,  0x01)) return false;
        if (!emc2305WriteReg(base + EMC2305_OFF_FAN_CFG1,    0x0B)) return false;
        if (!emc2305WriteReg(base + EMC2305_OFF_FAN_CFG2,    0x28)) return false;
        if (!emc2305WriteReg(base + EMC2305_OFF_SPINUP_CFG,  0x19)) return false;
        if (!emc2305WriteReg(base + EMC2305_OFF_MAX_STEP,    0x10)) return false;
        if (!emc2305WriteReg(base + EMC2305_OFF_MIN_DRIVE,   0x66)) return false;
        if (!emc2305WriteReg(base + EMC2305_OFF_VALID_TACH,  0xFE)) return false;
        if (!emc2305WriteReg(base + EMC2305_OFF_FAN_DRIVE,   0x00)) return false;
    }
    return true;
}

/**
 * @brief Set the PWM duty cycle for one fan channel.
 * @param fanNum 1–5.
 * @param pwm    0 (off) – 255 (full speed).
 * @return true on success, false if fanNum is out of range or I2C fails.
 */
bool emc2305SetFanPWM(uint8_t fanNum, uint8_t pwm) {
    if (fanNum < 1 || fanNum > 5) return false;
    return emc2305WriteReg(EMC2305_FAN_BASE[fanNum - 1] + EMC2305_OFF_FAN_DRIVE, pwm);
}

/**
 * @brief Read the current PWM drive register value for one fan channel.
 * @param fanNum 1–5.
 * @param[out] pwm Receives the register value (0–255).
 * @return true on success.
 */
bool emc2305ReadFanPWM(uint8_t fanNum, uint8_t &pwm) {
    if (fanNum < 1 || fanNum > 5) return false;
    return emc2305ReadReg(EMC2305_FAN_BASE[fanNum - 1] + EMC2305_OFF_FAN_DRIVE, pwm);
}

/**
 * @brief Read the raw 13-bit tachometer count for one fan.
 *
 * Reads the high byte first (latches the low byte in hardware), then reads
 * the low byte.  Reconstructs the 13-bit value as:
 * @code
 *   tachCount = (hi << 5) | (lo >> 3)
 * @endcode
 *
 * @param fanNum      1–5.
 * @param[out] tachCount  13-bit tach period count.
 * @return true on success.
 */
bool emc2305ReadFanTach(uint8_t fanNum, uint16_t &tachCount) {
    if (fanNum < 1 || fanNum > 5) return false;
    uint8_t base = EMC2305_FAN_BASE[fanNum - 1];
    uint8_t hi, lo;
    if (!emc2305ReadReg(base + EMC2305_OFF_TACH_READING_H, hi)) return false;
    if (!emc2305ReadReg(base + EMC2305_OFF_TACH_READING_L, lo)) return false;
    tachCount = ((uint16_t)hi << 5) | (lo >> 3);
    return true;
}

/**
 * @brief Convert a raw tach count to RPM.
 *
 * Formula: RPM = 3,932,160 / tachCount
 * Derived from: RPM = (CLK_Hz * 60) / (poles/2 * tachCount)
 * with CLK_Hz = 32,768 Hz and poles = 4 (2 edge pairs per revolution).
 *
 * @param tachCount Raw value from @ref emc2305ReadFanTach().
 * @return RPM as uint32_t, or 0 if tachCount indicates stall or overflow.
 */
uint32_t emc2305TachToRPM(uint16_t tachCount) {
    if (tachCount == 0 || tachCount >= 8191) return 0;
    return 3932160UL / tachCount;
}

/**
 * @brief Switch a fan between direct-PWM and closed-loop RPM modes.
 *
 * Performs a read-modify-write on FAN_CFG1 to set or clear the ENAG bit
 * (bit 7) without disturbing RANGE, EDGES, or UPDATE fields.
 *
 * @param fanNum     1–5.
 * @param closedLoop true → ENAG=1 (hardware PID); false → ENAG=0 (direct PWM).
 * @return true on success.
 */
bool emc2305SetMode(uint8_t fanNum, bool closedLoop) {
    if (fanNum < 1 || fanNum > 5) return false;
    uint8_t base = EMC2305_FAN_BASE[fanNum - 1];
    uint8_t cfg1;
    if (!emc2305ReadReg(base + EMC2305_OFF_FAN_CFG1, cfg1)) return false;
    if (closedLoop) cfg1 |=  (1 << 7);
    else            cfg1 &= ~(1 << 7);
    return emc2305WriteReg(base + EMC2305_OFF_FAN_CFG1, cfg1);
}

/**
 * @brief Write the closed-loop RPM target to the TACH_TARGET registers.
 *
 * Converts RPM to a 13-bit tach count using the inverse of
 * @ref emc2305TachToRPM(), then packs the result into the two
 * TACH_TARGET registers (high byte first):
 * @code
 *   tach = 3,932,160 / rpm
 *   TACH_TARGET_H = (tach >> 5) & 0xFF   (bits [12:5])
 *   TACH_TARGET_L = (tach & 0x1F) << 3   (bits [4:0] → register [7:3])
 * @endcode
 *
 * @param fanNum 1–5.
 * @param rpm    Target speed in RPM; must be > 0.
 * @return true on success, false if parameters are invalid or I2C fails.
 */
bool emc2305SetTargetRpm(uint8_t fanNum, uint16_t rpm) {
    if (fanNum < 1 || fanNum > 5 || rpm == 0) return false;
    uint16_t tach = (uint16_t)(3932160UL / rpm);
    uint8_t  base = EMC2305_FAN_BASE[fanNum - 1];
    if (!emc2305WriteReg(base + EMC2305_OFF_TACH_TARGET_H, (tach >> 5) & 0xFF)) return false;
    return  emc2305WriteReg(base + EMC2305_OFF_TACH_TARGET_L, (tach & 0x1F) << 3);
}

/**
 * @brief Read all four fault-status registers (0x24–0x27).
 * @param[out] fs Populated on success.
 * @return true if all four I2C reads succeed.
 */
bool emc2305ReadFaultStatus(Emc2305FaultStatus &fs) {
    if (!emc2305ReadReg(EMC2305_REG_FAN_STATUS,   fs.summary))   return false;
    if (!emc2305ReadReg(EMC2305_REG_STALL_STATUS, fs.stall))     return false;
    if (!emc2305ReadReg(EMC2305_REG_SPIN_STATUS,  fs.spin))      return false;
    if (!emc2305ReadReg(EMC2305_REG_DRIVE_STATUS, fs.driveFail)) return false;
    return true;
}

/**
 * @brief Enable or disable ramp-rate control for one fan.
 *
 * Performs a read-modify-write on FAN_CFG2, toggling the ENRC bit (bit 6).
 * The ramp step size is set by @c MAX_STEP (initialised to 0x10 in
 * @ref emc2305Init()).
 *
 * @param fanNum 1–5.
 * @param enable true to enable ramp-rate limiting.
 * @return true on success.
 */
bool emc2305EnableRamp(uint8_t fanNum, bool enable) {
    if (fanNum < 1 || fanNum > 5) return false;
    uint8_t base = EMC2305_FAN_BASE[fanNum - 1];
    uint8_t cfg2;
    if (!emc2305ReadReg(base + EMC2305_OFF_FAN_CFG2, cfg2)) return false;
    if (enable) cfg2 |=  (1 << 6);
    else        cfg2 &= ~(1 << 6);
    return emc2305WriteReg(base + EMC2305_OFF_FAN_CFG2, cfg2);
}

/**
 * @brief Enable or disable the EMC2305 SMBus watchdog.
 *
 * Performs a read-modify-write on the CONFIG register (0x20), toggling
 * the WD_EN bit (bit 5).  When enabled and no I2C activity is detected
 * within the watchdog window, the WATCHDOG bit in the fan status register
 * (0x24 bit 7) is set.
 *
 * @param enable true to arm the watchdog.
 * @return true on success.
 */
bool emc2305EnableWatchdog(bool enable) {
    uint8_t cfg;
    if (!emc2305ReadReg(EMC2305_REG_CONFIG, cfg)) return false;
    if (enable) cfg |=  (1 << 5);
    else        cfg &= ~(1 << 5);
    return emc2305WriteReg(EMC2305_REG_CONFIG, cfg);
}
