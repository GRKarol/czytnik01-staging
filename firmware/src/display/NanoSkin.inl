// Nano skin — DisplayManager's rendering of the Modern nav mode: flat
// rounded tiles, a tab rail, one accent color, proportional UI fonts.
//
// Included at the bottom of DisplayManager.cpp rather than compiled on its
// own: it shares that file's anonymous-namespace helpers (panelColor(), the
// reader glyph tables and layout math, the font-picker typeface override)
// and frame buffer constants. Shapes follow Arduino_GFX's algorithms (the
// library rsvpnano draws with) so corners and circles land on the same
// pixels as their UI; text goes through display/NanoUiFonts.h.

#include "display/NanoUiFonts.h"

namespace {

constexpr int kNanoScreenW = kDisplayWidth;
constexpr int kNanoScreenH = kDisplayHeight;
constexpr int kNanoRadius = 8;

constexpr uint16_t nanoRgb(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

// `alpha` (0-255) of `src` over `dst`, both plain RGB565.
inline uint16_t nanoMix565(uint16_t dst, uint16_t src, uint8_t alpha) {
  const uint32_t inv = 255U - alpha;
  const uint32_t r = (((src >> 11) & 0x1F) * alpha + ((dst >> 11) & 0x1F) * inv + 127) / 255U;
  const uint32_t g = (((src >> 5) & 0x3F) * alpha + ((dst >> 5) & 0x3F) * inv + 127) / 255U;
  const uint32_t b = ((src & 0x1F) * alpha + (dst & 0x1F) * inv + 127) / 255U;
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

// Active UI family for the static metric helpers (mirrors nanoUiFont_).
uint8_t gNanoUiFamily = 0;

uint8_t nanoStrikeForSize(uint8_t size) { return size <= 1 ? 0 : (size == 2 ? 1 : 2); }

const NanoUiStrike &nanoStrike(uint8_t family, uint8_t strike) {
  if (family >= kNanoUiFamilyCount) {
    family = 0;
  }
  return kNanoUiFamilies[family].strikes[std::min<uint8_t>(strike, 2)];
}

const NanoUiGlyph &nanoGlyphFor(const NanoUiStrike &strike, char c) {
  uint8_t value = LatinText::byteValue(c);
  if (value < kNanoUiFirstByte) {
    value = LatinText::fallbackAsciiByte(value);
    if (value < kNanoUiFirstByte) {
      value = '?';
    }
  }
  return strike.glyphs[value - kNanoUiFirstByte];
}

// Width in 1/16 px of text[from, to).
int32_t nanoRunWidth16(const NanoUiStrike &strike, const String &text, size_t from = 0,
                       size_t to = SIZE_MAX) {
  to = std::min<size_t>(to, text.length());
  int32_t width = 0;
  for (size_t i = from; i < to; ++i) {
    width += nanoGlyphFor(strike, text[i]).advance;
  }
  return width;
}

int nanoRunWidth(const NanoUiStrike &strike, const String &text) {
  return static_cast<int>((nanoRunWidth16(strike, text) + 15) / 16);
}

bool nanoTrimmableTail(char c) { return c == ' ' || c == ',' || c == ':' || c == '-' || c == ';'; }

String nanoEllipsize(const NanoUiStrike &strike, const String &text, int maxWidth) {
  if (maxWidth <= 0) {
    return String();
  }
  if (nanoRunWidth(strike, text) <= maxWidth) {
    return text;
  }
  const String ellipsis(kNanoUiEllipsisByte);
  const int32_t budget = static_cast<int32_t>(maxWidth) * 16 - nanoRunWidth16(strike, ellipsis);
  if (budget <= 0) {
    return String();
  }
  size_t keep = 0;
  int32_t width = 0;
  while (keep < text.length()) {
    const int32_t next = width + nanoGlyphFor(strike, text[keep]).advance;
    if (next > budget) {
      break;
    }
    width = next;
    ++keep;
  }
  while (keep > 0 && nanoTrimmableTail(text[keep - 1])) {
    --keep;
  }
  return keep == 0 ? ellipsis : text.substring(0, keep) + ellipsis;
}

// Greedy word wrap into at most maxLines lines; the last one keeps whatever
// is left, ellipsized. A single word wider than the line is ellipsized on
// its own line instead of being split mid-word.
std::vector<String> nanoWrap(const NanoUiStrike &strike, const String &text, int maxWidth,
                             uint8_t maxLines) {
  std::vector<String> lines;
  String rest = text;
  rest.trim();
  while (!rest.isEmpty() && lines.size() < maxLines) {
    const int newline = rest.indexOf('\n');
    const String paragraph = newline >= 0 ? rest.substring(0, newline) : rest;
    const bool last = lines.size() + 1 == maxLines;
    if (nanoRunWidth(strike, paragraph) <= maxWidth && !(last && newline >= 0)) {
      lines.push_back(paragraph);
      rest = newline >= 0 ? rest.substring(newline + 1) : String();
      rest.trim();
      continue;
    }
    if (last) {
      String flat = rest;
      flat.replace("\n", " ");
      lines.push_back(nanoEllipsize(strike, flat, maxWidth));
      break;
    }
    int breakAt = -1;
    int32_t width = 0;
    const int32_t budget = static_cast<int32_t>(maxWidth) * 16;
    for (size_t i = 0; i < paragraph.length(); ++i) {
      if (paragraph[i] == ' ' && width <= budget) {
        breakAt = static_cast<int>(i);
      }
      width += nanoGlyphFor(strike, paragraph[i]).advance;
      if (width > budget && breakAt >= 0) {
        break;
      }
    }
    if (breakAt <= 0) {
      const int space = paragraph.indexOf(' ');
      const String word = space > 0 ? paragraph.substring(0, space) : paragraph;
      lines.push_back(nanoEllipsize(strike, word, maxWidth));
      rest = rest.substring(word.length());
    } else {
      lines.push_back(paragraph.substring(0, breakAt));
      rest = rest.substring(breakAt + 1);
    }
    rest.trim();
  }
  return lines;
}

String nanoPercentLabel(uint8_t percent) { return String(static_cast<unsigned>(percent)) + "%"; }

uint8_t nanoLuma(uint16_t color) {
  const uint32_t r = ((color >> 11) & 0x1F) * 255 / 31;
  const uint32_t g = ((color >> 5) & 0x3F) * 255 / 63;
  const uint32_t b = (color & 0x1F) * 255 / 31;
  return static_cast<uint8_t>((r * 299 + g * 587 + b * 114) / 1000);
}

// Text in `color` over `background` that stays legible: a dark accent
// (pure blue on black) is lifted toward white, a pale one on a light
// background pushed toward black. Fills keep the raw accent.
uint16_t nanoReadable(uint16_t color, uint16_t background) {
  const int fg = nanoLuma(color);
  const int bg = nanoLuma(background);
  if (std::abs(fg - bg) >= 100) {
    return color;
  }
  return bg < 128 ? nanoMix565(color, 0xFFFF, 110) : nanoMix565(color, 0x0000, 110);
}

}  // namespace

// ─── State ──────────────────────────────────────────────────────────────────

void DisplayManager::setBatteryState(bool present, uint8_t percent, bool charging) {
  percent = std::min<uint8_t>(percent, 100);
  if (present != batteryPresent_ || percent != batteryPercent_ || charging != batteryCharging_) {
    // The classic reader screens cache on a render key; make them repaint
    // the indicator (the Nano frames compare pixels anyway).
    lastRenderKey_ = "";
  }
  batteryPresent_ = present;
  batteryPercent_ = percent;
  batteryCharging_ = charging;
}

int DisplayManager::frameStride() { return kVirtualBufferWidth; }

uint8_t DisplayManager::nanoUiFontCount() { return kNanoUiFamilyCount; }

const char *DisplayManager::nanoUiFontName(uint8_t family) {
  return kNanoUiFamilies[family < kNanoUiFamilyCount ? family : 0].name;
}

void DisplayManager::setNanoUiFont(uint8_t family) {
  if (family >= kNanoUiFamilyCount) {
    family = 0;
  }
  if (family == nanoUiFont_ && family == gNanoUiFamily) {
    return;
  }
  nanoUiFont_ = family;
  gNanoUiFamily = family;
  lastRenderKey_ = "";
}

uint8_t DisplayManager::nanoUiFont() const { return nanoUiFont_; }

// Fixed palettes, colors in NanoRole order (Background, Foreground, Muted,
// Subtle, Accent, OnAccent, SurfaceMuted, SurfaceActive, Outline,
// ProgressTrack). Where an rsvpnano theme's accentBar differs from its
// accent (Nord, Gruvbox, Tokyo Night, Solarized), the bar color is used as
// the accent: here the accent fills whole tiles and sliders, which the
// softer bar color carries better than their red focus-letter color.
namespace {

struct NanoPaletteDef {
  const char *name;
  uint16_t colors[10];
};

constexpr NanoPaletteDef kNanoPalettes[] = {
    {"Mocha", {0x18E5, 0xCEBE, 0xA579, 0x7C33, 0xF455, 0x1083, 0x3188, 0x422B, 0x5ACE, 0x3188}},  // rsvpnano catppuccin-mocha
    {"Macchiato", {0x2127, 0xCE9E, 0xA579, 0x8434, 0xEC32, 0x18C4, 0x31C9, 0x4A6C, 0x5B0F, 0x31C9}},  // rsvpnano catppuccin-macchiato
    {"Frappe", {0x31A8, 0xC69E, 0xA579, 0x8454, 0xE410, 0x2126, 0x422B, 0x52AD, 0x6350, 0x422B}},  // rsvpnano catppuccin-frappe
    {"Latte", {0xEF9E, 0x4A6D, 0x6B70, 0x8C74, 0xD067, 0xEF9E, 0xCE9B, 0xBE19, 0x9D16, 0xCE9B}},  // rsvpnano catppuccin-latte
    {"Dracula", {0x2946, 0xFFDE, 0xBDF7, 0x6394, 0xFBD8, 0x2946, 0x422B, 0x6394, 0x6394, 0x422B}},  // rsvpnano dracula
    {"Nord", {0x29A8, 0xEF7E, 0xDEFD, 0x8518, 0x8E1A, 0x29A8, 0x426B, 0x4AAD, 0x8518, 0x426B}},  // rsvpnano nord
    {"Gruvbox", {0x2945, 0xEED6, 0xACD0, 0x940E, 0xFDE5, 0x2945, 0x39C6, 0x5248, 0x62EA, 0x39C6}},  // rsvpnano gruvbox-dark
    {"Tokyo", {0x18C4, 0xC65E, 0x9D39, 0x52F1, 0x7D1E, 0x18C4, 0x2968, 0x3A0C, 0x52F1, 0x2968}},  // rsvpnano tokyo-night
    {"Solarized", {0x0146, 0xFFBC, 0x9514, 0x84B2, 0x245A, 0x0146, 0x09E9, 0x124A, 0x5B6E, 0x01A8}},  // rsvpnano solarized-dark
    {"Cream", {0xFFDB, 0x18C2, 0x6B4A, 0x83EC, 0x0373, 0xFFFF, 0xEEF5, 0xDE73, 0x9CAD, 0xDE73}},  // rsvpnano dyslexic
    {"Sepia", {0xF77B, 0x3965, 0x7B4B, 0x8BCC, 0xB2A5, 0xFFFF, 0xEEF8, 0xDE54, 0xACAF, 0xDE54}},  // ours: warm paper
    {"Graphite", {0x0000, 0xE73C, 0x8C51, 0x5AEB, 0x4D1F, 0x0000, 0x18E3, 0x2965, 0x39C7, 0x2124}},  // ours: OLED black, grey tiles
    {"Forest", {0x1924, 0xDF3B, 0x9D93, 0x6C2E, 0x7E2F, 0x1102, 0x2185, 0x3227, 0x4B0A, 0x2185}},  // ours: dark green
};
constexpr uint8_t kNanoFixedPaletteCount = sizeof(kNanoPalettes) / sizeof(kNanoPalettes[0]);

}  // namespace

uint8_t DisplayManager::nanoPaletteCount() { return kNanoFixedPaletteCount + 1; }

const char *DisplayManager::nanoPaletteName(uint8_t palette) {
  if (palette == kNanoPaletteClassic || palette > kNanoFixedPaletteCount) {
    return "Classic";
  }
  return kNanoPalettes[palette - 1].name;
}

bool DisplayManager::nanoPaletteSwatch(uint8_t palette, uint16_t &background, uint16_t &foreground,
                                       uint16_t &accent) {
  if (palette == kNanoPaletteClassic || palette > kNanoFixedPaletteCount) {
    return false;
  }
  const NanoPaletteDef &def = kNanoPalettes[palette - 1];
  background = def.colors[static_cast<uint8_t>(NanoRole::Background)];
  foreground = def.colors[static_cast<uint8_t>(NanoRole::Foreground)];
  accent = def.colors[static_cast<uint8_t>(NanoRole::Accent)];
  return true;
}

void DisplayManager::setNanoPalette(uint8_t palette, bool ownAccent) {
  if (palette > kNanoFixedPaletteCount) {
    palette = kNanoPaletteClassic;
  }
  if (palette == nanoPalette_ && ownAccent == nanoOwnAccent_) {
    return;
  }
  nanoPalette_ = palette;
  nanoOwnAccent_ = ownAccent;
  lastRenderKey_ = "";
}

uint16_t DisplayManager::nanoPaletteColor(uint8_t palette, NanoRole role) const {
  if (palette != kNanoPaletteClassic && palette <= kNanoFixedPaletteCount) {
    return kNanoPalettes[palette - 1].colors[static_cast<uint8_t>(role)];
  }
  // Dark = rsvpnano's default theme, Light = their light.toml (background
  // kept at this firmware's softer 0xDEDA instead of pure white, so the
  // surfaces are shifted to stay distinguishable on it), Night = their
  // night.toml. Accent is always the user's highlight color.
  const bool night = nightMode_;
  const bool light = !night && !darkMode_;
  switch (role) {
    case NanoRole::Background:
      return backgroundColor();
    case NanoRole::Foreground:
      return wordColor();
    case NanoRole::Muted:
      return night ? nanoRgb(0x7A, 0x4A, 0x00) : light ? nanoRgb(0x5E, 0x5E, 0x5E) : nanoRgb(0x8C, 0x8C, 0x8C);
    case NanoRole::Subtle:
      return night ? nanoRgb(0x8A, 0x53, 0x00) : light ? nanoRgb(0x5A, 0x5A, 0x5A) : 0x528A;
    case NanoRole::Accent:
      return focusColor();
    case NanoRole::OnAccent:
      return night ? 0x0000 : 0xFFFF;
    case NanoRole::SurfaceMuted:
      return night ? nanoRgb(0x1C, 0x10, 0x00) : light ? nanoRgb(0xEE, 0xEE, 0xEA) : nanoRgb(0x1E, 0x1E, 0x20);
    case NanoRole::SurfaceActive:
      return night ? nanoRgb(0x36, 0x21, 0x00) : light ? nanoRgb(0xC4, 0xC4, 0xBE) : nanoRgb(0x3A, 0x3A, 0x3E);
    case NanoRole::Outline:
      return night ? nanoRgb(0x6B, 0x42, 0x00) : light ? nanoRgb(0x9A, 0x9A, 0x9A) : 0x8410;
    case NanoRole::ProgressTrack:
      return night ? nanoRgb(0x3A, 0x24, 0x00) : light ? nanoRgb(0xBE, 0xBE, 0xB8) : nanoRgb(0x44, 0x44, 0x48);
  }
  return wordColor();
}

uint16_t DisplayManager::nanoColor(NanoRole role) const {
  if (nanoOwnAccent_ && nanoPalette_ != kNanoPaletteClassic) {
    if (role == NanoRole::Accent) {
      return focusColor();
    }
    if (role == NanoRole::OnAccent) {
      // The highlight colors range from yellow to deep blue: pick black or
      // white text by the accent's brightness instead of the palette's.
      const uint16_t accent = focusColor();
      const uint32_t luma = ((accent >> 11) & 0x1F) * 8 * 299 + ((accent >> 5) & 0x3F) * 4 * 587 +
                            (accent & 0x1F) * 8 * 114;
      return luma > 150000U ? 0x0000 : 0xFFFF;
    }
  }
  return nanoPaletteColor(nanoPalette_, role);
}

uint16_t DisplayManager::nanoBlend(NanoRole role, uint8_t alpha) const {
  return nanoMix565(nanoColor(NanoRole::Background), nanoColor(role), alpha);
}

// ─── Frame ──────────────────────────────────────────────────────────────────

void DisplayManager::nanoClearBackground(int width, int height) {
  if (virtualFrame_ == nullptr) {
    return;
  }
  const uint16_t background = panelColor(nanoColor(NanoRole::Background));
  for (int row = 0; row < height; ++row) {
    std::fill_n(virtualFrame_ + row * kVirtualBufferWidth, width, background);
  }
}

void DisplayManager::nanoBeginFrame() {
  nanoResetClip();
  nanoClearBackground(kNanoScreenW, kNanoScreenH);
}

void DisplayManager::nanoEndFrame() {
  if (!initialized_ || virtualFrame_ == nullptr) {
    return;
  }
  uint32_t hash = 2166136261U;
  for (int y = 0; y < kNanoScreenH; ++y) {
    const uint16_t *row = virtualFrame_ + y * kVirtualBufferWidth;
    for (int x = 0; x < kNanoScreenW; ++x) {
      hash = (hash ^ row[x]) * 16777619U;
    }
  }
  hash = (hash ^ static_cast<uint32_t>(uiOrientation_)) * 16777619U;
  if (hash == nanoFrameHash_ && panelWriteCount_ == nanoFramePanelWrites_ &&
      lastRenderKey_ == "nano") {
    return;
  }
  // Any other renderer comparing its own key must see a mismatch after this.
  lastRenderKey_ = "nano";
  flushScaledFrame(1, kNanoScreenW, kNanoScreenH);
  nanoFrameHash_ = hash;
  nanoFramePanelWrites_ = panelWriteCount_;
}

// ─── Primitives ─────────────────────────────────────────────────────────────

void DisplayManager::nanoSetClip(int x, int y, int w, int h) {
  nanoClipX0_ = std::max(0, x);
  nanoClipY0_ = std::max(0, y);
  nanoClipX1_ = std::min(kNanoScreenW, x + w);
  nanoClipY1_ = std::min(kNanoScreenH, y + h);
}

void DisplayManager::nanoResetClip() { nanoSetClip(0, 0, kNanoScreenW, kNanoScreenH); }

void DisplayManager::nanoSpan(int x, int y, int w, uint16_t color) {
  if (virtualFrame_ == nullptr || y < nanoClipY0_ || y >= nanoClipY1_ || w <= 0) {
    return;
  }
  const int x0 = std::max(x, nanoClipX0_);
  const int x1 = std::min(x + w, nanoClipX1_);
  if (x1 <= x0) {
    return;
  }
  std::fill_n(virtualFrame_ + y * kVirtualBufferWidth + x0, x1 - x0, panelColor(color));
}

void DisplayManager::nanoPixel(int x, int y, uint16_t color) {
  if (virtualFrame_ == nullptr || x < nanoClipX0_ || x >= nanoClipX1_ || y < nanoClipY0_ ||
      y >= nanoClipY1_) {
    return;
  }
  virtualFrame_[y * kVirtualBufferWidth + x] = panelColor(color);
}

void DisplayManager::nanoBlendPixel(int x, int y, uint16_t color, uint8_t alpha) {
  if (alpha == 0 || virtualFrame_ == nullptr || x < nanoClipX0_ || x >= nanoClipX1_ || y < nanoClipY0_ ||
      y >= nanoClipY1_) {
    return;
  }
  uint16_t &pixel = virtualFrame_[y * kVirtualBufferWidth + x];
  if (alpha >= 250) {
    pixel = panelColor(color);
    return;
  }
  const uint16_t under = panelColor(pixel);  // byte swap is its own inverse
  pixel = panelColor(nanoMix565(under, color, alpha));
}

void DisplayManager::nanoFillRect(int x, int y, int w, int h, uint16_t color) {
  for (int row = std::max(y, nanoClipY0_); row < std::min(y + h, nanoClipY1_); ++row) {
    nanoSpan(x, row, w, color);
  }
}

void DisplayManager::nanoDrawRect(int x, int y, int w, int h, uint16_t color) {
  if (w <= 0 || h <= 0) {
    return;
  }
  nanoSpan(x, y, w, color);
  nanoSpan(x, y + h - 1, w, color);
  nanoFillRect(x, y, 1, h, color);
  nanoFillRect(x + w - 1, y, 1, h, color);
}

void DisplayManager::nanoCircleHelper(int x0, int y0, int r, uint8_t corners, uint16_t color) {
  int f = 1 - r;
  int ddFx = 1;
  int ddFy = -2 * r;
  int x = 0;
  int y = r;
  while (x < y) {
    if (f >= 0) {
      --y;
      ddFy += 2;
      f += ddFy;
    }
    ++x;
    ddFx += 2;
    f += ddFx;
    if (corners & 0x4) {
      nanoPixel(x0 + x, y0 + y, color);
      nanoPixel(x0 + y, y0 + x, color);
    }
    if (corners & 0x2) {
      nanoPixel(x0 + x, y0 - y, color);
      nanoPixel(x0 + y, y0 - x, color);
    }
    if (corners & 0x8) {
      nanoPixel(x0 - y, y0 + x, color);
      nanoPixel(x0 - x, y0 + y, color);
    }
    if (corners & 0x1) {
      nanoPixel(x0 - y, y0 - x, color);
      nanoPixel(x0 - x, y0 - y, color);
    }
  }
}

void DisplayManager::nanoFillCircleHelper(int x0, int y0, int r, uint8_t corners, int delta,
                                          uint16_t color) {
  int f = 1 - r;
  int ddFx = 1;
  int ddFy = -r - r;
  int x = 0;
  int y = r;
  int px = x;
  int py = y;
  ++delta;
  while (x < y) {
    if (f >= 0) {
      --y;
      ddFy += 2;
      f += ddFy;
    }
    ++x;
    ddFx += 2;
    f += ddFx;
    if (x < y + 1) {
      if (corners & 1) nanoFillRect(x0 + x, y0 - y, 1, 2 * y + delta, color);
      if (corners & 2) nanoFillRect(x0 - x, y0 - y, 1, 2 * y + delta, color);
    }
    if (y != py) {
      if (corners & 1) nanoFillRect(x0 + py, y0 - px, 1, 2 * px + delta, color);
      if (corners & 2) nanoFillRect(x0 - py, y0 - px, 1, 2 * px + delta, color);
      py = y;
    }
    px = x;
  }
}

void DisplayManager::nanoFillRoundRect(int x, int y, int w, int h, int r, uint16_t color) {
  if (w <= 0 || h <= 0) {
    return;
  }
  r = std::max(0, std::min(r, std::min(w, h) / 2));
  nanoFillRect(x + r, y, w - 2 * r, h, color);
  nanoFillCircleHelper(x + w - r - 1, y + r, r, 1, h - 2 * r - 1, color);
  nanoFillCircleHelper(x + r, y + r, r, 2, h - 2 * r - 1, color);
}

void DisplayManager::nanoImage(const ui::Rect &rect, int r, const NanoImage &image) {
  if (!image.valid() || rect.w <= 0 || rect.h <= 0 || virtualFrame_ == nullptr) {
    return;
  }
  r = std::max(0, std::min(r, std::min<int>(rect.w, rect.h) / 2));
  // Largest source window with the rect's aspect, centred.
  int srcW = image.width;
  int srcH = image.height;
  if (static_cast<int32_t>(srcW) * rect.h > static_cast<int32_t>(srcH) * rect.w) {
    srcW = std::max(1, static_cast<int>(static_cast<int32_t>(srcH) * rect.w / rect.h));
  } else {
    srcH = std::max(1, static_cast<int>(static_cast<int32_t>(srcW) * rect.h / rect.w));
  }
  const int srcX = (image.width - srcW) / 2;
  const int srcY = (image.height - srcH) / 2;
  for (int j = 0; j < rect.h; ++j) {
    const int y = rect.y + j;
    if (y < nanoClipY0_ || y >= nanoClipY1_) {
      continue;
    }
    const int sy0 = srcY + j * srcH / rect.h;
    const int sy1 = std::max(sy0 + 1, srcY + (j + 1) * srcH / rect.h);
    // Rounded corners: rows inside the top/bottom radius start later.
    int inset = 0;
    const int cornerRow = j < r ? r - j : (j >= rect.h - r ? j - (rect.h - r - 1) : 0);
    if (cornerRow > 0) {
      const int dy = cornerRow;
      int dx = r;
      while (dx > 0 && (dx * dx + dy * dy) > r * r) {
        --dx;
      }
      inset = r - dx;
    }
    for (int i = inset; i < rect.w - inset; ++i) {
      const int x = rect.x + i;
      if (x < nanoClipX0_ || x >= nanoClipX1_) {
        continue;
      }
      const int sx0 = srcX + i * srcW / rect.w;
      const int sx1 = std::max(sx0 + 1, srcX + (i + 1) * srcW / rect.w);
      uint32_t red = 0;
      uint32_t green = 0;
      uint32_t blue = 0;
      uint32_t count = 0;
      for (int sy = sy0; sy < sy1; ++sy) {
        const uint16_t *row = image.pixels + sy * image.width;
        for (int sx = sx0; sx < sx1; ++sx) {
          const uint16_t c = row[sx];
          red += c >> 11;
          green += (c >> 5) & 0x3F;
          blue += c & 0x1F;
          ++count;
        }
      }
      const uint16_t color = static_cast<uint16_t>(((red / count) << 11) | ((green / count) << 5) | (blue / count));
      virtualFrame_[y * kVirtualBufferWidth + x] = panelColor(color);
    }
  }
}

void DisplayManager::nanoDrawRoundRect(int x, int y, int w, int h, int r, uint16_t color) {
  if (w <= 0 || h <= 0) {
    return;
  }
  r = std::max(0, std::min(r, std::min(w, h) / 2));
  nanoSpan(x + r, y, w - 2 * r, color);
  nanoSpan(x + r, y + h - 1, w - 2 * r, color);
  nanoFillRect(x, y + r, 1, h - 2 * r, color);
  nanoFillRect(x + w - 1, y + r, 1, h - 2 * r, color);
  nanoCircleHelper(x + r, y + r, r, 1, color);
  nanoCircleHelper(x + w - r - 1, y + r, r, 2, color);
  nanoCircleHelper(x + w - r - 1, y + h - r - 1, r, 4, color);
  nanoCircleHelper(x + r, y + h - r - 1, r, 8, color);
}

void DisplayManager::nanoDrawLine(int x0, int y0, int x1, int y1, uint16_t color) {
  const bool steep = std::abs(y1 - y0) > std::abs(x1 - x0);
  if (steep) {
    std::swap(x0, y0);
    std::swap(x1, y1);
  }
  if (x0 > x1) {
    std::swap(x0, x1);
    std::swap(y0, y1);
  }
  const int dx = x1 - x0;
  const int dy = std::abs(y1 - y0);
  int err = dx / 2;
  const int ystep = y0 < y1 ? 1 : -1;
  for (; x0 <= x1; ++x0) {
    if (steep) {
      nanoPixel(y0, x0, color);
    } else {
      nanoPixel(x0, y0, color);
    }
    err -= dy;
    if (err < 0) {
      y0 += ystep;
      err += dx;
    }
  }
}

void DisplayManager::nanoThickLine(int x0, int y0, int x1, int y1, uint16_t color) {
  // 2px stroke: the line plus a copy shifted across its minor axis.
  nanoDrawLine(x0, y0, x1, y1, color);
  if (std::abs(y1 - y0) > std::abs(x1 - x0)) {
    nanoDrawLine(x0 + 1, y0, x1 + 1, y1, color);
  } else {
    nanoDrawLine(x0, y0 + 1, x1, y1 + 1, color);
  }
}

void DisplayManager::nanoFillCircle(int cx, int cy, int r, uint16_t color) {
  nanoFillRect(cx, cy - r, 1, 2 * r + 1, color);
  nanoFillCircleHelper(cx, cy, r, 3, 0, color);
}

void DisplayManager::nanoDrawCircle(int cx, int cy, int r, uint16_t color) {
  nanoPixel(cx, cy + r, color);
  nanoPixel(cx, cy - r, color);
  nanoPixel(cx + r, cy, color);
  nanoPixel(cx - r, cy, color);
  nanoCircleHelper(cx, cy, r, 0xF, color);
}

void DisplayManager::nanoFillTriangle(int x0, int y0, int x1, int y1, int x2, int y2,
                                      uint16_t color) {
  if (y0 > y1) {
    std::swap(y0, y1);
    std::swap(x0, x1);
  }
  if (y1 > y2) {
    std::swap(y2, y1);
    std::swap(x2, x1);
  }
  if (y0 > y1) {
    std::swap(y0, y1);
    std::swap(x0, x1);
  }
  if (y0 == y2) {
    const int a = std::min(x0, std::min(x1, x2));
    const int b = std::max(x0, std::max(x1, x2));
    nanoSpan(a, y0, b - a + 1, color);
    return;
  }
  const int dx01 = x1 - x0, dy01 = y1 - y0, dx02 = x2 - x0, dy02 = y2 - y0;
  const int dx12 = x2 - x1, dy12 = y2 - y1;
  int32_t sa = 0;
  int32_t sb = 0;
  const int last = (y1 == y2) ? y1 : y1 - 1;
  int y = y0;
  for (; y <= last; ++y) {
    int a = x0 + (dy01 != 0 ? sa / dy01 : 0);
    int b = x0 + sb / dy02;
    sa += dx01;
    sb += dx02;
    if (a > b) std::swap(a, b);
    nanoSpan(a, y, b - a + 1, color);
  }
  sa = static_cast<int32_t>(dx12) * (y - y1);
  sb = static_cast<int32_t>(dx02) * (y - y0);
  for (; y <= y2; ++y) {
    int a = x1 + (dy12 != 0 ? sa / dy12 : 0);
    int b = x0 + sb / dy02;
    sa += dx12;
    sb += dx02;
    if (a > b) std::swap(a, b);
    nanoSpan(a, y, b - a + 1, color);
  }
}

// ─── UI text ────────────────────────────────────────────────────────────────

int DisplayManager::nanoTextWidth(const String &text, uint8_t size) {
  return nanoRunWidth(nanoStrike(gNanoUiFamily, nanoStrikeForSize(size)), text);
}

int DisplayManager::nanoLineHeight(uint8_t size) {
  return nanoStrike(gNanoUiFamily, nanoStrikeForSize(size)).lineHeight;
}

int DisplayManager::nanoCapHeight(uint8_t size) {
  return nanoStrike(gNanoUiFamily, nanoStrikeForSize(size)).capHeight;
}

std::vector<String> DisplayManager::nanoWrapText(const String &text, int maxWidth, uint8_t size) {
  std::vector<String> lines;
  const NanoUiStrike &strike = nanoStrike(gNanoUiFamily, nanoStrikeForSize(size));
  int start = 0;
  while (start <= static_cast<int>(text.length())) {
    int end = text.indexOf('\n', start);
    if (end < 0) {
      end = text.length();
    }
    const String paragraph = text.substring(start, end);
    if (paragraph.length() > 0) {
      for (const String &line : nanoWrap(strike, paragraph, maxWidth, 250)) {
        lines.push_back(line);
      }
    }
    lines.push_back(String());  // paragraph gap
    start = end + 1;
  }
  while (!lines.empty() && lines.back().isEmpty()) {
    lines.pop_back();
  }
  return lines;
}

String DisplayManager::nanoFitText(const String &text, int maxWidth, uint8_t size) {
  return nanoEllipsize(nanoStrike(gNanoUiFamily, nanoStrikeForSize(size)), text, maxWidth);
}

void DisplayManager::nanoDrawRun(int x, int baseline, const String &text, uint8_t family, uint8_t strike,
                                 uint16_t color) {
  if (virtualFrame_ == nullptr) {
    return;
  }
  const NanoUiStrike &s = nanoStrike(family, strike);
  int32_t pen16 = static_cast<int32_t>(x) * 16;
  for (size_t i = 0; i < text.length(); ++i) {
    const NanoUiGlyph &g = nanoGlyphFor(s, text[i]);
    if (g.width > 0) {
      const int gx = static_cast<int>((pen16 + 8) >> 4) + g.xOffset;
      const int gy = baseline + g.yOffset;
      uint32_t nibble = g.offset;
      for (int row = 0; row < g.height; ++row) {
        const int py = gy + row;
        if (py < nanoClipY0_ || py >= nanoClipY1_) {
          nibble += g.width;
          continue;
        }
        for (int col = 0; col < g.width; ++col, ++nibble) {
          const uint8_t packed = s.bitmap[nibble >> 1];
          const uint8_t a4 = (nibble & 1U) ? (packed & 0x0F) : (packed >> 4);
          if (a4 != 0) {
            nanoBlendPixel(gx + col, py, color, static_cast<uint8_t>(a4 * 17));
          }
        }
      }
    }
    pen16 += g.advance;
  }
}

void DisplayManager::nanoTextWithFamily(const ui::Rect &rect, const String &text, uint8_t family,
                                        uint8_t size, uint16_t color, NanoAlign align, uint8_t maxLines) {
  if (rect.w == 0 || rect.h == 0 || text.isEmpty()) {
    return;
  }
  maxLines = std::max<uint8_t>(1, std::min<uint8_t>(maxLines, 3));
  uint8_t strikeIndex = nanoStrikeForSize(size);
  // Only a rect too short for even one line steps the type down.
  while (strikeIndex > 0 && nanoStrike(family, strikeIndex).capHeight > rect.h) {
    --strikeIndex;
  }
  const NanoUiStrike &s = nanoStrike(family, strikeIndex);
  uint8_t fitLines = 1;
  while (fitLines < maxLines && s.capHeight + fitLines * s.lineHeight <= rect.h + 2) {
    ++fitLines;
  }
  const std::vector<String> lines = nanoWrap(s, text, rect.w, fitLines);
  if (lines.empty()) {
    return;
  }
  const int block = s.capHeight + (static_cast<int>(lines.size()) - 1) * s.lineHeight;
  int baseline = rect.y + (static_cast<int>(rect.h) - block + 1) / 2 + s.capHeight;
  for (const String &line : lines) {
    const int width = nanoRunWidth(s, line);
    int x = rect.x;
    if (align == NanoAlign::Center) {
      x = rect.x + (static_cast<int>(rect.w) - width) / 2;
    } else if (align == NanoAlign::End) {
      x = rect.x + static_cast<int>(rect.w) - width;
    }
    nanoDrawRun(x, baseline, line, family, strikeIndex, color);
    baseline += s.lineHeight;
  }
}

void DisplayManager::nanoText(const ui::Rect &rect, const String &text, uint8_t size, uint16_t color,
                              NanoAlign align, uint8_t maxLines) {
  nanoTextWithFamily(rect, text, nanoUiFont_, size, color, align, maxLines);
}

void DisplayManager::nanoTextLineAt(int x, int centreY, const String &text, uint8_t size, uint16_t color) {
  if (text.isEmpty()) {
    return;
  }
  const uint8_t strikeIndex = nanoStrikeForSize(size);
  const NanoUiStrike &s = nanoStrike(nanoUiFont_, strikeIndex);
  nanoDrawRun(x, centreY + s.capHeight / 2, text, nanoUiFont_, strikeIndex, color);
}

void DisplayManager::nanoTextInFamily(const ui::Rect &rect, const String &text, uint8_t family, uint8_t size,
                                      uint16_t color, NanoAlign align) {
  nanoTextWithFamily(rect, text, family, size, color, align, 1);
}

void DisplayManager::nanoSmallGlyph(int x, int y, char c, uint16_t color) {
  const uint8_t *rows = tinyRowsFor(c);
  for (int row = 0; row < kTinyGlyphHeight; ++row) {
    for (int col = 0; col < kTinyGlyphWidth; ++col) {
      if (rows[row] & (1 << (kTinyGlyphWidth - 1 - col))) {
        nanoPixel(x + col, y + row, color);
      }
    }
  }
}

// ─── Reader typefaces on the Nano frame ─────────────────────────────────────

void DisplayManager::nanoTypefaceGlyph(int x, int y, char c, uint16_t color, uint8_t scalePercent,
                                       ReaderTypeface typeface) {
  const ReaderGlyph glyph = glyphFor(c, typeface);
  if (glyph.width == 0) {
    return;
  }
  const bool invert = shouldDrawInvertedGlyph(c);
  const int glyphHeight = glyph.height;
  if (scalePercent == 100) {
    for (int row = 0; row < glyphHeight; ++row) {
      for (int col = 0; col < glyph.width; ++col) {
        const int sourceRow = invert ? glyphHeight - 1 - row : row;
        const int sourceCol = invert ? glyph.width - 1 - col : col;
        nanoBlendPixel(x + col, y + row, color, glyph.bitmap[sourceRow * glyph.width + sourceCol]);
      }
    }
    return;
  }
  const int scaledWidth = scaledPercentDimension(glyph.width, scalePercent);
  const int scaledHeight = scaledPercentDimension(glyphHeight, scalePercent);
  if (scalePercent > 100) {
    for (int dstRow = 0; dstRow < scaledHeight; ++dstRow) {
      const int sourceY = upscaleSource256(dstRow, glyphHeight, scaledHeight);
      for (int dstCol = 0; dstCol < scaledWidth; ++dstCol) {
        nanoBlendPixel(x + dstCol, y + dstRow, color,
                       glyphAlphaBilinear(glyph, upscaleSource256(dstCol, glyph.width, scaledWidth), sourceY, invert));
      }
    }
    return;
  }
  for (int dstRow = 0; dstRow < scaledHeight; ++dstRow) {
    const int sourceYStart = (dstRow * glyphHeight) / scaledHeight;
    const int sourceYEnd = std::min(glyphHeight, ((dstRow + 1) * glyphHeight + scaledHeight - 1) / scaledHeight);
    for (int dstCol = 0; dstCol < scaledWidth; ++dstCol) {
      const int sourceXStart = (dstCol * glyph.width) / scaledWidth;
      const int sourceXEnd =
          std::min(static_cast<int>(glyph.width), ((dstCol + 1) * glyph.width + scaledWidth - 1) / scaledWidth);
      uint32_t alphaSum = 0;
      uint32_t samples = 0;
      for (int sy = sourceYStart; sy < sourceYEnd; ++sy) {
        for (int sx = sourceXStart; sx < sourceXEnd; ++sx) {
          const int ly = invert ? glyphHeight - 1 - sy : sy;
          const int lx = invert ? glyph.width - 1 - sx : sx;
          alphaSum += glyph.bitmap[ly * glyph.width + lx];
          ++samples;
        }
      }
      if (samples > 0) {
        nanoBlendPixel(x + dstCol, y + dstRow, color, static_cast<uint8_t>(alphaSum / samples));
      }
    }
  }
}

int DisplayManager::nanoTypefaceTextWidth(const String &text, uint8_t scalePercent,
                                          ReaderTypeface typeface) const {
  const ReaderTypeface previous = gButtonLabelPreviewTypeface;
  if (typeface != ReaderTypeface::Count) {
    gButtonLabelPreviewTypeface = typeface;
  }
  const int width = measureSerifTextWidthScaled(text, scalePercent);
  gButtonLabelPreviewTypeface = previous;
  return width;
}

void DisplayManager::nanoTypefaceText(int x, int y, const String &text, uint16_t color, uint8_t scalePercent,
                                      ReaderTypeface typeface) {
  const ReaderTypeface previous = gButtonLabelPreviewTypeface;
  if (typeface != ReaderTypeface::Count) {
    gButtonLabelPreviewTypeface = typeface;
  }
  const ReaderTypeface face = effectiveReaderTypefaceForText(text);
  int cursorX = x;
  for (size_t i = 0; i < text.length(); ++i) {
    const ReaderGlyph glyph = glyphFor(text[i], face);
    const int xOffset = scaledSignedPercent(glyph.xOffset, scalePercent);
    const int width = glyph.width == 0 ? 0 : scaledPercentDimension(glyph.width, scalePercent);
    nanoTypefaceGlyph(cursorX + xOffset, y, text[i], color, scalePercent, face);
    int tracked = trackedAdvanceScaledPercent(glyph.xAdvance, scalePercent, i, text.length());
    if (i + 1 < text.length()) {
      const ReaderGlyph nextGlyph = glyphFor(text[i + 1], face);
      tracked -= opticalKerningAdjustment(text[i], text[i + 1], xOffset, width, tracked,
                                          scaledSignedPercent(nextGlyph.xOffset, scalePercent),
                                          scaledPercentDesiredGap(scalePercent));
    }
    cursorX += std::max(1, tracked);
  }
  gButtonLabelPreviewTypeface = previous;
}

void DisplayManager::nanoReaderPreview(const ui::Rect &area, const String &before, const String &word,
                                       const String &after) {
  if (word.isEmpty() || area.h < 8) {
    return;
  }
  const ReaderTypeface face = currentReaderTypeface();
  const int baseHeight = std::max(1, baseGlyphHeightForTypeface(face));
  // The reading screen's largest size, shrunk only if the strip is shorter.
  int scale = std::min(100, (static_cast<int>(area.h) * 100) / baseHeight);
  scale = std::max(20, scale);
  const uint8_t scalePercent = static_cast<uint8_t>(scale);
  const int textHeight = scaledPercentDimension(baseHeight, scalePercent);
  const int textY = area.y + (static_cast<int>(area.h) - textHeight) / 2;
  const int focusIndex = findFocusLetterIndex(word);
  const int currentX = rsvpStartXScaledPercent(word, focusIndex, kNanoScreenW, scalePercent, false);
  const TextLayoutMetrics layout = serifWordLayoutScaledPercent(word, focusIndex, scalePercent);
  const uint16_t ink = nanoColor(NanoRole::Foreground);
  const uint16_t focus = nanoColor(NanoRole::Accent);
  const uint16_t phantom = nanoMix565(nanoColor(NanoRole::Background), ink, 70);
  const int gap = std::max(8, scaledPercentDimension(22, scalePercent));

  nanoSetClip(area.x, area.y, area.w, area.h);
  // Anchor ticks where the reading screen puts its guide.
  const int anchorX = currentX + layout.focusCenterX;
  nanoFillRect(anchorX - 1, area.y + 2, 2, 6, focus);
  nanoFillRect(anchorX - 1, area.y + area.h - 8, 2, 6, focus);
  if (!before.isEmpty()) {
    const TextLayoutMetrics beforeLayout = serifWordLayoutScaledPercent(before, -1, scalePercent);
    nanoTypefaceText(currentX + layout.minX - gap - beforeLayout.maxX, textY, before, phantom, scalePercent);
  }
  // The word itself, glyph by glyph so the focus letter takes the accent.
  int cursorX = currentX;
  for (size_t i = 0; i < word.length(); ++i) {
    const ReaderGlyph glyph = glyphFor(word[i], face);
    const int xOffset = scaledSignedPercent(glyph.xOffset, scalePercent);
    const int width = glyph.width == 0 ? 0 : scaledPercentDimension(glyph.width, scalePercent);
    nanoTypefaceGlyph(cursorX + xOffset, textY, word[i],
                      static_cast<int>(i) == focusIndex && currentFocusHighlightEnabled() ? focus : ink,
                      scalePercent, face);
    int tracked = trackedAdvanceScaledPercent(glyph.xAdvance, scalePercent, i, word.length());
    if (i + 1 < word.length()) {
      const ReaderGlyph nextGlyph = glyphFor(word[i + 1], face);
      tracked -= opticalKerningAdjustment(word[i], word[i + 1], xOffset, width, tracked,
                                          scaledSignedPercent(nextGlyph.xOffset, scalePercent),
                                          scaledPercentDesiredGap(scalePercent));
    }
    cursorX += std::max(1, tracked);
  }
  if (!after.isEmpty()) {
    const TextLayoutMetrics afterLayout = serifWordLayoutScaledPercent(after, -1, scalePercent);
    nanoTypefaceText(currentX + layout.maxX + gap - afterLayout.minX, textY, after, phantom, scalePercent);
  }
  nanoResetClip();
}

void DisplayManager::nanoReaderSample(const ui::Rect &area, const String &before, const String &word,
                                      const String &after, uint8_t fontSizeLevel) {
  const int areaX = area.x;
  const int areaY = area.y;
  const int areaW = area.w;
  const int areaH = area.h;
  nanoFillRect(areaX, areaY, areaW, areaH, backgroundColor());
  if (word.isEmpty() || areaH < 16 || areaW < 40) {
    return;
  }
  const ReaderTypeface face = currentReaderTypeface();
  const int baseHeight = std::max(1, baseGlyphHeightForTypeface(face));
  const ReaderTextStyle style = readerTextStyle(fontSizeLevel, face);
  // Room for the guide lines above and below the word.
  const int fit = (areaH - kRsvpGuideTopOffset - kRsvpGuideBottomOffset - 4) * 100 / baseHeight;
  const uint8_t scalePercent = static_cast<uint8_t>(std::max(20, std::min<int>(style.scalePercent, fit)));
  const int textHeight = scaledPercentDimension(baseHeight, scalePercent);
  const int textY = areaY + (areaH - textHeight) / 2;
  const int focusIndex = findFocusLetterIndex(word);
  const int currentX = areaX + rsvpStartXScaledPercent(word, focusIndex, areaW, scalePercent, false);
  const TextLayoutMetrics layout = serifWordLayoutScaledPercent(word, focusIndex, scalePercent);
  const int anchorX = areaX + (areaW * currentAnchorPercent()) / 100;
  const uint16_t ink = wordColor();
  const uint16_t focus = focusColor();
  const uint16_t phantom = blendOverBackground(ink, style.alpha);

  nanoSetClip(areaX, areaY, areaW, areaH);
  drawRsvpAnchorGuide(anchorX, textY, textHeight);
  if (!before.isEmpty()) {
    const TextLayoutMetrics beforeLayout = serifWordLayoutScaledPercent(before, -1, scalePercent);
    nanoTypefaceText(currentX + layout.minX - style.currentGap - beforeLayout.maxX, textY, before, phantom,
                     scalePercent);
  }
  int cursorX = currentX;
  for (size_t i = 0; i < word.length(); ++i) {
    const ReaderGlyph glyph = glyphFor(word[i], face);
    const int xOffset = scaledSignedPercent(glyph.xOffset, scalePercent);
    const int width = glyph.width == 0 ? 0 : scaledPercentDimension(glyph.width, scalePercent);
    nanoTypefaceGlyph(cursorX + xOffset, textY, word[i],
                      static_cast<int>(i) == focusIndex && currentFocusHighlightEnabled() ? focus : ink,
                      scalePercent, face);
    int tracked = trackedAdvanceScaledPercent(glyph.xAdvance, scalePercent, i, word.length());
    if (i + 1 < word.length()) {
      const ReaderGlyph nextGlyph = glyphFor(word[i + 1], face);
      tracked -= opticalKerningAdjustment(word[i], word[i + 1], xOffset, width, tracked,
                                          scaledSignedPercent(nextGlyph.xOffset, scalePercent),
                                          scaledPercentDesiredGap(scalePercent));
    }
    cursorX += std::max(1, tracked);
  }
  if (!after.isEmpty()) {
    const TextLayoutMetrics afterLayout = serifWordLayoutScaledPercent(after, -1, scalePercent);
    nanoTypefaceText(currentX + layout.maxX + style.currentGap - afterLayout.minX, textY, after, phantom,
                     scalePercent);
  }
  nanoResetClip();
}

void DisplayManager::nanoScrollPreview(const ui::Rect &area, const std::vector<ContextWord> &words,
                                       size_t currentLocal) {
  if (words.empty() || area.h < 20 || area.w < 40) {
    return;
  }
  const ReaderTypeface face = currentReaderTypeface();
  const int baseHeight = std::max(1, baseGlyphHeightForTypeface(face));
  // Three lines of body text in the strip: the one with the current word
  // in the middle, what was just read above it, what comes next below.
  constexpr int kLines = 3;
  const int lineStep = static_cast<int>(area.h) / kLines;
  const int textHeightWanted = std::max(10, lineStep * 7 / 10);
  const uint8_t scalePercent = static_cast<uint8_t>(std::max(12, std::min(100, textHeightWanted * 100 / baseHeight)));
  const int textHeight = scaledPercentDimension(baseHeight, scalePercent);
  const int space = std::max(4, textHeight * 3 / 10);
  constexpr int kPad = 14;
  const int maxWidth = static_cast<int>(area.w) - kPad * 2;

  struct Placed {
    size_t index;
    int x;
    int width;
  };
  std::vector<std::vector<Placed>> lines(1);
  int cursor = 0;
  int currentLine = -1;
  for (size_t i = 0; i < words.size(); ++i) {
    const int width = nanoTypefaceTextWidth(words[i].text, scalePercent);
    const bool breakHere = !lines.back().empty() &&
                           ((words[i].paragraphStart && i > 0) || cursor + space + width > maxWidth);
    if (breakHere) {
      if (currentLine >= 0 && static_cast<int>(lines.size()) > currentLine + kLines) {
        break;  // enough lines after the current one
      }
      lines.emplace_back();
      cursor = 0;
    }
    const int x = lines.back().empty() ? 0 : cursor + space;
    lines.back().push_back({i, x, width});
    cursor = x + width;
    if (i == currentLocal) {
      currentLine = static_cast<int>(lines.size()) - 1;
    }
  }
  if (currentLine < 0) {
    currentLine = 0;
  }

  const uint16_t background = nanoColor(NanoRole::Background);
  const uint16_t ink = nanoColor(NanoRole::Foreground);
  const uint16_t read = nanoMix565(background, ink, 110);
  const uint16_t focus = nanoReadable(nanoColor(NanoRole::Accent), background);
  nanoSetClip(area.x, area.y, area.w, area.h);
  for (int row = 0; row < kLines; ++row) {
    const int lineIndex = currentLine - 1 + row;
    if (lineIndex < 0 || lineIndex >= static_cast<int>(lines.size())) {
      continue;
    }
    const int top = area.y + row * lineStep + (lineStep - textHeight) / 2;
    for (const Placed &placed : lines[static_cast<size_t>(lineIndex)]) {
      const int x = area.x + kPad + placed.x;
      uint16_t color = placed.index < currentLocal ? read : ink;
      if (placed.index == currentLocal) {
        // The word the reader resumes from: an accent underline and ink.
        nanoFillRoundRect(x - 3, top - 2, placed.width + 6, textHeight + 4, 4, nanoColor(NanoRole::SurfaceMuted));
        nanoFillRect(x, top + textHeight + 1, placed.width, 2, nanoColor(NanoRole::Accent));
        color = focus;
      }
      nanoTypefaceText(x, top, words[placed.index].text, color, scalePercent);
    }
  }
  nanoResetClip();
}

void DisplayManager::overrideNanoPalette(uint8_t palette, bool ownAccent) {
  // Temporary switch for one screen (the reader panel draws in the reading
  // colors); no render-key reset, the frame hash still dedups flushes.
  nanoPalette_ = palette > kNanoFixedPaletteCount ? kNanoPaletteClassic : palette;
  nanoOwnAccent_ = ownAccent;
}

// ─── Icons ──────────────────────────────────────────────────────────────────
// ~20px line icons, 2px strokes, centered in `rect`. The first few follow
// rsvpnano's src/ui/Icons.cpp; the rest are drawn in the same spirit.

void DisplayManager::nanoIcon(const ui::Rect &rect, NanoIcon icon, uint16_t ink, uint16_t surface) {
  const int cx = rect.x + rect.w / 2;
  const int cy = rect.y + rect.h / 2;
  switch (icon) {
    case NanoIcon::Bookmark: {
      const int width = std::max(1, std::min(12, static_cast<int>(rect.w) - 6));
      const int height = std::max(1, std::min(18, static_cast<int>(rect.h) - 4));
      const int x = cx - width / 2;
      const int y = cy - height / 2;
      nanoFillRect(x, y, width, height, ink);
      for (int row = 0; row <= std::min(5, height - 1); ++row) {
        const int half = std::min(row, width / 2);
        nanoSpan(cx - half, y + height - 6 + row, half * 2 + 1, surface);
      }
      break;
    }
    case NanoIcon::Books: {
      const int x = cx - 9;
      const int y = cy - 9;
      nanoFillRoundRect(x, y + 1, 4, 17, 1, ink);
      nanoFillRoundRect(x + 6, y + 3, 4, 15, 1, ink);
      nanoThickLine(x + 13, y + 4, x + 17, y + 17, ink);
      nanoFillRect(x - 1, y + 18, 20, 2, ink);
      break;
    }
    case NanoIcon::Edit: {
      const int x = cx - 9;
      const int y = cy - 9;
      nanoDrawRoundRect(x, y + 2, 14, 16, 2, ink);
      nanoThickLine(x + 6, y + 12, x + 17, y + 1, ink);
      nanoFillRect(x + 5, y + 12, 2, 2, ink);
      break;
    }
    case NanoIcon::Device: {
      const int x = cx - 7;
      const int y = cy - 10;
      nanoDrawRoundRect(x, y, 14, 20, 3, ink);
      nanoDrawRoundRect(x + 1, y + 1, 12, 18, 2, ink);
      nanoFillRect(cx - 2, y + 15, 4, 2, ink);
      break;
    }
    case NanoIcon::Language:
    case NanoIcon::Font: {
      const NanoUiStrike &s = nanoStrike(nanoUiFont_, 1);
      const String aa = "Aa";
      nanoDrawRun(cx - nanoRunWidth(s, aa) / 2, cy + s.capHeight / 2, aa, nanoUiFont_, 1, ink);
      break;
    }
    case NanoIcon::Hourglass: {
      const int x = cx - 7;
      const int y = cy - 9;
      nanoFillRect(x - 1, y, 16, 2, ink);
      nanoFillRect(x - 1, y + 17, 16, 2, ink);
      nanoThickLine(x + 1, y + 2, x + 12, y + 16, ink);
      nanoThickLine(x + 12, y + 2, x + 1, y + 16, ink);
      nanoFillTriangle(cx - 4, y + 16, cx + 4, y + 16, cx, y + 12, ink);
      break;
    }
    case NanoIcon::Power: {
      nanoDrawCircle(cx, cy + 1, 8, ink);
      nanoDrawCircle(cx, cy + 1, 7, ink);
      nanoFillRect(cx - 3, cy - 9, 7, 9, surface);
      nanoFillRect(cx - 1, cy - 9, 2, 10, ink);
      break;
    }
    case NanoIcon::Apps: {
      const int x = cx - 9;
      const int y = cy - 9;
      nanoFillRoundRect(x, y, 8, 8, 2, ink);
      nanoFillRoundRect(x + 10, y, 8, 8, 2, ink);
      nanoFillRoundRect(x, y + 10, 8, 8, 2, ink);
      nanoDrawRoundRect(x + 10, y + 10, 8, 8, 2, ink);
      break;
    }
    case NanoIcon::Palette: {
      nanoDrawCircle(cx, cy, 9, ink);
      nanoDrawCircle(cx, cy, 8, ink);
      nanoFillCircle(cx + 4, cy + 4, 3, surface);
      nanoFillCircle(cx - 4, cy - 3, 2, ink);
      nanoFillCircle(cx + 1, cy - 5, 2, ink);
      nanoFillCircle(cx - 4, cy + 3, 2, ink);
      break;
    }
    case NanoIcon::ChevronLeft:
      nanoThickLine(cx + 3, cy - 7, cx - 4, cy, ink);
      nanoThickLine(cx - 4, cy, cx + 3, cy + 7, ink);
      break;
    case NanoIcon::ChevronRight:
      nanoThickLine(cx - 3, cy - 7, cx + 4, cy, ink);
      nanoThickLine(cx + 4, cy, cx - 3, cy + 7, ink);
      break;
    case NanoIcon::Play:
      nanoFillTriangle(cx - 5, cy - 8, cx - 5, cy + 8, cx + 8, cy, ink);
      break;
    case NanoIcon::Pause:
      nanoFillRoundRect(cx - 6, cy - 8, 4, 16, 1, ink);
      nanoFillRoundRect(cx + 2, cy - 8, 4, 16, 1, ink);
      break;
    case NanoIcon::Plus:
      nanoFillRect(cx - 7, cy - 1, 14, 2, ink);
      nanoFillRect(cx - 1, cy - 7, 2, 14, ink);
      break;
    case NanoIcon::Minus:
      nanoFillRect(cx - 7, cy - 1, 14, 2, ink);
      break;
    case NanoIcon::List:
      for (int i = -1; i <= 1; ++i) {
        nanoFillRect(cx - 9, cy + i * 6 - 1, 3, 3, ink);
        nanoFillRect(cx - 4, cy + i * 6 - 1, 13, 2, ink);
      }
      break;
    case NanoIcon::Trash:
      nanoFillRect(cx - 8, cy - 6, 16, 2, ink);
      nanoFillRect(cx - 3, cy - 9, 6, 2, ink);
      nanoThickLine(cx - 6, cy - 3, cx - 5, cy + 8, ink);
      nanoThickLine(cx + 5, cy - 3, cx + 4, cy + 8, ink);
      nanoFillRect(cx - 5, cy + 8, 10, 2, ink);
      nanoFillRect(cx - 1, cy - 2, 2, 8, ink);
      break;
    case NanoIcon::Record:
      nanoFillCircle(cx, cy, 7, ink);
      break;
    case NanoIcon::Stop:
      nanoFillRoundRect(cx - 7, cy - 7, 14, 14, 2, ink);
      break;
    case NanoIcon::Wifi:
      for (int r = 4; r <= 12; r += 4) {
        nanoCircleHelper(cx, cy + 6, r, 0x3, ink);
        nanoCircleHelper(cx, cy + 6, r - 1, 0x3, ink);
      }
      nanoFillCircle(cx, cy + 6, 1, ink);
      break;
    case NanoIcon::Bluetooth:
      nanoFillRect(cx - 1, cy - 9, 2, 19, ink);
      nanoThickLine(cx, cy - 9, cx + 5, cy - 4, ink);
      nanoThickLine(cx + 5, cy - 4, cx - 5, cy + 5, ink);
      nanoThickLine(cx, cy + 9, cx + 5, cy + 4, ink);
      nanoThickLine(cx + 5, cy + 4, cx - 5, cy - 5, ink);
      break;
    case NanoIcon::Usb:
      nanoFillRect(cx - 1, cy - 6, 2, 13, ink);
      nanoFillTriangle(cx - 4, cy - 5, cx + 4, cy - 5, cx, cy - 10, ink);
      nanoFillCircle(cx, cy + 8, 2, ink);
      nanoThickLine(cx, cy + 3, cx - 6, cy - 1, ink);
      nanoFillRect(cx - 8, cy - 4, 4, 4, ink);
      nanoThickLine(cx, cy + 1, cx + 6, cy - 3, ink);
      nanoFillCircle(cx + 6, cy - 4, 2, ink);
      break;
    case NanoIcon::Phone:
      nanoDrawRoundRect(cx - 6, cy - 10, 12, 20, 3, ink);
      nanoDrawRoundRect(cx - 5, cy - 9, 10, 18, 2, ink);
      nanoFillRect(cx - 2, cy + 6, 4, 1, ink);
      break;
    case NanoIcon::SdCard: {
      const int x = cx - 7;
      const int y = cy - 9;
      nanoThickLine(x, y + 4, x + 4, y, ink);
      nanoFillRect(x + 4, y, 10, 2, ink);
      nanoFillRect(x + 12, y, 2, 18, ink);
      nanoFillRect(x, y + 4, 2, 14, ink);
      nanoFillRect(x, y + 16, 14, 2, ink);
      for (int i = 0; i < 3; ++i) {
        nanoFillRect(x + 4 + i * 3, y + 3, 1, 4, ink);
      }
      break;
    }
    case NanoIcon::Info:
      nanoDrawCircle(cx, cy, 9, ink);
      nanoDrawCircle(cx, cy, 8, ink);
      nanoFillRect(cx - 1, cy - 5, 2, 2, ink);
      nanoFillRect(cx - 1, cy - 1, 2, 7, ink);
      break;
    case NanoIcon::Download:
      nanoFillRect(cx - 1, cy - 9, 2, 12, ink);
      nanoThickLine(cx - 6, cy - 2, cx, cy + 4, ink);
      nanoThickLine(cx, cy + 4, cx + 6, cy - 2, ink);
      nanoFillRect(cx - 8, cy + 8, 16, 2, ink);
      break;
    case NanoIcon::Sort:
      nanoFillRect(cx - 5, cy - 7, 2, 16, ink);
      nanoThickLine(cx - 9, cy - 3, cx - 4, cy - 8, ink);
      nanoThickLine(cx - 4, cy - 8, cx + 1, cy - 3, ink);
      nanoFillRect(cx + 4, cy - 8, 2, 16, ink);
      nanoThickLine(cx, cy + 3, cx + 5, cy + 8, ink);
      nanoThickLine(cx + 5, cy + 8, cx + 10, cy + 3, ink);
      break;
    case NanoIcon::Check:
      nanoThickLine(cx - 7, cy, cx - 2, cy + 5, ink);
      nanoThickLine(cx - 2, cy + 5, cx + 7, cy - 5, ink);
      break;
    case NanoIcon::Help: {
      nanoDrawCircle(cx, cy, 9, ink);
      nanoDrawCircle(cx, cy, 8, ink);
      const NanoUiStrike &s = nanoStrike(nanoUiFont_, 0);
      nanoDrawRun(cx - nanoRunWidth(s, "?") / 2, cy + s.capHeight / 2, "?", nanoUiFont_, 0, ink);
      break;
    }
    case NanoIcon::Sun:
      nanoFillCircle(cx, cy, 4, ink);
      for (int i = 0; i < 8; ++i) {
        static const int8_t kDx[8] = {0, 5, 7, 5, 0, -5, -7, -5};
        static const int8_t kDy[8] = {-7, -5, 0, 5, 7, 5, 0, -5};
        nanoFillRect(cx + kDx[i] * 9 / 7 - 1, cy + kDy[i] * 9 / 7 - 1, 2, 2, ink);
      }
      break;
    case NanoIcon::Sliders:
      for (int i = -1; i <= 1; ++i) {
        nanoFillRect(cx - 9, cy + i * 6, 18, 2, ink);
        nanoFillRoundRect(cx + (i == 0 ? 2 : (i < 0 ? -6 : -1)), cy + i * 6 - 2, 5, 6, 2, ink);
      }
      break;
    case NanoIcon::Book:
      nanoDrawRoundRect(cx - 7, cy - 9, 14, 18, 2, ink);
      nanoDrawRoundRect(cx - 6, cy - 8, 12, 16, 2, ink);
      nanoFillRect(cx - 3, cy - 9, 2, 18, ink);
      break;
    case NanoIcon::Restart:
      nanoCircleHelper(cx, cy, 8, 0x1 | 0x4 | 0x8, ink);
      nanoCircleHelper(cx, cy, 7, 0x1 | 0x4 | 0x8, ink);
      nanoFillTriangle(cx + 2, cy - 11, cx + 2, cy - 3, cx + 8, cy - 7, ink);
      break;
    case NanoIcon::Rewind:
      // "<<": back to the start of the sentence.
      nanoThickLine(cx - 1, cy - 7, cx - 8, cy, ink);
      nanoThickLine(cx - 8, cy, cx - 1, cy + 7, ink);
      nanoThickLine(cx + 7, cy - 7, cx, cy, ink);
      nanoThickLine(cx, cy, cx + 7, cy + 7, ink);
      break;
    case NanoIcon::Target: {
      // Go to a position: a ring with a centre dot and four ticks.
      nanoDrawCircle(cx, cy, 7, ink);
      nanoDrawCircle(cx, cy, 6, ink);
      nanoFillCircle(cx, cy, 2, ink);
      nanoFillRect(cx - 1, cy - 11, 2, 4, ink);
      nanoFillRect(cx - 1, cy + 8, 2, 4, ink);
      nanoFillRect(cx - 11, cy - 1, 4, 2, ink);
      nanoFillRect(cx + 8, cy - 1, 4, 2, ink);
      break;
    }
    case NanoIcon::Moon:
      nanoFillCircle(cx, cy, 8, ink);
      nanoFillCircle(cx + 5, cy - 3, 7, surface);
      break;
    case NanoIcon::Image:
      nanoDrawRoundRect(cx - 10, cy - 8, 20, 16, 2, ink);
      nanoFillTriangle(cx - 7, cy + 5, cx - 1, cy - 2, cx + 4, cy + 5, ink);
      nanoFillTriangle(cx + 1, cy + 5, cx + 5, cy, cx + 8, cy + 5, ink);
      nanoFillCircle(cx + 5, cy - 4, 2, ink);
      break;
    case NanoIcon::None:
    default:
      break;
  }
}

void DisplayManager::nanoBatteryIcon(int x, int y, int w, int h, uint8_t percent, bool charging,
                                     uint16_t ink, uint16_t surface) {
  (void)surface;
  constexpr int kCapWidth = 2;
  if (w <= kCapWidth + 4 || h <= 4) {
    return;
  }
  percent = std::min<uint8_t>(percent, 100);
  const int bodyWidth = w - kCapWidth;
  nanoDrawRoundRect(x, y, bodyWidth, h, 3, ink);
  nanoFillRect(x + bodyWidth, y + h / 2 - 2, kCapWidth, 4, ink);
  const int innerWidth = std::max(0, bodyWidth - 4);
  const int fill = innerWidth * percent / 100;
  if (fill > 0) {
    nanoFillRoundRect(x + 2, y + 2, std::max(2, fill), h - 4, 1, nanoBatteryLevelColor(percent, charging));
  }
}

uint16_t DisplayManager::nanoBatteryLevelColor(uint8_t percent, bool charging) const {
  constexpr uint16_t kBatteryGood = nanoRgb(126, 176, 92);
  constexpr uint16_t kBatteryMedium = nanoRgb(214, 163, 58);
  constexpr uint16_t kBatteryLow = nanoRgb(200, 82, 82);
  return charging || percent > 35 ? kBatteryGood : percent <= 18 ? kBatteryLow : kBatteryMedium;
}

void DisplayManager::nanoChargingBolt(int cx, int cy, int h, uint16_t color) {
  // Two triangles: the upper one leans right, the lower one left, sharing
  // a short horizontal step in the middle -- the usual lightning glyph.
  const int half = std::max(4, h / 2);
  const int w = std::max(3, h * 5 / 12);
  nanoFillTriangle(cx + w / 2 + 1, cy - half, cx - w, cy + 1, cx + 1, cy + 1, color);
  nanoFillTriangle(cx - w / 2 - 1, cy + half, cx + w, cy - 1, cx - 1, cy - 1, color);
}

void DisplayManager::setBatteryStyle(uint8_t style) {
  if (style >= kBatteryStyleCount) {
    style = kBatteryStyleIconPercent;
  }
  if (style == batteryStyle_) {
    return;
  }
  batteryStyle_ = style;
  lastRenderKey_ = "";
}

String DisplayManager::batteryNumberLabel() const {
  if (batteryPresent_) {
    return String(static_cast<unsigned>(batteryPercent_));
  }
  String label = batteryLabel_;
  label.replace("%", "");
  label.trim();
  return label;
}

int DisplayManager::nanoBatteryIndicatorWidth(bool compact) const {
  if (!batteryPresent_ && batteryLabel_.isEmpty()) {
    return 0;
  }
  const int bolt = batteryCharging_ ? 12 : 0;
  switch (batteryStyleFor(compact)) {
    case kBatteryStyleNumberInIcon:
      return 34 + bolt;
    case kBatteryStyleNumberOnly:
      return nanoTextWidth(batteryNumberLabel(), 1) + 2 + bolt;
    case kBatteryStyleIconOnly:
      return 24 + bolt;
    case kBatteryStyleIconPercent:
    default: {
      const String label = batteryPresent_ ? nanoPercentLabel(batteryPercent_) : batteryLabel_;
      return 24 + 6 + nanoTextWidth(label, 1) + bolt;
    }
  }
}

uint8_t DisplayManager::batteryStyleFor(bool compact) const {
  // The icon-only rail is 60 px wide: "Ikona + %" and the long labels would
  // not fit next to its tabs, the number inside the icon does.
  if (compact && batteryStyle_ == kBatteryStyleIconPercent) {
    return kBatteryStyleNumberInIcon;
  }
  return batteryStyle_;
}

void DisplayManager::nanoBatteryInline(const ui::Rect &rect, bool iconOnly, NanoAlign align) {
  if (!batteryPresent_ && batteryLabel_.isEmpty()) {
    return;
  }
  const uint8_t style = batteryStyleFor(iconOnly);
  const uint16_t ink = nanoColor(NanoRole::Muted);
  const uint16_t background = nanoColor(NanoRole::Background);
  const int total = nanoBatteryIndicatorWidth(iconOnly);
  int x = rect.x + (static_cast<int>(rect.w) - total) / 2;
  if (align == NanoAlign::End) {
    x = rect.x + static_cast<int>(rect.w) - total;
  } else if (align == NanoAlign::Start) {
    x = rect.x;
  }
  const int cy = rect.y + static_cast<int>(rect.h) / 2;
  switch (style) {
    case kBatteryStyleNumberInIcon: {
      // A wider cell with the number set inside it; the fill runs as a thin
      // bar along the bottom so the digits stay on a plain background.
      constexpr int kW = 34;
      constexpr int kH = 17;
      const int y = cy - kH / 2;
      nanoDrawRoundRect(x, y, kW - 2, kH, 4, ink);
      nanoFillRect(x + kW - 2, cy - 2, 2, 4, ink);
      const int inner = kW - 6;
      const int fill = batteryPresent_ ? inner * std::min<uint8_t>(batteryPercent_, 100) / 100 : 0;
      if (fill > 0) {
        nanoFillRect(x + 2, y + kH - 4, std::max(2, fill), 2,
                     nanoBatteryLevelColor(batteryPercent_, batteryCharging_));
      }
      nanoText(ui::Rect(x + 1, y, kW - 4, kH - 2), batteryNumberLabel(), 1, nanoColor(NanoRole::Foreground),
               NanoAlign::Center);
      x += kW;
      break;
    }
    case kBatteryStyleNumberOnly: {
      const String label = batteryNumberLabel();
      const int w = nanoTextWidth(label, 1) + 2;
      nanoText(ui::Rect(x, rect.y, w, rect.h), label, 1, ink);
      x += w;
      break;
    }
    case kBatteryStyleIconOnly:
      nanoBatteryIcon(x, cy - 6, 24, 12, batteryPercent_, batteryCharging_, ink, background);
      x += 24;
      break;
    case kBatteryStyleIconPercent:
    default: {
      const String label = batteryPresent_ ? nanoPercentLabel(batteryPercent_) : batteryLabel_;
      nanoBatteryIcon(x, cy - 6, 24, 12, batteryPercent_, batteryCharging_, ink, background);
      const int labelW = nanoTextWidth(label, 1);
      nanoText(ui::Rect(x + 30, rect.y, labelW + 2, rect.h), label, 1, ink);
      x += 30 + labelW;
      break;
    }
  }
  if (batteryCharging_) {
    nanoChargingBolt(x + 7, cy, 13, nanoBatteryLevelColor(100, true));
  }
}

void DisplayManager::nanoBatteryStack(const ui::Rect &rect) {
  if (!batteryPresent_ && batteryLabel_.isEmpty()) {
    return;
  }
  nanoBatteryInline(ui::Rect(rect.x, rect.y, rect.w, 16));
}

// ─── Widgets ────────────────────────────────────────────────────────────────

void DisplayManager::nanoLabel(const ui::Rect &rect, const String &text, uint8_t size, NanoRole role,
                               NanoAlign align, uint8_t maxLines) {
  const uint16_t color = role == NanoRole::Accent
                             ? nanoReadable(nanoColor(role), nanoColor(NanoRole::Background))
                             : nanoColor(role);
  nanoText(rect, text, size, color, align, maxLines);
}

void DisplayManager::nanoSeparator(const ui::Rect &rect, const String &text) {
  int lineX = rect.x;
  if (!text.isEmpty()) {
    const String label = nanoFitText(text, rect.w, 1);
    const int labelWidth = nanoTextWidth(label, 1);
    nanoText(ui::Rect(rect.x, rect.y, labelWidth + 2, rect.h), label, 1, nanoColor(NanoRole::Muted));
    lineX = rect.x + labelWidth + 8;
  }
  if (lineX < rect.x + rect.w) {
    nanoSpan(lineX, rect.y + rect.h / 2, rect.x + rect.w - lineX, nanoBlend(NanoRole::Muted, 70));
  }
}

void DisplayManager::nanoButton(const ui::Rect &rect, const String &text, bool enabled,
                                NanoIcon icon, uint8_t textLines, const String &detailLeft,
                                const String &detailRight, bool pressed, bool armed,
                                ReaderTypeface previewTypeface) {
  const int x = rect.x;
  const int y = rect.y;
  const int w = rect.w;
  const int h = rect.h;
  const uint16_t surface = armed     ? nanoColor(NanoRole::Accent)
                           : pressed ? nanoColor(NanoRole::SurfaceActive)
                           : enabled ? nanoColor(NanoRole::SurfaceMuted)
                                     : nanoBlend(NanoRole::SurfaceMuted, 150);
  nanoFillRoundRect(x, y, w, h, kNanoRadius, surface);

  const uint16_t ink = armed     ? nanoColor(NanoRole::OnAccent)
                       : enabled ? nanoColor(NanoRole::Foreground)
                                 : nanoColor(NanoRole::Muted);
  const bool hasText = !text.isEmpty();
  if (!hasText && icon != NanoIcon::None) {
    nanoIcon(rect, icon, armed ? ink : (enabled ? nanoColor(NanoRole::Foreground) : ink), surface);
    return;
  }

  constexpr int kPad = 10;
  const int iconBox = icon == NanoIcon::None ? 0 : 22;
  const int textX = x + kPad + (iconBox > 0 ? iconBox + 8 : 0);
  const int textW = std::max(0, x + w - kPad - textX);
  const bool hasDetail = !detailLeft.isEmpty() || !detailRight.isEmpty();
  const int detailH = hasDetail ? nanoLineHeight(1) : 0;
  const int titleH = h - detailH - (hasDetail ? 6 : 0);
  if (icon != NanoIcon::None) {
    nanoIcon(ui::Rect(x + kPad, y + (titleH - 22) / 2 + (hasDetail ? 3 : 0), iconBox, 22), icon,
             armed ? ink : (enabled ? nanoReadable(nanoColor(NanoRole::Accent), surface) : ink), surface);
  }

  if (previewTypeface != ReaderTypeface::Count) {
    // Font picker: the label is the font's own name set in that font.
    const int sourceHeight = std::max(1, baseGlyphHeightForTypeface(previewTypeface));
    const int scale = std::max(10, std::min(100, (26 * 100) / sourceHeight));
    const uint8_t scalePercent = static_cast<uint8_t>(scale);
    String label = text;
    while (label.length() > 1 && nanoTypefaceTextWidth(label, scalePercent, previewTypeface) > textW) {
      label.remove(label.length() - 1);
    }
    const int labelW = nanoTypefaceTextWidth(label, scalePercent, previewTypeface);
    const int labelH = scaledPercentDimension(sourceHeight, scalePercent);
    const int labelX = icon == NanoIcon::None ? x + (w - labelW) / 2 : textX;
    nanoTypefaceText(labelX, y + (h - labelH) / 2, label, ink, scalePercent, previewTypeface);
    return;
  }

  const ui::Rect titleRect(textX, y + (hasDetail ? 4 : 0), textW, std::max(0, titleH));
  nanoText(titleRect, text, 2, ink, icon == NanoIcon::None ? NanoAlign::Center : NanoAlign::Start,
           hasDetail ? 1 : textLines);
  if (hasDetail) {
    const int detailY = y + h - detailH - 6;
    const uint16_t detailInk = armed ? ink : nanoColor(NanoRole::Muted);
    if (detailLeft.isEmpty() || detailRight.isEmpty()) {
      nanoText(ui::Rect(textX, detailY, textW, detailH), detailLeft.isEmpty() ? detailRight : detailLeft, 1,
               detailInk, icon == NanoIcon::None ? NanoAlign::Center : NanoAlign::Start);
    } else {
      const int rightW = std::min(textW / 2, nanoTextWidth(detailRight, 1));
      nanoText(ui::Rect(textX, detailY, textW - rightW - 8, detailH), detailLeft, 1, detailInk, NanoAlign::Start);
      nanoText(ui::Rect(textX + textW - rightW, detailY, rightW, detailH), detailRight, 1, detailInk,
               NanoAlign::End);
    }
  }
}

void DisplayManager::nanoIconButton(const ui::Rect &rect, NanoIcon icon, bool pressed) {
  const uint16_t surface =
      pressed ? nanoColor(NanoRole::SurfaceActive) : nanoColor(NanoRole::SurfaceMuted);
  nanoFillRoundRect(rect.x, rect.y, rect.w, rect.h, kNanoRadius, surface);
  nanoIcon(rect, icon, nanoColor(NanoRole::Foreground), surface);
}

void DisplayManager::nanoTile(const ui::Rect &rect, const String &label, NanoIcon icon, const String &detail,
                              bool pressed, bool accent, bool enabled) {
  const uint16_t surface = accent    ? nanoColor(NanoRole::Accent)
                           : pressed ? nanoColor(NanoRole::SurfaceActive)
                                     : nanoColor(NanoRole::SurfaceMuted);
  nanoFillRoundRect(rect.x, rect.y, rect.w, rect.h, kNanoRadius, surface);
  const uint16_t ink = accent ? nanoColor(NanoRole::OnAccent)
                              : nanoColor(enabled ? NanoRole::Foreground : NanoRole::Muted);
  const uint16_t iconInk =
      accent ? ink : (enabled ? nanoReadable(nanoColor(NanoRole::Accent), surface) : nanoColor(NanoRole::Muted));
  const uint16_t detailInk = accent ? ink : nanoColor(NanoRole::Muted);
  const int labelH = nanoLineHeight(2);
  const int detailH = detail.isEmpty() ? 0 : nanoLineHeight(1);
  const int tallBlock = 22 + 6 + labelH + (detailH > 0 ? detailH - 4 : 0);
  if (static_cast<int>(rect.h) < tallBlock + 6 || icon == NanoIcon::None) {
    // Short tile: icon left, text right (or centered text without icon).
    const int iconBox = icon == NanoIcon::None ? 0 : 22;
    const int textX = rect.x + 10 + (iconBox > 0 ? iconBox + 8 : 0);
    const int textW = std::max(0, rect.x + rect.w - 10 - textX);
    if (icon != NanoIcon::None) {
      nanoIcon(ui::Rect(rect.x + 10, rect.y + (rect.h - 22) / 2, 22, 22), icon, iconInk, surface);
    }
    const NanoAlign align = icon == NanoIcon::None ? NanoAlign::Center : NanoAlign::Start;
    if (detail.isEmpty()) {
      nanoText(ui::Rect(textX, rect.y, textW, rect.h), label, 2, ink, align, rect.h >= 44 ? 2 : 1);
    } else {
      const int block = labelH + detailH - 4;
      const int top = rect.y + (rect.h - block) / 2;
      nanoText(ui::Rect(textX, top, textW, labelH), label, 2, ink, align);
      nanoText(ui::Rect(textX, top + labelH - 4, textW, detailH), detail, 1, detailInk, align);
    }
    return;
  }
  // Tall tile: icon above the label (+ detail). A long label without a
  // detail line may take two lines when the tile is tall enough.
  const int bodyW = static_cast<int>(rect.w) - 12;
  const int twoLineBlock = 22 + 6 + labelH + nanoLineHeight(2) - 4;
  const bool twoLines = detailH == 0 && nanoTextWidth(label, 2) > bodyW && static_cast<int>(rect.h) >= twoLineBlock + 4;
  const int block = twoLines ? twoLineBlock : tallBlock;
  const int top = rect.y + std::max(3, (static_cast<int>(rect.h) - block) / 2);
  nanoIcon(ui::Rect(rect.x, top, rect.w, 22), icon, iconInk, surface);
  if (twoLines) {
    nanoText(ui::Rect(rect.x + 6, top + 26, bodyW, labelH + nanoLineHeight(2) - 4), label, 2, ink, NanoAlign::Center,
             2);
    return;
  }
  nanoText(ui::Rect(rect.x + 6, top + 26, bodyW, labelH), label, 2, ink, NanoAlign::Center);
  if (detailH > 0) {
    nanoText(ui::Rect(rect.x + 6, top + 26 + labelH - 4, bodyW, detailH), detail, 1, detailInk, NanoAlign::Center);
  }
}

void DisplayManager::nanoPill(const ui::Rect &rect, const String &text, NanoIcon icon, bool pressed, bool active) {
  const uint16_t surface = active    ? nanoColor(NanoRole::Accent)
                           : pressed ? nanoColor(NanoRole::SurfaceActive)
                                     : nanoColor(NanoRole::SurfaceMuted);
  nanoFillRoundRect(rect.x, rect.y, rect.w, rect.h, rect.h / 2, surface);
  const uint16_t ink = active ? nanoColor(NanoRole::OnAccent) : nanoColor(NanoRole::Foreground);
  uint8_t size = rect.h < 28 ? 1 : 2;
  if (size == 2 && nanoTextWidth(text, 2) > static_cast<int>(rect.w) - 20) {
    size = 1;
  }
  if (text.isEmpty()) {
    nanoIcon(rect, icon, ink, surface);
    return;
  }
  const int pad = std::min(static_cast<int>(rect.h) / 2, 10);
  int iconBox = icon == NanoIcon::None ? 0 : 20;
  if (iconBox > 0 && nanoTextWidth(text, size) + iconBox + 4 > static_cast<int>(rect.w) - pad * 2) {
    iconBox = 0;
    icon = NanoIcon::None;
  }
  const String label = nanoFitText(text, rect.w - pad * 2 - (iconBox > 0 ? iconBox + 4 : 0), size);
  const int contentW = iconBox + (iconBox > 0 ? 4 : 0) + nanoTextWidth(label, size);
  int x = rect.x + (static_cast<int>(rect.w) - contentW) / 2;
  if (icon != NanoIcon::None) {
    nanoIcon(ui::Rect(x, rect.y, iconBox, rect.h), icon, active ? ink : nanoColor(NanoRole::Accent), surface);
    x += iconBox + 4;
  }
  nanoText(ui::Rect(x, rect.y, rect.x + rect.w - x, rect.h), label, size, ink);
}

void DisplayManager::nanoRailBackground(const ui::Rect &rect) {
  nanoFillRect(rect.x, rect.y, rect.w, rect.h, nanoBlend(NanoRole::SurfaceMuted, 150));
}

void DisplayManager::nanoTab(const ui::Rect &rect, const String &text, bool active, NanoIcon icon,
                             bool pressed, bool badge, bool markerRight) {
  const ui::Rect pill(rect.x + 6, rect.y + 2, std::max(0, static_cast<int>(rect.w) - 12),
                      std::max(0, static_cast<int>(rect.h) - 4));
  if (active || pressed) {
    const uint16_t fill = active ? nanoColor(NanoRole::SurfaceActive) : nanoColor(NanoRole::SurfaceMuted);
    nanoFillRoundRect(pill.x, pill.y, pill.w, pill.h, kNanoRadius, fill);
  }
  const uint16_t surface = active ? nanoColor(NanoRole::SurfaceActive) : nanoBlend(NanoRole::SurfaceMuted, 150);
  if (active) {
    nanoFillRoundRect(markerRight ? pill.x + pill.w - 4 : pill.x, pill.y + pill.h / 4, 4,
                      std::max(4, pill.h / 2), 2, nanoColor(NanoRole::Accent));
  }
  const uint16_t iconInk =
      active ? nanoReadable(nanoColor(NanoRole::Accent), surface) : nanoColor(NanoRole::Muted);
  const uint16_t ink = nanoColor(active ? NanoRole::Foreground : NanoRole::Muted);
  if (text.isEmpty()) {
    nanoIcon(pill, icon, iconInk, surface);
    if (badge) {
      nanoFillCircle(pill.x + pill.w - 10, pill.y + 7, 3, nanoColor(NanoRole::Accent));
    }
    return;
  }
  const int iconX = pill.x + 10;
  nanoIcon(ui::Rect(iconX, pill.y, 22, pill.h), icon, iconInk, surface);
  const int textX = iconX + 30;
  nanoText(ui::Rect(textX, pill.y, std::max(0, pill.x + static_cast<int>(pill.w) - 8 - textX), pill.h), text, 2,
           ink, NanoAlign::Start);
  if (badge) {
    nanoFillCircle(iconX + 21, pill.y + pill.h / 2 - 8, 3, nanoColor(NanoRole::Accent));
  }
}

void DisplayManager::nanoSetting(const ui::Rect &rect, const String &label, const String &value,
                                 bool inlineLayout, bool pressed) {
  const uint16_t surface =
      pressed ? nanoColor(NanoRole::SurfaceActive) : nanoColor(NanoRole::SurfaceMuted);
  nanoFillRoundRect(rect.x, rect.y, rect.w, rect.h, kNanoRadius, surface);
  constexpr int kPad = 12;
  const int textWidth = std::max(0, static_cast<int>(rect.w) - kPad * 2);
  if (inlineLayout) {
    // Label left in the foreground color, value right in the accent. The
    // value keeps its natural width up to half the row; the label gets the
    // rest and ends in an ellipsis when it has to.
    const int valueNatural = nanoTextWidth(value, 2);
    const int labelNatural = nanoTextWidth(label, 2);
    int valueWidth = std::min(valueNatural, textWidth / 2);
    if (labelNatural + valueNatural + 10 <= textWidth) {
      valueWidth = valueNatural;
    } else if (labelNatural + 10 + valueWidth < textWidth) {
      valueWidth = textWidth - labelNatural - 10;
    }
    const int labelWidth = std::max(0, textWidth - valueWidth - (value.isEmpty() ? 0 : 10));
    nanoText(ui::Rect(rect.x + kPad, rect.y, labelWidth, rect.h), label, 2, nanoColor(NanoRole::Foreground),
             NanoAlign::Start, rect.h >= 48 ? 2 : 1);
    nanoText(ui::Rect(rect.x + rect.w - kPad - valueWidth, rect.y, valueWidth, rect.h), value, 2,
             nanoReadable(nanoColor(NanoRole::Accent), surface), NanoAlign::End);
  } else {
    const int labelH = nanoLineHeight(1);
    const int valueH = nanoLineHeight(2);
    const int block = labelH + valueH - 4;
    const int top = rect.y + (static_cast<int>(rect.h) - block) / 2;
    nanoText(ui::Rect(rect.x + kPad, top, textWidth, labelH), label, 1, nanoColor(NanoRole::Muted));
    nanoText(ui::Rect(rect.x + kPad, top + labelH - 4, textWidth, valueH), value, 2,
             nanoReadable(nanoColor(NanoRole::Accent), surface));
  }
}

void DisplayManager::nanoToggle(const ui::Rect &rect, const String &label, bool on, bool pressed) {
  const uint16_t surface =
      pressed ? nanoColor(NanoRole::SurfaceActive) : nanoColor(NanoRole::SurfaceMuted);
  nanoFillRoundRect(rect.x, rect.y, rect.w, rect.h, kNanoRadius, surface);
  constexpr int kSwitchWidth = 36;
  constexpr int kSwitchHeight = 20;
  const int switchX = rect.x + rect.w - kSwitchWidth - 12;
  const int switchY = rect.y + (static_cast<int>(rect.h) - kSwitchHeight) / 2;
  nanoFillRoundRect(switchX, switchY, kSwitchWidth, kSwitchHeight, kSwitchHeight / 2,
                    nanoColor(on ? NanoRole::Accent : NanoRole::ProgressTrack));
  nanoFillCircle(on ? switchX + kSwitchWidth - 10 : switchX + 9, switchY + kSwitchHeight / 2, 7,
                 on ? nanoColor(NanoRole::OnAccent) : nanoColor(NanoRole::Foreground));
  nanoText(ui::Rect(rect.x + 12, rect.y, std::max(0, switchX - 10 - (rect.x + 12)), rect.h), label, 2,
           nanoColor(NanoRole::Foreground), NanoAlign::Start, rect.h >= 48 ? 2 : 1);
}

void DisplayManager::nanoProgress(const ui::Rect &rect, int value, int minimum, int maximum) {
  value = std::max(minimum, std::min(value, maximum));
  nanoFillRoundRect(rect.x, rect.y, rect.w, rect.h, rect.h / 2, nanoColor(NanoRole::ProgressTrack));
  if (maximum > minimum && rect.w > 2) {
    const int fill = static_cast<int>(rect.w) * (value - minimum) / (maximum - minimum);
    if (fill > 0) {
      nanoFillRoundRect(rect.x, rect.y, std::max(fill, static_cast<int>(rect.h)), rect.h, rect.h / 2,
                        nanoColor(NanoRole::Accent));
    }
  }
}

void DisplayManager::nanoSlider(const ui::Rect &rect, const String &label, const String &valueText,
                                int value, int minimum, int maximum, bool pressed, bool dragging) {
  const int x = rect.x;
  const int y = rect.y;
  const int w = rect.w;
  const int h = rect.h;
  if (w <= 4 || h <= 4) {
    return;
  }
  const int range = maximum - minimum;
  value = std::max(minimum, std::min(value, maximum));
  // Even the minimum keeps a sliver of fill, so the tile never reads as a
  // plain button.
  constexpr int kMinFill = 10;
  const int fill = range > 0 ? kMinFill + (w - kMinFill) * (value - minimum) / range : w;

  const uint16_t surface = nanoColor(pressed || dragging ? NanoRole::SurfaceActive : NanoRole::SurfaceMuted);
  const uint16_t accent = nanoColor(NanoRole::Accent);
  nanoFillRoundRect(x, y, w, h, kNanoRadius, surface);
  nanoSetClip(x, y, fill, h);
  nanoFillRoundRect(x, y, w, h, kNanoRadius, dragging ? accent : nanoMix565(surface, accent, 200));
  nanoResetClip();
  if (fill > 12 && fill < w - 6) {
    // Grip at the fill edge: the part a finger drags.
    nanoFillRoundRect(x + fill - 5, y + h / 2 - 8, 3, 16, 1, nanoColor(NanoRole::OnAccent));
  }

  // Label left, value right, drawn twice: clipped to the filled and to the
  // empty part, so each half keeps its contrast.
  constexpr int kPad = 12;
  const int textWidth = std::max(0, w - kPad * 2);
  // A narrow tile that cannot hold both on one line puts the label in small
  // type above the value instead of cutting the label short.
  const bool stacked = h >= 44 && nanoTextWidth(label, 2) + nanoTextWidth(valueText, 2) + 10 > textWidth;
  const uint8_t labelSize = stacked ? 1 : 2;
  const NanoAlign valueAlign = stacked ? NanoAlign::Start : NanoAlign::End;
  const int valueWidth = stacked ? textWidth : std::min(nanoTextWidth(valueText, 2), textWidth / 2);
  const int labelWidth = stacked ? textWidth : std::max(0, textWidth - valueWidth - 10);
  const ui::Rect labelRect = stacked ? ui::Rect(x + kPad, y + 4, labelWidth, h / 2 - 4) : ui::Rect(x + kPad, y, labelWidth, h);
  const ui::Rect valueRect = stacked ? ui::Rect(x + kPad, y + h / 2 - 2, valueWidth, h / 2)
                                     : ui::Rect(x + w - kPad - valueWidth, y, valueWidth, h);
  const uint16_t onAccent = nanoColor(NanoRole::OnAccent);
  const uint16_t foreground = nanoColor(NanoRole::Foreground);
  nanoSetClip(x + fill, y, w - fill, h);
  nanoText(labelRect, label, labelSize, foreground, NanoAlign::Start);
  nanoText(valueRect, valueText, 2, foreground, valueAlign);
  nanoSetClip(x, y, fill, h);
  nanoText(labelRect, label, labelSize, onAccent, NanoAlign::Start);
  nanoText(valueRect, valueText, 2, onAccent, valueAlign);
  nanoResetClip();
}

void DisplayManager::nanoPaletteChip(const ui::Rect &rect, uint8_t palette, const String &name,
                                     bool selected, bool pressed) {
  const int x = rect.x;
  const int y = rect.y;
  const int w = rect.w;
  const int h = rect.h;
  if (w <= 16 || h <= 16) {
    return;
  }
  const uint16_t background = nanoPaletteColor(palette, NanoRole::Background);
  const uint16_t surface =
      nanoPaletteColor(palette, pressed ? NanoRole::SurfaceActive : NanoRole::SurfaceMuted);
  const uint16_t accent = nanoPaletteColor(palette, NanoRole::Accent);
  if (selected) {
    nanoFillRoundRect(x, y, w, h, kNanoRadius, nanoColor(NanoRole::Accent));
    nanoFillRoundRect(x + 2, y + 2, w - 4, h - 4, kNanoRadius - 2, background);
  } else {
    nanoFillRoundRect(x, y, w, h, kNanoRadius, background);
    nanoDrawRoundRect(x, y, w, h, kNanoRadius, nanoColor(NanoRole::SurfaceActive));
  }
  // Swatches: the palette's surface tile with its accent dot and bar.
  const int barY = y + h - 14;
  nanoFillRoundRect(x + 8, barY, w - 16, 8, 4, surface);
  nanoFillRoundRect(x + 8, barY, (w - 16) * 2 / 5, 8, 4, accent);
  const uint8_t nameSize = nanoTextWidth(name, 2) <= w - 18 ? 2 : 1;
  nanoText(ui::Rect(x + 9, y + 3, w - 18, std::max(0, h - 19)), name, nameSize,
           nanoPaletteColor(palette, NanoRole::Foreground), NanoAlign::Start);
}

void DisplayManager::nanoFontChip(const ui::Rect &rect, uint8_t family, const String &name, const String &sample,
                                  bool selected, bool pressed) {
  const uint16_t surface = nanoColor(pressed ? NanoRole::SurfaceActive : NanoRole::SurfaceMuted);
  if (selected) {
    nanoFillRoundRect(rect.x, rect.y, rect.w, rect.h, kNanoRadius, nanoColor(NanoRole::Accent));
    nanoFillRoundRect(rect.x + 2, rect.y + 2, rect.w - 4, rect.h - 4, kNanoRadius - 2, surface);
  } else {
    nanoFillRoundRect(rect.x, rect.y, rect.w, rect.h, kNanoRadius, surface);
  }
  const uint8_t nameStrike = nanoRunWidth(nanoStrike(family, 1), name) <= static_cast<int>(rect.w) - 16 ? 1 : 0;
  const NanoUiStrike &body = nanoStrike(family, nameStrike);
  const NanoUiStrike &small = nanoStrike(family, 0);
  const int block = body.capHeight + (sample.isEmpty() ? 0 : small.lineHeight);
  const int top = rect.y + (static_cast<int>(rect.h) - block) / 2;
  nanoTextWithFamily(ui::Rect(rect.x + 8, top - 4, rect.w - 16, body.capHeight + 8), name, family,
                     nameStrike == 1 ? 2 : 1, nanoColor(NanoRole::Foreground), NanoAlign::Center, 1);
  if (!sample.isEmpty()) {
    nanoTextWithFamily(ui::Rect(rect.x + 8, top + body.capHeight + 4, rect.w - 16, small.lineHeight), sample,
                       family, 1, nanoColor(NanoRole::Muted), NanoAlign::Center, 1);
  }
}

void DisplayManager::nanoLayoutChip(const ui::Rect &rect, bool compact, bool railRight, const String &name,
                                    const String &detail, bool selected, bool pressed) {
  const uint16_t surface = nanoColor(pressed ? NanoRole::SurfaceActive : NanoRole::SurfaceMuted);
  if (selected) {
    nanoFillRoundRect(rect.x, rect.y, rect.w, rect.h, kNanoRadius, nanoColor(NanoRole::Accent));
    nanoFillRoundRect(rect.x + 2, rect.y + 2, rect.w - 4, rect.h - 4, kNanoRadius - 2, surface);
  } else {
    nanoFillRoundRect(rect.x, rect.y, rect.w, rect.h, kNanoRadius, surface);
  }
  // Mini screen with the rail where this layout puts it.
  const int sw = std::min(120, static_cast<int>(rect.w) - 40);
  const int sh = 40;
  const int sx = rect.x + (static_cast<int>(rect.w) - sw) / 2;
  const int sy = rect.y + 10;
  const uint16_t screen = nanoColor(NanoRole::Background);
  const uint16_t rail = nanoColor(NanoRole::ProgressTrack);
  const uint16_t accent = nanoColor(NanoRole::Accent);
  const uint16_t ink = nanoColor(NanoRole::Muted);
  nanoFillRoundRect(sx, sy, sw, sh, 5, screen);
  const int railW = compact ? 14 : 38;
  const int railX = railRight ? sx + sw - railW : sx;
  nanoFillRoundRect(railX, sy, railW, sh, 4, rail);
  for (int i = 0; i < 3; ++i) {
    const int ty = sy + 6 + i * 11;
    nanoFillRect(railX + 4, ty, 5, 5, i == 0 ? accent : ink);
    if (!compact) {
      nanoFillRect(railX + 12, ty + 1, railW - 17, 3, i == 0 ? accent : ink);
    }
  }
  const int contentX = railRight ? sx + 5 : railX + railW + 5;
  const int contentW = sw - railW - 10;
  nanoFillRoundRect(contentX, sy + 6, contentW, 12, 3, rail);
  nanoFillRoundRect(contentX, sy + 22, contentW / 2 - 2, 12, 3, rail);
  nanoFillRoundRect(contentX + contentW / 2 + 2, sy + 22, contentW / 2 - 2, 12, 3, rail);
  if (selected) {
    // "Tap again": a swap arrow in the corner, the rail changes sides.
    const int cx = rect.x + rect.w - 20;
    const int cy = rect.y + 18;
    nanoFillCircle(cx, cy, 11, accent);
    const uint16_t on = nanoColor(NanoRole::OnAccent);
    nanoFillRect(cx - 6, cy - 3, 12, 2, on);
    nanoFillTriangle(cx + 7, cy - 2, cx + 3, cy - 6, cx + 3, cy + 2, on);
    nanoFillRect(cx - 6, cy + 3, 12, 2, on);
    nanoFillTriangle(cx - 7, cy + 4, cx - 3, cy, cx - 3, cy + 8, on);
  }
  const int textY = sy + sh + 4;
  const int nameH = nanoLineHeight(2);
  nanoText(ui::Rect(rect.x + 6, textY, rect.w - 12, nameH), name, 2, nanoColor(NanoRole::Foreground),
           NanoAlign::Center);
  if (!detail.isEmpty()) {
    nanoText(ui::Rect(rect.x + 6, textY + nameH - 2, rect.w - 12, nanoLineHeight(1)), detail, 1,
             selected ? nanoReadable(accent, surface) : nanoColor(NanoRole::Muted), NanoAlign::Center);
  }
}

void DisplayManager::nanoReadingThemeChip(const ui::Rect &rect, uint8_t theme, const String &name,
                                          const String &sampleHead, const String &sampleFocus,
                                          const String &sampleTail, bool selected, bool pressed) {
  uint16_t background = 0;
  uint16_t word = 0;
  uint16_t focus = 0;
  readerThemeColors(theme, background, word, focus);
  const int x = rect.x;
  const int y = rect.y;
  const int w = rect.w;
  const int h = rect.h;
  if (selected) {
    nanoFillRoundRect(x, y, w, h, kNanoRadius, nanoColor(NanoRole::Accent));
    nanoFillRoundRect(x + 3, y + 3, w - 6, h - 6, kNanoRadius - 2, background);
  } else {
    nanoFillRoundRect(x, y, w, h, kNanoRadius, pressed ? nanoColor(NanoRole::SurfaceActive) : background);
    nanoDrawRoundRect(x, y, w, h, kNanoRadius, nanoColor(NanoRole::SurfaceActive));
    if (pressed) {
      nanoFillRoundRect(x + 3, y + 3, w - 6, h - 6, kNanoRadius - 2, background);
    }
  }
  // A word as the reading screen draws it: focus letter in the letter
  // color, anchor ticks above and below.
  const ReaderTypeface face = currentReaderTypeface();
  const int baseHeight = std::max(1, baseGlyphHeightForTypeface(face));
  const int wordAreaH = h - 26;
  uint8_t scale = static_cast<uint8_t>(std::max(12, std::min(60, (wordAreaH - 16) * 100 / baseHeight)));
  const String &left = sampleHead;
  const String &mid = sampleFocus;
  const String &right = sampleTail;
  while (scale > 12 && nanoTypefaceTextWidth(left + mid + right, scale) > w - 24) {
    scale = static_cast<uint8_t>(scale - 2);
  }
  const int textH = scaledPercentDimension(baseHeight, scale);
  const int wl = nanoTypefaceTextWidth(left, scale);
  const int wm = nanoTypefaceTextWidth(mid, scale);
  const int wr = nanoTypefaceTextWidth(right, scale);
  const int total = wl + wm + wr;
  const int tx = x + (w - total) / 2;
  const int ty = y + 6 + (wordAreaH - textH) / 2;
  nanoSetClip(x + 3, y + 3, w - 6, h - 6);
  nanoTypefaceText(tx, ty, left, word, scale);
  nanoTypefaceText(tx + wl, ty, mid, focus, scale);
  nanoTypefaceText(tx + wl + wm, ty, right, word, scale);
  const int ax = tx + wl + wm / 2;
  nanoFillRect(ax, ty - 7, 2, 5, focus);
  nanoFillRect(ax, ty + textH + 2, 2, 5, focus);
  nanoResetClip();
  nanoText(ui::Rect(x + 6, y + h - 24, w - 12, 20), name, 2, nanoMix565(background, word, 190), NanoAlign::Center);
}

void DisplayManager::nanoReadingThemePill(const ui::Rect &rect, uint8_t theme, const String &name, bool selected,
                                          bool pressed) {
  uint16_t background = 0;
  uint16_t word = 0;
  uint16_t focus = 0;
  readerThemeColors(theme, background, word, focus);
  const int x = rect.x;
  const int y = rect.y;
  const int w = rect.w;
  const int h = rect.h;
  if (selected) {
    nanoFillRoundRect(x, y, w, h, kNanoRadius, nanoColor(NanoRole::Accent));
    nanoFillRoundRect(x + 3, y + 3, w - 6, h - 6, kNanoRadius - 2, background);
  } else {
    nanoFillRoundRect(x, y, w, h, kNanoRadius, pressed ? nanoColor(NanoRole::SurfaceActive) : background);
    nanoDrawRoundRect(x, y, w, h, kNanoRadius, nanoColor(NanoRole::SurfaceActive));
    if (pressed) {
      nanoFillRoundRect(x + 3, y + 3, w - 6, h - 6, kNanoRadius - 2, background);
    }
  }
  // The focus letter's color as a dot, the name in the theme's word color.
  constexpr int kDotR = 5;
  const int textW = std::min(nanoTextWidth(name, 2), w - 16 - kDotR * 2 - 6);
  const int startX = x + std::max(8, (w - (kDotR * 2 + 6 + textW)) / 2);
  nanoFillCircle(startX + kDotR, y + h / 2, kDotR, focus);
  nanoText(ui::Rect(startX + kDotR * 2 + 6, y, std::max(0, x + w - 8 - (startX + kDotR * 2 + 6)), h), name, 2, word,
           NanoAlign::Start);
}

void DisplayManager::nanoLetterColorTile(const ui::Rect &rect, uint16_t color, const String &label, bool pressed) {
  const uint16_t surface = nanoColor(pressed ? NanoRole::SurfaceActive : NanoRole::SurfaceMuted);
  nanoFillRoundRect(rect.x, rect.y, rect.w, rect.h, kNanoRadius, surface);
  const int r = std::max(5, std::min(12, static_cast<int>(rect.h) / 2 - 7));
  const int cx = rect.x + 12 + r;
  const int cy = rect.y + rect.h / 2;
  // Thin ring so a color close to the tile still reads as a swatch.
  nanoFillCircle(cx, cy, r + 2, nanoColor(NanoRole::Muted));
  nanoFillCircle(cx, cy, r, color);
  const int textX = cx + r + 10;
  nanoText(ui::Rect(textX, rect.y, std::max(0, rect.x + rect.w - 10 - textX), rect.h), label, 2,
           nanoColor(NanoRole::Foreground), NanoAlign::Start, rect.h >= 48 ? 2 : 1);
}

void DisplayManager::nanoColorSwatch(const ui::Rect &rect, uint16_t color, bool selected, bool pressed) {
  const int inset = pressed ? 3 : 1;
  if (selected) {
    nanoFillRoundRect(rect.x, rect.y, rect.w, rect.h, 8, nanoColor(NanoRole::Foreground));
    nanoFillRoundRect(rect.x + 3, rect.y + 3, rect.w - 6, rect.h - 6, 6, nanoColor(NanoRole::Background));
    nanoFillRoundRect(rect.x + 5, rect.y + 5, rect.w - 10, rect.h - 10, 5, color);
    return;
  }
  nanoFillRoundRect(rect.x + inset, rect.y + inset, rect.w - inset * 2, rect.h - inset * 2, 7, color);
}

// ─── Generic Button painting (Nano skin) ────────────────────────────────────

void DisplayManager::nanoGenericIcon(ui::IconId id, const ui::Rect &rect, uint16_t ink, uint16_t surface) {
  NanoIcon icon = NanoIcon::None;
  switch (id) {
    case ui::IconId::Back: icon = NanoIcon::ChevronLeft; break;
    case ui::IconId::Book: icon = NanoIcon::Books; break;
    case ui::IconId::SavePoint: icon = NanoIcon::Bookmark; break;
    case ui::IconId::Settings: icon = NanoIcon::Sliders; break;
    case ui::IconId::Plugin: icon = NanoIcon::Apps; break;
    case ui::IconId::Power: icon = NanoIcon::Power; break;
    case ui::IconId::Wifi: icon = NanoIcon::Wifi; break;
    case ui::IconId::Play: icon = NanoIcon::Play; break;
    case ui::IconId::Delete: icon = NanoIcon::Trash; break;
    case ui::IconId::Reset: icon = NanoIcon::Restart; break;
    case ui::IconId::Check: icon = NanoIcon::Check; break;
    case ui::IconId::Record: icon = NanoIcon::Record; break;
    case ui::IconId::Stop: icon = NanoIcon::Stop; break;
    case ui::IconId::Eye: icon = NanoIcon::Info; break;
    default: break;
  }
  nanoIcon(rect, icon, ink, surface);
}

void DisplayManager::drawNanoButton(const Button &button) {
  // Plugin screens lay their buttons edge to edge (full-height pairs, list
  // rows beside their delete zones). Painting each tile a few pixels inside
  // its tap rect gives them the same gutters as every other Nano screen;
  // the tap geometry the plugins hit-test stays untouched.
  const bool backCorner = button.icon == ui::IconId::Back && button.label.isEmpty() && button.width <= 48;
  const bool tile = button.kind != Button::ButtonKind::Slider && button.kind != Button::ButtonKind::Label &&
                    button.kind != Button::ButtonKind::Separator && !backCorner && button.width >= 40 &&
                    button.height >= 30;
  const int inset = tile ? 3 : 0;
  const ui::Rect rect{static_cast<uint16_t>(button.x + inset), static_cast<uint16_t>(button.y + inset),
                      static_cast<uint16_t>(button.width - inset * 2), static_cast<uint16_t>(button.height - inset * 2)};
  switch (button.kind) {
    case Button::ButtonKind::Separator:
      nanoSeparator(rect, button.label);
      return;
    case Button::ButtonKind::Label:
      nanoText(ui::Rect(rect.x + 6, rect.y, rect.w - 12, button.sublabel.isEmpty() ? rect.h : rect.h / 2),
               button.label, 2, nanoColor(NanoRole::Muted), NanoAlign::Center, 3);
      if (!button.sublabel.isEmpty()) {
        nanoText(ui::Rect(rect.x + 6, rect.y + rect.h / 2, rect.w - 12, rect.h / 2), button.sublabel, 2,
                 nanoColor(NanoRole::Muted), NanoAlign::Center, 2);
      }
      return;
    case Button::ButtonKind::Toggle:
      nanoToggle(rect, button.label, button.active, button.armed);
      return;
    case Button::ButtonKind::Slider: {
      // Full-screen drag editor. The bar keeps sliderTrackRectFor()'s x
      // range (the touch mapping reads it back).
      const bool hasValueLabels =
          !button.sliderValueLabels.empty() && button.sliderValue < button.sliderValueLabels.size();
      const String valueText = hasValueLabels ? button.sliderValueLabels[button.sliderValue]
                                              : String(button.sliderValue) + button.sliderUnit;
      nanoText(ui::Rect(rect.x + 64, rect.y + 4, std::max(0, static_cast<int>(rect.w) - 128), 22), button.label, 1,
               nanoColor(NanoRole::Muted), NanoAlign::Center);
      nanoText(ui::Rect(rect.x, rect.y + 26, rect.w, 34), valueText, 3, nanoColor(NanoRole::Foreground),
               NanoAlign::Center);
      const ui::Rect track = sliderTrackRectFor(button);
      constexpr int kBarHeight = 24;
      const int barY = track.y + track.h / 2 - kBarHeight / 2;
      const int range = static_cast<int>(button.sliderMax) - static_cast<int>(button.sliderMin);
      const int value = std::max(static_cast<int>(button.sliderMin),
                                 std::min(static_cast<int>(button.sliderValue), static_cast<int>(button.sliderMax)));
      const int fill = range > 0 ? 12 + (static_cast<int>(track.w) - 12) * (value - button.sliderMin) / range
                                 : static_cast<int>(track.w);
      nanoFillRoundRect(track.x, barY, track.w, kBarHeight, kBarHeight / 2, nanoColor(NanoRole::SurfaceMuted));
      nanoFillRoundRect(track.x, barY, std::max(fill, kBarHeight), kBarHeight, kBarHeight / 2,
                        nanoColor(NanoRole::Accent));
      nanoFillCircle(track.x + std::max(fill, kBarHeight) - kBarHeight / 2, barY + kBarHeight / 2,
                     kBarHeight / 2 - 4, nanoColor(NanoRole::OnAccent));
      const String minText = hasValueLabels && button.sliderMin < button.sliderValueLabels.size()
                                 ? button.sliderValueLabels[button.sliderMin]
                                 : String(button.sliderMin);
      const String maxText = hasValueLabels && button.sliderMax < button.sliderValueLabels.size()
                                 ? button.sliderValueLabels[button.sliderMax]
                                 : String(button.sliderMax);
      const int endY = barY + kBarHeight + 2;
      nanoText(ui::Rect(track.x, endY, track.w / 2, nanoLineHeight(1)), minText, 1, nanoColor(NanoRole::Muted),
               NanoAlign::Start);
      nanoText(ui::Rect(track.x + track.w / 2, endY, track.w / 2, nanoLineHeight(1)), maxText, 1,
               nanoColor(NanoRole::Muted), NanoAlign::End);
      return;
    }
    default:
      break;
  }

  const bool accentFill = button.armed || (button.active && button.icon != ui::IconId::None);
  const uint16_t surface = accentFill ? nanoColor(NanoRole::Accent) : nanoColor(NanoRole::SurfaceMuted);
  const uint16_t ink = accentFill ? nanoColor(NanoRole::OnAccent) : nanoColor(NanoRole::Foreground);
  if (button.label.isEmpty() && button.icon != ui::IconId::None) {
    // Icon-only button (back corner, delete zone, transport controls).
    nanoFillRoundRect(rect.x, rect.y, rect.w, rect.h, std::min<int>(kNanoRadius, rect.h / 2), surface);
    nanoGenericIcon(button.icon, rect, button.icon == ui::IconId::Delete && !accentFill
                                           ? nanoColor(NanoRole::Muted)
                                           : ink,
                    surface);
    return;
  }
  if (button.icon != ui::IconId::None && button.previewTypeface == ReaderTypeface::Count) {
    nanoFillRoundRect(rect.x, rect.y, rect.w, rect.h, kNanoRadius, surface);
    const uint16_t iconInk = accentFill ? ink : nanoColor(NanoRole::Accent);
    if (rect.h >= 70 && rect.w >= 120) {
      // Big launcher-style button (plugin home screens): icon over label.
      const int labelH = nanoLineHeight(2);
      const int top = rect.y + (static_cast<int>(rect.h) - (24 + 8 + labelH)) / 2;
      nanoGenericIcon(button.icon, ui::Rect(rect.x, top, rect.w, 24), iconInk, surface);
      nanoText(ui::Rect(rect.x + 8, top + 30, rect.w - 16, labelH), button.label, 2, ink, NanoAlign::Center);
      return;
    }
    nanoGenericIcon(button.icon, ui::Rect(rect.x + 8, rect.y, 24, rect.h), iconInk, surface);
    nanoText(ui::Rect(rect.x + 38, rect.y, std::max(0, static_cast<int>(rect.w) - 46), rect.h), button.label, 2, ink,
             NanoAlign::Start, rect.h >= 48 ? 2 : 1);
    return;
  }
  nanoButton(rect, button.label, true, NanoIcon::None, rect.h >= 48 ? 2 : 1, button.sublabel, "",
             button.active && !button.armed, button.armed, button.previewTypeface);
}

// ─── Full-screen status renderers (Nano skin) ───────────────────────────────

void DisplayManager::renderNanoStatusScreen(const String &title, const String &line1, const String &line2,
                                            int progressPercent) {
  nanoBeginFrame();
  constexpr int kMargin = 24;
  const int w = kNanoScreenW - kMargin * 2;
  nanoIcon(ui::Rect(6, 6, 24, 24), NanoIcon::ChevronLeft, nanoColor(NanoRole::Muted), nanoColor(NanoRole::Background));
  nanoBatteryInline(ui::Rect(kNanoScreenW - 96, 6, 90, 22));
  const int titleH = nanoLineHeight(1);
  const int line1H = line1.isEmpty() ? 0 : nanoLineHeight(3) + 6;
  const int line2H = line2.isEmpty() ? 0 : nanoLineHeight(2);
  const int barH = progressPercent >= 0 ? 18 : 0;
  const int block = titleH + line1H + line2H + barH;
  int y = std::max(28, (kNanoScreenH - block) / 2);
  nanoText(ui::Rect(kMargin, y, w, titleH), title, 1, nanoColor(NanoRole::Muted), NanoAlign::Center);
  y += titleH;
  if (!line1.isEmpty()) {
    nanoText(ui::Rect(kMargin, y + 2, w, line1H - 4), line1, 3, nanoColor(NanoRole::Foreground), NanoAlign::Center);
    y += line1H;
  }
  if (!line2.isEmpty()) {
    nanoLabel(ui::Rect(kMargin, y, w, line2H), line2, 2, NanoRole::Accent, NanoAlign::Center);
    y += line2H;
  }
  if (progressPercent >= 0) {
    const int barW = std::min(360, w);
    nanoProgress(ui::Rect((kNanoScreenW - barW) / 2, y + 10, barW, 6), progressPercent, 0, 100);
  }
  nanoEndFrame();
}

ui::Rect DisplayManager::qrCornerButtonRect() const {
  if (modernCardStyle_) {
    return ui::Rect(kNanoScreenW - 150, kNanoScreenH - 44, 142, 36);
  }
  return ui::Rect(kDisplayWidth - 74, kDisplayHeight - 24, 70, 20);
}

void DisplayManager::renderNanoQrScreen(const String &title, const String &line1, const bool *qrData,
                                        uint8_t qrSize, const String &hint, const String &cornerHint) {
  nanoBeginFrame();
  // QR: black modules on white with a 3-module quiet zone, as large as fits.
  const int quiet = 3;
  const int total = qrSize + quiet * 2;
  const int module = std::max(2, std::min(6, (kNanoScreenH - 12) / total));
  const int side = total * module;
  const int qx = 10;
  const int qy = (kNanoScreenH - side) / 2;
  nanoFillRoundRect(qx, qy, side, side, 6, 0xFFFF);
  for (uint8_t row = 0; row < qrSize; ++row) {
    for (uint8_t col = 0; col < qrSize; ++col) {
      if (qrData[row * qrSize + col]) {
        nanoFillRect(qx + (quiet + col) * module, qy + (quiet + row) * module, module, module, 0x0000);
      }
    }
  }
  const int textX = qx + side + 18;
  const int textW = kNanoScreenW - textX - 14;
  nanoBatteryInline(ui::Rect(kNanoScreenW - 96, 6, 90, 22));
  const int titleH = nanoLineHeight(3);
  const int line1H = line1.isEmpty() ? 0 : nanoLineHeight(2);
  const int hintH = hint.isEmpty() ? 0 : nanoLineHeight(1) * 2;
  const int bottom = cornerHint.isEmpty() ? kNanoScreenH - 8 : kNanoScreenH - 52;
  const int block = titleH + line1H + hintH;
  int y = std::max(24, 28 + (bottom - 28 - block) / 2);
  nanoText(ui::Rect(textX, y, textW, titleH), title, 3, nanoColor(NanoRole::Foreground));
  y += titleH;
  if (!line1.isEmpty()) {
    nanoLabel(ui::Rect(textX, y, textW, line1H), line1, 2, NanoRole::Accent);
    y += line1H;
  }
  if (!hint.isEmpty()) {
    nanoText(ui::Rect(textX, y, textW, hintH), hint, 1, nanoColor(NanoRole::Muted), NanoAlign::Start, 2);
  }
  if (!cornerHint.isEmpty()) {
    const ui::Rect button = qrCornerButtonRect();
    nanoPill(button, cornerHint, NanoIcon::None, false, true);
  }
  nanoEndFrame();
}

void DisplayManager::renderNanoFocusTimer(const String &mode, const String &timer, const String &instruction,
                                          int progressPercent, bool breakAccent) {
  nanoBeginFrame();
  const uint16_t accent = breakAccent ? focusTimerBreakColor() : nanoColor(NanoRole::Accent);
  // Back chevron: FocusTimerCore hit-tests the top-left corner (kBackZone*).
  nanoIcon(ui::Rect(8, 4, 22, 22), NanoIcon::ChevronLeft, nanoColor(NanoRole::Muted), nanoColor(NanoRole::Background));
  nanoText(ui::Rect(40, 4, kNanoScreenW - 150, 22), mode, 2, nanoReadable(accent, nanoColor(NanoRole::Background)));
  nanoBatteryInline(ui::Rect(kNanoScreenW - 96, 4, 90, 22));
  // The time in the reading font's big embedded face (Atkinson): the UI
  // strikes top out at ~26 px.
  constexpr uint8_t kTimerScale = 105;
  const int timerW = nanoTypefaceTextWidth(timer, kTimerScale, ReaderTypeface::AtkinsonHyperlegible);
  const int timerH = kEmbeddedAtkinsonHeight * kTimerScale / 100;
  const int timerTop = 30 + std::max(0, (100 - timerH) / 2);
  nanoTypefaceText((kNanoScreenW - timerW) / 2, timerTop, timer, nanoColor(NanoRole::Foreground), kTimerScale,
                   ReaderTypeface::AtkinsonHyperlegible);
  const int barY = 134;
  const int barX = 40;
  const int barW = kNanoScreenW - 80;
  nanoFillRoundRect(barX, barY, barW, 6, 3, nanoColor(NanoRole::ProgressTrack));
  if (progressPercent > 0) {
    nanoFillRoundRect(barX, barY, std::max(6, barW * std::min(progressPercent, 100) / 100), 6, 3, accent);
  }
  nanoText(ui::Rect(16, 146, kNanoScreenW - 32, 22), instruction, 1, nanoColor(NanoRole::Muted), NanoAlign::Center);
  nanoEndFrame();
}

void DisplayManager::renderNanoMenuList(const std::vector<String> &items, size_t selectedIndex) {
  // Same row geometry as the classic renderMenu() -- plugins hit-test it.
  const size_t itemCount = items.size();
  const size_t visibleCount =
      std::min(itemCount, static_cast<size_t>(std::max(1, kNanoScreenH / kCompactMenuRowHeight)));
  size_t firstVisible = 0;
  if (selectedIndex >= visibleCount / 2) {
    firstVisible = selectedIndex - visibleCount / 2;
  }
  if (firstVisible + visibleCount > itemCount) {
    firstVisible = itemCount - visibleCount;
  }
  const int rowHeight = kCompactMenuRowHeight;
  int y = std::max(0, (kNanoScreenH - rowHeight * static_cast<int>(visibleCount)) / 2);
  nanoBeginFrame();
  nanoIcon(ui::Rect(2, 2, 22, 22), NanoIcon::ChevronLeft, nanoColor(NanoRole::Muted), nanoColor(NanoRole::Background));
  nanoBatteryInline(ui::Rect(kNanoScreenW - 90, 2, 86, 20));
  for (size_t row = 0; row < visibleCount; ++row) {
    const size_t index = firstVisible + row;
    const bool selected = index == selectedIndex;
    if (selected) {
      nanoFillRoundRect(kCompactMenuX - 12, y + 1, kNanoScreenW - 2 * (kCompactMenuX - 12) - 90, rowHeight - 2, 6,
                        nanoColor(NanoRole::SurfaceActive));
      nanoFillRoundRect(kCompactMenuX - 10, y + 5, 3, rowHeight - 10, 1, nanoColor(NanoRole::Accent));
    }
    nanoText(ui::Rect(kCompactMenuX, y, kNanoScreenW - kCompactMenuX - 104, rowHeight), items[index], 2,
             nanoColor(selected ? NanoRole::Foreground : NanoRole::Muted));
    y += rowHeight;
  }
  nanoEndFrame();
}
