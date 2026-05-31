/**
 * @file emc2305.h
 * @brief EMC2305 5-channel PWM fan controller driver interface.
 *
 * The EMC2305 is controlled over I2C.  This driver exposes two operating
 * modes per fan channel:
 *
 * - **Direct PWM** (ENAG = 0): the host writes a duty-cycle value (0–255)
 *   directly to the FAN_DRIVE register.
 * - **Closed-loop RPM** (ENAG = 1): the EMC2305 hardware PID adjusts PWM
 *   to reach a tachometer target written to the TACH_TARGET registers.
 *
 * Call @ref emc2305Init() once after Wire.begin() before using any other
 * function.  All functions return false on I2C error; callers should treat
 * a false return as a hardware fault.
 *
 * @note I2C address is fixed at @ref EMC2305_I2C_ADDR (0x2C). Verify your
 *       hardware address-strap resistors match this value.
 */

#pragma once
#include <Arduino.h>

/** @brief 7-bit I2C address of the EMC2305 (address pins both pulled low). */
#define EMC2305_I2C_ADDR           0x2C

/** @defgroup emc2305_global_regs Global registers
 *  @{ */
#define EMC2305_REG_CONFIG         0x20 /**< Global configuration register. */
#define EMC2305_REG_PWM_POLARITY   0x2A /**< PWM output polarity (0 = non-inverted). */
#define EMC2305_REG_PWM_OUTPUT_CFG 0x2B /**< PWM output type (0 = open-drain, 0x1F = push-pull). */
#define EMC2305_REG_PWM_BASEF45    0x2C /**< PWM base frequency for fans 4 and 5. */
#define EMC2305_REG_PWM_BASEF123   0x2D /**< PWM base frequency for fans 1, 2, and 3. */
#define EMC2305_REG_SW_LOCK        0xEF /**< Software lock register; write 0x00 to unlock. */
#define EMC2305_REG_FAN_STATUS     0x24 /**< Fan status summary (watchdog, drive-fail, spin-fail, stall). */
#define EMC2305_REG_STALL_STATUS   0x25 /**< Per-fan stall flags (bit N = fan N+1). */
#define EMC2305_REG_SPIN_STATUS    0x26 /**< Per-fan spin-up fail flags. */
#define EMC2305_REG_DRIVE_STATUS   0x27 /**< Per-fan drive-fail flags (cannot reach PWM target). */
/** @} */

/**
 * @brief Per-fan register base addresses.
 *
 * Fan N (1-indexed) maps to @c EMC2305_FAN_BASE[N-1].
 * Add a per-fan offset constant to obtain the full register address.
 */
extern const uint8_t EMC2305_FAN_BASE[5];

/** @defgroup emc2305_fan_offsets Per-fan register offsets (added to EMC2305_FAN_BASE[n])
 *  @{ */
#define EMC2305_OFF_FAN_DRIVE      0x00 /**< PWM duty-cycle register (direct mode) or max-drive limit (closed-loop). */
#define EMC2305_OFF_PWM_DIVIDE     0x01 /**< PWM clock pre-divider. */
#define EMC2305_OFF_FAN_CFG1       0x02 /**< Fan config 1: ENAG (bit7), RANGE, EDGES, UPDATE. */
#define EMC2305_OFF_FAN_CFG2       0x03 /**< Fan config 2: ENRC (bit6) enables ramp-rate control. */
#define EMC2305_OFF_SPINUP_CFG     0x06 /**< Spin-up configuration (drive level and duration). */
#define EMC2305_OFF_MAX_STEP       0x07 /**< Maximum PWM step size per update interval. */
#define EMC2305_OFF_MIN_DRIVE      0x08 /**< Minimum drive level (stall prevention floor). */
#define EMC2305_OFF_VALID_TACH     0x09 /**< Valid tachometer threshold. */
#define EMC2305_OFF_TACH_TARGET_L  0x0C /**< Tach target low byte (bits [4:0] in bits [7:3]). */
#define EMC2305_OFF_TACH_TARGET_H  0x0D /**< Tach target high byte (bits [12:5]). */
#define EMC2305_OFF_TACH_READING_H 0x0E /**< Tach reading high byte (bits [12:5]). */
#define EMC2305_OFF_TACH_READING_L 0x0F /**< Tach reading low byte (bits [4:0] in bits [7:3]). */
/** @} */

/**
 * @brief Snapshot of all four fault-status registers.
 *
 * Populated by @ref emc2305ReadFaultStatus().  Each bit in @c stall,
 * @c spin, and @c driveFail corresponds to a fan channel (bit 0 = fan 1).
 */
struct Emc2305FaultStatus {
    uint8_t summary;   /**< 0x24: summary flags (watchdog, drive-fail, spin-fail, stall). */
    uint8_t stall;     /**< 0x25: per-fan stall bits. */
    uint8_t spin;      /**< 0x26: per-fan spin-up fail bits. */
    uint8_t driveFail; /**< 0x27: per-fan drive-fail bits. */
};

