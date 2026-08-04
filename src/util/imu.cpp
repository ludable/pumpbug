// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "imu.h"

#include "util/i2c_lock.h"

namespace ImuUtil {

// I2C address (StickS3 uses primary address 0x68, not the 0x69 default)
constexpr uint8_t BMI270_ADDR = 0x68;
constexpr uint32_t BMI270_FREQ = 400000;
constexpr uint32_t BMI270_FREQ_FAST = 1000000;

// BMI270 operations must not prevent the FIFO task from observing a stop
// request when another shared-bus user fails to release the mutex.
constexpr uint32_t kI2cLockTimeoutMs = 100;

bool bmiRead(uint8_t reg, uint8_t* data, size_t len) {
  // Hold the shared-bus lock for the whole transaction so the main loop's
  // battery/orientation reads can't interleave and corrupt it. See i2c_lock.h.
  I2cLock lock(kI2cLockTimeoutMs);
  if (!lock.acquired()) {
    M5_LOGE("BMI270 read 0x%02X skipped: I2C lock timeout", reg);
    return false;
  }
  if (!M5.In_I2C.readRegister(BMI270_ADDR, reg, data, len, BMI270_FREQ_FAST)) {
    M5_LOGE("BMI270 read 0x%02X failed", reg);
    return false;
  }
  return true;
}

bool bmiWrite(uint8_t reg, const uint8_t* data, size_t len) {
  I2cLock lock(kI2cLockTimeoutMs);
  if (!lock.acquired()) {
    M5_LOGE("BMI270 write 0x%02X skipped: I2C lock timeout", reg);
    return false;
  }
  if (!M5.In_I2C.writeRegister(BMI270_ADDR, reg, data, len, BMI270_FREQ_FAST)) {
    M5_LOGE("BMI270 write 0x%02X failed", reg);
    return false;
  }
  return true;
}

//
// General BMI270 registers
//

constexpr uint8_t CHIP_ID_REG_ADDR = 0x00;
constexpr uint8_t BMI270_DEFAULT_CHIP_ID = 0x24;

bool bmiCanConnect() {
  uint8_t chip_id;
  if (!bmiRead8(CHIP_ID_REG_ADDR, &chip_id)) {
    M5_LOGD("Can't read CHIP_ID");
    return false;
  }
  if (chip_id != BMI270_DEFAULT_CHIP_ID) {
    M5_LOGD("CHIP_ID mismatch: 0x%02x != 0x%02x", chip_id,
            BMI270_DEFAULT_CHIP_ID);
    return false;
  }
  return true;
}

constexpr uint8_t PWR_CONF_REG_ADDR = 0x7C;
constexpr uint8_t PWR_CTRL_REG_ADDR = 0x7D;

union pwr_conf_t {
  struct {
    uint8_t adv_power_save : 1;
    uint8_t fifo_self_wake_up : 1;  // For low power mode
    uint8_t fup_en : 1;             // Fast power up
  };
  uint8_t value;
};

union pwr_ctrl_t {
  struct {
    uint8_t aux_en : 1;   // Magnetometer enabled
    uint8_t gyr_en : 1;   // Gyroscope enabled
    uint8_t acc_en : 1;   // Accelerometer enabled
    uint8_t temp_en : 1;  // Temperature sensor enabled
  };
  uint8_t value;
};

constexpr uint8_t CMD_REG_ADDR = 0x7E;

enum class bmi270_cmd {
  g_trigger = 0x02,   // Trigger special gyro operations
  usr_gain = 0x03,    // Applies new gyro gain value
  nvm_prog = 0xa0,    // Writes the NVM backed registers into NVM
  fifo_flush = 0xb0,  // Clears FIFO content
  softreset = 0xb6,   // All user config settings reset to defaults
};

//
// Accelerometer
//

constexpr uint8_t ACC_CONF_REG_ADDR = 0x40;

union acc_conf_t {
  enum class acc_odr_t : uint8_t {
    // Output data rate in Hz
    reserved = 0,
    odr_0p78,  // 25/32 = 0.78 Hz
    odr_1p5,   // 25/16 = 1.5 Hz
    odr_3p1,   // 25/8 = 3.1 Hz
    odr_6p25,  // 25/4 = 6.25 Hz
    odr_12p5,  // 25/2 = 12.5 Hz
    odr_25,    // 25 Hz
    odr_50,    // etc
    odr_100,
    odr_200,
    odr_400,
    odr_800,
    odr_1k6,
    odr_3k2,   // Reserved
    odr_6k4,   // Reserved
    odr_12k8,  // Reserved
  };
  enum class acc_bwp_t : uint8_t {
    // Filter configuration (for filter_perf == hp) or
    // averaging(for filter_perf == ulp); see spec sheet
    osr4_avg1,
    osr2_avg2,
    norm_avg4,
    cic_avg8,
    res_avg16,
    res_avg32,
    res_avg64,
    res_avg128,
  };
  enum class acc_filter_perf_t : uint8_t {
    ulp,  // Power-optimized
    hp,   // Performance-optimized
  };
  struct {
    acc_odr_t acc_odr : 4;
    acc_bwp_t acc_bwp : 3;
    acc_filter_perf_t acc_filter_perf : 1;
  };
  uint8_t value;
};

constexpr uint8_t ACC_RANGE_REG_ADDR = 0x41;

acc_range_t getAccelerometerRange() {
  acc_range_t range;
  if (!bmiRead8(ACC_RANGE_REG_ADDR, (uint8_t*)&range)) {
    M5_LOGE("Failed to read BMI270 accelerometer range");
    return acc_range_t::read_error;
  }
  return range;
}

bool setAccelerometerRange(acc_range_t range) {
  if (!bmiWrite8(ACC_RANGE_REG_ADDR, (uint8_t)range)) {
    M5_LOGE("Failed to write BMI270 accelerometer range");
    return false;
  }
  return true;
}

//
// FIFO mode
//

constexpr uint8_t FIFO_CONFIG_0_REG_ADDR = 0x48;
constexpr uint8_t FIFO_CONFIG_1_REG_ADDR = 0x49;

union fifo_config_0_t {
  struct {
    uint8_t fifo_stop_on_full : 1;
    uint8_t fifo_time_en : 1;  // Return sensor time after last valid frame
  };
  uint8_t value;
};

union fifo_config_1_t {
  enum class fifo_tag_t : uint8_t {
    // For fifo_tag_int1 and fifo_tag_int2
    int_edge = 0,
    int_level = 1,
    acc_sat = 2,
    gyr_sat = 3,
  };
  struct {
    fifo_tag_t fifo_tag_int1 : 2;  // Interrupt 1 tag enable
    fifo_tag_t fifo_tag_int2 : 2;  // Interrupt 2 tag enable
    uint8_t fifo_header_en : 1;    // Frame header enable
    uint8_t fifo_aux_en : 1;       // Enable magnetometer data
    uint8_t fifo_acc_en : 1;       // Enable accelerometer data
    uint8_t fifo_gyr_en : 1;       // Enable gyroscope data
  };
  uint8_t value;
};

constexpr uint8_t FIFO_LENGTH_0_REG_ADDR = 0x24;
constexpr uint8_t FIFO_DATA_REG_ADDR = 0x26;

bool readFifoLength(uint16_t& length) {
  uint8_t b[2];
  // Note this reads both address 0x24 (FIFO_LENGTH_0) and 0x25 (FIFO_LENGTH_1)
  if (!bmiRead(FIFO_LENGTH_0_REG_ADDR, b, 2)) return false;
  length = ((uint16_t)b[1] << 8 | b[0]) & 0x1FFF;
  return true;
}

//
// AccelFIFO
//

struct AccelFIFO::_saved_imu_config_t {
  pwr_conf_t pwr_conf;
  pwr_ctrl_t pwr_ctrl;
  acc_conf_t acc_conf;
  acc_range_t acc_range;
  fifo_config_0_t fifo_config0;
  fifo_config_1_t fifo_config1;
};

AccelFIFO::~AccelFIFO() {
  if (_saved_imu_config) delete _saved_imu_config;
}

bool AccelFIFO::flushFIFO() {
  return bmiWrite8(CMD_REG_ADDR, (uint8_t)bmi270_cmd::fifo_flush);
}

bool AccelFIFO::begin(batch_processor_t batchProcessor, void* param,
                      acc_range_t range) {
  switch (_state) {
    case State::NOT_INIT:
    case State::STOPPED:
      _batchProcessor = batchProcessor;
      _param = param;
      _range = range;
      return setupIMU();
    default:
      return false;
  }
}

bool AccelFIFO::end() {
  switch (_state) {
    case State::TASK_RUNNING:
      _state = State::STOP_REQUESTED;
      return true;
    default:
      return false;
  }
}

bool AccelFIFO::setupIMU() {
  if (!bmiCanConnect()) return false;

  if (!_saved_imu_config) {
    _saved_imu_config = new _saved_imu_config_t();
  }
  if (!bmiRead8(PWR_CONF_REG_ADDR, &_saved_imu_config->pwr_conf.value) ||
      !bmiRead8(PWR_CTRL_REG_ADDR, &_saved_imu_config->pwr_ctrl.value) ||
      !bmiRead8(ACC_CONF_REG_ADDR, &_saved_imu_config->acc_conf.value) ||
      !bmiRead8(ACC_RANGE_REG_ADDR, (uint8_t*)&_saved_imu_config->acc_range) ||
      !bmiRead8(FIFO_CONFIG_0_REG_ADDR,
                &_saved_imu_config->fifo_config0.value) ||
      !bmiRead8(FIFO_CONFIG_1_REG_ADDR, &_saved_imu_config->fifo_config1.value))
    return false;

  // Full performance, no advanced power save. Enable accel only.
  if (!bmiWrite8(PWR_CONF_REG_ADDR, 0)) return false;
  pwr_ctrl_t pwr_ctrl;
  pwr_ctrl.acc_en = 1;
  if (!bmiWrite8(PWR_CTRL_REG_ADDR, pwr_ctrl.value)) return false;

  // ODR, BWP
  acc_conf_t acc_conf;
  acc_conf.acc_odr = acc_conf_t::acc_odr_t::odr_1k6;  // 1600 Hz
  acc_conf.acc_bwp =
      acc_conf_t::acc_bwp_t::norm_avg4;  // Normal rate (no oversampling)
  acc_conf.acc_filter_perf = acc_conf_t::acc_filter_perf_t::hp;
  if (!bmiWrite8(ACC_CONF_REG_ADDR, acc_conf.value)) return false;

  // Range
  if (!bmiWrite8(ACC_RANGE_REG_ADDR, (uint8_t)_range)) return false;

  // FIFO config
  fifo_config_0_t config0;
  config0.fifo_stop_on_full = 0;  // Overwrite existing data if full
  config0.fifo_time_en = 0;       // Do not add timestamp after last valid frame
  if (!bmiWrite8(FIFO_CONFIG_0_REG_ADDR, config0.value)) return false;
  fifo_config_1_t config1;
  config1.fifo_header_en = 0;  // No header, only data
  config1.fifo_acc_en = 1;     // Get accelerometer data only
  config1.fifo_gyr_en = 0;
  config1.fifo_aux_en = 0;
  if (!bmiWrite8(FIFO_CONFIG_1_REG_ADDR, config1.value)) return false;

  if (!flushFIFO()) return false;

  _state = State::TASK_RUNNING;
  if (xTaskCreatePinnedToCore(s_task, "imu_fifo", 4096, this, 5, nullptr, 0) !=
      pdPASS) {
    M5_LOGE("AccelFIFO task creation failed");
    restoreIMU();
    _state = State::NOT_INIT;
    return false;
  }
  return true;
}

bool AccelFIFO::restoreIMU() {
  bool error = false;
  if (!bmiWrite8(FIFO_CONFIG_1_REG_ADDR,
                 _saved_imu_config->fifo_config1.value)) {
    M5_LOGE("Error restoring FIFO config 1 register");
    error = true;
  }
  if (!bmiWrite8(FIFO_CONFIG_0_REG_ADDR,
                 _saved_imu_config->fifo_config0.value)) {
    M5_LOGE("Error restoring FIFO config 0 register");
    error = true;
  }
  if (!bmiWrite8(ACC_RANGE_REG_ADDR, (uint8_t)_saved_imu_config->acc_range)) {
    M5_LOGE("Error restoring accelerometer range register");
    error = true;
  }
  if (!bmiWrite8(ACC_CONF_REG_ADDR, _saved_imu_config->acc_conf.value)) {
    M5_LOGE("Error restoring accelerometer config register");
    error = true;
  }
  if (!bmiWrite8(PWR_CTRL_REG_ADDR, _saved_imu_config->pwr_ctrl.value)) {
    M5_LOGE("Error restoring power control register");
    error = true;
  }
  if (!bmiWrite8(PWR_CONF_REG_ADDR, _saved_imu_config->pwr_conf.value)) {
    M5_LOGE("Error restoring power config register");
    error = true;
  }

  flushFIFO();

  delete _saved_imu_config;
  _saved_imu_config = nullptr;
  return error;
}

void AccelFIFO::s_task(void* _this) { static_cast<AccelFIFO*>(_this)->task(); };

static float rangeToGramsPerLSB(acc_range_t range) {
  float range_g;
  switch (range) {
    case acc_range_t::range_2g:
      range_g = 2.0;
      break;
    case acc_range_t::range_4g:
      range_g = 4.0;
      break;
    case acc_range_t::range_8g:
      range_g = 8.0;
      break;
    default:
      range_g = 16.0;
  }
  return range_g / 32768.0f;
}

#define DEBUG_TASK_TIMING 0

void AccelFIFO::task() {
  const size_t BATCH_BYTES = BATCH_FRAMES * 6;
  uint8_t* rawDataBuffer = new uint8_t[BATCH_BYTES];
  frame_t* valueBuffer = new frame_t[BATCH_FRAMES];

  const float g_per_lsb = rangeToGramsPerLSB(_range);

  // setupIMU() publishes TASK_RUNNING before task creation. Preserve any
  // STOP_REQUESTED value written before this task first runs.
  M5_LOGD("Started AccelFIFO task");

  while (_state == State::TASK_RUNNING) {
    // 1,600 Hz x 6 bytes (ax, ay, az) = 9,600 bytes/s = 9.6 B/ms
    // One batch (256 frames = 1536 B) fills in 1536 / 9.6 = 160 ms.
    // We wake every 40 ms and drain at most one batch per wake. The
    // worst-case FIFO level at wake-up is one batch + one wake's growth:
    // BATCH_BYTES + 40 ms * 9.6 B/ms = 1536 + 384 = 1920 B
    // FIFO size is 2048, leaving 2048 - 1920 = 128 B
    // This gives us a margin of around 13ms: 128 B / 9.6 B/ms = 13.33 ms
    vTaskDelay(pdMS_TO_TICKS(40));

#if DEBUG_TASK_TIMING > 0
    uint32_t t0 = micros();
#endif
    uint16_t available;
    if (!readFifoLength(available)) {
      M5_LOGE("Error reading IMU FIFO length");
      continue;
    }
#if DEBUG_TASK_TIMING > 0
    uint32_t t_len = micros() - t0;
#endif

    // Detect overrun (fifo_stop_on_full=1 keeps the buffer pinned at ~2048)
    if (available >= 2000) {
      _overflowCount.fetch_add(1, std::memory_order_relaxed);
      M5_LOGW("Overflow in IMU FIFO");
    }

    while (available >= BATCH_BYTES) {
      // Note this register is a special one that is read in burst mode;
      // it doesn't increase the address as it returns bytes
#if DEBUG_TASK_TIMING > 0
      t0 = micros();
#endif
      if (!bmiRead(FIFO_DATA_REG_ADDR, rawDataBuffer, BATCH_BYTES)) break;
#if DEBUG_TASK_TIMING > 0
      uint32_t t_read = micros() - t0;
#endif

#if DEBUG_TASK_TIMING > 0
      t0 = micros();
#endif
      const uint8_t* rawFrame = rawDataBuffer;
      frame_t* valueFrame = valueBuffer;
      for (size_t i = 0; i < BATCH_FRAMES; ++i) {
        int16_t x = (int16_t)((uint16_t)rawFrame[1] << 8 | rawFrame[0]);
        int16_t y = (int16_t)((uint16_t)rawFrame[3] << 8 | rawFrame[2]);
        int16_t z = (int16_t)((uint16_t)rawFrame[5] << 8 | rawFrame[4]);
        rawFrame += 6;
        valueFrame[i] = {x * g_per_lsb, y * g_per_lsb, z * g_per_lsb};
      }
#if DEBUG_TASK_TIMING > 0
      uint32_t t_decode = micros() - t0;
#endif

#if DEBUG_TASK_TIMING > 0
      t0 = micros();
#endif
      _batchProcessor(valueBuffer, BATCH_FRAMES, _param);
#if DEBUG_TASK_TIMING > 0
      uint32_t t_proc = micros() - t0;
#endif

#if DEBUG_TASK_TIMING > 0
      static uint32_t lastLog = 0;
      if (millis() - lastLog > 1000) {
        M5_LOGI(
            "len=%u read=%u dec=%u proc=%u total=%u  avail_before=%u  BATCH=%u",
            t_len, t_read, t_decode, t_proc, t_len + t_read + t_decode + t_proc,
            available, (unsigned)BATCH_BYTES);
        lastLog = millis();
      }
#endif

      available -= BATCH_BYTES;
    }
  }

  restoreIMU();

  delete[] rawDataBuffer;
  delete[] valueBuffer;

  _state = State::STOPPED;
  M5_LOGD("Ended AccelFIFO task");

  vTaskDelete(nullptr);
}

}  // namespace ImuUtil
