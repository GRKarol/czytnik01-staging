#include <Arduino.h>
#include <algorithm>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <cerrno>
#if ARDUINO_USB_MODE == 0
#include "tusb.h"
#endif

#include "app/App.h"
#include "board/BoardConfig.h"

App app;

// Arduino core's loopTask (where setup()/loop() run, and where the manual
// "Aktualizacja firmware" button drives OtaUpdater::checkAndInstall()
// synchronously) defaults to an 8192 B stack. The mbedTLS handshake +
// HTTPClient/WiFiClientSecure/HTTPUpdate chain that checkAndInstall() walks
// through already needed 20480 B on a dedicated FreeRTOS task for the
// background auto-check (see kOtaCheckTaskStackBytes in App.cpp, added after
// a stack-overflow panic on that exact call path) — the manual button runs
// the same code plus HTTPUpdate's flash-write buffers on top, on the
// smaller default stack, which is the most likely cause of the freeze/crash
// reported when tapping it. Override the core's weak
// getArduinoLoopTaskStackSize() to give loopTask the same headroom.
size_t getArduinoLoopTaskStackSize(void) { return 24576; }

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

// Double-boot hunt ("the noise shows twice at power-on"). RTC memory keeps
// these through every reset except a real power loss, so a second boot
// right after the first shows up as boot #2 with the reason of the reset.
constexpr uint32_t kBootTraceMagic = 0x464C5752;  // "FLWR"
RTC_NOINIT_ATTR uint32_t gBootTraceMagic;
RTC_NOINIT_ATTR uint32_t gBootsSincePowerOn;
RTC_NOINIT_ATTR uint32_t gLastUptimeMs;

// The last 8 boots also go to NVS, read back over the cable later ("boot"
// on the console) even when no port was open while they happened.
struct BootTraceEntry {
  uint8_t reason;
  uint8_t bootNumber;
  uint16_t flags;  // bit 0: PWR held at boot
  uint32_t previousUptimeMs;
};
constexpr size_t kBootTraceEntries = 8;

void printBootHistory() {
  BootTraceEntry ring[kBootTraceEntries] = {};
  Preferences prefs;
  if (!prefs.begin("boottrace", true)) {
    Serial.println("[boot] no history yet");
    return;
  }
  prefs.getBytes("ring", ring, sizeof(ring));
  prefs.end();
  for (size_t i = 0; i < kBootTraceEntries; ++i) {
    if (ring[i].bootNumber == 0) {
      break;
    }
    Serial.printf("[boot] history %u: boot #%u since power-on, reason %u, PWR %s, previous run %lu ms\n",
                  static_cast<unsigned>(i), ring[i].bootNumber, ring[i].reason,
                  (ring[i].flags & 1U) != 0 ? "held" : "not held",
                  static_cast<unsigned long>(ring[i].previousUptimeMs));
  }
}

void traceBoot(bool pwrButtonHeld) {
  const esp_reset_reason_t reason = esp_reset_reason();
  const uint32_t previousUptimeMs = gBootTraceMagic == kBootTraceMagic ? gLastUptimeMs : 0;
  if (reason == ESP_RST_POWERON || gBootTraceMagic != kBootTraceMagic) {
    gBootsSincePowerOn = 0;
  }
  gBootTraceMagic = kBootTraceMagic;
  ++gBootsSincePowerOn;
  gLastUptimeMs = 0;
  Serial.printf("[boot] #%lu since power-on, reset reason %d, previous run lasted %lu ms\n",
                static_cast<unsigned long>(gBootsSincePowerOn), static_cast<int>(reason),
                static_cast<unsigned long>(previousUptimeMs));

  BootTraceEntry ring[kBootTraceEntries] = {};
  Preferences prefs;
  if (!prefs.begin("boottrace", false)) {
    return;
  }
  prefs.getBytes("ring", ring, sizeof(ring));
  for (size_t i = kBootTraceEntries - 1; i > 0; --i) {
    ring[i] = ring[i - 1];
  }
  ring[0] = {static_cast<uint8_t>(reason), static_cast<uint8_t>(std::min<uint32_t>(gBootsSincePowerOn, 255)),
             static_cast<uint16_t>(pwrButtonHeld ? 1U : 0U), previousUptimeMs};
  prefs.putBytes("ring", ring, sizeof(ring));
  prefs.end();
  printBootHistory();
}

// USBCDC's operator bool only turns true once the host raises DTR and RTS
// together; a terminal or esptool that opens the port with DTR alone (RTS
// low keeps the chip out of its reset sequence) never got past it, and the
// reader went back to sleep after every flash. DTR alone means a PC has the
// port open.
bool usbHostHasPortOpen() {
#if ARDUINO_USB_MODE == 0
  return Serial || tud_cdc_n_connected(0);
#else
  return static_cast<bool>(Serial);
#endif
}

// Cable console for card trouble ("SD write failed" with free space left):
// one command per line on the USB serial port.
//   df          card size and use
//   ls <dir>    files with sizes
//   wt <KB>     write a test file in 4 KB chunks, read it back, delete it
//   boot        the last 8 boots (traceBoot)
void listCardDir(const String &path) {
  File dir = SD_MMC.open(path);
  if (!dir || !dir.isDirectory()) {
    Serial.printf("[con] %s: not a directory\n", path.c_str());
    return;
  }
  uint32_t count = 0;
  for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    Serial.printf("[con] %s%s %lu\n", entry.name(), entry.isDirectory() ? "/" : "",
                  static_cast<unsigned long>(entry.size()));
    ++count;
    entry.close();
  }
  dir.close();
  Serial.printf("[con] %lu entries in %s\n", static_cast<unsigned long>(count), path.c_str());
}

