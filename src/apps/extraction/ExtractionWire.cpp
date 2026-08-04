// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "ExtractionWire.h"

#include <cstring>

#include "ble/scale_time.h"
#include "extraction_encoding.h"

namespace pump_scale {

// The fixed header's last field, decisionGainCg (i32), sits at offset 50, i.e.
// bytes [50, 54). encode and decode both index hdr[+50] inside a
// hdr[kCompactHeaderBytes] buffer, so the constant must cover that field or
// both sides would run past the buffer. Catch any future shrink at compile
// time.
static_assert(kCompactHeaderBytes >= 54,
              "header layout exceeds kCompactHeaderBytes");

namespace {

void writeTargetBlock(uint8_t* dst, const Extraction& e) {
  writeU16LE(dst + 0, e.target.targetCg);
  dst[2] = e.target.armed ? 1 : 0;
  writeU16LE(dst + 3, e.target.tauMs);
  writeI16LE(dst + 5, e.target.cCg);
  writeU16LE(dst + 7, e.target.reactionLeadMs);
  writeU32LE(dst + 9, e.alarm.tMs);
  writeI16LE(dst + 13, e.alarm.ctx.yieldCg);
  writeI16LE(dst + 15, e.alarm.ctx.flowCgPerS);
  dst[17] = e.alarm.ctx.flowValid ? 1 : 0;
  writeI16LE(dst + 18, e.alarm.ctx.projectedFinalCg);
}

bool readTargetBlock(const WireSource& src, Extraction& out) {
  uint8_t buf[kCompactTargetBlockBytes];
  if (!src(buf, sizeof(buf))) return false;
  out.target.targetCg = readU16LE(buf + 0);
  out.target.armed = buf[2] != 0;
  out.target.tauMs = readU16LE(buf + 3);
  out.target.cCg = readI16LE(buf + 5);
  out.target.reactionLeadMs = readU16LE(buf + 7);
  out.alarm.tMs = readU32LE(buf + 9);
  out.alarm.ctx.yieldCg = readI16LE(buf + 13);
  out.alarm.ctx.flowCgPerS = readI16LE(buf + 15);
  out.alarm.ctx.flowValid = buf[17] != 0;
  out.alarm.ctx.projectedFinalCg = readI16LE(buf + 18);
  return true;
}

}  // namespace

namespace {

constexpr uint8_t FLAG_EVENTS_OVERFLOWED = 0x01;
constexpr uint8_t FLAG_SCALE_TIMERS = 0x02;

// Chunked writer: collects bytes in a stack buffer, flushes to the sink
// when full. The encoder calls put() for raw bytes (header) and varint
// bytes (events/samples).
class ChunkedWriter {
 public:
  explicit ChunkedWriter(const WireSink& sink) : _sink(sink) {}
  ~ChunkedWriter() { flush(); }

  void put(const uint8_t* data, size_t len) {
    while (len) {
      size_t avail = sizeof(_buf) - _n;
      if (avail == 0) {
        flush();
        avail = sizeof(_buf);
      }
      const size_t take = len < avail ? len : avail;
      std::memcpy(_buf + _n, data, take);
      _n += take;
      data += take;
      len -= take;
    }
  }

  void flush() {
    if (_n) {
      _sink(_buf, _n);
      _n = 0;
    }
  }

