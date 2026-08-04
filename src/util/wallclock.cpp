// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "util/wallclock.h"

#include <M5Unified.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

#include <atomic>

#include "net/WifiManager.h"

namespace wallclock {

namespace {
// Anything before 2025-01-01 UTC is treated as "clock not yet usable".
// Catches the default 1970/2000 epoch the system clock starts at and any
// implausible RTC reads (cold board with no backup).
constexpr uint32_t kPlausibleEpoch = 1735689600;  // 2025-01-01T00:00:00Z

bool _initDone = false;
bool _sntpStarted = false;
WifiManager::State _prevWifiState = WifiManager::State::Off;
std::atomic<bool> _ntpJustSynced{false};

void onSntpSync(struct timeval* /*tv*/) {
  // Runs on the SNTP task. Just flag — the next update() tick handles
  // the RTC write off this callback so we don't touch I2C from here.
  _ntpJustSynced.store(true, std::memory_order_release);
}

uint32_t systemEpoch() {
  const time_t now = time(nullptr);
  return now > 0 ? static_cast<uint32_t>(now) : 0;
}

void persistToRtc() {
  if (!M5.Rtc.isEnabled()) return;
  const time_t t = time(nullptr);
  if (t <= 0) return;
  struct tm utc;
  gmtime_r(&t, &utc);
  M5.Rtc.setDateTime(&utc);
  M5_LOGI("clock: NTP sync, persisted to RTC (epoch=%u)",
          static_cast<unsigned>(t));
}
}  // namespace

void initFromRtc() {
  if (_initDone) return;
  _initDone = true;

  sntp_set_time_sync_notification_cb(onSntpSync);

  if (!M5.Rtc.isEnabled()) {
    M5_LOGI("clock: no RTC hardware; waiting for NTP after STA up");
    return;
  }

  // VL (voltage-low) is the BM8563's "I lost backup power" flag. When it's
  // set, the date registers may hold a stale value that happens to land
  // past kPlausibleEpoch — trusting that would stamp shots with bogus
  // startUtcSec until NTP eventually arrives. Skip the seed entirely;
  // utcNow() stays at 0 until NTP completes, and the write-back in
  // update() will clear VL going forward.
  if (M5.Rtc.getVoltLow()) {
    M5_LOGW("clock: RTC voltage-low flag set; contents unreliable, awaiting NTP");
    return;
  }

  M5.Rtc.setSystemTimeFromRtc();
  const uint32_t e = systemEpoch();
  if (e >= kPlausibleEpoch) {
    M5_LOGI("clock: seeded from RTC (epoch=%u)", static_cast<unsigned>(e));
  } else {
    M5_LOGI("clock: RTC reads implausible time, awaiting NTP");
  }
}

uint32_t utcNow() {
  const uint32_t e = systemEpoch();
  return e >= kPlausibleEpoch ? e : 0;
}

bool isSet() { return utcNow() != 0; }

void update() {
  // Fire SNTP exactly once per boot, when STA first goes Connected.
  // configTime's polling mode handles transient drops + reconnects after
  // that, so we don't need to re-trigger on every reconnect.
  const auto s = wifiManager.state();
  if (!_sntpStarted && s == WifiManager::State::StaConnected &&
      _prevWifiState != WifiManager::State::StaConnected) {
    // UTC; we don't track a local offset on-device.
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    _sntpStarted = true;
    M5_LOGI("clock: SNTP started");
  }
  _prevWifiState = s;

  // The notification callback flags this from the SNTP task; we do the
  // RTC write here on the main loop where the I2C bus is already shared
  // with other readers.
  if (_ntpJustSynced.exchange(false, std::memory_order_acq_rel)) {
    persistToRtc();
  }
}

}  // namespace wallclock
