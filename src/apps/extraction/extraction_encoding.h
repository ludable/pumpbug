// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

#include "Extraction.h"

namespace pump_scale {

inline constexpr size_t kMaxSampleDeltaBytes = 16;

size_t uvarintSize(uint32_t v);
uint32_t zigzag(int32_t v);
int32_t unzigzag(uint32_t v);

size_t writeU32LE(uint8_t* dst, uint32_t v);
size_t writeI32LE(uint8_t* dst, int32_t v);
size_t writeU16LE(uint8_t* dst, uint16_t v);
size_t writeI16LE(uint8_t* dst, int16_t v);
size_t writeUvarint(uint8_t* dst, uint32_t v);
size_t writeSvarint(uint8_t* dst, int32_t v);

// Inverse fixed-width reads, for parsing the compact header block.
uint32_t readU32LE(const uint8_t* src);
int32_t readI32LE(const uint8_t* src);
uint16_t readU16LE(const uint8_t* src);
int16_t readI16LE(const uint8_t* src);

struct SampleDeltaCursor {
  uint32_t prevTMs;
  int32_t prevCg;
  bool includeScaleTimers;
};

// Both helpers advance `cursor`; construct a fresh cursor for each size/write
// pass using the same anchor values.
size_t sampleDeltaEncodedSize(SampleDeltaCursor& cursor, const Sample& sample);
size_t writeSampleDelta(uint8_t* dst, SampleDeltaCursor& cursor,
                        const Sample& sample);

}  // namespace pump_scale
