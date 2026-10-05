// tools/nanosim: renders Nano-skin screens to PPM files on the PC.
#include <Arduino.h>
#include <cstdio>
#include <string>

#include "display/DisplayManager.h"

static void dump(const DisplayManager &d, const char *name) {
  const uint16_t *fb = d.frameBuffer();
  const int stride = DisplayManager::frameStride();
  std::string path = std::string("out/") + name + ".ppm";
  FILE *f = fopen(path.c_str(), "wb");
  fprintf(f, "P6\n%d %d\n255\n", BoardConfig::DISPLAY_WIDTH, BoardConfig::DISPLAY_HEIGHT);
  for (int y = 0; y < BoardConfig::DISPLAY_HEIGHT; ++y) {
    for (int x = 0; x < BoardConfig::DISPLAY_WIDTH; ++x) {
      uint16_t v = fb[y * stride + x];
      v = static_cast<uint16_t>((v << 8) | (v >> 8));
      const uint8_t r = ((v >> 11) & 0x1F) * 255 / 31;
      const uint8_t g = ((v >> 5) & 0x3F) * 255 / 63;
      const uint8_t b = (v & 0x1F) * 255 / 31;
      fputc(r, f);
      fputc(g, f);
      fputc(b, f);
    }
  }
  fclose(f);
}

void runScreens(DisplayManager &d, void (*dumpFn)(const DisplayManager &, const char *));
void runPluginScreens(DisplayManager &d, void (*dumpFn)(const DisplayManager &, const char *));
void runUpdateScreens(DisplayManager &d, void (*dumpFn)(const DisplayManager &, const char *));
void runSaverScreens(DisplayManager &d, void (*dumpFn)(const DisplayManager &, const char *));
void runTutorialScreens(DisplayManager &d, void (*dumpFn)(const DisplayManager &, const char *));
void runWizardScreens(DisplayManager &d, void (*dumpFn)(const DisplayManager &, const char *));
void runWizard2Screens(DisplayManager &d, void (*dumpFn)(const DisplayManager &, const char *));
void runWizard3Screens(DisplayManager &d, void (*dumpFn)(const DisplayManager &, const char *));
void runExtrasScreens(DisplayManager &d, void (*dumpFn)(const DisplayManager &, const char *));
void runFontSizeScreens(DisplayManager &d, void (*dumpFn)(const DisplayManager &, const char *));

int main() {
  DisplayManager d;
  d.begin();
  d.setBatteryState(true, 76, false);
  d.setModernCardStyle(true);
  runScreens(d, dump);
  d.setNanoUiFont(0);
  runPluginScreens(d, dump);
  d.setNanoUiFont(0);
  runUpdateScreens(d, dump);
  d.setNanoUiFont(0);
  runSaverScreens(d, dump);
  runTutorialScreens(d, dump);
  runWizardScreens(d, dump);
  runWizard2Screens(d, dump);
  runWizard3Screens(d, dump);
  d.setNanoUiFont(0);
  runExtrasScreens(d, dump);
  runFontSizeScreens(d, dump);
  return 0;
}