/**
 * @brief Initialise the EMC2305 with push-pull, 26 kHz, direct-PWM defaults.
 *
 * Must be called after Wire.begin().  Unlocks the chip, configures global
 * PWM frequency and polarity, then sets each fan to direct-PWM mode with
 * drive = 0 (fans off).  Ramp-rate control and watchdog are disabled here
 * and must be enabled separately if desired.
 *
 * @return true on success, false if any I2C transaction fails.
 */
bool emc2305Init();

/**
 * @brief Write a single byte to an EMC2305 register.
 * @param reg   Register address.
 * @param value Byte to write.
 * @return true if the I2C transaction completed with no error.
 */
bool emc2305WriteReg(uint8_t reg, uint8_t value);

/**
 * @brief Read a single byte from an EMC2305 register.
 * @param reg   Register address.
 * @param[out] value Receives the register contents on success.
 * @return true if the I2C transaction completed with no error.
 */
bool emc2305ReadReg(uint8_t reg, uint8_t &value);

/**
 * @brief Set the PWM duty cycle for one fan (direct-PWM mode).
 * @param fanNum Fan number, 1–5.
 * @param pwm    Duty-cycle value 0 (off) to 255 (100%).
 * @return true on success.
 */
bool emc2305SetFanPWM(uint8_t fanNum, uint8_t pwm);

/**
 * @brief Read the current FAN_DRIVE register value for one fan.
 * @param fanNum Fan number, 1–5.
 * @param[out] pwm Receives the register value (0–255).
 * @return true on success.
 */
bool emc2305ReadFanPWM(uint8_t fanNum, uint8_t &pwm);

/**
 * @brief Read the raw 13-bit tachometer count for one fan.
 *
 * The tach period is measured in units of a fixed clock and stored as a
 * 13-bit value split across the high and low reading registers.  Convert to
 * RPM using @ref emc2305TachToRPM().
 *
 * @param fanNum    Fan number, 1–5.
 * @param[out] tachCount  Raw 13-bit tach count (higher = slower fan).
 * @return true on success.
 */
bool emc2305ReadFanTach(uint8_t fanNum, uint16_t &tachCount);

/**
 * @brief Convert a raw tach count to RPM.
 *
 * Uses the formula: RPM = 3,932,160 / tachCount, which assumes a 2-pole
 * (single-edge-pair) fan and the default EMC2305 tach clock settings.
 *
 * @param tachCount Raw 13-bit value from @ref emc2305ReadFanTach().
 * @return Calculated RPM, or 0 if tachCount is 0 or ≥ 8191 (invalid).
 */
uint32_t emc2305TachToRPM(uint16_t tachCount);

/**
 * @brief Switch a fan between direct-PWM and closed-loop RPM control.
 *
 * Sets or clears the ENAG bit (bit 7) in FAN_CFG1.  When switching to
 * closed-loop, call @ref emc2305SetTargetRpm() immediately after.
 *
 * @param fanNum     Fan number, 1–5.
 * @param closedLoop true to enable hardware PID (ENAG=1), false for direct PWM.
 * @return true on success.
 */
bool emc2305SetMode(uint8_t fanNum, bool closedLoop);

/**
 * @brief Set the closed-loop RPM target for one fan.
 *
 * Converts the target RPM to a 13-bit tach-period value and writes it to
 * the TACH_TARGET_H and TACH_TARGET_L registers.  The fan must already be
 * in closed-loop mode (see @ref emc2305SetMode()).
 *
 * @param fanNum Fan number, 1–5.
 * @param rpm    Desired speed in RPM; must be > 0.
 * @return true on success, false if fanNum or rpm is invalid.
 */
bool emc2305SetTargetRpm(uint8_t fanNum, uint16_t rpm);

/**
 * @brief Read all four fault-status registers in one call.
 * @param[out] fs Populated with the contents of registers 0x24–0x27.
 * @return true if all four reads succeeded.
 */
bool emc2305ReadFaultStatus(Emc2305FaultStatus &fs);

/**
 * @brief Enable or disable ramp-rate control for one fan.
 *
 * Sets or clears the ENRC bit (bit 6) in FAN_CFG2.  When enabled, the
 * EMC2305 limits the rate of PWM change to the value in MAX_STEP, which
 * reduces mechanical stress on fan bearings during speed transitions.
 *
 * @param fanNum Fan number, 1–5.
 * @param enable true to enable ramp-rate control.
 * @return true on success.
 */
bool emc2305EnableRamp(uint8_t fanNum, bool enable);

/**
 * @brief Enable or disable the EMC2305 SMBus watchdog.
 *
 * When enabled, the chip sets a watchdog-timeout flag in the status register
 * if no I2C activity is detected within the watchdog period (~1 s typical).
 * Sets or clears the WD_EN bit (bit 5) in the CONFIG register.
 *
 * @param enable true to enable the watchdog.
 * @return true on success.
 */
bool emc2305EnableWatchdog(bool enable);
