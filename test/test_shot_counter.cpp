// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include <nvs.h>

#include <cassert>
#include <cstdio>
#include <limits>

#include "apps/extraction/ShotCounter.h"

void testM5LogInfo(const char*, ...) {}
void testM5LogWarning(const char*, ...) {}
void testM5LogError(const char*, ...) {}

int main() {
  testNvs.reset();
  pump_scale::ShotCounter counter;
  assert(counter.load());
  assert(counter.value() == 0);

  assert(counter.increment());
  assert(counter.value() == 1);
  assert(testNvs.value("shotcount", "count") == 1);

  testNvs.nextWriteError = ESP_FAIL;
  assert(!counter.increment());
  assert(counter.value() == 2);
  assert(testNvs.value("shotcount", "count") == 1);
  assert(counter.increment());
  assert(counter.value() == 3);
  assert(testNvs.value("shotcount", "count") == 3);

  testNvs.nextCommitError = ESP_FAIL;
  assert(!counter.increment());
  assert(counter.value() == 4);
  assert(testNvs.value("shotcount", "count") == 3);
  assert(counter.increment());
  assert(counter.value() == 5);
  assert(testNvs.value("shotcount", "count") == 5);

  testNvs.nextCommitError = ESP_FAIL;
  assert(!counter.reset());
  assert(counter.value() == 5);
  assert(counter.reset());
  assert(counter.value() == 0);

  testNvs.setU64("shotcount", "count", 7);
  pump_scale::ShotCounter openFailure;
  testNvs.nextOpenError = ESP_FAIL;
  assert(!openFailure.load());
  assert(openFailure.value() == 0);
  assert(openFailure.load());
  assert(openFailure.value() == 7);

  pump_scale::ShotCounter readFailure;
  testNvs.nextReadError = ESP_FAIL;
  assert(!readFailure.load());
  assert(readFailure.value() == 0);
  assert(readFailure.load());
  assert(readFailure.value() == 7);

  testNvs.setU32("shotcount", "count", 7);
  pump_scale::ShotCounter typeMismatch;
  assert(!typeMismatch.load());

  testNvs.values.clear();
  testNvs.ensureNamespace("shotcount");
  pump_scale::ShotCounter missingKey;
  assert(missingKey.load());
  assert(missingKey.value() == 0);

  testNvs.setU64("shotcount", "count", std::numeric_limits<uint64_t>::max());
  pump_scale::ShotCounter saturated;
  assert(saturated.load());
  testNvs.nextWriteError = ESP_FAIL;
  assert(saturated.increment());
  assert(testNvs.nextWriteError == ESP_FAIL);

  std::puts("OK: shot counter NVS contracts");
  return 0;
}
