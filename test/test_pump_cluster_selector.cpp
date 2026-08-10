// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include <cassert>
#include <cmath>
#include <cstdio>

#include "vibration/PumpClusterSelector.h"
#include "vibration/VibrationWindowTrigger.h"

namespace {

constexpr size_t kSpectrumSize = 128;
constexpr float kBinHz = 6.25f;

struct Fixture {
  float spectrum[kSpectrumSize] = {};
  uint16_t bins[8] = {};
  float energies[8] = {};
  uint16_t counts[8] = {};
  size_t count = 0;

  void add(uint16_t bin, float energy, uint16_t binCount = 1) {
    bins[count] = bin;
    energies[count] = energy;
    counts[count] = binCount;
    spectrum[bin] = energy;
    ++count;
  }

  PumpClusterSelection select(
      float noisePerBin = 1.0f,
      float pumpMinHz = VibrationWindowTrigger::PEAK_MIN_HZ,
      float pumpMaxHz = VibrationWindowTrigger::PEAK_MAX_HZ) const {
    return selectPumpCluster(spectrum, kSpectrumSize, bins, energies, counts,
                             count, kBinHz, noisePerBin, pumpMinHz, pumpMaxHz);
  }
};

void noClustersProduceNoCandidate() {
  Fixture f;
  const auto result = f.select();
  assert(!result.hasCluster);
  assert(std::isnan(result.peakHz));
  assert(std::isnan(result.rawSnrDb));
}

void inBandDominantKeepsExistingCalculation() {
  Fixture f;
  f.add(19, 100.0f);  // 118.75 Hz.

  const auto result = f.select();
  assert(result.hasCluster);
  assert(result.clusterIndex == 0);
  assert(std::fabs(result.peakHz - 118.75f) < 0.01f);
  assert(std::fabs(result.rawSnrDb - 20.0f) < 0.01f);
  assert(result.peakHz == result.dominantPeakHz);
}

void outOfBandDominantFallsBackToInBandCluster() {
  Fixture f;
  f.add(19, 100.0f);
  f.add(38, 1000.0f);  // 237.5 Hz.

  const auto result = f.select();
  assert(result.hasCluster);
  assert(result.clusterIndex == 0);
  assert(std::fabs(result.peakHz - 118.75f) < 0.01f);
  assert(std::fabs(result.dominantPeakHz - 237.5f) < 0.01f);
}

void unrelatedDominantDoesNotSuppressInBandCluster() {
  Fixture f;
  f.add(19, 100.0f);
  f.add(40, 1000.0f);  // 250 Hz is not a multiple of 118.75 Hz.

  const auto result = f.select();
  assert(result.hasCluster);
  assert(result.clusterIndex == 0);
}

void noInBandClusterProducesNoCandidate() {
  Fixture f;
  f.add(40, 1000.0f);  // 250 Hz.

  const auto result = f.select();
  assert(!result.hasCluster);
  assert(std::isnan(result.peakHz));
  assert(std::fabs(result.dominantPeakHz - 250.0f) < 0.01f);
}

void highestEnergyInBandClusterWins() {
  Fixture f;
  f.add(15, 1000.0f, 1);  // 93.75 Hz, 20.0 dB.
  f.add(20, 1600.0f, 2);  // 125 Hz, about 19.0 dB.
  f.add(60, 5000.0f, 1);  // Out-of-band dominant.

  const auto result = f.select(10.0f);
  assert(result.hasCluster);
  assert(result.clusterIndex == 1);
  assert(std::fabs(result.rawSnrDb - 19.03f) < 0.01f);
}

void inBandDominantWinsOverNarrowerHigherSnrCluster() {
  Fixture f;
  f.add(20, 1600.0f, 2);  // Highest energy, about 19.0 dB.
  f.add(15, 1000.0f, 1);  // Lower energy, 20.0 dB.

  const auto result = f.select(10.0f);
  assert(result.hasCluster);
  assert(result.clusterIndex == 0);
  assert(result.peakHz == result.dominantPeakHz);
}

void inBandDominantIsSelectedFromSeveralValidClusters() {
  Fixture f;
  f.add(16, 800.0f);   // 100 Hz.
  f.add(19, 1200.0f);  // 118.75 Hz, global dominant.
  f.add(40, 600.0f);   // 250 Hz.

  const auto result = f.select();
  assert(result.hasCluster);
  assert(result.clusterIndex == 1);
  assert(result.peakHz == result.dominantPeakHz);
}

void pumpBandEdgesAreInclusive() {
  Fixture lower;
  lower.add(15, 100.0f);  // 93.75 Hz.
  assert(lower.select(1.0f, 93.75f, 125.0f).hasCluster);

  Fixture upper;
  upper.add(20, 100.0f);  // 125 Hz.
  assert(upper.select(1.0f, 93.75f, 125.0f).hasCluster);
}

void fallbackRejectsPeakOutsideBand() {
  Fixture f;
  f.add(15, 100.0f);   // 93.75 Hz.
  f.add(40, 1000.0f);  // Out-of-band dominant.
  assert(!f.select(1.0f, 100.0f, 130.0f).hasCluster);
}

void invalidClusterInputsDoNotBecomeEvidence() {
  Fixture edge;
  edge.add(0, 100.0f);
  assert(!edge.select().hasCluster);

  Fixture invalidMagnitude;
  invalidMagnitude.add(19, 100.0f);
  invalidMagnitude.spectrum[18] = -1.0f;
  assert(!invalidMagnitude.select().hasCluster);

  Fixture emptyCluster;
  emptyCluster.add(19, 100.0f, 0);
  const auto result = emptyCluster.select();
  assert(!result.hasCluster);
  assert(std::isfinite(result.dominantPeakHz));
  assert(std::isnan(result.rawSnrDb));
}

void invalidDominantFrequencyDoesNotHideValidInBandCluster() {
  Fixture f;
  f.add(0, 1000.0f);  // Cannot be interpolated.
  f.add(19, 100.0f);  // 118.75 Hz.

  const auto result = f.select();
  assert(result.hasCluster);
  assert(result.clusterIndex == 1);
  assert(std::fabs(result.peakHz - 118.75f) < 0.01f);
  assert(std::isnan(result.dominantPeakHz));
}

void existingTriggerRejectsWeakSelectedCluster() {
  Fixture weak;
  weak.add(19, 10.0f);    // 10 dB.
  weak.add(38, 1000.0f);  // Out-of-band dominant.
  const auto weakResult = weak.select();
  assert(weakResult.hasCluster);

  VibrationWindowTrigger trigger;
  assert(!trigger.step(true, weakResult.rawSnrDb, weakResult.peakHz));
  assert(!trigger.step(true, weakResult.rawSnrDb, weakResult.peakHz));

  Fixture strong;
  strong.add(19, 100.0f);  // 20 dB.
  strong.add(38, 1000.0f);
  const auto strongResult = strong.select();
  assert(!trigger.step(true, strongResult.rawSnrDb, strongResult.peakHz));
  assert(trigger.step(true, strongResult.rawSnrDb, strongResult.peakHz));
}

}  // namespace

int main() {
  noClustersProduceNoCandidate();
  inBandDominantKeepsExistingCalculation();
  outOfBandDominantFallsBackToInBandCluster();
  unrelatedDominantDoesNotSuppressInBandCluster();
  noInBandClusterProducesNoCandidate();
  highestEnergyInBandClusterWins();
  inBandDominantWinsOverNarrowerHigherSnrCluster();
  inBandDominantIsSelectedFromSeveralValidClusters();
  pumpBandEdgesAreInclusive();
  fallbackRejectsPeakOutsideBand();
  invalidClusterInputsDoNotBecomeEvidence();
  invalidDominantFrequencyDoesNotHideValidInBandCluster();
  existingTriggerRejectsWeakSelectedCluster();
  std::puts("OK: all assertions passed");
}
