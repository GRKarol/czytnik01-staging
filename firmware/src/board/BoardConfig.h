#pragma once

#include <Arduino.h>

namespace BoardConfig {

enum class UiOrientation : uint8_t {
  Landscape = 0,
  LandscapeFlipped,
  Portrait,
  PortraitFlipped,
};

constexpr int PIN_BOOT_BUTTON = 0;
constexpr int PIN_PWR_BUTTON = 16;
constexpr int PIN_BATTERY_ADC = 4;

constexpr int PIN_LCD_CS = 9;
constexpr int PIN_LCD_SCLK = 10;
constexpr int PIN_LCD_DATA0 = 11;
constexpr int PIN_LCD_DATA1 = 12;
constexpr int PIN_LCD_DATA2 = 13;
constexpr int PIN_LCD_DATA3 = 14;
constexpr int PIN_LCD_RST = 21;
constexpr int PIN_LCD_BACKLIGHT = 8;

constexpr int PANEL_NATIVE_WIDTH = 172;
constexpr int PANEL_NATIVE_HEIGHT = 640;
constexpr int DISPLAY_WIDTH = 640;
constexpr int DISPLAY_HEIGHT = 172;
constexpr bool UI_ROTATED_180 = true;  // Keep BOOT/PWR at the top edge in landscape.

constexpr int PIN_SD_CLK = 41;
constexpr int PIN_SD_CMD = 39;
constexpr int PIN_SD_D0 = 40;
constexpr int PIN_I2C_SDA = 47;
constexpr int PIN_I2C_SCL = 48;
constexpr int PIN_TOUCH_SDA = 17;
constexpr int PIN_TOUCH_SCL = 18;

constexpr int TCA9554_ADDRESS = 0x20;
constexpr uint8_t TCA9554_PIN_BATTERY_ADC_ENABLE = 1;
constexpr uint8_t TCA9554_PIN_SYS_EN = 6;
constexpr uint8_t TCA9554_PIN_AUDIO_ENABLE = 7;

constexpr int PIN_AUDIO_MCLK = 7;
constexpr int PIN_AUDIO_BCLK = 15;
constexpr int PIN_AUDIO_WS = 46;
constexpr int PIN_AUDIO_DIN = 6;
constexpr int PIN_AUDIO_DOUT = 45;
constexpr uint8_t ES8311_ADDRESS = 0x18;

// Waveshare's own reference firmware for this board (S3_LCD_3_49 in their
// codec_board component, github.com/waveshareteam/ESP32-S3-Touch-LCD-3.49)
// confirms the mic path never runs through the ES8311 at all: this board has
// two codecs sharing one I2S bus (same MCLK/BCLK/WS/DIN/DOUT pins as above)
// — ES8311 for DAC/speaker output only, and a separate ES7210 ADC codec for
// microphone input. Every previous ES8311 ADC-register fix (0x15/0x16/0x17/
// 0x1B/0x1C) was chasing a chip that was never wired to a live microphone
// signal on this board, which is why Pzm stayed at 0% regardless.
constexpr uint8_t ES7210_ADDRESS = 0x40;

struct BatteryStatus {
  bool present = false;
  float voltage = 0.0f;
  uint8_t percent = 0;
};

bool begin();
void lightSleepUntilBootButton();
void holdBacklightOffForDeepSleep();
void enablePwrButtonExt0Wakeup();
bool readBatteryStatus(BatteryStatus &status);
bool releaseBatteryPowerHold();

// Serializes every Wire1 transaction (TCA9554 IO expander, ES8311 codec and
// QMI8658 IMU all share this one bus). AudioRecorder's record/playback
// tasks run pinned to core 0 while the rest of the app (battery polling,
// IMU reads, AudioManager beeps) runs on core 1 — two real cores means
// Wire1 calls from both can execute at literally the same instant. Besides
// corrupting TwoWire's internal transaction state, that turns a
// read-modify-write on the TCA9554 output register (audio-enable,
// battery-ADC-gate and SYS_EN packed into one byte) into a lost update
// that silently clears a bit another task just set — which is exactly
// what was cutting the speaker mid-playback. Recursive so a function that
// already holds the lock can safely call another locking function.
class I2cBusLock {
 public:
  I2cBusLock();
  ~I2cBusLock();
  I2cBusLock(const I2cBusLock &) = delete;
  I2cBusLock &operator=(const I2cBusLock &) = delete;

 private:
  bool acquired_ = false;
};

}  // namespace BoardConfig
