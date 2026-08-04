// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>
#include <esp_heap_caps.h>

#include <cassert>
#include <cstddef>

namespace ui {

// Allocate `bytes` of pixel memory for a long-lived sprite. Prefer PSRAM so
// permanent UI buffers don't occupy the internal DMA-capable heap that Wi-Fi /
// LWIP need for packet buffers. Internal DMA remains the fallback: faster for
// display pushes, but too scarce to reserve for full-screen UI buffers unless
// PSRAM is unavailable. Returns nullptr only if both pools are exhausted.
inline void* allocPixelBuffer(size_t bytes, const char* tag) {
  void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) return p;

  p = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL |
                                  MALLOC_CAP_8BIT);
  if (p) {
    M5_LOGW("%s: %u B PSRAM alloc failed, fell back to internal DMA", tag,
            (unsigned)bytes);
    return p;
  }
  M5_LOGE("%s: %u B pixel buffer alloc failed (PSRAM + internal DMA)", tag,
          (unsigned)bytes);
  return p;
}

// A sprite whose 16bpp backing buffer is allocated exactly once, for the
// largest footprint it will ever take. It is then only re-pointed (never
// reallocated) to dimensions that fit within that buffer — e.g. the transpose
// on an orientation flip, or a smaller sub-layout — so it never takes part in
// the heap churn / fragmentation that a per-resize createSprite/deleteSprite
// causes.
//
// Lifecycle: allocate() once at boot (while SRAM is unfragmented), then shape()
// whenever the dimensions change. Pass it where an LGFX_Sprite* is expected
// (implicit conversion) and reach the sprite's methods through it with ->.
class PersistentSprite {
 public:
  PersistentSprite() = default;
  ~PersistentSprite() {
    if (_buf) heap_caps_free(_buf);
  }
  PersistentSprite(const PersistentSprite&) = delete;
  PersistentSprite& operator=(const PersistentSprite&) = delete;

  // Reserve the backing buffer once, sized for any layout up to capW × capH.
  // Call at boot. Calling again is a no-op (the buffer is permanent) — and a
  // bug, so it also asserts. Returns true if a buffer is held; false only if
  // both SRAM and PSRAM are exhausted, which leaves shape() unusable.
  bool allocate(int capW, int capH) {
    const size_t required_capacity = (size_t)capW * capH * 2;
    if (_buf) {
      M5_LOGE("PersistentSprite::allocate called twice");
      return required_capacity == _capacity;
    }
    _capacity = required_capacity;
    _buf = allocPixelBuffer(_capacity + 4, "PersistentSprite");
    return _buf != nullptr;
  }

  bool allocated() const { return _buf != nullptr; }

  // Present the buffer at w × h — a pure re-point (setBuffer), never a realloc;
  // w*h must fit the reserved capacity. Re-points only when the shape actually
  // changes (and clears it, since setBuffer leaves the buffer dirty), so it's
  // cheap to call every frame. Returns the sprite (bufferless if allocate()
  // failed; the caller can check getBuffer()).
  LGFX_Sprite& shape(int w, int h) {
    assert(_buf != nullptr && "PersistentSprite::shape before allocate");
    assert((size_t)w * h * 2 <= _capacity &&
           "PersistentSprite shape too large");
    if (_buf && (!_sprite.getBuffer() || _sprite.width() != w ||
                 _sprite.height() != h)) {
      // 16bpp; the buffer is marked Preallocated, so LGFX never frees it.
      _sprite.setBuffer(_buf, w, h, 16);
      _sprite.fillScreen(0);
    }
    return _sprite;
  }

  LGFX_Sprite& sprite() { return _sprite; }
  LGFX_Sprite* operator->() { return &_sprite; }
  operator LGFX_Sprite*() { return &_sprite; }

 private:
  LGFX_Sprite _sprite{&M5.Display};
  void* _buf = nullptr;
  size_t _capacity = 0;
};

}  // namespace ui
