// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

struct PumpClusterSelection {
  bool hasCluster = false;
  size_t clusterIndex = 0;
  float rawSnrDb = std::numeric_limits<float>::quiet_NaN();
  float peakHz = std::numeric_limits<float>::quiet_NaN();
  float dominantPeakHz = std::numeric_limits<float>::quiet_NaN();
};

// Refines a cluster's center-bin frequency using the adjacent FFT bins.
inline float interpolatedClusterHz(const float* magnitudeSquared,
                                   size_t spectrumSize, uint16_t centerBin,
                                   float binHz) {
  static constexpr float kEps = 1e-20f;
  if (!magnitudeSquared || centerBin == 0 || centerBin + 1 >= spectrumSize ||
      !std::isfinite(binHz) || binHz <= 0.0f) {
    return std::numeric_limits<float>::quiet_NaN();
  }

  for (size_t bin = centerBin - 1; bin <= centerBin + 1; ++bin) {
    if (!std::isfinite(magnitudeSquared[bin]) || magnitudeSquared[bin] < 0.0f) {
      return std::numeric_limits<float>::quiet_NaN();
    }
  }

  const float lower = std::log10(magnitudeSquared[centerBin - 1] + kEps);
  const float center = std::log10(magnitudeSquared[centerBin] + kEps);
  const float upper = std::log10(magnitudeSquared[centerBin + 1] + kEps);
  const float denominator = lower - 2.0f * center + upper;
  float offset = 0.0f;
  if (std::isfinite(denominator) && denominator != 0.0f) {
    offset = std::clamp(0.5f * (lower - upper) / denominator, -1.0f, 1.0f);
  }
  return (static_cast<float>(centerBin) + offset) * binHz;
}

inline float clusterSnrDb(float energy, uint16_t binCount, float noisePerBin) {
  static constexpr float kEps = 1e-20f;
  if (!std::isfinite(energy) || energy < 0.0f || binCount == 0 ||
      !std::isfinite(noisePerBin) || noisePerBin < 0.0f) {
    return std::numeric_limits<float>::quiet_NaN();
  }
  const float noise = noisePerBin * static_cast<float>(binCount);
  return 10.0f * std::log10((energy + kEps) / (noise + kEps));
}

// Selects the highest-energy cluster inside the pump band, then computes its
// SNR for the trigger. dominantPeakHz retains the highest-energy cluster across
// the full analysis band for diagnostics.
inline PumpClusterSelection selectPumpCluster(
    const float* magnitudeSquared, size_t spectrumSize,
    const uint16_t* clusterBins, const float* clusterEnergies,
    const uint16_t* clusterCounts, size_t clusterCount, float binHz,
    float noisePerBin, float pumpMinHz, float pumpMaxHz) {
  PumpClusterSelection result;
  if (!magnitudeSquared || !clusterBins || !clusterEnergies || !clusterCounts ||
      clusterCount == 0 || !std::isfinite(binHz) || binHz <= 0.0f ||
      !std::isfinite(pumpMinHz) || !std::isfinite(pumpMaxHz) ||
      pumpMinHz > pumpMaxHz) {
    return result;
  }

  size_t dominant = clusterCount;
  float dominantEnergy = -std::numeric_limits<float>::infinity();
  for (size_t i = 0; i < clusterCount; ++i) {
    if (std::isfinite(clusterEnergies[i]) &&
        clusterEnergies[i] > dominantEnergy) {
      dominant = i;
      dominantEnergy = clusterEnergies[i];
    }
  }
  if (dominant == clusterCount) return result;

  result.dominantPeakHz = interpolatedClusterHz(magnitudeSquared, spectrumSize,
                                                clusterBins[dominant], binHz);

  auto select = [&](size_t index, float peakHz, float snrDb) {
    result.hasCluster = true;
    result.clusterIndex = index;
    result.rawSnrDb = snrDb;
    result.peakHz = peakHz;
  };

  float selectedEnergy = -std::numeric_limits<float>::infinity();
  for (size_t i = 0; i < clusterCount; ++i) {
    const float peakHz =
        i == dominant ? result.dominantPeakHz
                      : interpolatedClusterHz(magnitudeSquared, spectrumSize,
                                              clusterBins[i], binHz);
    if (!std::isfinite(peakHz) || peakHz < pumpMinHz || peakHz > pumpMaxHz) {
      continue;
    }

    const float snrDb =
        clusterSnrDb(clusterEnergies[i], clusterCounts[i], noisePerBin);
    if (!std::isfinite(snrDb) || clusterEnergies[i] <= selectedEnergy) continue;
    selectedEnergy = clusterEnergies[i];
    select(i, peakHz, snrDb);
  }
  return result;
}
