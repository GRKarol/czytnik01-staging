#include <Arduino.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <driver/gpio.h>

#include "app/App.h"
#include "board/BoardConfig.h"

App app;

namespace {

// Diagnostic only. On this board, power-on is a hardware cold boot done by
// the SYS_EN latch on the power-management chip (see BoardConfig::
// releaseBatteryPowerHold) before the ESP32 ever runs — by the time this
// line executes, the decision to boot has already been made in hardware.
// Serial logs confirm resetReason is POWERON or SW every time; wakeup cause
// is never EXT0 in practice, so there is no software hook available here to
// gate "hold long enough to power on." A prior attempt at that
// (requirePowerOnHoldOrResleep) never actually fired and was removed.
void logResetReason() {
  Serial.printf("[main] resetReason=%d wakeup cause=%d pwrPin=%d\n",
                static_cast<int>(esp_reset_reason()),
                static_cast<int>(esp_sleep_get_wakeup_cause()),
                digitalRead(BoardConfig::PIN_PWR_BUTTON));
  Serial.flush();
}

}  // namespace

// Called very early by ESP-IDF before app_main/setup.
// Forces backlight pin HIGH (off, active-low) at the hardware level
// to prevent pixel noise from being visible during boot.
extern "C" void app_main_early_init() __attribute__((constructor));
void app_main_early_init() {
  gpio_reset_pin(static_cast<gpio_num_t>(BoardConfig::PIN_LCD_BACKLIGHT));
  gpio_set_direction(static_cast<gpio_num_t>(BoardConfig::PIN_LCD_BACKLIGHT), GPIO_MODE_OUTPUT);
  gpio_set_level(static_cast<gpio_num_t>(BoardConfig::PIN_LCD_BACKLIGHT), 1);
}

void setup() {
  // Redundant backlight off — the constructor above should have done this
  // but ensure it stays off through Arduino init.
  pinMode(BoardConfig::PIN_LCD_BACKLIGHT, OUTPUT);
  digitalWrite(BoardConfig::PIN_LCD_BACKLIGHT, HIGH);

  Serial.begin(115200);
  esp_log_level_set("*", ESP_LOG_INFO);
  const bool pwrButtonHeld = BoardConfig::begin();
  logResetReason();
  // Skip long serial wait — no need to block boot for 2s.
  delay(20);

  if (!pwrButtonHeld) {
    // Booted without PWR being pressed - normally this means the USB cable
    // was plugged in just to charge, so the reader drops straight back into
    // the same deep-sleep "off" state a normal power-off uses (see below).
    //
    // But that decision used to happen within ~20ms of boot (this whole
    // block plus the delay() above) — far faster than Windows can enumerate
    // a USB CDC port and a terminal can open it, so plugging the cable in
    // specifically to capture logs over Serial always got nothing: the
    // device was already back in deep sleep before the host side was even
    // ready to read. Give a real host a grace window to attach first. A dumb
    // USB power brick never enumerates CDC's data lines, so Serial's DTR
    // line-state (USBCDC::operator bool()) only goes true when an actual PC
    // has the port open — that's a reliable "this is a deliberate tethered
    // session, not silent charging" signal.
    constexpr uint32_t kUsbHostGraceMs = 2500;
    constexpr uint32_t kUsbHostPollMs = 100;
    bool hostAttached = false;
    for (uint32_t waited = 0; waited < kUsbHostGraceMs; waited += kUsbHostPollMs) {
      if (Serial) {
        hostAttached = true;
        break;
      }
      delay(kUsbHostPollMs);
    }

    if (hostAttached) {
      // A PC opened the port before we gave up — boot normally so touch and
      // the app (dictaphone included) are usable while logs stream out. Note
      // this skips BoardConfig::holdBatteryPowerIfAvailable() (only armed
      // when PWR is physically held), so unplugging the cable here cuts
      // power immediately instead of continuing on battery.
      Serial.println("[main] USB host attached without PWR press; booting for tethered testing "
                      "(no battery latch — unplugging cuts power immediately)");
    } else {
      Serial.println("[main] no PWR press at boot (charging); staying off");
      Serial.flush();
      BoardConfig::holdBacklightOffForDeepSleep();
      BoardConfig::releaseBatteryPowerHold();
      BoardConfig::enablePwrButtonExt0Wakeup();
      esp_deep_sleep_start();
    }
  }

  Serial.println("[main] app setup");
  app.begin();
}

void loop() {
  const uint32_t now = millis();
  app.update(now);
  // Yield to FreeRTOS idle task — allows light sleep between iterations
  // when no work is pending. Saves ~30-40% CPU power in idle states.
  delay(1);
}
