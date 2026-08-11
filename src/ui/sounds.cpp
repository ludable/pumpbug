// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "ui/sounds.h"

#include <M5Unified.h>

#include <algorithm>

#include "power/SpeakerCodec.h"

namespace sounds {
namespace {

constexpr int kUiChannel = 6;
constexpr int kTargetChannel = 7;
constexpr int kBackflushChannel = 5;
constexpr uint16_t kCountdownFrequencies[] = {2100, 2181, 2264, 2351, 2440};
constexpr uint16_t kCountdownFreqEndHz = kCountdownFrequencies[4];

// Keep the amplifier active across a normal cluster of interactions. Hardware
// tests have occasionally produced a distorted cue when the StickS3 amplifier
// was restarted only milliseconds after release. Five seconds reduces those
// rapid off/on transitions; it is a mitigation, not a measured settling-time
// requirement.
constexpr uint32_t kCueKeepAliveMs = 5000;

bool ensurePowered() {
  return power::speakerCodec.keepUntil(millis() + kCueKeepAliveMs);
}

}  // namespace

void warmUpSpeaker() { ensurePowered(); }

void buttonPress() {
  if (!ensurePowered()) return;
  M5.Speaker.setChannelVolume(kUiChannel, 64);
  M5.Speaker.tone(1200, 25, kUiChannel, true);
}

void buttonHold() {
  if (!ensurePowered()) return;
  M5.Speaker.setChannelVolume(kUiChannel, 128);
  M5.Speaker.tone(1500, 100, kUiChannel, true);
}

void buttonDoublePress() {
  if (!ensurePowered()) return;
  M5.Speaker.setChannelVolume(kUiChannel, 128);
  M5.Speaker.tone(1800, 100, kUiChannel, true);
}

uint16_t countdownBeepFreq(int bucket) {
  // bucket 1..5 maps to array index 0..4 (bucket 1 -> final pitch).
  const int index = std::max(0, std::min(4, 5 - bucket));
  return kCountdownFrequencies[index];
}

// Powers the codec at the start of the alert so the first beep isn't clipped by
// speaker-init latency. The keep-alive is re-extended by each beep below,
// holding the codec across the whole countdown.
void prepareTargetAlert() { ensurePowered(); }

// The target cues share one dedicated channel so a re-issued tone replaces the
// previous one rather than stacking (the default tone() path auto-allocates a
// channel each call). Perceptually spaced countdown pitches (5 s -> 1 s) give a
// gentle ~65 cent rise per step; cadence remains the primary cue.
void targetApproach(uint16_t freqHz) {
  if (!ensurePowered()) return;
  M5.Speaker.setChannelVolume(kTargetChannel, 200);
  M5.Speaker.tone(freqHz, 50, kTargetChannel, true);
}

// Sustained cut-now tone. Reuses the final countdown pitch so the cut never
// drifts from the end of the countdown sequence.
void targetCut() {
  if (!ensurePowered()) return;
  M5.Speaker.setChannelVolume(kTargetChannel, 220);
  M5.Speaker.tone(kCountdownFreqEndHz, 500, kTargetChannel, true);
}

void targetSilence() {
  if (power::speakerCodec.isSpeakerRunning()) M5.Speaker.stop(kTargetChannel);
}

// Uses a short, quiet cue on the backflush channel. The caller chooses the
// pitch so the countdown schedule remains with the routine that owns it.
void backflushCountdown(uint16_t freqHz) {
  if (!ensurePowered()) return;
  M5.Speaker.setChannelVolume(kBackflushChannel, 180);
  M5.Speaker.tone(freqHz, 60, kBackflushChannel, true);
}

void backflushStartPump() {
  if (!ensurePowered()) return;
  M5.Speaker.setChannelVolume(kBackflushChannel, 200);
  M5.Speaker.tone(1900, 220, kBackflushChannel, true);
}

void backflushStopPump() {
  if (!ensurePowered()) return;
  M5.Speaker.setChannelVolume(kBackflushChannel, 220);
  M5.Speaker.tone(1100, 400, kBackflushChannel, true);
}

void backflushComplete() {
  if (!ensurePowered()) return;
  M5.Speaker.setChannelVolume(kBackflushChannel, 220);
  M5.Speaker.tone(2300, 600, kBackflushChannel, true);
}

void backflushSilence() {
  if (power::speakerCodec.isSpeakerRunning())
    M5.Speaker.stop(kBackflushChannel);
}

}  // namespace sounds
