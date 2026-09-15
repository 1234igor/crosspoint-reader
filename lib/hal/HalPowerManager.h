#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <freertos/semphr.h>

#include <cassert>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;

  mutable int lastLightSleepStatus_ = 0;
  mutable int _batteryCachedPercent = 0;         // Last read battery percentage (0-100)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect access to currentLockMode

 public:
#if BOARD_HAS_PSRAM
  static constexpr int LOW_POWER_FREQ = 80;  // MHz
#else
  static constexpr int LOW_POWER_FREQ = 10;  // MHz
#endif
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms
  static constexpr unsigned long BATTERY_POLL_MS = 1500;       // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO& gpio) const;

  // Light-sleep the chip for up to `sliceMs` (timer wake), waking early the
  // moment the power button reaches its pressed level (GPIO level wake). RAM,
  // peripherals, the SD mount and the panel image are all retained; the caller
  // resumes exactly where it was. Returns false WITHOUT sleeping when unsafe:
  // a performance Lock is held (a render is in flight), WiFi is up, or USB is
  // connected (light sleep kills the CDC link). Ported from upstream #2525,
  // which measured idle at 2.8 mA vs 9.7 mA on an X3. Used by the input lock.
  // ignoreUsb: sleep even when the USB heuristic says connected. On the X3
  // "USB" is inferred from the gauge's charge-current sign, which can read
  // positive on battery; the only cost of sleeping on real USB is a dropped
  // serial link. The input lock passes true.
  bool lightSleep(const HalGPIO& gpio, unsigned long sliceMs, bool ignoreUsb = false) const;

  // Deep sleep that keeps the board powered. startDeepSleep() drives GPIO13
  // low on the C3 Xteink boards, which is the battery power-off: the next
  // press is a cold boot (POWERON, RTC RAM lost, full image validation,
  // ~1 s before setup()). The input lock needs a real deep-sleep wake
  // (~100 ms, RTC retained) so its double tap can be judged in time, so it
  // holds GPIO13 HIGH and the panel reset defined, arms the power button and
  // sleeps the chip only. Non-Xteink boards fall back to startDeepSleep().
  // maxSleepUs > 0 also arms a timer wake: the caller caps how long the board
  // stays powered (the input lock turns it into a real power-off at the cap).
  [[noreturn]] void startRetainedDeepSleep(HalGPIO& gpio, uint64_t maxSleepUs = 0) const;
  // Release the pad holds startRetainedDeepSleep() armed. Must run on the
  // wake path BEFORE any bus or storage init: a held pad silently ignores
  // pinMode/SPI muxing, so SD and display init would fail against it.
  static void releaseRetainedSleepHolds();
  // Why the last lightSleep() call declined (for the lock trace): 0 slept,
  // 1 performance lock held, 2 WiFi up, 3 USB connected, else the esp_err_t.
  int lastLightSleepStatus() const { return lastLightSleepStatus_; }

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
