// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include <M5Unified.h>

#include <vector>

#include "ButtonInput.h"
#include "MainNavigation.h"
#include "apps/extraction/ExtractionApp.h"
#include "apps/onboarding/OnboardingScreen.h"
#include "apps/wifi/WifiStatusScreen.h"
#include "ble/BleScaleService.h"
#include "diagnostics/DiagnosticsModule.h"
#include "diagnostics/HeapMonitor.h"
#include "diagnostics/PanicDump.h"
#include "diagnostics/PowerEventLog.h"
#include "diagnostics/reset_reason.h"
#include "net/HttpServer.h"
#include "net/NetworkServicesHost.h"
#include "net/SseServer.h"
#include "net/WifiManager.h"
#include "power/PowerManager.h"
#include "ui/Menu.h"
#include "ui/Screen.h"
#include "ui/UiHost.h"
#include "ui/sounds.h"
#include "ui/theme.h"
#include "util/i2c_lock.h"
#include "util/orientation.h"
#include "util/power.h"
#include "util/storage.h"
#include "util/wallclock.h"

DeviceOrientation orientation;
namespace power {
PowerManager powerManager{bleScale};
}  // namespace power

ExtractionApp extractionApp;
OnboardingScreen onboardingScreen;
WifiStatusScreen wifiStatusScreen;

Menu mainMenu;
DiagnosticsModule diagnosticsModule;
MainNavigation mainNavigation{mainMenu, extractionApp.extractionScreen(),
                              onboardingScreen,
                              diagnosticsModule.storageRecoveryScreen()};
NetworkServicesHost onlineServices;
UiHost uiHost{orientation};
UiDebugRemote uiDebugRemote{uiHost};
ButtonInput buttonInput{uiDebugRemote};

void requestDraw() { uiHost.requestDraw(); }

void setup() {
  auto cfg = M5.config();
  // M5Unified enables the external 5 V output unless the application opts out.
  // This product does not power IR or external hardware from that rail.
  cfg.output_power = false;
  M5.begin(cfg);

  buttonInput.begin();

  M5.Log.setLogLevel(m5::log_target_serial, ESP_LOG_VERBOSE);
  M5.Log.setLogLevel(m5::log_target_display, ESP_LOG_ERROR);

  M5.Display.setBrightness(255);

  theme::setLightMode(true);

  M5_LOGI("START firmware=%s", PB_FIRMWARE_VERSION);
  diagnostics::logResetReason();
  sounds::warmUpSpeaker();

  // Install the allocator's failed-alloc callback as early as possible so any
  // allocation failure from here on is counted (see HeapMonitor). Cheap and
  // idempotent; the periodic sampling is driven from loop().
  heapMonitor.begin();

  // Allocate the host-canvas backing buffer now, while internal SRAM is still
  // unfragmented (before LittleFS / BLE / Wi-Fi run).
  if (!uiHost.allocateCanvas()) {
    M5_LOGE("hostCanvas: backing buffer allocation failed");
  }

  power::disableWakeUpOnMotion();
  power::configurePowerButton();

  // Storage reset and first-use initialization run before network and UI tasks
  // can access LittleFS. A damaged initialized volume remains untouched and is
  // presented as recovery by MainNavigation.
  storage::mount();
  extractionApp.begin();

  // Seed wall clock from RTC (if present); SNTP fires later in loop()
  // once STA is up.
  wallclock::initFromRtc();

  // Power-event log: load the persisted ring, then record this boot as a Wake.
  // Done right after the wall clock is seeded so the event carries a real
  // timestamp when an RTC is present; the power module owns the rest (battery
  // state, reset reason).
  powerEventLog.begin();
  power::recordWakeEvent();

  bleScale.setControllerStartedCallback([](uint32_t generation) {
    wifiManager.scheduleBluetoothConditioning(generation);
  });
  bleScale.setStartupRequired(true);

  // Register every route before network startup can bring HTTP and SSE online.
  onlineServices.registerBuiltInRoutes();
  diagnostics::registerPanicRoute(httpServer);
  extractionApp.registerWith(httpServer, sseServer);
  diagnosticsModule.begin(httpServer);
  onlineServices.startIfConfigured();

  // Let any connected extraction-web client know before we deep sleep,
  // so it can enter wake polling instead of waiting on heartbeats.
  power::powerManager.setPreSleepCallback(
      []() { extractionApp.notifySleeping(); });

  std::vector<Menu::Item> mainMenuItems{
      Menu::Item::open("Set Target", extractionApp.setTargetScreen()),
      Menu::Item::open("Reset Counter", extractionApp.resetShotCounterScreen()),
      Menu::Item::open("Wi-Fi", wifiStatusScreen),
  };
  mainMenuItems.push_back(
      Menu::Item::open("Diagnostics", diagnosticsModule.menu()));
  mainMenuItems.push_back(Menu::Item::open("Tips", onboardingScreen));
  mainMenu.init("PUMP BUG", mainMenuItems);

  // Surface an unacknowledged crash: if a core dump is on record (set on the
  // last panic, cleared only by the user), raise the status-bar crash marker.
  // Read once at boot — it survives clean reboots until the dump is cleared.
  uiHost.setChromeAlert(diagnostics::hasPanicDump());

  if (!power::powerManager.setScaleRadioPolicy(
          power::ScaleRadioPolicy::ForceRunning)) {
    M5_LOGE("Scale Bluetooth controller failed to start");
  }
  uiHost.begin(mainNavigation);
}

void loop() {
  // Shared-bus contention with the BMI270 FIFO task can corrupt PM1 reads.
  // M5Unified does not use I2C from update() on this board currently, so this
  // lock just protects against that changing in the future.
  {
    I2cLock lock;
    M5.update();
  }

  const button::Event buttonEvent = buttonInput.poll();
  bleScale.updateStartup(millis());

  const bool orientationChanged = orientation.update();
  if (orientationChanged) {
    power::powerManager.notifyActivity();
    orientation.rotateDisplay();
  }

  power::powerManager.update();
  onlineServices.update();
  // wall-clock observes the freshly updated Wi-Fi state and starts SNTP when
  // STA first reaches connected.
  wallclock::update();

  // A crash report was cleared (here on-device, or over the web on another
  // task): resync the status-bar crash marker from the source of truth.
  // Rare, so the flash read in hasPanicDump() is fine.
  if (diagnostics::consumePanicAlertResync())
    uiHost.setChromeAlert(diagnostics::hasPanicDump());

  uiHost.update({buttonEvent, orientationChanged},
                onlineServices.networkStatus());

  // Heap diagnostics: sample free/largest-block, bucket the worst case over
  // time, and count allocation failures. Internally rate-limited, so calling it
  // every loop is cheap. Exposed via the logs web page (?log=heap) and the
  // on-device Logs HEAP page.
  heapMonitor.tick();

  const uint32_t loopDelayMs = power::powerManager.loopDelayMs();
  if (loopDelayMs) delay(loopDelayMs);
}
