// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

#include "Extraction.h"

// ShotReplayer — turns a finalized Extraction back into the live input stream
// that produced it, so the whole on-device pipeline (recorder, pour tracker,
// target alert, chart) can be exercised against a real recorded shot without
// the espresso machine.
//
// It walks the record on a real-time virtual clock: each tick() maps elapsed
// wall time to a point in the shot and emits the (pumpOn, ScaleSnapshot) a
// sensor would have produced then. The pump level comes from the
// PUMP_ON/PUMP_OFF events; the scale weight is the stored raw sample value, so
// the absolute on-scale weight is reproduced exactly. Before the first stored
// sample arrives, the emitted reading is the record's startRawCg (the display
// zero) when present, else 0.
//
// Feed the emitted inputs to a SANDBOX ExtractionRecorder, never the live one:
// replay must not graduate or persist anything, nor disturb the live recording.
// The replayer holds a pointer to the source Extraction (no copy), so that
// record must outlive the replay — it does, living in the controller, which is
// not ticked while a replay is running.
//
// Pure logic, no Arduino/BLE/FS dependency; host-testable.
namespace pump_scale {

class ShotReplayer {
 public:
  // Begin replaying `shot` from `nowMs`. The shot must outlive the replay.
  void start(const Extraction& shot, uint32_t nowMs) {
    _shot = &shot;
    _durationMs = shot.endMs - shot.beginMs;  // unsigned, millis()-wrap safe
    _lastNowMs = nowMs;
    _virtualElapsedMs = 0;
    _sampleCursor = 0;
    _paused = false;
    _active = true;
  }

  void stop() { _active = false; }
  bool active() const { return _active; }
  bool paused() const { return _paused; }

  // Toggle pause. Folds time up to nowMs into the virtual clock first so a
  // toggle neither drops nor double-counts the in-flight frame's delta.
  void setPaused(bool paused, uint32_t nowMs) {
    advance(nowMs);
    _paused = paused;
  }

  // Returns the replay position in the extraction's timestamp coordinate
  // system.
  uint32_t virtualNowMs() const {
    return (_shot ? _shot->beginMs : 0) + _virtualElapsedMs;
  }

  // Advance to nowMs and emit this frame's synthesized inputs. Returns false
  // once playback has run past the shot's end (the caller should stop replay).
  // On true, pumpOn and snap are always set; newSample is true on the frames
  // that introduce a fresh scale reading.
  bool tick(uint32_t nowMs, bool& pumpOn, ScaleSnapshot& snap,
            bool& newSample) {
    if (!_active || !_shot) return false;
    advance(nowMs);
    if (_virtualElapsedMs > _durationMs) return false;  // played to the end

    // Compare in offsets from the shot's begin, never absolute tMs: a shot can
    // span the 32-bit millis() wrap, after which post-wrap timestamps are
    // numerically smaller than the current replay position. The unsigned
    // (tMs - beginMs) is the real elapsed offset on both sides of the wrap,
    // matching how the rest of the format treats shot times.
    const uint32_t elapsed = _virtualElapsedMs;
    const uint32_t beginMs = _shot->beginMs;

    // Pump level: the last PUMP_ON/PUMP_OFF at or before the current replay
    // position. Events are time-ordered ascending, so stop once they pass it.
    pumpOn = false;
    for (uint16_t i = 0; i < _shot->eventCount; ++i) {
      const Event& e = _shot->events[i];
      if (static_cast<uint32_t>(e.tMs - beginMs) > elapsed) break;
      if (e.kind == EventKind::PUMP_ON) {
        pumpOn = true;
      } else if (e.kind == EventKind::PUMP_OFF) {
        pumpOn = false;
      }
    }

    // Consume every sample now due; the latest one becomes the live reading.
    newSample = false;
    while (_sampleCursor < _shot->sampleCount &&
           static_cast<uint32_t>(_shot->samples[_sampleCursor].tMs - beginMs) <=
               elapsed) {
      ++_sampleCursor;
      newSample = true;
    }

    snap = ScaleSnapshot{};
    snap.present = true;
    if (_sampleCursor > 0) {
      const Sample& s = _shot->samples[_sampleCursor - 1];
      snap.grams = s.cg / 100.0f;
      snap.timestampMs = s.tMs;
      snap.scaleTimerMs = s.scaleTimerMs;
    } else {
      // Before the first sample: scale connected with the cup on it, but no
      // BLE reading has arrived yet. Emit the display zero (startRawCg) if the
      // record carries one, else 0.
      snap.grams =
          (_shot->startRawCg != Extraction::NO_WEIGHT ? _shot->startRawCg : 0) /
          100.0f;
      snap.timestampMs = 0;
    }
    return true;
  }

 private:
  void advance(uint32_t nowMs) {
    const uint32_t dt = nowMs - _lastNowMs;
    _lastNowMs = nowMs;
    if (!_paused) _virtualElapsedMs += dt;
  }

  const Extraction* _shot = nullptr;
  uint32_t _durationMs = 0;
  uint32_t _lastNowMs = 0;
  uint32_t _virtualElapsedMs = 0;
  uint16_t _sampleCursor = 0;
  bool _paused = false;
  bool _active = false;
};

}  // namespace pump_scale
