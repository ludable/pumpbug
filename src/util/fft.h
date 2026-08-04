// SPDX-FileCopyrightText: 2021 M5Stack
// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: MIT AND AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

enum class FftWindow {
  Rectangular,  // no window
  Hann,         // good default for narrow-peak detection
  Hamming,
};

// Adapted from the Mic_FFT sample.
// Real-input radix-2 FFT engine.
// Size is fixed by begin(bits); call end() before changing it.
// Input is float; output is magnitude (or magnitude squared) per bin.
//
// Output is calibrated for peak amplitude of single tones: a unit-amplitude
// sinusoid `cos(2*pi*f*t)` produces `mag[k] == 1.0` at its bin (window/length
// gain divided out, single-sided ×2 applied). DC (bin 0) and Nyquist read
// 2× their true amplitude because the single-sided correction does not apply
// there — those bins are typically not of interest.
class FftEngine {
 public:
  FftEngine() = default;
  ~FftEngine() { end(); }

  FftEngine(const FftEngine&) = delete;
  FftEngine& operator=(const FftEngine&) = delete;

  // Initialize for FFT size = (1 << bits). bits in [2, 14].
  // Returns false on bad arg or allocation failure.
  bool begin(uint8_t bits, FftWindow window = FftWindow::Hann);
  void end();

  // Forward FFT of `samples` (length size()).
  // Writes |X[k]|     to `mag`     [k = 0 .. bins()-1].
  // Writes |X[k]|^2   to `mag_sq`  [k = 0 .. bins()-1].
  // `samples` and the output array may not overlap.
  void compute(const float* samples, float* mag);
  void computeMagSq(const float* samples, float* mag_sq);

  bool isReady() const { return _bits != 0; }
  uint8_t bits() const { return _bits; }
  size_t size() const { return _bits ? (1u << _bits) : 0; }
  size_t bins() const { return size() >> 1; }  // useful real-input bins

  // Frequency / bin helpers (sample_rate in Hz).
  float binWidthHz(float sample_rate) const {
    return sample_rate / static_cast<float>(size());
  }
  float freqOfBin(size_t k, float sample_rate) const {
    return k * binWidthHz(sample_rate);
  }
  size_t binOfFreq(float freq, float sample_rate) const;

 private:
  void _initTables(FftWindow window);
  void _butterflies();  // in-place on _fr, _fi
  static void* _alloc(size_t n);
  static void _free(void* p);

  uint8_t _bits = 0;
  float _outScale = 1.0f;   // peak-amplitude calibration: 2 / (N * coherentGain)
  float* _fr = nullptr;     // real,        length N
  float* _fi = nullptr;     // imag,        length N
  float* _wnd = nullptr;    // window,      length N (nullptr if rectangular)
  uint16_t* _br = nullptr;  // bit-reverse, length N
  float* _w = nullptr;      // twiddle,     length 3N/4
};
