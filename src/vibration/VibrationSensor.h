// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <atomic>
#include <cstdint>

// Optional instrumentation can inspect intermediate analysis values without
// changing the trigger result. The enabling build supplies its implementation.
#ifndef PB_VIBRATION_INSTRUMENTATION
#define PB_VIBRATION_INSTRUMENTATION 0
#endif

#if PB_VIBRATION_INSTRUMENTATION
#include "VibrationSensorInstrumentation.h"
#endif
#include "VibrationWindowTrigger.h"
#include "util/ema.h"
#include "util/fft.h"
#include "util/hpf.h"
#include "util/imu.h"

// Converts accelerometer samples into the pump-on signal used by the extraction
// recorder. The FIFO callback applies a high-pass filter and FFT, selects a
// cluster in the pump-frequency band, and feeds its frequency, SNR, and
// stationarity to VibrationWindowTrigger.
//
// Use one instance at a time. Multiple instances would race on the BMI270 and
// the shared M5.In_I2C bus.
class VibrationSensor {
 public:
  static constexpr size_t FFT_INPUT_SIZE = 256;
  static constexpr size_t FFT_OUTPUT_SIZE = 128;
  static constexpr size_t FFT_BITS = 8;
  static constexpr float STATIONARY_FLUX_MAX = 2.5f;

#if PB_VIBRATION_INSTRUMENTATION
  static_assert(
      FFT_INPUT_SIZE == VibrationSensorInstrumentation::FFT_INPUT_SIZE,
      "instrumentation input size must match the sensor FFT");
  static_assert(
      FFT_OUTPUT_SIZE == VibrationSensorInstrumentation::FFT_OUTPUT_SIZE,
      "instrumentation output size must match the sensor FFT");
#endif

  struct TriggerFeatures {
    float decisionSnrDb = 0.0f;
    float peakHz = 0.0f;
    float spectralFlux = 0.0f;
    bool stationary = false;
  };

  struct Data
#if PB_VIBRATION_INSTRUMENTATION
      : public VibrationSensorInstrumentation::Data
#endif
  {
    TriggerFeatures triggerFeatures;
    bool triggered = false;
  };

  struct DetectionTransition {
    uint32_t ms = 0;
    uint32_t detectedForMs = 0;
    float rawSnrDb = 0.0f;
    float smoothedSnrDb = 0.0f;
    float peakHz = 0.0f;
    float dominantPeakHz = 0.0f;
    float spectralFlux = 0.0f;
    uint8_t closeFailureMask = VibrationWindowTrigger::FailureNone;
    bool stationary = false;
    VibrationWindowTrigger::Event event = VibrationWindowTrigger::Event::None;
  };

  using TransitionCallback = void (*)(const DetectionTransition& transition);

#if PB_VIBRATION_INSTRUMENTATION
  using DiagnosticFrame = VibrationSensorInstrumentation::Frame;
#endif

  ~VibrationSensor();

  // Starts vibration analysis for one owner, which must call end() before
  // another owner calls begin(). Returns false if analysis cannot start.
  // `onTransition`, when supplied, runs on the FIFO task after the new snapshot
  // is published and must remain short and nonblocking.
  bool begin(TransitionCallback onTransition = nullptr);

  // Stops the FIFO and waits for its callback to finish.
  void end();

  // Clears the analyzer state while preserving the monotonically increasing
  // sequence number.
  void reset();

  // Increments after each FIFO batch has been analyzed.
  uint32_t seq() const { return _dataSeq.load(std::memory_order_relaxed); }

  unsigned long fifoOverflowCount() const {
    return _accelFifo.getOverflowCount();
  }

  // Copies the latest trigger result and returns its sequence number.
  uint32_t snapshot(Data& out);

#if PB_VIBRATION_INSTRUMENTATION
  // Copies the latest scalar measurements without the waveform and spectrum.
  uint32_t diagnosticFrame(DiagnosticFrame& out);
#endif

 private:
  Data _taskData = {};
  SemaphoreHandle_t _taskMutex = nullptr;
  std::atomic<uint32_t> _dataSeq{0};

  static void s_fifoCallback(const ImuUtil::AccelFIFO::frame_t frames[],
                             unsigned long frameCount, void* param);
  void fifoCallback(const ImuUtil::AccelFIFO::frame_t frames[],
                    unsigned long frameCount);

  ImuUtil::AccelFIFO _accelFifo;
  FftEngine _fft;
  float _samples[FFT_INPUT_SIZE] = {};
  float _fftMag[FFT_OUTPUT_SIZE] = {};

  static constexpr float HPF_FC_HZ = 8.0f;
  HPF _hpf = HPF::fromCutoff(HPF_FC_HZ, ImuUtil::AccelFIFO::FRAME_FREQ);

  static constexpr float ANALYSIS_FMIN_HZ = 10.0f;
  static constexpr float ANALYSIS_FMAX_HZ = 800.0f;
  static constexpr float TOP_K_ENERGY_FRACTION = 0.8f;
  // A Hann-windowed tone spans adjacent bins. Merging one-bin gaps produces a
  // single cluster without combining separate tones.
  static constexpr uint16_t CLUSTER_GAP = 1;
  static constexpr float SNR_EMA_ALPHA = 0.35f;

  float _magSq[FFT_OUTPUT_SIZE] = {};
  uint16_t _peakBins[FFT_OUTPUT_SIZE];
  uint16_t _clusterScratch[FFT_OUTPUT_SIZE];
  float _clusterEnergies[FFT_OUTPUT_SIZE];
  uint16_t _clusterCounts[FFT_OUTPUT_SIZE];
  EMA _smoothedSnr{SNR_EMA_ALPHA};
  uint32_t _detectedSinceMs = 0;
#if PB_VIBRATION_INSTRUMENTATION
  VibrationSensorInstrumentation _instrumentation;
#endif
  VibrationWindowTrigger _trigger;
  TransitionCallback _onTransition = nullptr;

  DetectionTransition analyze();
  void resetAnalysisLocked();
};
