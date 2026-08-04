// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "apps/extraction/Extraction.h"
#include "apps/extraction/ExtractionWire.h"

namespace pump_scale {

// Metadata read from a shot file's compact header. It supplies the history list
// without decoding the sample and event streams.
struct ShotMeta {
  uint32_t id;
  uint32_t startUtcSec;
  uint32_t beginMs;
  uint32_t durationMs;
  uint32_t totalPumpOnMs;
  int16_t yieldCg;
};

struct ShotBinary {
  std::unique_ptr<uint8_t[]> data;
  size_t size = 0;
};

// Persistent FIFO of finalized extractions.
//
// Records are compact blobs named /shots/00000001.bin, with a monotonically
// increasing id stored in the NVS "shots" namespace. The 4.375 MiB history
// partition retains at most 250 records. A maximum-size compact record encodes
// to 10,442 bytes and fits in three 4 KiB filesystem blocks, so the cap uses
// about 67% of the partition before filesystem metadata.
//
// Each save writes and verifies the new record before removing older records.
// A failed write therefore preserves the existing history. History pages
// validate record headers and omit incomplete or corrupt files.
class ShotStore {
 public:
  static constexpr size_t kMaxShots = 250;

  ShotStore();
  ~ShotStore();

  // Writes and verifies `ext`, then enforces FIFO retention. Returns false
  // without removing existing history when the new record cannot be saved.
  bool save(const Extraction& ext);

  // Newest-first page of valid records with id < `before`; before == 0 starts
  // at the newest record. Headers are validated during the directory walk so
  // corrupt records neither consume a page slot nor hide older valid shots.
  std::vector<uint32_t> page(uint32_t before, size_t limit, bool* hasMore,
                             size_t* total, uint32_t* outRevision);

  // Reads one record's compact-header metadata without decoding its samples.
  // Returns false when the file is missing or its header is invalid.
  bool readMeta(uint32_t id, ShotMeta& out);

  // Copies one validated compact record while holding the store mutex. The
  // caller can then perform network I/O without delaying a concurrent save.
  bool readBinary(uint32_t id, ShotBinary& out);

  // Decodes one stored record into `out`.
  bool load(uint32_t id, Extraction& out);
  // Removes one record and advances the HTTP list revision.
  bool erase(uint32_t id);
  // Newest record with a valid compact header, or zero when history is empty.
  // The filesystem is authoritative so restore still works after NVS loss.
  uint32_t newestId();

  // Mutations advance the O(1) revision without requiring a directory scan.
  // List ETags combine it with bootNonce so a browser cannot confuse records
  // from different storage lifetimes.
  uint32_t revision();
  // Random for each boot and immutable after construction.
  uint32_t bootNonce() const { return _bootNonce; }

 private:
  mutable SemaphoreHandle_t _mutex;
  uint32_t _bootNonce = 0;
  uint32_t _revision = 0;

  // Helpers require _mutex.
  bool _scanIdsUnlocked(std::vector<uint32_t>& ids);
  uint32_t _reserveNextId(uint32_t minimumLastId);
  bool _writeRecordUnlocked(const Extraction& ext, uint32_t id,
                            size_t encodedSize);
  void _enforceRetentionUnlocked(const std::vector<uint32_t>& ids);
  bool _readMetaUnlocked(uint32_t id, ShotMeta& out);
  static void _pathFor(uint32_t id, char* buf, size_t cap);
};

}  // namespace pump_scale
