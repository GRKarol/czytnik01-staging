// Host implementations of the hardware hooks DisplayManager touches.
#include <Arduino.h>
#include <SD_MMC.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "display/SdFontLoader.h"
#include "display/axs15231b.h"

SerialStub Serial;
SdMmcStub SD_MMC;

static uint32_t gFakeMillis = 100000;
uint32_t millis() { return gFakeMillis; }
uint32_t micros() { return gFakeMillis * 1000; }
void delay(uint32_t ms) { gFakeMillis += ms; }

void axs15231bInit() {}
void axs15231bSetBacklight(bool) {}
void axs15231bSetBrightnessPercent(uint8_t) {}
void axs15231bSleep() {}
void axs15231bWake() {}
void axs15231bPushColors(uint16_t, uint16_t, uint16_t, uint16_t, const uint16_t *) {}

SdFontLoader::~SdFontLoader() {}
// Reads /fonts/<name>.fnt from $NANOSIM_FONTS on the PC (unset: no SD fonts,
// every SD face falls back to Atkinson as on a card without the pack).
bool SdFontLoader::load(const String &path) {
  unload();
  const char *root = getenv("NANOSIM_FONTS");
  const std::string sdPath = path.c_str();
  const size_t slash = sdPath.find_last_of('/');
  if (root == nullptr || slash == std::string::npos) {
    status_ = Status::FileNotFound;
    return false;
  }
  FILE *f = fopen((std::string(root) + "/" + sdPath.substr(slash + 1)).c_str(), "rb");
  if (f == nullptr) {
    status_ = Status::FileNotFound;
    return false;
  }
  uint8_t header[16];
  if (fread(header, 1, 16, f) != 16) {
    fclose(f);
    status_ = Status::InvalidFormat;
    return false;
  }
  const uint16_t glyphCount = static_cast<uint16_t>(header[10] | (header[11] << 8));
  const uint32_t bitmapLength = header[12] | (header[13] << 8) | (header[14] << 16) | (static_cast<uint32_t>(header[15]) << 24);
  const size_t dataBytes = glyphCount * sizeof(EmbeddedFontGlyph) + bitmapLength;
  buffer_ = static_cast<uint8_t *>(malloc(dataBytes));
  const size_t got = fread(buffer_, 1, dataBytes, f);
  fclose(f);
  if (got != dataBytes) {
    unload();
    status_ = Status::InvalidFormat;
    return false;
  }
  variant_.glyphs = reinterpret_cast<const EmbeddedFontGlyph *>(buffer_);
  variant_.bitmaps = buffer_ + glyphCount * sizeof(EmbeddedFontGlyph);
  variant_.firstChar = header[6];
  variant_.lastChar = header[7];
  variant_.height = header[8];
  status_ = Status::Loaded;
  return true;
}
void SdFontLoader::unload() {
  free(buffer_);
  buffer_ = nullptr;
  variant_ = {};
  status_ = Status::NotLoaded;
}
