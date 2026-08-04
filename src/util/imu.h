// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <atomic>

namespace ImuUtil {

//
// Communication with Bosch BMI270 IMU beyond what M5.Imu provides
// https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf
//

bool bmiRead(uint8_t reg, uint8_t* data, size_t len);
bool bmiWrite(uint8_t reg, const uint8_t* data, size_t len);

inline bool bmiRead8(uint8_t reg, uint8_t* value) {
  return bmiRead(reg, value, 1);
}
inline bool bmiWrite8(uint8_t reg, uint8_t value) {
  return bmiWrite(reg, &value, 1);
}

//
// Accelerometer range
//

enum class acc_range_t : uint8_t {
  range_2g = 0x00,
  range_4g = 0x01,
  range_8g = 0x02,
  range_16g = 0x03,
  read_error = 0xFF,
};

acc_range_t getAccelerometerRange();
bool setAccelerometerRange(acc_range_t);

//
// Use accelerometer in BMI270 FIFO mode for efficient sampling at the highest
// frequency supported (1600Hz). Pair with FFT for e.g. vibration detection.
//

class AccelFIFO {
  enum class State { NOT_INIT, TASK_RUNNING, STOP_REQUESTED, STOPPED };
  std::atomic<State> _state{State::NOT_INIT};

  acc_range_t _range;

  struct _saved_imu_config_t;
  _saved_imu_config_t* _saved_imu_config;
  bool setupIMU();
  bool restoreIMU();

  bool flushFIFO();

  std::atomic<unsigned long> _overflowCount{0};
  static void s_task(void*);
  void task();

 public:
  AccelFIFO() : _saved_imu_config(nullptr) {}
  ~AccelFIFO();

  // Each batch consists of 256 data points (frames)
  // This is handy for e.g. a typical FFT window
  static constexpr uint32_t BATCH_FRAMES = 256;
  static constexpr uint32_t FRAME_FREQ = 1600;  // Hz

  // Each data point has data for the three axis
  struct frame_t {
    float ax, ay, az;
  };

  // The batch processor is called for every batch with the frame buffer
  // containing BATCH_FRAMES frames. BATCH_FRAMES is also passed in the
  // length parameter. The param is the same passed by the caller in the
  // invocation to the begin() method. The call comes from a separate
  // FreeRTOS task, do not block for extended periods of time.
  typedef void (*batch_processor_t)(const frame_t frames[],
                                    unsigned long frameCount, void* param);

  // Starts the FIFO mode and a background task to gather the frames.
  bool begin(batch_processor_t batchProcessor, void* param = nullptr,
             acc_range_t range = acc_range_t::range_2g);

  // Requests the end of the task. Monitor isStopped() to wait for actual end.
  bool end();

  bool isStopped() const {
    return _state == State::STOPPED || _state == State::NOT_INIT;
  }

  // Count the times the task couldn't keep up with the IMU.
  unsigned long getOverflowCount() const {
    return _overflowCount.load(std::memory_order_relaxed);
  }

 private:
  batch_processor_t _batchProcessor;
  void* _param;
};

};  // namespace ImuUtil
