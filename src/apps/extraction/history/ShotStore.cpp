// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "ShotStore.h"

#include <LittleFS.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <esp_random.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <new>

#include "apps/extraction/extraction_encoding.h"
#include "util/scoped_lock.h"
#include "util/storage.h"

namespace pump_scale {
namespace {
constexpr const char* kNvsNamespace = "shots";
constexpr const char* kKeyLastId = "last_id";
constexpr const char* kShotsDir = "/shots";

using Lock = ScopedLock;

uint32_t parseIdFromName(const char* name) {
  // LittleFS ports differ on whether name() includes the directory prefix.
  const char* slash = std::strrchr(name, '/');
  const char* base = slash ? slash + 1 : name;
  if (std::strlen(base) < 12 || std::strcmp(base + 8, ".bin") != 0) return 0;

  uint32_t id = 0;
  for (int i = 0; i < 8; ++i) {
    if (base[i] < '0' || base[i] > '9') return 0;
    id = id * 10 + static_cast<uint32_t>(base[i] - '0');
  }
  return id;
}

bool hasSupportedShotHeader(const uint8_t* header) {
  return header[0] == 'E' && header[1] == 'X' && header[2] == 'T' &&
         header[3] == 'R' && isCompactVersionSupported(header[4]);
}

bool isValidShotHeader(const uint8_t* header, size_t size) {
  return size == kCompactHeaderBytes && hasSupportedShotHeader(header);
}
}  // namespace

ShotStore::ShotStore() : _mutex(xSemaphoreCreateMutex()) {
  _bootNonce = esp_random();
}

ShotStore::~ShotStore() {
  if (_mutex) {
    vSemaphoreDelete(_mutex);
    _mutex = nullptr;
  }
}

void ShotStore::_pathFor(uint32_t id, char* buf, size_t cap) {
  std::snprintf(buf, cap, "%s/%08u.bin", kShotsDir, static_cast<unsigned>(id));
}

bool ShotStore::_scanIdsUnlocked(std::vector<uint32_t>& ids) {
  ids.clear();
  if (!LittleFS.exists(kShotsDir)) return true;

  File dir = LittleFS.open(kShotsDir);
  if (!dir || !dir.isDirectory()) {
    M5_LOGE("ShotStore: cannot open %s", kShotsDir);
    return false;
  }

  File file = dir.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      const uint32_t id = parseIdFromName(file.name());
      if (id != 0) ids.push_back(id);
    }
    file = dir.openNextFile();
  }
  std::sort(ids.begin(), ids.end());
  return true;
}

uint32_t ShotStore::_reserveNextId(uint32_t minimumLastId) {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, false)) return 0;
  const uint32_t last = std::max(prefs.getUInt(kKeyLastId, 0), minimumLastId);
  if (last == UINT32_MAX) {
    prefs.end();
    M5_LOGE("ShotStore: shot id space exhausted");
    return 0;
  }
  const uint32_t next = last + 1;
  // Preferences returns the number of bytes committed, or zero on failure.
  // Reserving the id durably before writing prevents reuse after a reboot.
  const bool ok = prefs.putUInt(kKeyLastId, next) > 0;
  prefs.end();
  if (!ok) {
    M5_LOGE("ShotStore: NVS commit of last_id failed");
    return 0;
  }
  return next;
}

bool ShotStore::_readMetaUnlocked(uint32_t id, ShotMeta& out) {
  char path[32];
  _pathFor(id, path, sizeof(path));
  File file = LittleFS.open(path, "r");
  if (!file) return false;

  uint8_t header[kCompactHeaderBytes];
  const size_t got =
      file.readBytes(reinterpret_cast<char*>(header), sizeof(header));
  file.close();
  if (!isValidShotHeader(header, got)) return false;

  out = ShotMeta{};
  out.id = id;
  out.beginMs = readU32LE(header + 8);
  // Unsigned subtraction preserves the duration across a millis() wrap.
  out.durationMs = readU32LE(header + 20) - out.beginMs;
  out.totalPumpOnMs = readU32LE(header + 24);
  out.startUtcSec = readU32LE(header + 28);
  out.yieldCg = readI16LE(header + 32);
  return true;
}

bool ShotStore::_writeRecordUnlocked(const Extraction& ext, uint32_t id,
                                     size_t encodedSize) {
  char path[32];
  _pathFor(id, path, sizeof(path));
  File file = LittleFS.open(path, "w");
  if (!file) {
    M5_LOGE("ShotStore: cannot open %s", path);
    return false;
  }

  bool writeOk = true;
  const bool encoded = encodeCompact(ext, [&](const uint8_t* data, size_t len) {
    if (writeOk && file.write(data, len) != len) writeOk = false;
  });
  file.close();

  File check = LittleFS.open(path, "r");
  uint8_t header[kCompactHeaderBytes];
  size_t headerBytes = 0;
  size_t fileSize = 0;
  if (check) {
    fileSize = check.size();
    headerBytes =
        check.readBytes(reinterpret_cast<char*>(header), sizeof(header));
    check.close();
  }
  const bool verified = encoded && writeOk && fileSize == encodedSize &&
                        isValidShotHeader(header, headerBytes);
  if (verified) return true;

  if (!LittleFS.remove(path)) {
    M5_LOGW("ShotStore: could not remove incomplete %s", path);
  }
  M5_LOGE("ShotStore: write verification failed for %s", path);
  return false;
}