void writeTestCard(uint32_t kilobytes) {
  static constexpr const char *kPath = "/_wtest.bin";
  constexpr size_t kChunk = 4096;
  auto *buffer = static_cast<uint8_t *>(malloc(kChunk));
  if (buffer == nullptr) {
    Serial.println("[con] wt: no memory");
    return;
  }
  for (size_t i = 0; i < kChunk; ++i) buffer[i] = static_cast<uint8_t>(i * 7);
  errno = 0;
  File out = SD_MMC.open(kPath, FILE_WRITE);
  if (!out) {
    Serial.printf("[con] wt: open failed errno=%d\n", errno);
    free(buffer);
    return;
  }
  const uint32_t startedMs = millis();
  uint32_t written = 0;
  uint32_t shortWrites = 0;
  for (uint32_t done = 0; done < kilobytes * 1024U; done += kChunk) {
    errno = 0;
    const size_t n = out.write(buffer, kChunk);
    written += n;
    if (n != kChunk) {
      ++shortWrites;
      Serial.printf("[con] wt: short write %u/%u at %lu B errno=%d\n", static_cast<unsigned>(n),
                    static_cast<unsigned>(kChunk), static_cast<unsigned long>(done), errno);
      if (shortWrites >= 5) break;
    }
  }
  out.close();
  const uint32_t tookMs = millis() - startedMs;
  File in = SD_MMC.open(kPath, FILE_READ);
  const uint32_t onCard = in ? static_cast<uint32_t>(in.size()) : 0;
  if (in) in.close();
  const bool removed = SD_MMC.remove(kPath);
  Serial.printf("[con] wt: wrote %lu B in %lu ms, %lu short, file on card %lu B, removed=%d\n",
                static_cast<unsigned long>(written), static_cast<unsigned long>(tookMs),
                static_cast<unsigned long>(shortWrites), static_cast<unsigned long>(onCard), removed);
  free(buffer);
}

void pollSerialConsole() {
  static String line;
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c != 0x0A && c != 0x0D) {
      if (line.length() < 96) line += c;
      continue;
    }
    line.trim();
    if (line == "df") {
      Serial.printf("[con] card %llu/%llu MB used, type %d\n", SD_MMC.usedBytes() / (1024ULL * 1024ULL),
                    SD_MMC.totalBytes() / (1024ULL * 1024ULL), static_cast<int>(SD_MMC.cardType()));
    } else if (line.startsWith("ls")) {
      String path = line.substring(2);
      path.trim();
      listCardDir(path.isEmpty() ? String("/") : path);
    } else if (line.startsWith("wt")) {
      const long kilobytes = line.substring(2).toInt();
      writeTestCard(kilobytes > 0 ? static_cast<uint32_t>(kilobytes) : 256U);
    } else if (line == "boot") {
      printBootHistory();
    } else if (!line.isEmpty()) {
      Serial.printf("[con] unknown: %s (df, ls <dir>, wt <KB>, boot)\n", line.c_str());
    }
    line = "";
  }
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
  traceBoot(pwrButtonHeld);
  // Skip long serial wait — no need to block boot for 2s.
  delay(20);

  // A software restart (ESP.restart(), e.g. after installing an OTA update
  // or the PWR-button double-tap restart) re-runs this whole function with
  // nobody physically touching PWR, which used to look identical to "cable
  // plugged in just to charge" below and drop the reader straight back into
  // deep sleep with the power hold released — the update would say
  // "restarting" and then the device would just turn off and stay off. Only
  // a genuine cold boot (fresh power-on, no prior running session) should
  // ever second-guess a missing PWR press this way; a SW reset means we were
  // already in a legitimate powered-on session a moment ago and explicitly
  // asked to come back.
  const bool genuineColdBoot = esp_reset_reason() == ESP_RST_POWERON;

  if (!pwrButtonHeld && genuineColdBoot) {
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
      if (usbHostHasPortOpen()) {
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
  gLastUptimeMs = now;
  app.update(now);
  pollSerialConsole();

  // Lag hunt: how busy the UI loop is. One line per 5 s, e.g.
  // "[perf] loop 5 s: 812 passes, longest 420 ms, busy 61%".
  static uint32_t windowStartMs = now;
  static uint32_t passes = 0;
  static uint32_t longestMs = 0;
  static uint32_t busyMs = 0;
  const uint32_t tookMs = millis() - now;
  ++passes;
  busyMs += tookMs;
  longestMs = tookMs > longestMs ? tookMs : longestMs;
  if (now - windowStartMs >= 5000) {
    Serial.printf("[perf] loop 5 s: %lu passes, longest %lu ms, busy %lu%%\n",
                  static_cast<unsigned long>(passes), static_cast<unsigned long>(longestMs),
                  static_cast<unsigned long>(busyMs * 100 / (now - windowStartMs)));
    windowStartMs = now;
    passes = 0;
    longestMs = 0;
    busyMs = 0;
  }
  // Yield to FreeRTOS idle task — allows light sleep between iterations
  // when no work is pending. Saves ~30-40% CPU power in idle states.
  delay(1);
}
