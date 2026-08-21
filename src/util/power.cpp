// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "power.h"

#include <M5PM1.h>
#include <M5Unified.h>
#include <esp_system.h>  // esp_reset_reason()

#include "imu.h"
#include "power/PowerEventLog.h"
#include "power/battery_level.h"
#include "util/i2c_lock.h"
#include "util/wallclock.h"

#ifndef IMU_POWER_ENABLE_DIAGNOSTICS
#define IMU_POWER_ENABLE_DIAGNOSTICS 0
#endif

#define HANDLE_ERR(__FN__, __LABEL__)                    \
  {                                                      \
    int err = __FN__;                                    \
    if (err) {                                           \
      M5_LOGE(__LABEL__ " failed, error code: %d", err); \
      return false;                                      \
    }                                                    \
  }

namespace {
power::PowerBootDiagnostics gBootDiagnostics;
bool gBootDiagnosticsSealed = false;
uint32_t gWakeRecordedMs = 0;
bool gWakeTimestampPending = false;

void markBootFlag(power::PowerBootFlag flag) {
  if (gBootDiagnosticsSealed) return;
  gBootDiagnostics.flags |= static_cast<uint16_t>(flag);
}
}  // namespace

namespace StickS3PowerHardware {

// The M5StickS3 wake-on-motion example uses SparkFun's BMI270 class. That
// library configured wake-on-motion, but could not be integrated safely
// because it restarts the shared I2C bus and conflicts with M5Unified timing.
// This shutdown path instead performs the equivalent operations through
// M5.In_I2C:
//
//   imu.enableFeature(BMI2_ANY_MOTION);
//   imu.setInterruptPinConfig(... active-low push-pull INT1 ...);
//   imu.mapInterruptToPin(BMI2_ANY_MOTION_INT, BMI2_INT1);
//
// M5Unified supplied the register map and initialization behavior. Bosch's
// SensorAPI, obtained through the SparkFun library, supplied the feature layout
// and enable logic. THIRD_PARTY_NOTICES.md records the provenance and licenses.
// PM1 GPIO4 supplies the pull-up and falling-edge wake function, allowing
// BMI270 INT1 to idle high and wake PM1 by driving low.

// Registers from M5Unified BMI270_Class.hpp / Bosch BMI270 SensorAPI.
static constexpr uint8_t CHIP_ID_ADDR = 0x00;
static constexpr uint8_t INT_STATUS_0_ADDR = 0x1C;
static constexpr uint8_t INTERNAL_STATUS_ADDR = 0x21;
static constexpr uint8_t ACC_CONF_ADDR = 0x40;
static constexpr uint8_t PWR_CONF_ADDR = 0x7C;
static constexpr uint8_t PWR_CTRL_ADDR = 0x7D;
static constexpr uint8_t FEAT_PAGE_ADDR = 0x2F;
static constexpr uint8_t FEATURES_REG_ADDR = 0x30;
static constexpr uint8_t INT1_IO_CTRL_ADDR = 0x53;
static constexpr uint8_t INT1_MAP_FEAT_ADDR = 0x56;
static constexpr uint8_t INT_MAP_DATA_ADDR = 0x58;

// INT1_IO_CTRL (0x53): bit3=output_en, bit2=od(0=push-pull), bit1=lvl.
// The SparkFun configuration used active-low, push-pull, output-enabled,
// input-disabled INT1 with non-latched interrupts. PM1 GPIO4 gets a pull-up and
// wakes on the falling edge, so the idle level is high and BMI270 wake drives
// the line low.
static constexpr uint8_t INT_IO_LEVEL = 0x02;
static constexpr uint8_t INT_IO_OPEN_DRAIN = 0x04;
static constexpr uint8_t INT_IO_OUTPUT_ENABLE = 0x08;
static constexpr uint8_t INT_IO_INPUT_ENABLE = 0x10;
static constexpr uint8_t INT_LATCH_ENABLE = 0x01;

// INT1_MAP_FEAT (0x56): bit6 = any-motion (BMI2_ANY_MOT_INT_MASK)
static constexpr uint8_t INT1_MAP_ANY_MOTION = 0x40;

// Any-motion feature location from Bosch bmi270.c feature table + bmi2.c
// offsets: page=1, start_addr=0x0C, enable_offset=0x03 -> byte index 0x0F.
// SparkFun's enableFeature(BMI2_ANY_MOTION) only toggles this enable bit. It
// leaves the BMI270 config-file defaults for duration, threshold, and axes.
static constexpr uint8_t ANY_MOT_PAGE = 0x01;
static constexpr uint8_t ANY_MOT_START = 0x0C;
static constexpr uint8_t ANY_MOT_ENABLE_BYTE = ANY_MOT_START + 0x03;
static constexpr uint8_t ANY_MOT_ENABLE_MASK = 0x80;

static constexpr uint8_t BMI270_CHIP_ID = 0x24;
static constexpr uint8_t PWR_CONF_ADV_POWER_SAVE = 0x01;
static constexpr uint8_t PWR_CTRL_ACC_ENABLE = 0x04;

// PM1
M5PM1 _pm1;
bool _is_pm1_init = false;

bool _initPM1() {
  if (_is_pm1_init) {
#if IMU_POWER_ENABLE_DIAGNOSTICS
    M5_LOGV("PM1 already initialized");
#endif
    return true;
  }
  HANDLE_ERR(_pm1.begin(&M5.In_I2C, M5PM1_DEFAULT_ADDR, M5PM1_I2C_FREQ_DEFAULT),
             "PM1 initialization");
#if IMU_POWER_ENABLE_DIAGNOSTICS
  M5_LOGV("PM1 initialization successful");
#endif
  _is_pm1_init = true;
  return true;
}

#if IMU_POWER_ENABLE_DIAGNOSTICS
void _logWakeState(const char* label) {
  uint8_t wake_src = 0;
  if (_pm1.getWakeSource(&wake_src, M5PM1_CLEAN_NONE) == M5PM1_OK) {
    M5_LOGI("%s PM1 wake_src=0x%02X%s%s%s%s%s%s%s", label, wake_src,
            (wake_src & M5PM1_WAKE_SRC_EXT_WAKE) ? " EXT" : "",
            (wake_src & M5PM1_WAKE_SRC_PWRBTN) ? " PWRBTN" : "",
            (wake_src & M5PM1_WAKE_SRC_RSTBTN) ? " RSTBTN" : "",
            (wake_src & M5PM1_WAKE_SRC_CMD_RST) ? " CMD_RST" : "",
            (wake_src & M5PM1_WAKE_SRC_VIN) ? " VIN" : "",
            (wake_src & M5PM1_WAKE_SRC_5VINOUT) ? " 5VINOUT" : "",
            (wake_src & M5PM1_WAKE_SRC_TIM) ? " TIMER" : "");
  }

  uint8_t gpio_status = 0;
  if (_pm1.irqGetGpioStatus(&gpio_status, M5PM1_CLEAN_NONE) == M5PM1_OK) {
    M5_LOGI("%s PM1 gpio_irq=0x%02X", label, gpio_status);
  }

  uint8_t gpio4_level = 0;
  if (_pm1.gpioGetInput(M5PM1_GPIO_NUM_4, &gpio4_level) == M5PM1_OK) {
    M5_LOGI("%s PM1 gpio4_level=%u", label, gpio4_level);
  }
}
#endif

void _clearWakeState() {
  uint8_t value = 0;
  _pm1.getWakeSource(&value, M5PM1_CLEAN_ALL);
  _pm1.irqGetGpioStatus(&value, M5PM1_CLEAN_ALL);
}

using namespace ImuUtil;

bool _setBmiAdvPowerSave(bool enable) {
  uint8_t pwr_conf = 0;
  if (!bmiRead8(PWR_CONF_ADDR, &pwr_conf)) return false;

  if (enable) {
    pwr_conf |= PWR_CONF_ADV_POWER_SAVE;
  } else {
    pwr_conf &= ~PWR_CONF_ADV_POWER_SAVE;
  }

  if (!bmiWrite8(PWR_CONF_ADDR, pwr_conf)) return false;
  delayMicroseconds(450);
  return true;
}

bool _ensureBmiReady() {
  uint8_t chip_id = 0;
  if (!bmiRead8(CHIP_ID_ADDR, &chip_id)) return false;
  if (chip_id != BMI270_CHIP_ID) {
    M5_LOGE("Unexpected BMI270 chip id: 0x%02X", chip_id);
    return false;
  }

  uint8_t internal_status = 0;
  for (uint8_t retry = 0; retry < 20; ++retry) {
    if (!bmiRead8(INTERNAL_STATUS_ADDR, &internal_status)) return false;
    if ((internal_status & 0x0F) == 0x01) break;
    delay(1);
  }

  if ((internal_status & 0x0F) != 0x01) {
    M5_LOGE("BMI270 config not initialized: 0x%02X", internal_status);
    return false;
  }

  // Any-motion is an accelerometer-based feature. M5Unified normally enables
  // accel/gyro after BMI270 init, but forcing the accelerometer bit here keeps
  // this shutdown path correct even if the rest of the firmware stops sampling
  // IMU data before calling us.
  uint8_t pwr_ctrl = 0;
  if (!bmiRead8(PWR_CTRL_ADDR, &pwr_ctrl)) return false;
  pwr_ctrl |= PWR_CTRL_ACC_ENABLE;
  if (!bmiWrite8(PWR_CTRL_ADDR, pwr_ctrl)) return false;
  delay(2);
  return true;
}

bool _configureBmiAnyMotion(bool enable) {
  uint8_t pwr_conf = 0;
  if (!bmiRead8(PWR_CONF_ADDR, &pwr_conf)) return false;

  // Bosch's API avoids feature-config writes while advanced power save is
  // active. M5Unified currently leaves advanced power save disabled, but this
  // guard preserves the caller's original state so the register sequence stays
  // valid if that default changes.
  const bool restore_adv_power_save = (pwr_conf & PWR_CONF_ADV_POWER_SAVE) != 0;
  if (restore_adv_power_save && !_setBmiAdvPowerSave(false)) return false;

  if (!bmiWrite8(FEAT_PAGE_ADDR, ANY_MOT_PAGE)) {
    M5_LOGE("Failed to select BMI270 any-motion feature page");
    return false;
  }

  uint8_t feat_config[16] = {0};
  if (!bmiRead(FEATURES_REG_ADDR, feat_config, sizeof(feat_config))) {
    M5_LOGE("Failed to read BMI270 feature config");
    return false;
  }

  const uint8_t before = feat_config[ANY_MOT_ENABLE_BYTE];
#if IMU_POWER_ENABLE_DIAGNOSTICS
  M5_LOGD("BMI270 any-motion config before: %02X %02X %02X %02X",
          feat_config[ANY_MOT_START], feat_config[ANY_MOT_START + 1],
          feat_config[ANY_MOT_START + 2], feat_config[ANY_MOT_START + 3]);
#endif
  if (enable) {
    feat_config[ANY_MOT_ENABLE_BYTE] |= ANY_MOT_ENABLE_MASK;
  } else {
    feat_config[ANY_MOT_ENABLE_BYTE] &= ~ANY_MOT_ENABLE_MASK;
  }

#if IMU_POWER_ENABLE_DIAGNOSTICS
  M5_LOGD(
      "BMI270 any-motion enable byte: 0x%02X -> 0x%02X; after: %02X %02X %02X "
      "%02X",
      before, feat_config[ANY_MOT_ENABLE_BYTE], feat_config[ANY_MOT_START],
      feat_config[ANY_MOT_START + 1], feat_config[ANY_MOT_START + 2],
      feat_config[ANY_MOT_START + 3]);
#else
  (void)before;
#endif

  if (!bmiWrite(FEATURES_REG_ADDR, feat_config, sizeof(feat_config))) {
    M5_LOGE("Failed to write BMI270 feature config");
    return false;
  }

  if (restore_adv_power_save && !_setBmiAdvPowerSave(true)) return false;
  return true;
}

bool _configureBmiInt1ForAnyMotion() {
  uint8_t int_config[3] = {0};
  if (!bmiRead(INT1_IO_CTRL_ADDR, int_config, sizeof(int_config))) return false;

  // The three-byte burst covers INT1_IO_CTRL, INT2_IO_CTRL, and INT_LATCH.
  // Only INT1 is used. INT2 is preserved, while INT1 is forced to SparkFun's
  // known-working output shape and latch is forced off.
  int_config[0] &=
      ~(INT_IO_OPEN_DRAIN | INT_IO_OUTPUT_ENABLE | INT_IO_INPUT_ENABLE);
  int_config[0] &= ~INT_IO_LEVEL;
  int_config[0] |= INT_IO_OUTPUT_ENABLE;
  int_config[2] &= ~INT_LATCH_ENABLE;

  if (!bmiWrite(INT1_IO_CTRL_ADDR, int_config, sizeof(int_config)))
    return false;

  uint8_t int_map[2] = {0};
  if (!bmiRead(INT1_MAP_FEAT_ADDR, int_map, sizeof(int_map))) return false;
  // Map any-motion to INT1 and explicitly unmap it from INT2. This mirrors
  // SparkFun mapInterruptToPin(BMI2_ANY_MOTION_INT, BMI2_INT1), which routes
  // feature interrupts through bmi2_map_feat_int().
  int_map[0] |= INT1_MAP_ANY_MOTION;
  int_map[1] &= ~INT1_MAP_ANY_MOTION;
  return bmiWrite(INT1_MAP_FEAT_ADDR, int_map, sizeof(int_map));
}

bool _clearBmiDataInterruptMap() {
  // M5Unified maps data-ready events onto the interrupt pins during BMI270
  // initialization. Clear that mapping so only the any-motion feature can wake
  // the PM1.
  if (!bmiWrite8(INT_MAP_DATA_ADDR, 0x00)) return false;
#if IMU_POWER_ENABLE_DIAGNOSTICS
  M5_LOGD("BMI270 data interrupts unmapped from INT pins");
#endif
  return true;
}

#if IMU_POWER_ENABLE_DIAGNOSTICS
bool _logBmiInterruptConfig(const char* label) {
  uint8_t int_config[3] = {0};
  if (!bmiRead(INT1_IO_CTRL_ADDR, int_config, sizeof(int_config))) return false;

  uint8_t feat_map[2] = {0};
  if (!bmiRead(INT1_MAP_FEAT_ADDR, feat_map, sizeof(feat_map))) return false;

  uint8_t data_map = 0;
  if (!bmiRead8(INT_MAP_DATA_ADDR, &data_map)) return false;

  uint8_t pwr_conf = 0;
  if (!bmiRead8(PWR_CONF_ADDR, &pwr_conf)) return false;

  M5_LOGI(
      "%s BMI270 int1=0x%02X int2=0x%02X latch=0x%02X feat_map=0x%02X/0x%02X "
      "data_map=0x%02X pwr_conf=0x%02X",
      label, int_config[0], int_config[1], int_config[2], feat_map[0],
      feat_map[1], data_map, pwr_conf);
  return true;
}
#endif

bool _configureBmiLowPowerStandby() {
  // Force accel-only operation. _ensureBmiReady only OR-s in acc_en, so a
  // prior runtime mode that left gyr_en/aux_en/temp_en set would still draw
  // their currents through shutdown and defeat advanced power save.
  if (!bmiWrite8(PWR_CTRL_ADDR, PWR_CTRL_ACC_ENABLE)) return false;

  // ACC_CONF for minimum standby current:
  //   acc_odr        = 0x07 (50 Hz) — BMI270 datasheet §4.8.1 sets 50 Hz as
  //                    the minimum ODR for any-motion in low-power mode.
  //   acc_bwp        = 0x02 (norm_avg4) — averaging recommended in LP mode.
  //   acc_filter_perf= 0   (low-power filter path). Together with APS, this
  //                    is what produces the ~15× current reduction.
  // Field decoding matches src/util/imu.cpp:85 (acc_conf_t).
  static constexpr uint8_t ACC_CONF_LP_50HZ = 0x27;
  return bmiWrite8(ACC_CONF_ADDR, ACC_CONF_LP_50HZ);
}

bool _clearBmiInterruptStatus() {
  uint8_t int_status[2] = {0};
  if (!bmiRead(INT_STATUS_0_ADDR, int_status, sizeof(int_status))) return false;
#if IMU_POWER_ENABLE_DIAGNOSTICS
  M5_LOGD("BMI270 interrupt status: 0x%02X 0x%02X", int_status[0],
          int_status[1]);
#endif
  return true;
}

bool _waitForMotionInterruptIdle() {
  static constexpr uint32_t timeout_ms = 2000;
  static constexpr uint32_t stable_ms = 250;
  static constexpr uint32_t poll_ms = 25;

  uint32_t waited_ms = 0;
  uint32_t stable_inactive_ms = 0;

  while (waited_ms < timeout_ms) {
    uint8_t level = 0;
    HANDLE_ERR(_pm1.gpioGetInput(M5PM1_GPIO_NUM_4, &level), "gpioGetInput");

    if (level) {
      stable_inactive_ms += poll_ms;
      if (stable_inactive_ms >= stable_ms) return true;
    } else {
      stable_inactive_ms = 0;
      _clearBmiInterruptStatus();
    }

    delay(poll_ms);
    waited_ms += poll_ms;
  }

  M5_LOGW("BMI270 INT1 did not return inactive before shutdown");
  return false;
}

// Public API

bool enableWakeUpOnMotionAndShutdown() {
  // NB: this holds the I2cLock for the whole function — well past the "~15 ms,
  // contention is brief" budget documented in util/i2c_lock.h. _waitForMotion-
  // InterruptIdle() alone can poll for up to 2 s, during which the imu_fifo
  // drain task (the only other bus user) is blocked and its FIFO overflows.
  // That's tolerable *only* because every path through here is terminal: on
  // success the PMIC cuts power inside this call; on failure the caller falls
  // through to powerOff(). The dropped FIFO frames are discarded at shutdown
  // and there is no task watchdog to trip. If this ever gains a non-shutdown
  // caller, narrow the lock to the individual transactions instead.
  I2cLock lock;
  if (!_initPM1()) return false;

  // Configure PM1 before touching BMI270 so the LDO stays on after PM1 shutdown
  // and the wake input has a stable idle-high state before GPIO wake is armed.
  // Wake is disabled until the BMI270 line has been verified inactive.
  HANDLE_ERR(_pm1.gpioSetWakeEnable(M5PM1_GPIO_NUM_4, false),
             "gpioSetWakeEnable");
  HANDLE_ERR(_pm1.gpioSetMode(M5PM1_GPIO_NUM_4, M5PM1_GPIO_MODE_INPUT),
             "gpioSetMode");
  HANDLE_ERR(_pm1.gpioSetPull(M5PM1_GPIO_NUM_4, M5PM1_GPIO_PULL_UP),
             "gpioSetPull");
  HANDLE_ERR(_pm1.gpioSetFunc(M5PM1_GPIO_NUM_4, M5PM1_GPIO_FUNC_WAKE),
             "gpioSetFunc");
  HANDLE_ERR(_pm1.gpioSetWakeEdge(M5PM1_GPIO_NUM_4, M5PM1_GPIO_WAKE_FALLING),
             "gpioSetWakeEdge");
  HANDLE_ERR(_pm1.setLdoEnable(true), "setLdoEnable");
  HANDLE_ERR(_pm1.ldoSetPowerHold(true), "ldoSetPowerHold");
  HANDLE_ERR(_pm1.setLedEnLevel(true), "setLedEnLevel");

  if (!_ensureBmiReady()) {
    M5_LOGE("BMI270 is not ready for motion wake");
    return false;
  }

  if (!_configureBmiAnyMotion(true)) {
    M5_LOGE("Failed to configure BMI270 any-motion feature");
    return false;
  }

  if (!_configureBmiInt1ForAnyMotion()) {
    M5_LOGE("Failed to configure BMI270 INT1");
    return false;
  }

  if (!_clearBmiDataInterruptMap()) {
    M5_LOGE("Failed to unmap BMI270 data interrupts");
    return false;
  }

  // Switch the accel to its low-power standby configuration before arming.
  // The subsequent _waitForMotionInterruptIdle loop will absorb any transient
  // INT1 activity caused by the ODR / filter change.
  if (!_configureBmiLowPowerStandby()) {
    M5_LOGE("Failed to set BMI270 low-power standby config");
    return false;
  }

#if IMU_POWER_ENABLE_DIAGNOSTICS
  if (!_logBmiInterruptConfig("pre-arm")) return false;
#endif

  if (!_clearBmiInterruptStatus()) return false;
  if (!_waitForMotionInterruptIdle()) {
    M5_LOGW("Motion wake not armed because INT1 is active");
    return false;
  }

  // Final BMI270 write before shutdown: enable advanced power save. APS=1 in
  // accel-only low-power filter mode drops standby current to roughly
  // 1/15th of normal (~210 µA → ~13 µA per datasheet §4.5 / Table 6). The
  // any-motion feature engine continues to operate. APS=1 imposes a ≥450 µs
  // delay between subsequent register writes; we issue no further writes
  // before _pm1.shutdown(), so the constraint is satisfied trivially.
  if (!_setBmiAdvPowerSave(true)) {
    M5_LOGE("Failed to enable BMI270 advanced power save");
    return false;
  }

  _clearWakeState();
#if IMU_POWER_ENABLE_DIAGNOSTICS
  _logWakeState("pre-shutdown");
#endif
  HANDLE_ERR(_pm1.gpioSetWakeEnable(M5PM1_GPIO_NUM_4, true),
             "gpioSetWakeEnable");
  delay(10);
#if IMU_POWER_ENABLE_DIAGNOSTICS
  _logWakeState("armed");
#endif

  M5_LOGI("Shutting down with motion wake-up enabled");
  HANDLE_ERR(_pm1.shutdown(), "shutdown");
  return true;
}

bool disableWakeUpOnMotion() {
  I2cLock lock;

  // These raw reads do not require the M5PM1 wrapper. Attempt them before its
  // initialization guard so a failed wrapper init still leaves useful display
  // power diagnostics in the persisted Wake entry.
  if (!gBootDiagnosticsSealed) {
    uint8_t gpioOut = 0;
    if (M5.In_I2C.readRegister(M5PM1_DEFAULT_ADDR, M5PM1_REG_GPIO_OUT, &gpioOut,
                               1, M5PM1_I2C_FREQ_100K)) {
      markBootFlag(power::PowerBootLcdRailRead);
      if ((gpioOut & (1u << M5PM1_GPIO_NUM_2)) != 0)
        markBootFlag(power::PowerBootLcdRailOn);
    }
  }

  if (!_initPM1()) return false;
  markBootFlag(power::PowerBootPm1Ready);

  if (!gBootDiagnosticsSealed) {
    uint8_t wakeSource = 0;
    if (_pm1.getWakeSource(&wakeSource, M5PM1_CLEAN_NONE) == M5PM1_OK) {
      gBootDiagnostics.pm1WakeSource = wakeSource;
      markBootFlag(power::PowerBootWakeSourceValid);
    }
    uint8_t gpioIrq = 0;
    if (_pm1.irqGetGpioStatus(&gpioIrq, M5PM1_CLEAN_NONE) == M5PM1_OK) {
      gBootDiagnostics.pm1GpioIrq = gpioIrq;
      markBootFlag(power::PowerBootGpioIrqValid);
    }
  }

#if IMU_POWER_ENABLE_DIAGNOSTICS
  _logWakeState("startup");
  _logBmiInterruptConfig("startup");
#endif
  _clearBmiInterruptStatus();

  // Disable PM1 wake on GPIO4 and release power hold.
  // BMI270 any-motion feature does not need to be explicitly disabled here:
  // if the device is subsequently powered off via the button (without going
  // through enableWakeUpOnMotionAndShutdown), the PM1 will cut L1, the BMI270
  // loses power, and its register state resets automatically.
  HANDLE_ERR(_pm1.gpioSetWakeEnable(M5PM1_GPIO_NUM_4, false),
             "gpioSetWakeEnable");
  HANDLE_ERR(_pm1.gpioSetFunc(M5PM1_GPIO_NUM_4, M5PM1_GPIO_FUNC_GPIO),
             "gpioSetFunc");
  _clearWakeState();
  HANDLE_ERR(_pm1.ldoSetPowerHold(false), "ldoSetPowerHold");

  M5_LOGI("Wake-up on motion disabled");
  return true;
}

bool setStatusLedEnabled(bool enabled) {
  I2cLock lock;
  if (!_initPM1()) return false;
  HANDLE_ERR(_pm1.setLedEnLevel(enabled), "setLedEnLevel");
  return true;
}

bool setExtPowerEnabled(bool enabled) {
  I2cLock lock;
  if (!_initPM1()) return false;
  HANDLE_ERR(_pm1.setBoostEnable(enabled), "setBoostEnable");
  return true;
}

}  // namespace StickS3PowerHardware

