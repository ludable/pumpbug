// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// Host-compilable unit test for the AcaiaV2 codec (no NimBLE / Arduino deps).
//
//   g++ -std=c++17 -Wall -Wextra test/test_acaia_codec.cpp \
//       src/ble/AcaiaV2Codec.cpp -Isrc/ble -o /tmp/test_acaia_codec
//   /tmp/test_acaia_codec
//
// Frames below exercise representative weight, timer, and settings messages.

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

#include "AcaiaV2Codec.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                              \
    }                                                            \
  } while (0)

struct Capture {
  std::vector<AcaiaV2::Event> events;
  std::vector<AcaiaV2::InboundKind> frameKinds;
  std::vector<size_t> frameEmitted;
};

void onEvent(const AcaiaV2::Event& ev, void* user) {
  static_cast<Capture*>(user)->events.push_back(ev);
}

void onFrame(const uint8_t* /*frame*/, size_t /*len*/, AcaiaV2::InboundKind kind,
             size_t emitted, void* user) {
  auto* cap = static_cast<Capture*>(user);
  cap->frameKinds.push_back(kind);
  cap->frameEmitted.push_back(emitted);
}

// Feed bytes through a fresh Receiver, optionally split at `splitAt`.
Capture run(const std::vector<uint8_t>& bytes, size_t splitAt = 0) {
  Capture cap;
  AcaiaV2::Receiver rx;
  rx.setEventCallback(onEvent, &cap);
  if (splitAt == 0 || splitAt >= bytes.size()) {
    rx.pushNotify(bytes.data(), bytes.size(), onFrame, &cap);
  } else {
    rx.pushNotify(bytes.data(), splitAt, onFrame, &cap);
    rx.pushNotify(bytes.data() + splitAt, bytes.size() - splitAt, onFrame, &cap);
  }
  return cap;
}

std::vector<uint8_t> makeFrame(uint8_t type, const std::vector<uint8_t>& body) {
  std::vector<uint8_t> frame;
  frame.reserve(body.size() + 5);
  frame.push_back(0xEF);
  frame.push_back(0xDD);
  frame.push_back(type);
  frame.insert(frame.end(), body.begin(), body.end());
  uint8_t ce = 0;
  uint8_t co = 0;
  for (size_t i = 0; i < body.size(); ++i) {
    if ((i & 1) == 0) {
      ce = static_cast<uint8_t>(ce + body[i]);
    } else {
      co = static_cast<uint8_t>(co + body[i]);
    }
  }
  frame.push_back(ce);
  frame.push_back(co);
  return frame;
}

// Stable zero-weight message.
const std::vector<uint8_t> kWeightFrame = {0xEF, 0xDD, 0x0C, 0x0C, 0x05, 0x00,
                                           0x00, 0x00, 0x00, 0x01, 0x00, 0x07,
                                           0x00, 0x00, 0x02, 0x14, 0x07};
// Settings/status frame the scale pushes on connect.
const std::vector<uint8_t> kSettingsFrame = {0xEF, 0xDD, 0x08, 0x09, 0x37, 0x02,
                                             0x04, 0x01, 0x00, 0x01, 0x01, 0x00,
                                             0x0D, 0x3C};

void testWeightIsSingleEvent() {
  Capture cap = run(kWeightFrame);

  // The message produces one weight event and no timer event.
  CHECK(cap.frameKinds.size() == 1);
  CHECK(cap.frameKinds[0] == AcaiaV2::InboundKind::Weight);

  // Exactly one decoded event, and it is the weight (0.0 g, stable).
  CHECK(cap.frameEmitted.size() == 1 && cap.frameEmitted[0] == 1);
  CHECK(cap.events.size() == 1);
  if (cap.events.size() == 1) {
    CHECK(cap.events[0].type == AcaiaV2::EventType::WEIGHT);
    CHECK(std::fabs(cap.events[0].weight.grams) < 1e-6f);
    CHECK(cap.events[0].weight.stable);
    CHECK(cap.events[0].weight.hasScaleTimer);
    CHECK(cap.events[0].weight.scaleTimerMs == 200);
  }

  // The trailing 07 00 00 02 must NOT surface as a timer event.
  for (const auto& ev : cap.events) {
    CHECK(ev.type != AcaiaV2::EventType::TIMER);
  }

  // Stateless classifier agrees.
  CHECK(AcaiaV2::classifyInbound(kWeightFrame.data(), kWeightFrame.size()) ==
        AcaiaV2::InboundKind::Weight);
}

