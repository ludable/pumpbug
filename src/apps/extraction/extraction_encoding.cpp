// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "extraction_encoding.h"

namespace pump_scale {

size_t uvarintSize(uint32_t v) {
  size_t n = 1;
  while (v >= 0x80) {
    v >>= 7;
    ++n;
  }
  return n;
}

uint32_t zigzag(int32_t v) {
  const uint32_t u = static_cast<uint32_t>(v);
  return (u << 1) ^ (v < 0 ? 0xFFFFFFFFu : 0u);
}

int32_t unzigzag(uint32_t v) {
  return static_cast<int32_t>((v >> 1) ^ (0u - (v & 1u)));
}

uint32_t readU32LE(const uint8_t* src) {
  return static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) |
         (static_cast<uint32_t>(src[2]) << 16) |
         (static_cast<uint32_t>(src[3]) << 24);
}

int32_t readI32LE(const uint8_t* src) {
  return static_cast<int32_t>(readU32LE(src));
}

uint16_t readU16LE(const uint8_t* src) {
  return static_cast<uint16_t>(src[0] | (src[1] << 8));
}

int16_t readI16LE(const uint8_t* src) {
  return static_cast<int16_t>(readU16LE(src));
}

size_t writeU32LE(uint8_t* dst, uint32_t v) {
  dst[0] = static_cast<uint8_t>(v);
  dst[1] = static_cast<uint8_t>(v >> 8);
  dst[2] = static_cast<uint8_t>(v >> 16);
  dst[3] = static_cast<uint8_t>(v >> 24);
  return 4;
}

size_t writeI32LE(uint8_t* dst, int32_t v) {
  return writeU32LE(dst, static_cast<uint32_t>(v));
}

size_t writeU16LE(uint8_t* dst, uint16_t v) {
  dst[0] = static_cast<uint8_t>(v);
  dst[1] = static_cast<uint8_t>(v >> 8);
  return 2;
}

size_t writeI16LE(uint8_t* dst, int16_t v) {
  const uint16_t u = static_cast<uint16_t>(v);
  dst[0] = static_cast<uint8_t>(u);
  dst[1] = static_cast<uint8_t>(u >> 8);
  return 2;
}

size_t writeUvarint(uint8_t* dst, uint32_t v) {
  size_t i = 0;
  while (v >= 0x80) {
    dst[i++] = static_cast<uint8_t>(v) | 0x80;
    v >>= 7;
  }
  dst[i++] = static_cast<uint8_t>(v);
  return i;
}

size_t writeSvarint(uint8_t* dst, int32_t v) {
  return writeUvarint(dst, zigzag(v));
}

size_t sampleDeltaEncodedSize(SampleDeltaCursor& cursor, const Sample& sample) {
  const uint32_t dt = sample.tMs - cursor.prevTMs;
  const int32_t dc = static_cast<int32_t>(sample.cg) - cursor.prevCg;
  size_t n = uvarintSize(dt) + uvarintSize(zigzag(dc));
  if (cursor.includeScaleTimers) {
    n += uvarintSize(sample.scaleTimerMs);
  }
  cursor.prevTMs = sample.tMs;
  cursor.prevCg = sample.cg;
  return n;
}

size_t writeSampleDelta(uint8_t* dst, SampleDeltaCursor& cursor,
                        const Sample& sample) {
  const uint32_t dt = sample.tMs - cursor.prevTMs;
  const int32_t dc = static_cast<int32_t>(sample.cg) - cursor.prevCg;
  size_t n = writeUvarint(dst, dt);
  n += writeSvarint(dst + n, dc);
  if (cursor.includeScaleTimers) {
    n += writeUvarint(dst + n, sample.scaleTimerMs);
  }
  cursor.prevTMs = sample.tMs;
  cursor.prevCg = sample.cg;
  return n;
}

}  // namespace pump_scale