namespace power {

void preparePM1ForBoot() {
  // M5GFX identifies the StickS3 by reading PM1 before it enables the L3B rail
  // that powers the LCD. PM1 uses the first I2C transaction after sleep only
  // as a wake signal, so that read is expected to fail. Prime the fixed
  // internal bus here and allow the same 10 ms wake delay used by M5PM1 before
  // M5.begin() performs hardware detection.
  static constexpr uint8_t kPm1Address = M5PM1_DEFAULT_ADDR;
  static constexpr uint32_t kPm1I2cFrequency = M5PM1_I2C_FREQ_100K;
  static constexpr int kInternalSda = 47;
  static constexpr int kInternalScl = 48;

  if (!M5.In_I2C.begin(I2C_NUM_1, kInternalSda, kInternalScl)) return;
  markBootFlag(power::PowerBootPrewakeBusReady);

  const bool started = M5.In_I2C.start(kPm1Address, false, kPm1I2cFrequency);
  if (started) markBootFlag(power::PowerBootPrewakeTransactionStarted);
  if (started && M5.In_I2C.stop())
    markBootFlag(power::PowerBootPrewakeTransactionSucceeded);
  delay(10);

  // Capture PM1's wake-side state before normalizing it. M5GFX and M5PM1 both
  // disable PM1 I2C idle sleep during successful initialization; doing so here
  // removes the timing dependency between this wake pulse and autodetection.
  uint8_t config = 0;
  if (M5.In_I2C.readRegister(kPm1Address, M5PM1_REG_I2C_CFG, &config, 1,
                             kPm1I2cFrequency)) {
    gBootDiagnostics.pm1I2cConfig = config;
    markBootFlag(power::PowerBootI2cConfigRead);
  }
  if (M5.In_I2C.writeRegister8(kPm1Address, M5PM1_REG_I2C_CFG, 0x00,
                               kPm1I2cFrequency)) {
    uint8_t normalized = 0;
    if (M5.In_I2C.readRegister(kPm1Address, M5PM1_REG_I2C_CFG, &normalized, 1,
                               kPm1I2cFrequency) &&
        (normalized & M5PM1_I2C_CFG_SLEEP_MASK) == 0)
      markBootFlag(power::PowerBootI2cSleepDisabled);
  }
}

BatteryStatus getBatteryStatus() {
  // The battery and charger share M5.In_I2C with the BMI270 FIFO drain. Keep
  // the related reads together so callers cannot combine values separated by
  // another task's I2C transaction.
  I2cLock lock;
  // External power from the VIN rail (getVBUSVoltage reads PM1 VIN, ~5 V
  // plugged / ~0 unplugged; returns -1 on read failure, which falls below the
  // threshold and so fails safe to "not plugged in"). VIN is the USB-C / DC
  // charging input. NB: the PM1 also has a separate 5VINOUT (Grove) port that
  // can be an external 5 V input (regs 0x26/0x27); we don't poll it — add it
  // here (OR the two readings) if Grove-powering ever needs to count as
  // external power.
  // M5.Power.isCharging() proved unreliable here: it reflects CHG_STAT, which
  // reads "not charging" once the pack is full (even while plugged) and, worse,
  // gets silently corrupted to "charging" under shared-bus contention with e.g.
  // the IMU FIFO drain (readRegister8 returns 0 on a failed read → maps to
  // is_charging). VIN reads cleanly under the same load and reports actual
  // input presence — the right signal for both "plugged in" and "don't sleep
  // while plugged in".
  static constexpr int16_t kExternalPowerThresholdMv = 4000;
  const int voltageMv = M5.Power.getBatteryVoltage();
  const int percent =
      voltageMv > 0 ? estimateStickS3BatteryPercent(voltageMv) : -1;
  const bool hasExternalPower =
      M5.Power.getVBUSVoltage() > kExternalPowerThresholdMv;
  ChargingState charging = ChargingState::Unknown;
  if (hasExternalPower) {
    const auto rawCharging = M5.Power.isCharging();
    if (rawCharging == m5::Power_Class::is_charging)
      charging = ChargingState::Charging;
    else if (rawCharging == m5::Power_Class::is_discharging)
      charging = ChargingState::NotCharging;
  }
  return {percent, voltageMv > 0 ? voltageMv : -1, hasExternalPower, charging};
}

void logBatteryStatus() {
  const BatteryStatus status = getBatteryStatus();
  M5_LOGD("Battery status: %d%%%s", status.percent,
          status.hasExternalPower ? " (ext power)" : "");
}

bool enableWakeUpOnMotionAndShutdown() {
  return StickS3PowerHardware::enableWakeUpOnMotionAndShutdown();
}

bool disableWakeUpOnMotion() {
  return StickS3PowerHardware::disableWakeUpOnMotion();
}

void configurePowerButton() {
  I2cLock lock;
  if (!StickS3PowerHardware::_initPM1()) {
    M5_LOGE("Power button: PM1 initialization failed");
    return;
  }
  // PM1 resets the ESP32 on a single click unless firmware explicitly takes
  // ownership. The double-click power-off and download-mode settings occupy
  // separate bits and are preserved by this read-modify-write operation.
  if (StickS3PowerHardware::_pm1.setSingleResetDisable(true) != M5PM1_OK) {
    M5_LOGE("Power button: failed to disable single-click reset");
    return;
  }
  // PowerManager waits at least kPowerButtonDoubleClickWindowMs after release,
  // so this PM1 interval must remain no longer than that constant.
  if (StickS3PowerHardware::_pm1.btnSetConfig(
          M5PM1_BTN_TYPE_DOUBLE, M5PM1_BTN_DOUBLE_CLICK_DELAY_500MS) !=
      M5PM1_OK) {
    M5_LOGE("Power button: failed to configure double-click interval");
    return;
  }
  // Clear button IRQs left by wake-up or older firmware that consumed PM1
  // click events directly.
  if (StickS3PowerHardware::_pm1.irqClearBtnAll() != M5PM1_OK) {
    M5_LOGE("Power button: failed to clear pending events");
    return;
  }
  bool ignored = false;
  if (StickS3PowerHardware::_pm1.btnGetFlag(&ignored) != M5PM1_OK) {
    M5_LOGE("Power button: failed to clear pending press flag");
    return;
  }
}

bool pollPowerButton(PowerButtonSample& sample) {
  I2cLock lock;
  if (!StickS3PowerHardware::_initPM1()) return false;

  // M5Unified 0.2.14 does not connect pmic_m5pm1 to M5.BtnPWR; newer releases
  // do, so revisit this path when upgrading. The PM1 flag retains taps between
  // polls, while the live state lets PowerManager leave held gestures to PM1.
  if (StickS3PowerHardware::_pm1.btnGetFlag(&sample.wasPressed) != M5PM1_OK)
    return false;
  if (StickS3PowerHardware::_pm1.btnGetState(&sample.isPressed) != M5PM1_OK)
    return false;
  return true;
}

bool setStatusLedEnabled(bool enabled) {
  return StickS3PowerHardware::setStatusLedEnabled(enabled);
}

bool setExtPowerEnabled(bool enabled) {
  return StickS3PowerHardware::setExtPowerEnabled(enabled);
}

namespace {
// Capture the current battery + wall-clock state and append a power-lifecycle
// event. Shared by the wake and sleep entry points so the gather logic lives
// in one place.
void recordPowerEvent(PowerEventLog& eventLog, PowerEventKind kind,
                      uint8_t resetReason) {
  const BatteryStatus b = getBatteryStatus();
  PowerSleepDiagnostics sleep{};
  if (kind == PowerEventKind::Sleep) {
    uint8_t config = 0;
    I2cLock lock;
    if (M5.In_I2C.readRegister(M5PM1_DEFAULT_ADDR, M5PM1_REG_I2C_CFG, &config,
                               1, M5PM1_I2C_FREQ_100K)) {
      sleep.flags |= PowerSleepPm1I2cConfigValid;
      sleep.pm1I2cConfig = config;
    }
  }
  eventLog.record(
      kind, b.percent, b.hasExternalPower, wallclock::utcNow(), resetReason,
      kind == PowerEventKind::Wake ? gBootDiagnostics : PowerBootDiagnostics{},
      sleep);
}
}  // namespace

void recordWakeEvent(PowerEventLog& eventLog) {
  if (M5.Display.getBoard() == m5::board_t::board_M5StickS3)
    markBootFlag(power::PowerBootDisplayDetected);
  gWakeRecordedMs = millis();
  gWakeTimestampPending = !wallclock::isSet();
  recordPowerEvent(eventLog, PowerEventKind::Wake,
                   static_cast<uint8_t>(esp_reset_reason()));
  gBootDiagnosticsSealed = true;
}

void recordSleepEvent(PowerEventLog& eventLog) {
  recordPowerEvent(eventLog, PowerEventKind::Sleep, /*resetReason=*/0);
}

void backfillWakeTimestamp(PowerEventLog& eventLog) {
  if (!gWakeTimestampPending) return;
  const uint32_t nowUtc = wallclock::utcNow();
  if (nowUtc == 0) return;

  const uint32_t elapsedSec = (millis() - gWakeRecordedMs) / 1000;
  const uint32_t eventUtc = nowUtc > elapsedSec ? nowUtc - elapsedSec : nowUtc;
  if (eventLog.backfillLatestWakeTimestamp(eventUtc)) {
    gWakeTimestampPending = false;
    M5_LOGI("Power log: backfilled wake time (epoch=%u)",
            static_cast<unsigned>(eventUtc));
  }
}

}  // namespace power