void ShotStore::_enforceRetentionUnlocked(const std::vector<uint32_t>& ids) {
  const size_t removeCount =
      ids.size() > kMaxShots ? ids.size() - kMaxShots : 0;
  for (size_t index = 0; index < removeCount; ++index) {
    char path[32];
    _pathFor(ids[index], path, sizeof(path));
    if (!LittleFS.remove(path)) {
      M5_LOGE("ShotStore: cannot remove oldest record %s", path);
      break;
    }
    ++_revision;
  }
}

bool ShotStore::save(const Extraction& ext) {
  Lock lock(_mutex);
  if (storage::mountState() != storage::MountState::Ready) {
    M5_LOGE("ShotStore: storage unavailable");
    return false;
  }
  if (ext.version != kCurrentExtractionVersion) {
    M5_LOGE("ShotStore: refusing to persist non-current record");
    return false;
  }
  size_t encodedSize = 0;
  if (!encodeCompactSize(ext, encodedSize)) {
    M5_LOGE("ShotStore: record version cannot be encoded");
    return false;
  }
  if (!LittleFS.exists(kShotsDir) && !LittleFS.mkdir(kShotsDir)) {
    M5_LOGE("ShotStore: cannot create %s", kShotsDir);
    return false;
  }

  std::vector<uint32_t> ids;
  if (!_scanIdsUnlocked(ids)) return false;

  const uint32_t minimumLastId = ids.empty() ? 0 : ids.back();
  const uint32_t id = _reserveNextId(minimumLastId);
  if (id == 0) {
    M5_LOGE("ShotStore: reserveNextId failed");
    return false;
  }

  if (!_writeRecordUnlocked(ext, id, encodedSize)) return false;

  ++_revision;
  ids.push_back(id);
  _enforceRetentionUnlocked(ids);

  M5_LOGI("ShotStore: saved shot id=%u", static_cast<unsigned>(id));
  return true;
}

std::vector<uint32_t> ShotStore::page(uint32_t before, size_t limit,
                                      bool* hasMore, size_t* total,
                                      uint32_t* outRevision) {
  Lock lock(_mutex);
  // The page and its ETag revision describe the same locked filesystem view.
  if (outRevision) *outRevision = _revision;

  std::vector<uint32_t> ids;
  size_t totalCount = 0;
  File dir = LittleFS.open(kShotsDir);
  if (dir && dir.isDirectory()) {
    File file = dir.openNextFile();
    while (file) {
      if (!file.isDirectory()) {
        const uint32_t id = parseIdFromName(file.name());
        // A valid filename is insufficient: a partial record must not consume
        // a page slot or leave valid older records hidden behind an empty page.
        uint8_t header[kCompactHeaderBytes];
        const size_t got =
            file.readBytes(reinterpret_cast<char*>(header), sizeof(header));
        if (id != 0 && isValidShotHeader(header, got)) {
          ++totalCount;
          if (before == 0 || id < before) ids.push_back(id);
        }
      }
      file = dir.openNextFile();
    }
  }
  if (total) *total = totalCount;

  std::sort(ids.begin(), ids.end(), std::greater<uint32_t>());
  if (hasMore) *hasMore = ids.size() > limit;
  if (ids.size() > limit) ids.resize(limit);
  return ids;
}

bool ShotStore::readMeta(uint32_t id, ShotMeta& out) {
  Lock lock(_mutex);
  return _readMetaUnlocked(id, out);
}

bool ShotStore::readBinary(uint32_t id, ShotBinary& out) {
  Lock lock(_mutex);
  out = ShotBinary{};

  char path[32];
  _pathFor(id, path, sizeof(path));
  File file = LittleFS.open(path, "r");
  if (!file) return false;

  const size_t size = file.size();
  if (size < kCompactHeaderBytes || size > kMaxCompactRecordBytes) {
    file.close();
    return false;
  }

  std::unique_ptr<uint8_t[]> data(new (std::nothrow) uint8_t[size]);
  if (!data) {
    file.close();
    return false;
  }
  const bool readOk =
      file.readBytes(reinterpret_cast<char*>(data.get()), size) == size;
  file.close();
  if (!readOk || !hasSupportedShotHeader(data.get())) {
    return false;
  }
  out.data = std::move(data);
  out.size = size;
  return true;
}

bool ShotStore::load(uint32_t id, Extraction& out) {
  Lock lock(_mutex);
  char path[32];
  _pathFor(id, path, sizeof(path));
  File file = LittleFS.open(path, "r");
  if (!file) return false;
  const bool ok = decodeCompact(
      [&](uint8_t* dst, size_t len) {
        return file.readBytes(reinterpret_cast<char*>(dst), len) == len;
      },
      out);
  file.close();
  return ok;
}

bool ShotStore::erase(uint32_t id) {
  Lock lock(_mutex);
  char path[32];
  _pathFor(id, path, sizeof(path));
  if (!LittleFS.remove(path)) return false;
  ++_revision;
  return true;
}

uint32_t ShotStore::newestId() {
  Lock lock(_mutex);
  if (!LittleFS.exists(kShotsDir)) return 0;

  std::vector<uint32_t> ids;
  if (!_scanIdsUnlocked(ids)) return 0;

  ShotMeta meta{};
  for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
    if (_readMetaUnlocked(*it, meta)) return *it;
  }
  return 0;
}

uint32_t ShotStore::revision() {
  Lock lock(_mutex);
  return _revision;
}

}  // namespace pump_scale
