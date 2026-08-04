// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

// Named audio cues. Volume/pitch policy lives here; the codec's power is owned
// by power::SpeakerCodec, on which these cues express demand.
namespace sounds {

// Start the speaker keep-alive without playing audio. Doing this during boot
// makes the first sound play reliably on the StickS3.
void warmUpSpeaker();
void buttonPress();
void buttonHold();
void buttonDoublePress();

uint16_t countdownBeepFreq(int bucket);

// Audio cues for the target-weight countdown. Call prepareTargetAlert() before
// the first countdown beep, targetApproach() for each approach cue, targetCut()
// at the predicted stop point, and targetSilence() when the extraction or alert
// ends. Preparing the alert powers the speaker before the first short cue; each
// later cue extends the same powered period.
void prepareTargetAlert();
void targetApproach(uint16_t freqHz);
void targetCut();
void targetSilence();

}  // namespace sounds
