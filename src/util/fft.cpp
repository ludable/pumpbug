// SPDX-FileCopyrightText: 2021 M5Stack
// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: MIT AND AGPL-3.0-only

#include <cmath>
#include <cstring>

#include "fft.h"

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
#include "esp_heap_caps.h"
void* FftEngine::_alloc(size_t n) {
  return heap_caps_malloc(n, MALLOC_CAP_8BIT);
}
void FftEngine::_free(void* p) {
  if (p) heap_caps_free(p);
}
#else
#include <cstdlib>
void* FftEngine::_alloc(size_t n) { return std::malloc(n); }
void FftEngine::_free(void* p) {
  if (p) std::free(p);
}
#endif

bool FftEngine::begin(uint8_t bits, FftWindow window) {
  end();
  if (bits < 2 || bits > 14) return false;

  const size_t N = 1u << bits;
  _fr = static_cast<float*>(_alloc(N * sizeof(float)));
  _fi = static_cast<float*>(_alloc(N * sizeof(float)));
  _br = static_cast<uint16_t*>(_alloc(N * sizeof(uint16_t)));
  _w = static_cast<float*>(_alloc((N * 3 / 4) * sizeof(float)));
  if (!_fr || !_fi || !_br || !_w) {
    end();
    return false;
  }

  if (window != FftWindow::Rectangular) {
    _wnd = static_cast<float*>(_alloc(N * sizeof(float)));
    if (!_wnd) {
      end();
      return false;
    }
  }

  _bits = bits;
  _initTables(window);
  return true;
}

void FftEngine::end() {
  _free(_fr);
  _fr = nullptr;
  _free(_fi);
  _fi = nullptr;
  _free(_br);
  _br = nullptr;
  _free(_w);
  _w = nullptr;
  _free(_wnd);
  _wnd = nullptr;
  _bits = 0;
  _outScale = 1.0f;
}

void FftEngine::_initTables(FftWindow window) {
  const size_t N = 1u << _bits;

  // ---- Bit-reversal index table ----
  size_t je = 1;
  _br[0] = 0;
  _br[1] = static_cast<uint16_t>(N >> 1);
  for (size_t i = 0; i < static_cast<size_t>(_bits) - 1; ++i) {
    _br[je << 1] = _br[je] >> 1;
    je <<= 1;
    for (size_t j = 1; j < je; ++j) {
      _br[je + j] = _br[je] + _br[j];
    }
  }

  // ---- Twiddle (cosine) table, indexed as [sin | cos] via N/4 offset ----
  const float omega = 2.0f * static_cast<float>(M_PI) / static_cast<float>(N);
  const size_t s2 = N >> 1;
  const size_t s4 = N >> 2;
  _w[0] = 0.0f;
  _w[s2] = 0.0f;
  _w[s4] = 1.0f;
  for (size_t i = 1; i < s4; ++i) {
    const float f = cosf(omega * i);
    _w[s4 + i] = f;
    _w[s4 - i] = f;
    _w[s4 + s2 - i] = -f;
  }

  // ---- Window table ----
  if (_wnd) {
    const float denom = static_cast<float>(N - 1);
    switch (window) {
      case FftWindow::Hann:
        for (size_t i = 0; i < N; ++i)
          _wnd[i] =
              0.5f * (1.0f - cosf(2.0f * static_cast<float>(M_PI) * i / denom));
        break;
      case FftWindow::Hamming:
        for (size_t i = 0; i < N; ++i)
          _wnd[i] =
              0.54f - 0.46f * cosf(2.0f * static_cast<float>(M_PI) * i / denom);
        break;
      default:
        break;
    }
  }

  // ---- Output amplitude calibration ----
  // _outScale = 2 / (N * coherentGain), where coherentGain is the mean window
  // value (1.0 for rectangular). After scaling, a unit-amplitude sinusoid
  // produces |X[k]| == 1.0 at its bin.
  float coherentGain = 1.0f;
  if (_wnd) {
    float sum = 0.0f;
    for (size_t i = 0; i < N; ++i) sum += _wnd[i];
    coherentGain = sum / static_cast<float>(N);
  }
  _outScale = 2.0f / (static_cast<float>(N) * coherentGain);
}

__attribute__((optimize("-O3"))) void FftEngine::_butterflies() {
  const size_t N = 1u << _bits;
  const size_t s4 = N >> 2;
  float* fr = _fr;
  float* fi = _fi;
  const float* w_tab = _w;

  size_t s = 1;
  size_t i = 0;
  size_t je = N;
  do {
    const size_t ke = s;
    s <<= 1;
    je >>= 1;
    size_t j = 0;
    do {
      size_t k = 0;
      const size_t m = ke * ((j << 1) + 1);
      const size_t l = s * j;
      auto frm_p = &fr[m];
      auto fim_p = &fi[m];
      auto frl_p = &fr[l];
      auto fil_p = &fi[l];
      auto wi_p = &w_tab[0];
      auto wr_p = &w_tab[s4];
      do {
        const float wi = *wi_p;
        const float wr = *wr_p;
        const float frm = *frm_p;
        const float fim = *fim_p;
        const float Wxmr = frm * wr + fim * wi;
        const float Wxmi = fim * wr - frm * wi;
        const float frl = *frl_p;
        const float fil = *fil_p;
        *frm_p++ = frl - Wxmr;
        *frl_p++ = frl + Wxmr;
        *fim_p++ = fil - Wxmi;
        *fil_p++ = fil + Wxmi;
        wi_p += je;
        wr_p += je;
      } while (++k < ke);
    } while (++j < je);
  } while (++i < _bits);
}

void FftEngine::computeMagSq(const float* samples, float* mag_sq) {
  if (!_bits) return;
  const size_t N = 1u << _bits;

  // Load input through (optional) window into bit-reversed positions; zero
  // imag.
  if (_wnd) {
    for (size_t n = 0; n < N; ++n) _fr[_br[n]] = samples[n] * _wnd[n];
  } else {
    for (size_t n = 0; n < N; ++n) _fr[_br[n]] = samples[n];
  }
  std::memset(_fi, 0, N * sizeof(float));

  _butterflies();

  const float k2 = _outScale * _outScale;
  const size_t half = N >> 1;
  for (size_t k = 0; k < half; ++k) {
    const float re = _fr[k];
    const float im = _fi[k];
    mag_sq[k] = (re * re + im * im) * k2;
  }
}

void FftEngine::compute(const float* samples, float* mag) {
  if (!_bits) return;
  computeMagSq(samples, mag);
  const size_t half = (1u << _bits) >> 1;
  for (size_t k = 0; k < half; ++k) mag[k] = sqrtf(mag[k]);
}

size_t FftEngine::binOfFreq(float freq, float sample_rate) const {
  if (!_bits) return 0;
  const size_t half = (1u << _bits) >> 1;
  long k = lroundf(freq * static_cast<float>(1u << _bits) / sample_rate);
  if (k < 0) k = 0;
  if (k > static_cast<long>(half) - 1) k = static_cast<long>(half) - 1;
  return static_cast<size_t>(k);
}
