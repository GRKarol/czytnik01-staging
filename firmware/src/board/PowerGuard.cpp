#include "board/PowerGuard.h"

#include <atomic>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board/BoardConfig.h"
#include "display/axs15231b.h"

namespace PowerGuard {

namespace {

constexpr uint32_t kHardResetHoldMs = 10000;
constexpr uint32_t kStuckLoopMs = 3000;
constexpr uint32_t kStuckPowerOffHoldMs = 2000;
constexpr uint32_t kPollMs = 50;
// Waits this long for PWR to be let go before restarting anyway.
constexpr uint32_t kReleaseWaitMs = 30000;
constexpr int kLatchReleaseAttempts = 3;
constexpr uint32_t kTaskStackBytes = 4096;

std::atomic<uint32_t> gLoopAliveMs{0};
std::atomic<bool> gShuttingDown{false};

bool pwrHeld() { return digitalRead(BoardConfig::PIN_PWR_BUTTON) == LOW; }

[[noreturn]] void cutPower(const char *why) {
  Serial.printf("[power-guard] %s: cutting the power\n", why);
  Serial.flush();
  axs15231bSetBacklight(false);
  // The main loop may be stuck in the middle of an I2C transfer on the same
  // bus: try the latch a few times.
  for (int attempt = 0; attempt < kLatchReleaseAttempts; ++attempt) {
    if (BoardConfig::releaseBatteryPowerHold()) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  // PWR itself keeps the battery connected while pressed.
  const uint32_t waitStartMs = millis();
  while (pwrHeld() && millis() - waitStartMs < kReleaseWaitMs) {
    vTaskDelay(pdMS_TO_TICKS(kPollMs));
  }
  // Still running: the USB cable powers the board.
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  for (;;) {
  }
}

void guardTask(void *) {
  bool held = false;
  uint32_t heldSinceMs = 0;
  for (;;) {
    const uint32_t nowMs = millis();
    if (!pwrHeld()) {
      held = false;
    } else {
      if (!held) {
        held = true;
        heldSinceMs = nowMs;
      }
      const uint32_t heldMs = nowMs - heldSinceMs;
      if (heldMs >= kHardResetHoldMs) {
        cutPower("PWR held 10 s");
      }
      const uint32_t aliveMs = gLoopAliveMs.load();
      if (!gShuttingDown.load() && aliveMs != 0 && heldMs >= kStuckPowerOffHoldMs &&
          nowMs - aliveMs >= kStuckLoopMs) {
        cutPower("main loop stuck, PWR held");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(kPollMs));
  }
}

}  // namespace

void begin() {
  static bool started = false;
  if (started) {
    return;
  }
  started = true;
  xTaskCreatePinnedToCore(guardTask, "pwr_guard", kTaskStackBytes, nullptr, configMAX_PRIORITIES - 2, nullptr,
                          tskNO_AFFINITY);
}

void mainLoopAlive(uint32_t nowMs) { gLoopAliveMs.store(nowMs == 0 ? 1 : nowMs); }

void setShuttingDown(bool shuttingDown) { gShuttingDown.store(shuttingDown); }

}  // namespace PowerGuard