void testChecksumByteInSeparateNotification() {
  // Split so the final checksum byte arrives in a second notification.
  Capture cap = run(kWeightFrame, kWeightFrame.size() - 1);
  CHECK(cap.frameKinds.size() == 1);
  CHECK(cap.frameKinds[0] == AcaiaV2::InboundKind::Weight);
  CHECK(cap.events.size() == 1);
}

void testSettings() {
  Capture cap = run(kSettingsFrame);
  CHECK(cap.frameKinds.size() == 1);
  CHECK(cap.frameKinds[0] == AcaiaV2::InboundKind::Settings);
  CHECK(cap.frameEmitted.size() == 1 && cap.frameEmitted[0] == 0);
  CHECK(cap.events.empty());
}

void testLegacyHeartbeatResponseWeight() {
  // Legacy scales may wrap a weight sample inside event type 0x0B. In that
  // shape the nested kind follows two response bytes, then the normal weight
  // payload.
  const std::vector<uint8_t> frame = makeFrame(
      0x0C, {0x0B, 0x0B, 0x00, 0x00, 0x05, 0x34, 0x12, 0x00, 0x00, 0x02,
             0x00});
  Capture cap = run(frame);
  CHECK(cap.frameKinds.size() == 1);
  CHECK(cap.frameKinds[0] == AcaiaV2::InboundKind::Weight);
  CHECK(cap.frameEmitted.size() == 1 && cap.frameEmitted[0] == 1);
  CHECK(cap.events.size() == 1);
  if (cap.events.size() == 1) {
    CHECK(cap.events[0].type == AcaiaV2::EventType::WEIGHT);
    CHECK(std::fabs(cap.events[0].weight.grams - 46.60f) < 1e-4f);
    CHECK(cap.events[0].weight.stable);
    CHECK(!cap.events[0].weight.hasScaleTimer);
  }
}

void testShortLegacyHeartbeatResponseIsNotSample() {
  const std::vector<uint8_t> shortWeight = makeFrame(
      0x0C,
      {0x0A, 0x0B, 0x00, 0x00, 0x05, 0x34, 0x12, 0x00, 0x00, 0x02});
  Capture weightCap = run(shortWeight);
  CHECK(weightCap.frameKinds.size() == 1);
  CHECK(weightCap.frameKinds[0] == AcaiaV2::InboundKind::Unknown);
  CHECK(weightCap.frameEmitted.size() == 1 && weightCap.frameEmitted[0] == 0);
  CHECK(weightCap.events.empty());

  const std::vector<uint8_t> shortTimer =
      makeFrame(0x0C, {0x07, 0x0B, 0x00, 0x00, 0x07, 0x00, 0x01});
  Capture timerCap = run(shortTimer);
  CHECK(timerCap.frameKinds.size() == 1);
  CHECK(timerCap.frameKinds[0] == AcaiaV2::InboundKind::Unknown);
  CHECK(timerCap.frameEmitted.size() == 1 && timerCap.frameEmitted[0] == 0);
  CHECK(timerCap.events.empty());
}

void testBackToBackFrames() {
  std::vector<uint8_t> two = kWeightFrame;
  two.insert(two.end(), kWeightFrame.begin(), kWeightFrame.end());
  Capture cap = run(two);
  CHECK(cap.frameKinds.size() == 2);
  CHECK(cap.events.size() == 2);
}

void testBadChecksumRejected() {
  std::vector<uint8_t> bad = kWeightFrame;
  bad[bad.size() - 1] ^= 0xFF;  // corrupt the odd checksum
  Capture cap = run(bad);
  // Either dropped silently or surfaced as Rejected, but never a weight event.
  CHECK(cap.events.empty());
  for (auto k : cap.frameKinds) CHECK(k == AcaiaV2::InboundKind::Rejected);
}

}  // namespace

int main() {
  testWeightIsSingleEvent();
  testChecksumByteInSeparateNotification();
  testSettings();
  testLegacyHeartbeatResponseWeight();
  testShortLegacyHeartbeatResponseIsNotSample();
  testBackToBackFrames();
  testBadChecksumRejected();
  if (g_failures == 0) {
    std::printf("OK: all assertions passed\n");
    return 0;
  }
  std::printf("%d assertion(s) failed\n", g_failures);
  return 1;
}
