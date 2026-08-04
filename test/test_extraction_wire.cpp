// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// Host-compilable unit tests for the compact Extraction wire encoder (v6).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "ExtractionWire.h"
#include "ble/scale_time.h"

namespace {

int g_failures = 0;

static_assert(pump_scale::kMaxCompactRecordBytes == 10'442);

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

std::vector<uint8_t> encode(const pump_scale::Extraction& e) {
  std::vector<uint8_t> out;
  pump_scale::encodeCompact(e, [&](const uint8_t* data, size_t len) {
    out.insert(out.end(), data, data + len);
  });
  CHECK(out.size() == pump_scale::encodeCompactSize(e));
  return out;
}

uint32_t readUvarint(const std::vector<uint8_t>& data, size_t& p) {
  uint32_t v = 0;
  uint8_t shift = 0;
  while (p < data.size()) {
    const uint8_t b = data[p++];
    v |= static_cast<uint32_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) return v;
    shift += 7;
  }
  CHECK(false && "unterminated uvarint");
  return 0;
}

int32_t readSvarint(const std::vector<uint8_t>& data, size_t& p) {
  const uint32_t u = readUvarint(data, p);
  return static_cast<int32_t>((u >> 1) ^ (0u - (u & 1u)));
}

bool decode(const std::vector<uint8_t>& bytes, pump_scale::Extraction& out) {
  size_t off = 0;
  return pump_scale::decodeCompact(
      [&](uint8_t* dst, size_t len) {
        if (off + len > bytes.size()) return false;
        std::memcpy(dst, bytes.data() + off, len);
        off += len;
        return true;
      },
      out);
}

// Field-by-field equality over the serialized surface. `hasTargetSnapshot` is
// intentionally excluded: it is an in-RAM marker set by setTargetSnapshot() on
// live records and by decodeCompact() for v6+ records, so a makeShot()/encode/
// decode round-trip naturally produces false on the source and true on the
// decoded copy. It is checked explicitly in the round-trip tests.
bool sameSerialized(const pump_scale::Extraction& a,
                    const pump_scale::Extraction& b) {
  if (a.phase != b.phase || a.endCause != b.endCause ||
      a.eventsOverflowed != b.eventsOverflowed || a.beginMs != b.beginMs ||
      a.lastPumpOffMs != b.lastPumpOffMs || a.stableMs != b.stableMs ||
      a.endMs != b.endMs || a.totalPumpOnMs != b.totalPumpOnMs ||
      a.startUtcSec != b.startUtcSec || a.yieldStatus != b.yieldStatus ||
      a.yieldCg != b.yieldCg || a.startRawCg != b.startRawCg ||
      a.settledRawCg != b.settledRawCg || a.pourMs != b.pourMs ||
      a.decisionGainCg != b.decisionGainCg ||
      a.observedSampleCount != b.observedSampleCount ||
      a.eventCount != b.eventCount || a.sampleCount != b.sampleCount ||
      a.target.targetCg != b.target.targetCg ||
      a.target.armed != b.target.armed || a.target.tauMs != b.target.tauMs ||
      a.target.cCg != b.target.cCg ||
      a.target.reactionLeadMs != b.target.reactionLeadMs ||
      a.alarm.tMs != b.alarm.tMs ||
      a.alarm.ctx.yieldCg != b.alarm.ctx.yieldCg ||
      a.alarm.ctx.flowCgPerS != b.alarm.ctx.flowCgPerS ||
      a.alarm.ctx.flowValid != b.alarm.ctx.flowValid ||
      a.alarm.ctx.projectedFinalCg != b.alarm.ctx.projectedFinalCg) {
    return false;
  }
  for (uint16_t i = 0; i < a.eventCount; ++i) {
    if (a.events[i].tMs != b.events[i].tMs ||
        a.events[i].kind != b.events[i].kind) {
      return false;
    }
  }
  for (uint16_t i = 0; i < a.sampleCount; ++i) {
    if (a.samples[i].tMs != b.samples[i].tMs ||
        a.samples[i].cg != b.samples[i].cg ||
        a.samples[i].scaleTimerMs != b.samples[i].scaleTimerMs) {
      return false;
    }
  }
  return true;
}

pump_scale::Extraction makeShot() {
  pump_scale::Extraction e{};
  e.phase = pump_scale::Phase::DONE;
  e.endCause = pump_scale::EndCause::STABLE;
  e.beginMs = 1'000'000;
  e.lastPumpOffMs = 1'025'000;
  e.stableMs = 1'026'500;
  e.endMs = 1'027'000;
  e.totalPumpOnMs = 24'000;
  e.startUtcSec = 1'700'000'000;
  e.yieldStatus = pump_scale::YieldStatus::OK;
  e.yieldCg = 3600;
  e.startRawCg = 1500;    // 15 g cup left on an un-tared scale
  e.settledRawCg = 5100;  // 15 g cup + 36 g coffee
  e.pourMs = 7000;
  e.decisionGainCg = 3700;
  e.observedSampleCount = 5;
  e.events[0] = {1'000'000, pump_scale::EventKind::BEGIN};
  e.events[1] = {1'000'000, pump_scale::EventKind::PUMP_ON};
  e.events[2] = {1'025'000, pump_scale::EventKind::PUMP_OFF};
  e.events[3] = {1'026'500, pump_scale::EventKind::STABLE_DETECTED};
  e.events[4] = {1'027'000, pump_scale::EventKind::END};
  e.eventCount = 5;
  // Raw scale samples: cup at 15 g, then rising to 51 g.
  e.samples[0] = {1'002'000, 1500, 2000};
  e.samples[1] = {1'005'000, 2300, 5000};
  e.samples[2] = {1'010'000, 3500, 10000};
  e.samples[3] = {1'020'000, 5150, 20000};
  e.samples[4] = {1'026'000, 5100, 26000};
  e.sampleCount = 5;
  return e;
}

void testRoundTripFullShot() {
  const pump_scale::Extraction e = makeShot();
  pump_scale::Extraction d{};
  CHECK(decode(encode(e), d));
  CHECK(d.hasTargetSnapshot);
  CHECK(sameSerialized(e, d));
}

void testRoundTripDisturbedYieldStatus() {
  pump_scale::Extraction e = makeShot();
  e.yieldStatus = pump_scale::YieldStatus::DISTURBED;
  e.yieldCg = e.decisionGainCg;
  e.settledRawCg = -7140;

  pump_scale::Extraction d{};
  CHECK(decode(encode(e), d));
  CHECK(d.yieldStatus == pump_scale::YieldStatus::DISTURBED);
  CHECK(sameSerialized(e, d));
}

void testRoundTripNoScaleTimers() {
  pump_scale::Extraction e{};
  e.beginMs = 500;
  e.yieldStatus = pump_scale::YieldStatus::OK;
  e.yieldCg = 2500;
  e.startRawCg = 1000;
  e.settledRawCg = 3500;
  e.pourMs = 5000;
  e.decisionGainCg = 2600;
  e.samples[0] = {600, 1000, scale_time::UNKNOWN_MS};
  e.samples[1] = {700, 3500, scale_time::UNKNOWN_MS};
  e.sampleCount = 2;
  pump_scale::Extraction d{};
  CHECK(decode(encode(e), d));
  CHECK(sameSerialized(e, d));
  for (uint16_t i = 0; i < d.sampleCount; ++i) {
    CHECK(d.samples[i].scaleTimerMs == scale_time::UNKNOWN_MS);
  }
}

void testRoundTripEmpty() {
  pump_scale::Extraction e{};  // IDLE, header-only
  e.beginMs = 42;
  pump_scale::Extraction d{};
  CHECK(decode(encode(e), d));
  CHECK(sameSerialized(e, d));
}

void testRejectsBadMagic() {
  std::vector<uint8_t> bytes = encode(makeShot());
  bytes[0] = 'X';
  pump_scale::Extraction d{};
  CHECK(!decode(bytes, d));
}

void testRejectsBadVersion() {
  // Firmware history is current-version only.
  std::vector<uint8_t> bytes = encode(makeShot());
  bytes[4] = pump_scale::kCompactVersion + 1;
  pump_scale::Extraction d{};
  CHECK(!decode(bytes, d));
  bytes[4] = pump_scale::kCompactVersion - 1;
  CHECK(!decode(bytes, d));
}

void testRejectsUnknownEndCause() {
  std::vector<uint8_t> bytes = encode(makeShot());
  bytes[6] = 3;
  pump_scale::Extraction d{};
  CHECK(!decode(bytes, d));
}

void testRejectsTruncated() {
  std::vector<uint8_t> bytes = encode(makeShot());
  bytes.resize(bytes.size() - 1);
  pump_scale::Extraction d{};
  CHECK(!decode(bytes, d));
}

void testRejectsTruncatedHeader() {
  std::vector<uint8_t> bytes = encode(makeShot());
  bytes.resize(pump_scale::kCompactHeaderBytes - 1);
  pump_scale::Extraction d{};
  CHECK(!decode(bytes, d));
}

void testRejectsOverlongSampleCount() {
  std::vector<uint8_t> bytes = encode(makeShot());
  const uint16_t bogus = pump_scale::Extraction::MAX_SAMPLES + 1;
  bytes[42] = static_cast<uint8_t>(bogus);
  bytes[43] = static_cast<uint8_t>(bogus >> 8);
  pump_scale::Extraction d{};
  CHECK(!decode(bytes, d));
}

void testTargetBlockGoldenBytes() {
  // Byte-for-byte guard for the v6 trailing block. The layout is part of the
  // persistent on-flash format; this test fails if a refactor reorders the
  // writes symmetrically (round-trip alone would not catch that).
  pump_scale::Extraction e = makeShot();
  e.target.targetCg = 0x1234;
  e.target.armed = true;
  e.target.tauMs = 0x0567;
  e.target.cCg = static_cast<int16_t>(0x1234);
  e.target.reactionLeadMs = 0x089A;
  e.alarm.tMs = 0xDEADBEEF;
  e.alarm.ctx.yieldCg = static_cast<int16_t>(0x0ABC);
  e.alarm.ctx.flowCgPerS = static_cast<int16_t>(0x0DEF);
  e.alarm.ctx.flowValid = true;
  e.alarm.ctx.projectedFinalCg = static_cast<int16_t>(0x1234);

  const std::vector<uint8_t> out = encode(e);
  CHECK(out.size() >= pump_scale::kCompactTargetBlockBytes);
  const size_t tail = out.size() - pump_scale::kCompactTargetBlockBytes;
  const uint8_t expected[pump_scale::kCompactTargetBlockBytes] = {
      0x34, 0x12,              // targetCg
      0x01,                    // armed
      0x67, 0x05,              // tauMs
      0x34, 0x12,              // cCg
      0x9A, 0x08,              // reactionLeadMs
      0xEF, 0xBE, 0xAD, 0xDE,  // alarmTriggeredMs
      0xBC, 0x0A,              // alarmYieldCg
      0xEF, 0x0D,              // alarmFlowCgPerS
      0x01,                    // alarmFlowValid
      0x34, 0x12,              // alarmProjectedFinalCg
  };
  for (size_t i = 0; i < pump_scale::kCompactTargetBlockBytes; ++i) {
    CHECK(out[tail + i] == expected[i]);
  }
}

void testTargetAndAlarmRoundTrip() {
  pump_scale::Extraction e = makeShot();
  e.target.targetCg = 3600;
  e.target.armed = true;
  e.target.tauMs = 800;
  e.target.cCg = 50;
  e.target.reactionLeadMs = 250;
  e.alarm.tMs = 1'020'000;
  e.alarm.ctx.yieldCg = 3500;
  e.alarm.ctx.flowCgPerS = 123;
  e.alarm.ctx.flowValid = true;
  e.alarm.ctx.projectedFinalCg = 3620;
  e.events[e.eventCount++] = {1'020'000,
                              pump_scale::EventKind::ALARM_TRIGGERED};

  pump_scale::Extraction d{};
  CHECK(decode(encode(e), d));
  CHECK(d.hasTargetSnapshot);
  CHECK(sameSerialized(e, d));
}

void testZeroTargetStillASnapshot() {
  // A v6 shot recorded with no target set must still carry hasTargetSnapshot,
  // so replay does not fall back to the user's current target settings.
  pump_scale::Extraction e = makeShot();
  e.target.targetCg = 0;
  e.target.armed = false;
  e.target.tauMs = 750;
  e.target.cCg = 0;
  e.target.reactionLeadMs = 0;

  pump_scale::Extraction d{};
  CHECK(decode(encode(e), d));
  CHECK(d.hasTargetSnapshot);
  CHECK(d.target.targetCg == 0);
  CHECK(d.target.armed == false);
}

void testResolveReplayCoeffs() {
  // Whole-struct replacement on hasTargetSnapshot. The recorded target must
  // be used as-is when present, even if targetCg == 0 / armed == false, so a
  // disarmed or zero-target shot replays exactly as it was recorded. Only
  // when the shot has no snapshot do we fall back to the caller-supplied live
  // coefficients.
  pump_scale::TargetCoeffs live;
  live.targetCg = 4000;
  live.armed = true;
  live.tauMs = 600;
  live.cCg = 30;
  live.reactionLeadMs = 200;

  {
    pump_scale::Extraction shot;
    shot.hasTargetSnapshot = true;
    shot.target.targetCg = 3600;
    shot.target.armed = true;
    shot.target.tauMs = 800;
    shot.target.cCg = 50;
    shot.target.reactionLeadMs = 250;
    const pump_scale::TargetCoeffs c =
        pump_scale::resolveReplayCoeffs(shot, live);
    CHECK(c.targetCg == 3600);
    CHECK(c.armed == true);
    CHECK(c.tauMs == 800);
    CHECK(c.cCg == 50);
    CHECK(c.reactionLeadMs == 250);
  }

  {
    // Disarmed/zero-target snapshot still wins over live armed target.
    pump_scale::Extraction shot;
    shot.hasTargetSnapshot = true;
    shot.target.targetCg = 0;
    shot.target.armed = false;
    shot.target.tauMs = 750;
    shot.target.cCg = 0;
    shot.target.reactionLeadMs = 0;
    const pump_scale::TargetCoeffs c =
        pump_scale::resolveReplayCoeffs(shot, live);
    CHECK(c.targetCg == 0);
    CHECK(c.armed == false);
    CHECK(c.tauMs == 750);
    CHECK(c.cCg == 0);
    CHECK(c.reactionLeadMs == 0);
  }

  {
    // No snapshot -> use live coeffs wholesale.
    pump_scale::Extraction shot;
    shot.hasTargetSnapshot = false;
    const pump_scale::TargetCoeffs c =
        pump_scale::resolveReplayCoeffs(shot, live);
    CHECK(c.targetCg == 4000);
    CHECK(c.armed == true);
    CHECK(c.tauMs == 600);
    CHECK(c.cCg == 30);
    CHECK(c.reactionLeadMs == 200);
  }
}

void testRawScaleTimerField() {
  pump_scale::Extraction e{};
  e.beginMs = 1000;
  e.yieldStatus = pump_scale::YieldStatus::OK;
  e.samples[0] = {1100, 500, 2300};
  e.sampleCount = 1;

  const std::vector<uint8_t> out = encode(e);
  CHECK(out.size() == pump_scale::encodeCompactSize(e));
  CHECK(out.size() > pump_scale::kCompactHeaderBytes);
  CHECK(out[4] == pump_scale::kCompactVersion);
  CHECK((out[7] & 0x02) != 0);

  size_t p = pump_scale::kCompactHeaderBytes;
  CHECK(readUvarint(out, p) == 100);
  CHECK(readSvarint(out, p) == 500);
  CHECK(readUvarint(out, p) == 2300);
  // A v6 record carries a trailing target/alarm block after the samples.
  CHECK(p == out.size() - pump_scale::kCompactTargetBlockBytes);
}

void testScaleTimerFieldOmittedWhenAbsent() {
  pump_scale::Extraction e{};
  e.beginMs = 1000;
  e.samples[0] = {1100, 500, scale_time::UNKNOWN_MS};
  e.sampleCount = 1;

  const std::vector<uint8_t> out = encode(e);
  CHECK(out.size() == pump_scale::encodeCompactSize(e));
  CHECK((out[7] & 0x02) == 0);

  size_t p = pump_scale::kCompactHeaderBytes;
  CHECK(readUvarint(out, p) == 100);
  CHECK(readSvarint(out, p) == 500);
  CHECK(p == out.size() - pump_scale::kCompactTargetBlockBytes);
}

void testMaximumCompactSizeBound() {
  pump_scale::Extraction e{};
  e.beginMs = 0;
  uint32_t eventTMs = 0;
  for (size_t i = 0; i < pump_scale::Extraction::MAX_EVENTS; ++i) {
    --eventTMs;  // UINT32_MAX delta from the previous timestamp.
    e.events[i] = {eventTMs, pump_scale::EventKind::PUMP_ON};
  }
  e.eventCount = pump_scale::Extraction::MAX_EVENTS;

  uint32_t sampleTMs = 0;
  for (size_t i = 0; i < pump_scale::Extraction::MAX_SAMPLES; ++i) {
    --sampleTMs;
    const int16_t cg = (i % 2 == 0) ? INT16_MIN : INT16_MAX;
    // UINT32_MAX means "timer unavailable"; the next value down is the
    // largest timer value that is encoded.
    e.samples[i] = {sampleTMs, cg, UINT32_MAX - 1};
  }
  e.sampleCount = pump_scale::Extraction::MAX_SAMPLES;

  CHECK(pump_scale::encodeCompactSize(e) == pump_scale::kMaxCompactRecordBytes);
  CHECK(encode(e).size() == pump_scale::kMaxCompactRecordBytes);
}

}  // namespace

int main() {
  testMaximumCompactSizeBound();
  testRawScaleTimerField();
  testScaleTimerFieldOmittedWhenAbsent();
  testRoundTripFullShot();
  testRoundTripDisturbedYieldStatus();
  testRoundTripNoScaleTimers();
  testRoundTripEmpty();
  testRejectsBadMagic();
  testRejectsBadVersion();
  testRejectsUnknownEndCause();
  testRejectsTruncated();
  testRejectsTruncatedHeader();
  testRejectsOverlongSampleCount();
  testTargetBlockGoldenBytes();
  testTargetAndAlarmRoundTrip();
  testZeroTargetStillASnapshot();
  testResolveReplayCoeffs();
  if (g_failures == 0) {
    std::printf("OK: all assertions passed\n");
    return 0;
  }
  std::printf("%d assertion(s) failed\n", g_failures);
  return 1;
}
