// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "VibrationSensor.h"

#include <M5Unified.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "util/topk.h"

using namespace ImuUtil;

VibrationSensor::~VibrationSensor() {
  end();
  if (_taskMutex) {
    vSemaphoreDelete(_taskMutex);
    _taskMutex = nullptr;
  }
}

bool VibrationSensor::begin(TransitionCallback onTransition) {
  // Only one screen may run the shared sensor. Check before touching the FFT
  // buffers used by the active FIFO callback.
  if (!_accelFifo.isStopped()) {
    M5_LOGE("VibrationSensor already running");
    return false;
  }

  if (!_taskMutex) {
    _taskMutex = xSemaphoreCreateMutex();
    if (!_taskMutex) {
      M5_LOGE("Mutex creation failed");
      return false;
    }
  }

  // Initialize the immutable FFT tables once so changing screens does not
  // repeatedly allocate and release them.
  if (!_fft.isReady() && !_fft.begin(FFT_BITS, FftWindow::Hann)) {
    M5_LOGE("FFT begin failed");
    return false;
  }

  xSemaphoreTake(_taskMutex, portMAX_DELAY);
  _onTransition = onTransition;
  resetAnalysisLocked();
  xSemaphoreGive(_taskMutex);

  if (!_accelFifo.begin(s_fifoCallback, this)) {
    _onTransition = nullptr;
    M5_LOGE("AccelFIFO begin failed");
    return false;
  }
  return true;
}

void VibrationSensor::end() {
  _accelFifo.end();
  while (!_accelFifo.isStopped()) delay(10);
  _onTransition = nullptr;
}

void VibrationSensor::reset() {
  if (!_taskMutex) return;
  xSemaphoreTake(_taskMutex, portMAX_DELAY);
  resetAnalysisLocked();
  xSemaphoreGive(_taskMutex);
}

void VibrationSensor::resetAnalysisLocked() {
  _hpf.reset();
  std::memset(_magSq, 0, sizeof(_magSq));
  _smoothedSnr.reset();
  _trigger.reset();
  _detectedSinceMs = 0;
#if PB_VIBRATION_INSTRUMENTATION
  _instrumentation.reset();
#endif
  _taskData = Data{};
}

uint32_t VibrationSensor::snapshot(Data& out) {
  xSemaphoreTake(_taskMutex, portMAX_DELAY);
  out = _taskData;
  const uint32_t s = _dataSeq.load(std::memory_order_relaxed);
  xSemaphoreGive(_taskMutex);
  return s;
}

#if PB_VIBRATION_INSTRUMENTATION
uint32_t VibrationSensor::diagnosticFrame(DiagnosticFrame& out) {
  xSemaphoreTake(_taskMutex, portMAX_DELAY);
  const uint32_t sequence = _dataSeq.load(std::memory_order_relaxed);
  _instrumentation.copyFrame(_taskData, sequence, out);
  xSemaphoreGive(_taskMutex);
  return sequence;
}
#endif

void VibrationSensor::s_fifoCallback(const AccelFIFO::frame_t frames[],
                                     unsigned long frameCount, void* param) {
  ((VibrationSensor*)param)->fifoCallback(frames, frameCount);
}

void VibrationSensor::fifoCallback(const AccelFIFO::frame_t frames[],
                                   unsigned long frameCount) {
  assert(AccelFIFO::BATCH_FRAMES == frameCount);

  // Note we're taking the mutex through the whole computation.
  // If this causes contention with the consumer, we can work on
  // a copy instead and only block while copying.
  xSemaphoreTake(_taskMutex, portMAX_DELAY);

  for (size_t i = 0; i < frameCount; i++) {
    const auto f = frames[i];
    float a = sqrtf(f.ax * f.ax + f.ay * f.ay + f.az * f.az);
    _samples[i] = _hpf(a);
  }

  _fft.compute(_samples, _fftMag);

  const DetectionTransition transition = analyze();

  _dataSeq.fetch_add(1, std::memory_order_relaxed);
  xSemaphoreGive(_taskMutex);

  if (_onTransition &&
      transition.event != VibrationWindowTrigger::Event::None) {
    _onTransition(transition);
  }
}

