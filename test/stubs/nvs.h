// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

using esp_err_t = int;
using nvs_handle_t = uint32_t;

constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;
constexpr esp_err_t ESP_ERR_NVS_TYPE_MISMATCH = 0x1103;
constexpr esp_err_t ESP_ERR_NVS_INVALID_HANDLE = 0x1107;
constexpr esp_err_t ESP_ERR_NVS_READ_ONLY = 0x110c;

enum nvs_open_mode_t { NVS_READONLY, NVS_READWRITE };

enum class TestNvsType { U32, U64 };

struct TestNvsValue {
  TestNvsType type;
  uint64_t value;
};

struct TestNvsState {
  std::unordered_map<std::string, std::unordered_map<std::string, TestNvsValue>>
      values;
  esp_err_t nextOpenError = ESP_OK;
  esp_err_t nextReadError = ESP_OK;
  esp_err_t nextWriteError = ESP_OK;
  esp_err_t nextCommitError = ESP_OK;

  bool open = false;
  bool readOnly = true;
  std::string openNamespace;
  std::unordered_map<std::string, TestNvsValue> pending;

  void reset() {
    values.clear();
    nextOpenError = ESP_OK;
    nextReadError = ESP_OK;
    nextWriteError = ESP_OK;
    nextCommitError = ESP_OK;
    open = false;
    readOnly = true;
    openNamespace.clear();
    pending.clear();
  }

  void ensureNamespace(const char* name) { values.try_emplace(name); }

  void setU32(const char* name, const char* key, uint32_t value) {
    values[name][key] = {TestNvsType::U32, value};
  }

  void setU64(const char* name, const char* key, uint64_t value) {
    values[name][key] = {TestNvsType::U64, value};
  }

  uint64_t value(const char* name, const char* key) const {
    return values.at(name).at(key).value;
  }
};

inline TestNvsState testNvs;

inline esp_err_t nvs_open(const char* name, nvs_open_mode_t mode,
                          nvs_handle_t* handle) {
  if (testNvs.nextOpenError != ESP_OK) {
    const esp_err_t error = testNvs.nextOpenError;
    testNvs.nextOpenError = ESP_OK;
    return error;
  }
  if (mode == NVS_READONLY &&
      testNvs.values.find(name) == testNvs.values.end()) {
    return ESP_ERR_NVS_NOT_FOUND;
  }
  testNvs.values.try_emplace(name);
  testNvs.open = true;
  testNvs.readOnly = mode == NVS_READONLY;
  testNvs.openNamespace = name;
  testNvs.pending.clear();
  *handle = 1;
  return ESP_OK;
}

inline void nvs_close(nvs_handle_t) {
  testNvs.open = false;
  testNvs.openNamespace.clear();
  testNvs.pending.clear();
}

inline esp_err_t testNvsGet(nvs_handle_t handle, const char* key,
                            TestNvsType type, uint64_t& value) {
  if (!testNvs.open || handle != 1) return ESP_ERR_NVS_INVALID_HANDLE;
  if (testNvs.nextReadError != ESP_OK) {
    const esp_err_t error = testNvs.nextReadError;
    testNvs.nextReadError = ESP_OK;
    return error;
  }
  const auto& values = testNvs.values.at(testNvs.openNamespace);
  const auto found = values.find(key);
  if (found == values.end()) return ESP_ERR_NVS_NOT_FOUND;
  if (found->second.type != type) return ESP_ERR_NVS_TYPE_MISMATCH;
  value = found->second.value;
  return ESP_OK;
}

inline esp_err_t nvs_get_u32(nvs_handle_t handle, const char* key,
                             uint32_t* value) {
  uint64_t stored = 0;
  const esp_err_t error = testNvsGet(handle, key, TestNvsType::U32, stored);
  if (error == ESP_OK) *value = static_cast<uint32_t>(stored);
  return error;
}

inline esp_err_t nvs_get_u64(nvs_handle_t handle, const char* key,
                             uint64_t* value) {
  uint64_t stored = 0;
  const esp_err_t error = testNvsGet(handle, key, TestNvsType::U64, stored);
  if (error == ESP_OK) *value = stored;
  return error;
}

inline esp_err_t testNvsSet(nvs_handle_t handle, const char* key,
                            TestNvsValue value) {
  if (!testNvs.open || handle != 1) return ESP_ERR_NVS_INVALID_HANDLE;
  if (testNvs.readOnly) return ESP_ERR_NVS_READ_ONLY;
  if (testNvs.nextWriteError != ESP_OK) {
    const esp_err_t error = testNvs.nextWriteError;
    testNvs.nextWriteError = ESP_OK;
    return error;
  }
  testNvs.pending[key] = value;
  return ESP_OK;
}

inline esp_err_t nvs_set_u32(nvs_handle_t handle, const char* key,
                             uint32_t value) {
  return testNvsSet(handle, key, {TestNvsType::U32, value});
}

inline esp_err_t nvs_set_u64(nvs_handle_t handle, const char* key,
                             uint64_t value) {
  return testNvsSet(handle, key, {TestNvsType::U64, value});
}

inline esp_err_t nvs_commit(nvs_handle_t handle) {
  if (!testNvs.open || handle != 1) return ESP_ERR_NVS_INVALID_HANDLE;
  if (testNvs.nextCommitError != ESP_OK) {
    const esp_err_t error = testNvs.nextCommitError;
    testNvs.nextCommitError = ESP_OK;
    return error;
  }
  auto& values = testNvs.values.at(testNvs.openNamespace);
  for (const auto& pending : testNvs.pending) {
    values[pending.first] = pending.second;
  }
  testNvs.pending.clear();
  return ESP_OK;
}
