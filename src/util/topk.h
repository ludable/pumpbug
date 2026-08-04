// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

// Find the indices of the largest-magnitude entries of arr[0..n) that together
// account for at least `target` fraction of the total sum. Indices are written
// to `buf[0..k)` in descending-magnitude order, where k is the return value.
//
// Stable: when two bins have equal value, the lower index always comes first.
//
// Algorithm: O(n) sum + O(n) heapify + O(k log n) pops + O(k) relocation.
// For "few dominant bins" cases (k << n) this is substantially faster than a
// full sort.
//
// Parameters
//   arr     : input array. Not modified. Values are assumed non-negative
//             (e.g. FFT magnitudes or power).
//   n       : array length. Must fit in uint16_t (n <= 65535).
//   target  : fraction of total sum to accumulate before stopping, in (0, 1].
//             Values > 1 fall through to "as many as fit in maxK or all n".
//   buf     : caller-owned buffer with capacity >= n. On return, buf[0..k)
//             holds the selected indices in descending-magnitude order; the
//             remainder of the buffer is scratch (undefined contents).
//   maxK    : optional cap on the number of indices returned. Defaults to
//             unlimited.
//
// Returns the number of indices written into buf. 0 if n == 0, maxK == 0,
// target <= 0, or the total sum is non-positive.
inline size_t topByEnergyFraction(const float* arr, size_t n, float target,
                                  uint16_t* buf,
                                  size_t maxK = static_cast<size_t>(-1)) {
  assert(arr && buf);
  assert(n <= std::numeric_limits<uint16_t>::max());
  if (n == 0 || maxK == 0 || target <= 0.0f) return 0;

  // Total sum (single pass).
  float total = 0.0f;
  for (size_t i = 0; i < n; ++i) total += arr[i];
  if (total <= 0.0f) return 0;

  // Build a max-heap of indices, comparing indirectly through arr[]. The
  // source array is never reordered. The comparator's secondary key (lower
  // index "greater" on tie) makes the selection stable.
  for (size_t i = 0; i < n; ++i) buf[i] = static_cast<uint16_t>(i);
  auto cmp = [arr](uint16_t a, uint16_t b) {
    if (arr[a] != arr[b]) return arr[a] < arr[b];
    return a > b;  // tie-break: lower index wins
  };
  std::make_heap(buf, buf + n, cmp);  // O(n)

  // Pop largest until the accumulated fraction meets the target, the cap is
  // reached, or the heap is empty.
  const float wanted = target * total;
  float acc = 0.0f;
  size_t k = 0;
  while (k < n && k < maxK && acc < wanted) {
    std::pop_heap(buf, buf + n - k, cmp);  // O(log n)
    acc += arr[buf[n - k - 1]];
    ++k;
  }

  // The popped indices are at buf[n-k..n) in ascending magnitude. Reverse
  // them in place (now descending at the tail), then shift to the front so
  // the caller reads buf[0..k) directly. memmove handles the overlap when
  // k > n/2. O(k) total.
  std::reverse(buf + n - k, buf + n);
  std::memmove(buf, buf + n - k, k * sizeof(uint16_t));
  return k;
}

// Cluster nearby bin indices (typically the result of topByEnergyFraction)
// into groups that almost certainly belong to the same spectral feature:
// FFT windowing spreads a single tone across multiple adjacent bins, so a
// raw top-K count tends to over-report the number of distinct tones.
//
// Two bins are in the same cluster if their index difference is <= `gap`:
//   gap = 0 : every bin is its own cluster (no merging).
//   gap = 1 : directly adjacent bins merge — catches typical Hann leakage.
//   gap = 2 : allows one-bin holes — useful if leakage spans 3 bins and the
//             middle one occasionally falls out of the top-K cut.
//
// For each cluster the returned center is the bin with the largest magSq
// (argmax). Ties go to the lower bin index, matching topByEnergyFraction's
// stable ordering. The cluster's energy is the sum of magSq over its bins,
// which captures power that windowing leaked off the peak — a better signal
// estimator than the peak bin alone.
//
// Cluster centers, energies, and bin counts are written in ascending bin
// order (not by magnitude). centers[i], energies[i], counts[i] describe the
// same cluster.
//
// Parameters
//   indices    : top-K bin indices. Values may be in any order. Not
//                modified directly by this function (see aliasing note).
//   k          : length of `indices`.
//   magSq      : the underlying power spectrum, indexed by bin number.
//   gap        : maximum index distance between adjacent bins in the same
//                cluster. 0 disables merging.
//   scratch    : caller-owned buffer with capacity >= k. Reordered during
//                the call; contents undefined on return. Must NOT alias
//                `centers`, `energies`, or `counts`.
//   centers    : caller-owned output buffer for cluster center bin
//                indices. May alias `indices` (the input is fully copied
//                into scratch before any centers are written, so
//                overwriting `indices` is safe).
//   energies   : caller-owned output buffer for cluster energies (sum of
//                magSq across the cluster's bins). Parallel to `centers`.
//   counts     : caller-owned output buffer for cluster bin counts (number
//                of input indices that fell into each cluster). Parallel
//                to `centers`. Useful for per-bin noise estimates that need
//                to scale by cluster width.
//   centersCap : capacity of `centers`, `energies`, and `counts` (all the
//                same). If the actual cluster count exceeds this, the
//                first centersCap clusters (in ascending bin order) are
//                written and the rest are silently dropped — the caller
//                can detect overflow by sizing centersCap == k and
//                checking the return value.
//
// Returns the number of clusters written.
inline size_t clusterPeaks(const uint16_t* indices, size_t k,
                           const float* magSq, uint16_t gap,
                           uint16_t* scratch, uint16_t* centers,
                           float* energies, uint16_t* counts,
                           size_t centersCap) {
  assert(indices && magSq && scratch && centers && energies && counts);
  if (k == 0 || centersCap == 0) return 0;

  // Sort indices ascending so adjacency checks are meaningful. Copy first
  // so the input isn't modified (and so `centers` may alias `indices`).
  std::memcpy(scratch, indices, k * sizeof(uint16_t));
  std::sort(scratch, scratch + k);

  // Sweep, opening a new cluster whenever the gap to the previous bin
  // exceeds the threshold. Within a cluster, the center is the loudest bin
  // (argmax of magSq, ties broken to the lower index), the energy is the
  // sum of magSq across all bins, and the count is the number of bins.
  size_t n = 0;
  size_t start = 0;
  for (size_t i = 1; i <= k; ++i) {
    const bool boundary = (i == k) || (scratch[i] - scratch[i - 1] > gap);
    if (!boundary) continue;
    if (n >= centersCap) break;

    uint16_t best = scratch[start];
    float sum = magSq[best];
    for (size_t j = start + 1; j < i; ++j) {
      const uint16_t b = scratch[j];
      sum += magSq[b];
      if (magSq[b] > magSq[best]) best = b;
    }
    centers[n] = best;
    energies[n] = sum;
    counts[n] = static_cast<uint16_t>(i - start);
    ++n;
    start = i;
  }
  return n;
}