VibrationSensor::DetectionTransition VibrationSensor::analyze() {
  static constexpr float kEps = 1e-20f;

  const float binHz =
      ImuUtil::AccelFIFO::FRAME_FREQ / static_cast<float>(FFT_INPUT_SIZE);
  const size_t kMin =
      std::max<size_t>(1, (size_t)ceilf(ANALYSIS_FMIN_HZ / binHz));
  const size_t kMax = std::min<size_t>(
      FFT_OUTPUT_SIZE - 2, (size_t)floorf(ANALYSIS_FMAX_HZ / binHz));

  // 1. Compute current power spectrum, total band energy, and log-domain
  // spectral flux in a single pass. Flux is the mean squared bin-by-bin
  // log-power change vs. the previous frame; bandEnergy is the running sum
  // of magSq over the analysis band, used below as the denominator basis
  // for SNR. _magSq[k] is overwritten with the new value as we go.
  float sumSqLogDiff = 0.0f;
  size_t fluxBins = 0;
  float bandEnergy = 0.0f;
  for (size_t k = 0; k < FFT_OUTPUT_SIZE; ++k) {
    const float newMs = _fftMag[k] * _fftMag[k];
    if (k >= kMin && k <= kMax) {
      const float ldiff = log10f(newMs + kEps) - log10f(_magSq[k] + kEps);
      sumSqLogDiff += ldiff * ldiff;
      ++fluxBins;
      bandEnergy += newMs;
    }
    _magSq[k] = newMs;
  }
  const float flux = fluxBins ? (sumSqLogDiff / fluxBins) : 0.0f;

  // 2. Peak search + cluster merge.
  // topByEnergyFraction runs only over the analysis band so peaks in the
  // HPF transition zone (bins 0..kMin-1) and near Nyquist can't appear as
  // the dominant tone. Returned indices are relative to the band slice and
  // shifted back to absolute bin indices for the cluster step.
  // clusterPeaks then collapses adjacent bins (windowing leakage) into
  // single clusters, with each cluster's energy = sum of its bins' magSq
  // and each count = number of bins in the cluster, so numPeaks reflects
  // distinct tonal features, not raw bin count. The "dominant" cluster
  // (highest summed energy) is the signal we report. topKEnergy (sum of
  // all top-K bins' magSq) is recovered after clustering by summing the
  // cluster energies.
  const size_t bandLen = kMax - kMin + 1;
  const size_t numTopK = topByEnergyFraction(_magSq + kMin, bandLen,
                                             TOP_K_ENERGY_FRACTION, _peakBins);
  for (size_t i = 0; i < numTopK; ++i) {
    _peakBins[i] = static_cast<uint16_t>(_peakBins[i] + kMin);
  }
  const size_t numPeaks = clusterPeaks(
      _peakBins, numTopK, _magSq, CLUSTER_GAP, _clusterScratch, _peakBins,
      _clusterEnergies, _clusterCounts, FFT_OUTPUT_SIZE);
  float topKEnergy = 0.0f;
  for (size_t i = 0; i < numPeaks; ++i) topKEnergy += _clusterEnergies[i];

  // Pick the dominant cluster (highest summed energy). Defaults below cover
  // the degenerate case where the band has no energy at all
  // (topByEnergyFraction returned 0): centerBin sits at kMin so parabolic
  // interp stays in-bounds, signalMs is 0 so the downstream SNR computes
  // to a very negative number rather than reading uninitialised state,
  // dominantClusterBins is 0 so the SNR computation knows to bail.
  size_t centerBin = kMin;
  float signalMs = 0.0f;
  uint16_t dominantClusterBins = 0;
  if (numPeaks > 0) {
    size_t dominantCluster = 0;
    for (size_t i = 1; i < numPeaks; ++i) {
      if (_clusterEnergies[i] > _clusterEnergies[dominantCluster]) {
        dominantCluster = i;
      }
    }
    centerBin = _peakBins[dominantCluster];
    signalMs = _clusterEnergies[dominantCluster];
    dominantClusterBins = _clusterCounts[dominantCluster];
  }

  // 3. Parabolic interpolation (log-domain) for sub-bin peak frequency.
  const float lm = log10f(_magSq[centerBin - 1] + kEps);
  const float l0 = log10f(_magSq[centerBin] + kEps);
  const float lp = log10f(_magSq[centerBin + 1] + kEps);
  const float denom = lm - 2.0f * l0 + lp;
  float delta = 0.0f;
  if (denom != 0.0f) {
    delta = std::min(1.0f, std::max(-1.0f, 0.5f * (lm - lp) / denom));
  }
  const float peakHz = (static_cast<float>(centerBin) + delta) * binHz;

  // 4. SNR: dominant cluster / noise floor.
  // Per-bin noise: total energy minus top K energy, divided by count of bins
  // outside of top K.
  // Noise floor: per bin noise multiplied by number of bins in the dominant
  // cluster.
  // Excluding the strongest bins estimates background energy without counting
  // the signal itself. Scaling that estimate by the dominant cluster's width
  // compares signal and noise over the same number of frequency bins.
  //
  // Guard: bandLen == numTopK means the entire band is "top" (only happens
  // when bandEnergy is degenerate), and dominantClusterBins == 0 means no
  // clusters were found. In both cases the metric is undefined; emit NaN so
  // the EMA below short-circuits and downstream readers see "no value".
  float noisePerBin = std::numeric_limits<float>::quiet_NaN();
  float snrDb;
  if (bandLen <= numTopK || dominantClusterBins == 0) {
    snrDb = std::numeric_limits<float>::quiet_NaN();
  } else {
    noisePerBin =
        (bandEnergy - topKEnergy) / static_cast<float>(bandLen - numTopK);
    if (noisePerBin < 0.0f) noisePerBin = 0.0f;
    const float noiseMs = noisePerBin * static_cast<float>(dominantClusterBins);
    snrDb = 10.0f * log10f((signalMs + kEps) / (noiseMs + kEps));
  }

  // 5. Movement prevents transient bumps from raising the smoothed SNR. A
  // non-finite value is skipped because feeding one to the EMA would make every
  // later result non-finite.
  const bool isStationary = flux < STATIONARY_FLUX_MAX;
  if (isStationary && std::isfinite(snrDb)) {
    _smoothedSnr.update(snrDb);
  }

  const uint32_t nowMs = millis();
  const VibrationWindowTrigger::StepResult triggerResult =
      _trigger.stepDetailed(isStationary, _smoothedSnr.value(), peakHz);
  _taskData.triggerFeatures = {_smoothedSnr.value(), peakHz, flux,
                               isStationary};
  _taskData.triggered = triggerResult.active;

  DetectionTransition transition;
  if (triggerResult.event == VibrationWindowTrigger::Event::Opened) {
    _detectedSinceMs = nowMs;
    transition = {nowMs,
                  0,
                  snrDb,
                  _smoothedSnr.value(),
                  peakHz,
                  flux,
                  VibrationWindowTrigger::FailureNone,
                  isStationary,
                  triggerResult.event};
  } else if (triggerResult.event == VibrationWindowTrigger::Event::Closed) {
    transition = {nowMs,
                  nowMs - _detectedSinceMs,
                  snrDb,
                  _smoothedSnr.value(),
                  peakHz,
                  flux,
                  triggerResult.closeFailureMask,
                  isStationary,
                  triggerResult.event};
    _detectedSinceMs = 0;
  }

#if PB_VIBRATION_INSTRUMENTATION
  VibrationSensorInstrumentation::Analysis analysis{};
  analysis.samples = _samples;
  analysis.fftMagnitude = _fftMag;
  analysis.magnitudeSquared = _magSq;
  analysis.peakBins = _peakBins;
  analysis.peakCount = numPeaks;
  analysis.minBin = kMin;
  analysis.maxBin = kMax;
  analysis.binHz = binHz;
  analysis.noisePerBin = noisePerBin;
  analysis.rawSnrDb = snrDb;
  analysis.smoothedSnrDb = _smoothedSnr.value();
  analysis.spectralFlux = flux;
  analysis.bandEnergy = bandEnergy;
  analysis.peakHz = peakHz;
  analysis.stationary = isStationary;
  analysis.nowMs = nowMs;
  _instrumentation.analyze(_taskData, analysis);
#endif
  return transition;
}
