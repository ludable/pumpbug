// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "Extraction.h"

// Compact binary serialization for pump_scale::Extraction.
//
// The HTTP API and LittleFS history use the same format and encoder. The
// browser and firmware decoders follow the layout below.
//
// Byte layout (little-endian throughout):
//
//   offset  size  field
//   ------  ----  -----
//        0     4  magic "EXTR"
//        4     1  version (= 6)
//        5     1  phase (Phase enum, u8)
//        6     1  endCause (EndCause enum, u8)
//        7     1  flags
//                   bit 0  eventsOverflowed
//                   bit 1  sample scale-timer fields present
//        8     4  beginMs              (u32 LE, reference for offsets below;
//                                       also the first PUMP_ON — shots begin
//                                       on pump)
//       12     4  lastPumpOffMs
//       16     4  stableMs             (0 = unset)
//       20     4  endMs                (0 = not yet DONE)
//       24     4  totalPumpOnMs        (interval, not millis)
//       28     4  startUtcSec          (Unix epoch seconds, 0 = unknown)
//       32     2  yieldCg              (i16, INT16_MIN = NO_WEIGHT)
//       34     2  startRawCg           (i16, INT16_MIN = NO_WEIGHT)
//       36     2  settledRawCg         (i16, INT16_MIN = NO_WEIGHT)
//       38     2  observedSampleCount
//       40     2  eventCount
//       42     2  sampleCount
//       44     1  yieldStatus          (YieldStatus enum, u8)
//       45     1  reserved             (must be 0)
//       46     4  pourMs               (u32 LE)
//       50     4  decisionGainCg       (i32 LE)
//       54        end of fixed header
//
// Trailing block (after the event and sample streams):
//   offset  size  field
//   ------  ----  -----
//        0     2  targetCg             (u16 LE, 0 = no target)
//        2     1  targetArmed          (0/1)
//        3     2  tauMs                (u16 LE)
//        5     2  cCg                  (i16 LE)
//        7     2  reactionLeadMs       (u16 LE)
//        9     4  alarmTriggeredMs     (u32 LE, 0 = never)
//       13     2  alarmYieldCg         (i16 LE)
//       15     2  alarmFlowCgPerS      (i16 LE)
//       17     1  alarmFlowValid       (0/1)
//       18     2  alarmProjectedFinalCg (i16 LE)
//       20        end of trailing block
// Samples are RAW scale centigrams (not baseline-relative yield). Renderers
// subtract startRawCg when they want to display yield. decisionGainCg and
// pourMs are the graduation evidence. yieldCg is normally the user-facing
// endpoint span. DISTURBED marks replacement of an unusable final endpoint.
//
// Events (eventCount records, LEB128 stream — read sequentially):
//   uvarint  tMsDelta   from previous event's tMs (first delta from beginMs)
//   u8       kind       EventKind enum
//
// Samples (sampleCount records, LEB128 stream):
//   uvarint  tMsDelta   from previous sample's tMs (first delta from beginMs)
//   svarint  cgDelta    zigzag-encoded raw scale centigrams; first sample's
//                       "delta" is from 0 (i.e. absolute cg).
//   uvarint  scaleTimerMs
//                       present only when flags bit 1 is set. Raw millisecond
//                       value, or scale_time::UNKNOWN_MS.
//
// uvarint = unsigned LEB128: 7 data bits per byte, MSB=1 continues.
// svarint = uvarint applied to zigzag(value):
//   zz(n) = (uint32(n) << 1) ^ (n < 0 ? 0xffffffff : 0)
//
// Decoder maintains running uint32 tMs and cg accumulators; sample N's wire
// tMs is beginMs + sum(deltas[0..N]) modulo 2^32, absolute cg is
// sum(cgDeltas[0..N]). Renderers that need monotonic coordinates should unwrap
// decoded tMs values around beginMs.

namespace pump_scale {

inline constexpr size_t kCompactHeaderBytes = 54;
inline constexpr size_t kCompactTargetBlockBytes = 20;
inline constexpr size_t kMaxVarint32Bytes = 5;
inline constexpr size_t kMaxWeightDeltaVarintBytes = 3;
inline constexpr size_t kMaxCompactRecordBytes =
    kCompactHeaderBytes +
    Extraction::MAX_EVENTS * (kMaxVarint32Bytes + sizeof(uint8_t)) +
    Extraction::MAX_SAMPLES *
        (2 * kMaxVarint32Bytes + kMaxWeightDeltaVarintBytes) +
    kCompactTargetBlockBytes;
// On-device readers accept exactly this version.
inline constexpr uint8_t kCompactVersion = 6;

// Sink receives encoded bytes in chunks. Returns nothing — the encoder
// trusts the sink not to throw. WebServer wrapper calls sendContent; a
// disk writer would call File::write.
using WireSink = std::function<void(const uint8_t* data, size_t len)>;

// Pre-pass: number of bytes encodeCompact() will emit for `ext`. Useful
// for setting Content-Length before streaming.
size_t encodeCompactSize(const Extraction& ext);

// Stream-encode `ext` to `sink`. Internally buffers into chunks of a few
// hundred bytes so the sink isn't called per varint.
void encodeCompact(const Extraction& ext, const WireSink& sink);

// Pull source for decode: copies exactly `len` bytes into `dst`, returning
// false if fewer are available (a truncated or corrupt blob), which aborts the
// decode. The mirror of WireSink — a memory cursor in tests, a File reader on
// device — so decoding never has to buffer the whole blob in RAM.
using WireSource = std::function<bool(uint8_t* dst, size_t len)>;

// Decode a compact blob back into `out`, the inverse of encodeCompact.
// Returns false on a bad magic/version/end cause, an event/sample count that
// would overrun Extraction's fixed arrays, or a truncated stream; `out` is
// left partially filled on failure, so discard it.
bool decodeCompact(const WireSource& src, Extraction& out);

}  // namespace pump_scale