 private:
  const WireSink& _sink;
  uint8_t _buf[256];
  size_t _n = 0;
};

uint8_t flagsByte(const Extraction& e) {
  uint8_t f = 0;
  if (e.eventsOverflowed) f |= FLAG_EVENTS_OVERFLOWED;
  if (sampleRangeHasScaleTimer(e.samples, e.sampleCount)) {
    f |= FLAG_SCALE_TIMERS;
  }
  return f;
}

}  // namespace

size_t encodeCompactSize(const Extraction& ext) {
  size_t total = kCompactHeaderBytes;

  // Events: uvarint tMsDelta + 1 byte kind.
  uint32_t prevT = ext.beginMs;
  for (uint16_t i = 0; i < ext.eventCount; ++i) {
    const uint32_t delta = ext.events[i].tMs - prevT;
    total += uvarintSize(delta) + 1;
    prevT = ext.events[i].tMs;
  }

  // Samples: uvarint tMsDelta + svarint cgDelta, plus raw
  // uvarint scaleTimerMs iff the record-level flag says timer fields exist.
  const bool includeScaleTimers =
      sampleRangeHasScaleTimer(ext.samples, ext.sampleCount);
  SampleDeltaCursor sampleCursor{ext.beginMs, 0, includeScaleTimers};
  for (uint16_t i = 0; i < ext.sampleCount; ++i) {
    total += sampleDeltaEncodedSize(sampleCursor, ext.samples[i]);
  }

  // v6 trailing target/alarm block.
  total += kCompactTargetBlockBytes;

  return total;
}

void encodeCompact(const Extraction& ext, const WireSink& sink) {
  ChunkedWriter w(sink);
  uint8_t hdr[kCompactHeaderBytes];

  hdr[0] = 'E';
  hdr[1] = 'X';
  hdr[2] = 'T';
  hdr[3] = 'R';
  hdr[4] = kCompactVersion;
  hdr[5] = static_cast<uint8_t>(ext.phase);
  hdr[6] = static_cast<uint8_t>(ext.endCause);
  const uint8_t flags = flagsByte(ext);
  hdr[7] = flags;
  writeU32LE(hdr + 8, ext.beginMs);
  writeU32LE(hdr + 12, ext.lastPumpOffMs);
  writeU32LE(hdr + 16, ext.stableMs);
  writeU32LE(hdr + 20, ext.endMs);
  writeU32LE(hdr + 24, ext.totalPumpOnMs);
  writeU32LE(hdr + 28, ext.startUtcSec);
  writeI16LE(hdr + 32, ext.yieldCg);
  writeI16LE(hdr + 34, ext.startRawCg);
  writeI16LE(hdr + 36, ext.settledRawCg);
  writeU16LE(hdr + 38, ext.observedSampleCount);
  writeU16LE(hdr + 40, ext.eventCount);
  writeU16LE(hdr + 42, ext.sampleCount);
  hdr[44] = static_cast<uint8_t>(ext.yieldStatus);
  hdr[45] = 0;  // reserved
  writeU32LE(hdr + 46, ext.pourMs);
  writeI32LE(hdr + 50, ext.decisionGainCg);
  w.put(hdr, sizeof(hdr));

  uint8_t scratch[kMaxSampleDeltaBytes];

  // Events.
  uint32_t prevT = ext.beginMs;
  for (uint16_t i = 0; i < ext.eventCount; ++i) {
    size_t n = writeUvarint(scratch, ext.events[i].tMs - prevT);
    scratch[n++] = static_cast<uint8_t>(ext.events[i].kind);
    w.put(scratch, n);
    prevT = ext.events[i].tMs;
  }

  // Samples.
  const bool includeScaleTimers = (flags & FLAG_SCALE_TIMERS) != 0;
  SampleDeltaCursor sampleCursor{ext.beginMs, 0, includeScaleTimers};
  for (uint16_t i = 0; i < ext.sampleCount; ++i) {
    const size_t n = writeSampleDelta(scratch, sampleCursor, ext.samples[i]);
    w.put(scratch, n);
  }

  // v6 trailing target/alarm block.
  uint8_t targetBlock[kCompactTargetBlockBytes];
  writeTargetBlock(targetBlock, ext);
  w.put(targetBlock, sizeof(targetBlock));

  // ChunkedWriter dtor flushes the tail.
}

namespace {

// Read one LEB128 uvarint from the pull source. Caps at 5 bytes — a u32 can
// never need more — so a corrupt stream of continuation bytes can't loop.
bool readUvarintSrc(const WireSource& src, uint32_t& out) {
  uint32_t v = 0;
  for (int i = 0; i < 5; ++i) {
    uint8_t b;
    if (!src(&b, 1)) return false;
    v |= static_cast<uint32_t>(b & 0x7F) << (7 * i);
    if ((b & 0x80) == 0) {
      out = v;
      return true;
    }
  }
  return false;
}

bool readSvarintSrc(const WireSource& src, int32_t& out) {
  uint32_t u;
  if (!readUvarintSrc(src, u)) return false;
  out = unzigzag(u);
  return true;
}

}  // namespace

bool decodeCompact(const WireSource& src, Extraction& out) {
  out = Extraction{};

  // The header is a fixed 54 bytes (kCompactHeaderBytes), the same layout
  // encodeCompact() writes. Pull it all at once; the variable-length event and
  // sample streams follow.
  uint8_t hdr[kCompactHeaderBytes];
  if (!src(hdr, sizeof(hdr))) return false;
  if (hdr[0] != 'E' || hdr[1] != 'X' || hdr[2] != 'T' || hdr[3] != 'R') {
    return false;
  }
  const uint8_t version = hdr[4];
  if (version != kCompactVersion) return false;
  if (hdr[6] > static_cast<uint8_t>(EndCause::TIMEOUT)) return false;

  out.phase = static_cast<Phase>(hdr[5]);
  out.endCause = static_cast<EndCause>(hdr[6]);
  const uint8_t flags = hdr[7];
  out.eventsOverflowed = (flags & FLAG_EVENTS_OVERFLOWED) != 0;
  out.beginMs = readU32LE(hdr + 8);
  out.lastPumpOffMs = readU32LE(hdr + 12);
  out.stableMs = readU32LE(hdr + 16);
  out.endMs = readU32LE(hdr + 20);
  out.totalPumpOnMs = readU32LE(hdr + 24);
  out.startUtcSec = readU32LE(hdr + 28);
  out.yieldCg = readI16LE(hdr + 32);
  out.startRawCg = readI16LE(hdr + 34);
  out.settledRawCg = readI16LE(hdr + 36);
  out.observedSampleCount = readU16LE(hdr + 38);
  const uint16_t eventCount = readU16LE(hdr + 40);
  const uint16_t sampleCount = readU16LE(hdr + 42);
  out.yieldStatus = static_cast<YieldStatus>(hdr[44]);
  // hdr[45] reserved
  out.pourMs = readU32LE(hdr + 46);
  out.decisionGainCg = readI32LE(hdr + 50);

  // A corrupt or forged count must never overrun the fixed arrays.
  if (eventCount > Extraction::MAX_EVENTS) return false;
  if (sampleCount > Extraction::MAX_SAMPLES) return false;

  // Events: uvarint tMsDelta (first from beginMs) + u8 kind.
  uint32_t prevT = out.beginMs;
  for (uint16_t i = 0; i < eventCount; ++i) {
    uint32_t dt;
    if (!readUvarintSrc(src, dt)) return false;
    prevT += dt;
    uint8_t kind;
    if (!src(&kind, 1)) return false;
    out.events[i] = {prevT, static_cast<EventKind>(kind)};
  }
  out.eventCount = eventCount;

  // Samples: uvarint tMsDelta + svarint cgDelta (raw scale centigrams), then a
  // raw uvarint scaleTimerMs when the scale-timer flag is set.
  const bool haveScaleTimers = (flags & FLAG_SCALE_TIMERS) != 0;
  uint32_t sPrevT = out.beginMs;
  int32_t prevCg = 0;
  for (uint16_t i = 0; i < sampleCount; ++i) {
    uint32_t dt;
    int32_t dc;
    if (!readUvarintSrc(src, dt) || !readSvarintSrc(src, dc)) return false;
    sPrevT += dt;
    prevCg += dc;
    uint32_t scaleTimerMs = scale_time::UNKNOWN_MS;
    if (haveScaleTimers && !readUvarintSrc(src, scaleTimerMs)) return false;
    out.samples[i] = {sPrevT, saturateCg(prevCg), scaleTimerMs};
  }
  out.sampleCount = sampleCount;

  if (!readTargetBlock(src, out)) return false;

  // Current-version records always include the target/alarm block. In-flight
  // records remain false until setTargetSnapshot() captures their settings.
  out.hasTargetSnapshot = true;

  return true;
}

}  // namespace pump_scale
