// Reading-size check: the same words in several typefaces at Large, Medium
// and Small. Run with NANOSIM_FONTS pointing at a folder of .fnt files to
// include the card fonts.
#include <string>

#include "display/DisplayManager.h"

void runFontSizeScreens(DisplayManager &d, void (*dump)(const DisplayManager &, const char *)) {
  const struct {
    DisplayManager::ReaderTypeface face;
    const char *name;
  } faces[] = {
      {DisplayManager::ReaderTypeface::Standard, "standard"},
      {DisplayManager::ReaderTypeface::OpenDyslexic, "opendyslexic"},
      {DisplayManager::ReaderTypeface::AtkinsonHyperlegible, "atkinson"},
      {DisplayManager::ReaderTypeface::Literata, "literata"},
      {DisplayManager::ReaderTypeface::Cardo, "cardo"},
  };
  const char *levels[] = {"large", "medium", "small"};
  for (const auto &face : faces) {
    DisplayManager::TypographyConfig config;
    config.typeface = face.face;
    d.setTypographyConfig(config);
    for (uint8_t level = 0; level < 3; ++level) {
      d.renderPhantomRsvpWord("was", "Hexameter", "quickly", level, "Rozdzial 3", 42, true);
      dump(d, (std::string("size_") + face.name + "_" + levels[level]).c_str());
    }
  }
}
