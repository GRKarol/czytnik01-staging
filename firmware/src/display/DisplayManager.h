#pragma once

#include <Arduino.h>
#include <vector>

#include "board/BoardConfig.h"
#include "display/Icons.h"
#include "ui/UiGrid.h"

class DisplayManager {
 public:
  enum class ReaderTypeface : uint8_t {
    Standard = 0,
    OpenDyslexic = 1,
    AtkinsonHyperlegible = 2,
    // Book-typeface additions (tools/generate_embedded_font.py) — glyph data
    // for these lives on the SD card, loaded on demand by SdFontLoader (see
    // sdFontBaseName()/ensureExtraTypefaceLoaded() in DisplayManager.cpp).
    Literata = 3,
    Merriweather = 4,
    Lora = 5,
    Bitter = 6,
    EBGaramond = 7,
    Vollkorn = 8,
    Gelasio = 9,
    PtSerif = 10,
    IbmPlexSerif = 11,
    Cardo = 12,
    ZillaSlab = 13,
    OldStandard = 14,
    Domine = 15,
    Alegreya = 16,
    Newsreader = 17,
    NotoSerif = 18,
    Spectral = 19,
    Count = 20,
  };

  struct TypographyConfig {
    ReaderTypeface typeface = ReaderTypeface::Standard;
    bool focusHighlight = true;
    int8_t trackingPx = 0;
    uint8_t anchorPercent = 35;
    uint8_t guideHalfWidth = 20;
    uint8_t guideGap = 0;
  };

  struct ContextWord {
    String text;
    bool paragraphStart = false;
    bool current = false;
  };

  struct ReaderChrome {
    ReaderChrome()
        : showBattery(true),
          showChapter(true),
          showProgress(true),
          showPreviousSentenceHint(true),
          showSavePointButton(false) {}

    bool showBattery;
    bool showChapter;
    bool showProgress;
    bool showPreviousSentenceHint;
    bool showSavePointButton;
  };

  struct LibraryItem {
    String title;
    String subtitle;
  };

  struct Button {
    // Shape the button draws itself as. Rect covers the default label/icon
    // tile; Toggle and Cycle exist so a setting's current value is legible
    // at a glance (a slider knob position, or which of N dots is lit)
    // instead of every control looking like the same rectangle regardless
    // of what it does.
    enum class ButtonKind : uint8_t {
      Rect = 0,
      Toggle = 1,
      Cycle = 2,
      // Non-interactive divider row (e.g. the "---" line between installed
      // plugins and the plugin library entry) — drawn as a thin line, never
      // tappable, never armed.
      Separator = 3,
      // Full-width drag slider (e.g. pacing delay editors): sliderValue
      // between sliderMin/sliderMax is drawn as a big numeric readout plus a
      // track+knob. Touch handling lives in App::handlePacingDelayEditorTouch,
      // which reads the exact same rect back via sliderTrackRectFor() so the
      // knob's drawn position and the drag hit-test never drift apart.
      Slider = 4,
      // Plain description text (e.g. a plugin's description in its detail
      // screen) — no border/fill, never armed or tap-selectable, so it reads
      // as prose instead of a button that does nothing when tapped. label is
      // line 1, sublabel (optional) is line 2.
      Label = 5,
    };

    String label;
    String sublabel;  // optional second line (library-style title+subtitle buttons)
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    bool accent = false;
    bool active = false;
    // Two-step tap confirm (arm-then-confirm, see App::handleGridTap):
    // drawn filled/outlined in focusColor() instead of the normal style.
    bool armed = false;
    // Icon badge in the button's corner. iconBitmap (if set) wins over the
    // vector glyph — that's the seam for swapping placeholders with real
    // RGB565 art later, same pattern as the embedded fonts.
    ui::IconId icon = ui::IconId::None;
    const uint16_t *iconBitmap = nullptr;
    uint8_t iconW = 0;
    uint8_t iconH = 0;
    // Icon-only buttons (label empty) otherwise scale the glyph to fill
    // almost the whole tile (min(width,height)-8) — fine for a square back
    // corner, comically oversized for a wide/short delete zone in a list
    // row. 0 = no cap (existing auto-fit behavior).
    uint8_t iconMaxSize = 0;
    ButtonKind kind = ButtonKind::Rect;
    // Kind::Toggle: `active` is the on/off value, drawn as a track+knob.
    // Kind::Cycle: cycleCount dots are drawn, cycleState (0-based) is lit —
    // for settings that step through a short fixed list of named values.
    uint8_t cycleState = 0;
    uint8_t cycleCount = 0;
    // Kind::Slider only.
    uint16_t sliderMin = 0;
    uint16_t sliderMax = 0;
    uint16_t sliderValue = 0;
    // Suffix drawn right after the big numeric readout (e.g. " ms", " WPM").
    String sliderUnit = " ms";
    // Kind::Slider only, optional: when non-empty, the big readout shows
    // sliderValueLabels[sliderValue] (e.g. "Maly"/"Sredni"/"Duzy" for font
    // size) instead of the raw number+unit — for a slider over a short list
    // of named stops rather than a true numeric range.
    std::vector<String> sliderValueLabels;
    // Font-picker screen only: draw this button's label in that specific
    // typeface instead of the globally active reader typeface, so each
    // button previews its own font by name. ReaderTypeface::Count is the
    // sentinel for "no override, use the global one" (see drawButtons()).
    ReaderTypeface previewTypeface = ReaderTypeface::Count;
  };

  // Track rect (in the same virtual-screen coordinates as touch events) for
  // a Kind::Slider button, derived purely from that button's own x/y/w/h.
  // drawButtons() and App's drag handler both call this with the same
  // button geometry so the drawn knob and the touch hit-test can't diverge.
  static ui::Rect sliderTrackRectFor(const Button &button);

  ~DisplayManager();

  bool begin();
  void setBatteryLabel(const String &label);
  void setBrightnessPercent(uint8_t percent);
  void setFocusColorIndex(uint8_t index);
  uint8_t focusColorIndex() const;
  void setDarkMode(bool darkMode);
  void setNightMode(bool nightMode);
  void setUiOrientation(BoardConfig::UiOrientation orientation);
  void setUiRotated180(bool rotated180);
  void setTypographyConfig(const TypographyConfig &config);
  // True at most once per failed/missing SD font load (see SdFontLoader) —
  // clears itself on read so callers show the warning exactly once, right
  // after the user action that triggered the load attempt.
  bool consumeFontLoadFailure();
  // Non-destructive peek (unlike consumeFontLoadFailure): true once the
  // currently-configured typeface's glyph data is actually usable — always
  // true for the 3 built-in faces, true for an SD-backed face only once its
  // .fnt pair has been read into PSRAM. Lets callers retry a boot-time SD
  // load that raced the card mount without disturbing the picker's one-shot
  // failure toast.
  bool isActiveTypefaceLoaded() const;
  // True for the 3 built-in (flash) faces; false for the 17 SD-backed ones
  // added by tools/generate_embedded_font.py --fnt-output.
  static bool isSdBackedTypeface(ReaderTypeface typeface);
  // Lowercase file stem under /fonts/ for an SD-backed typeface (e.g.
  // "literata"); empty for built-in faces. Callers build "/fonts/<name>.fnt"
  // and "/fonts/<name>_70.fnt" from it.
  static String sdFontFileBaseName(ReaderTypeface typeface);
  // Cheap existence check (SD_MMC.exists, no PSRAM allocation) — always true
  // for built-in faces; for SD-backed faces only once both the base and
  // _70 .fnt files are present on the card. Safe to call from any task.
  static bool isTypefaceAvailableOnSd(ReaderTypeface typeface);
  void setScrollFontSize(uint8_t level);
  void setScrollLineSpacing(uint8_t level);
  void setScrollMargin(uint8_t level);
  TypographyConfig typographyConfig() const;
  bool darkMode() const;
  bool nightMode() const;
  void prepareForSleep();
  bool wakeFromSleep();
  void renderCenteredWord(const String &word, uint16_t color = 0xFFFF);
  void renderBootSplash(uint32_t blackMs);
  void fadeInBacklight(uint32_t fadeMs);
  void fadeOutBacklight(uint32_t fadeMs);
  void renderRsvpWord(const String &word, const String &chapterLabel = "",
                      uint8_t progressPercent = 0, bool showFooter = true,
                      const String &footerStatusLabel = "",
                      ReaderChrome chrome = ReaderChrome());
  void renderRsvpWordWithWpm(const String &word, uint16_t wpm, const String &chapterLabel = "",
                             uint8_t progressPercent = 0, bool showFooter = true,
                             const String &footerStatusLabel = "",
                             ReaderChrome chrome = ReaderChrome());
  void renderPhantomRsvpWord(const String &beforeText, const String &word, const String &afterText,
                             uint8_t fontSizeLevel, const String &chapterLabel = "",
                             uint8_t progressPercent = 0, bool showFooter = true,
                             const String &footerStatusLabel = "",
                             ReaderChrome chrome = ReaderChrome());
  void renderPhantomRsvpWordWithWpm(const String &beforeText, const String &word,
                                    const String &afterText, uint8_t fontSizeLevel, uint16_t wpm,
                                    const String &chapterLabel = "",
                                    uint8_t progressPercent = 0, bool showFooter = true,
                                    const String &footerStatusLabel = "",
                                    ReaderChrome chrome = ReaderChrome());
  void renderTypographyPreview(const String &beforeText, const String &word, const String &afterText,
                               uint8_t fontSizeLevel, const String &title,
                               const String &line1 = "", const String &line2 = "");
  void renderScrollView(const std::vector<ContextWord> &words, uint32_t contentToken,
                        size_t windowStartIndex, size_t currentWordIndex,
                        uint16_t scrollProgressPermille = 0, const String &chapterLabel = "",
                        uint8_t progressPercent = 0, const String &overlayText = "",
                        const String &footerStatusLabel = "",
                        ReaderChrome chrome = ReaderChrome());
  void renderWordTickerView(const std::vector<ContextWord> &words, size_t currentWordIndex,
                            uint8_t fontSizeLevel, uint16_t motionPermille = 0,
                            const String &chapterLabel = "", uint8_t progressPercent = 0,
                            const String &overlayText = "", bool showFooter = true,
                            ReaderChrome chrome = ReaderChrome());
  void renderMenu(const char *const *items, size_t itemCount, size_t selectedIndex);
  void renderMenu(const std::vector<String> &items, size_t selectedIndex);
  void renderMenuWithDPad(const std::vector<String> &items, size_t selectedIndex);
  // Swipe/scroll nav mode: a bare, full-width scrollable text list — no
  // button chrome, no D-Pad panel. Drag scrolls (App::handleSwipeListGesture
  // shifts selectedIndex before calling this), tap picks the row under the
  // finger. See DisplayManager::renderMenuWithDPad() for the shared
  // windowing math this mirrors.
  void renderMenuScroll(const std::vector<String> &items, size_t selectedIndex);
  void renderLibrary(const std::vector<LibraryItem> &items, size_t selectedIndex);
  void renderTextEntry(const String &title, const String &prompt, const String &value,
                       const String &helperText, const std::vector<Button> &buttons);
  void renderButtonGrid(const String &title, const std::vector<Button> &buttons, size_t pageIndex,
                        size_t pageCount, const String &toastText = "",
                        bool showBatteryBadge = true, bool dotsOnLeft = false,
                        bool prominentTitle = false);
  void renderStatus(const String &title, const String &line1 = "", const String &line2 = "");
  // `hint` to trzecia, przygaszona linijka pod QR-em. Domyślnie zdanie dla
  // ekranu parowania z telefonem; ekran „zainstaluj aplikację" podaje swoje.
  void renderStatusWithQr(const String &title, const String &line1, const bool *qrData,
                          uint8_t qrSize, const String &hint = "Scan to connect");
  void renderProgress(const String &title, const String &line1 = "", const String &line2 = "",
                      int progressPercent = -1);
  void renderLifeScreensaver(const std::vector<uint32_t> &cells, uint16_t columns, uint16_t rows,
                             uint32_t generation,
                             const std::vector<uint32_t> *dimCells = nullptr,
                             const String &hintText = "", uint8_t hintAlpha = 0,
                             const String &styleLabel = "", uint8_t styleLabelAlpha = 0);
  void renderFocusTimerScreen(const String &mode, const String &genre, const String &timer,
                              const String &instruction, const String &footer = "",
                              int progressPercent = -1, bool breakAccent = false);

  // Word-wraps `body` to the display width and draws the page starting at
  // `scrollLine` (0 = top), with `title` as a header line and a scroll
  // indicator on the right edge. Returns the total wrapped line count so a
  // caller (e.g. a plugin) can clamp scrollLine without redoing the wrap
  // math itself — see PluginDisplayService::renderArticleReader.
  int renderArticleReader(const String &title, const String &body, int scrollLine);

 private:
  bool initPanel();
  bool allocateBuffers();
  bool drawBitmap(int xStart, int yStart, int xEnd, int yEnd, const void *colorData);
  void fillScreen(uint16_t color);
  void clearVirtualBuffer(int width, int height);
  uint16_t backgroundColor() const;
  uint16_t wordColor() const;
  uint16_t focusColor() const;
  uint16_t dimColor() const;
  uint16_t footerColor() const;
  uint16_t selectedBarColor() const;
  uint16_t blendOverBackground(uint16_t rgb565, uint8_t alpha) const;
  int chooseTextScale(const String &word) const;
  int measureTextWidth(const String &word) const;
  int measureSerifTextWidth(const String &text, int divisor) const;
  int measureSerif70TextWidth(const String &text) const;
  int measureSerifTextWidthScaled(const String &text, uint8_t scalePercent) const;
  int measureTinyTextWidth(const String &text, int scale) const;
  String fitSerifText(const String &text, int maxWidth, int divisor) const;
  String fitSerifTextScaled(const String &text, int maxWidth, uint8_t scalePercent) const;
  String fitSerifTextTrailingScaled(const String &text, int maxWidth, uint8_t scalePercent) const;
  String fitTinyText(const String &text, int maxWidth, int scale) const;
  String fitTinyTextTrailing(const String &text, int maxWidth, int scale) const;
  void drawGlyph(int x, int y, char c, uint16_t color);
  void drawGlyph(int x, int y, char c, uint16_t color, ReaderTypeface typeface);
  void drawSerifGlyphScaled(int x, int y, char c, uint16_t color, int divisor);
  void drawSerifGlyphScaled(int x, int y, char c, uint16_t color, int divisor,
                            ReaderTypeface typeface);
  void drawSerif70Glyph(int x, int y, char c, uint16_t color);
  void drawSerif70Glyph(int x, int y, char c, uint16_t color, ReaderTypeface typeface);
  void drawSerifGlyphScaledPercent(int x, int y, char c, uint16_t color, uint8_t scalePercent);
  void drawSerifGlyphScaledPercent(int x, int y, char c, uint16_t color, uint8_t scalePercent,
                                   ReaderTypeface typeface);
  void fillVirtualRect(int x, int y, int width, int height, uint16_t color);
  void drawSerifTextAt(const String &text, int x, int y, uint16_t color, int divisor);
  void drawSerif70TextAt(const String &text, int x, int y, uint16_t color);
  void drawSerifTextScaledAt(const String &text, int x, int y, uint16_t color,
                             uint8_t scalePercent);
  void drawTinyGlyph(int x, int y, char c, uint16_t color, int scale);
  void drawTinyTextAt(const String &text, int x, int y, uint16_t color, int scale);
  void drawTinyTextCentered(const String &text, int y, uint16_t color, int scale);
  void drawTinyTextCentered(const String &text, int y, uint16_t color, int scale, int width,
                            int xOffset);
  void drawSerif70TextCentered(const String &text, int y, uint16_t color, int width, int xOffset);
  void drawSerifTextScaledCentered(const String &text, int y, uint16_t color, uint8_t scalePercent,
                                   int width, int xOffset);
  void drawButtons(const std::vector<Button> &buttons);
  void drawIcon(ui::IconId id, int x, int y, int size, uint16_t color);
  void blitIconBitmap(const uint16_t *bitmap, uint8_t w, uint8_t h, int x, int y);
  // Straight-line helper for the vector icon placeholders in drawIcon() —
  // linear-interpolation stepping (not true Bresenham, but plenty for
  // icons a few dozen pixels across) so glyphs aren't limited to
  // axis-aligned rects.
  void drawIconLine(int x0, int y0, int x1, int y1, uint16_t color, int thickness = 1);
  void drawFilledCircle(int cx, int cy, int radius, uint16_t color);
  void drawBatteryBadge();
  void drawBatteryBadge(int logicalWidth, int logicalHeight);
  void drawPreviousSentenceHint();
  void drawSavePointButton();
  void drawSavePointButton(int logicalWidth, int logicalHeight);
  void drawFooter(const String &chapterLabel, const String &statusLabel,
                  const ReaderChrome &chrome);
  void drawRsvpAnchorGuide(int anchorX, int textY, int textHeight);
  void drawWordAt(const String &word, int x, int y, uint16_t color);
  void drawRsvpWordAt(const String &word, int x, int y, int focusIndex);
  void drawRsvp70WordAt(const String &word, int x, int y, int focusIndex);
  void drawRsvpWordScaledAt(const String &word, int x, int y, int focusIndex, int divisor);
  void drawRsvpWordScaledPercentAt(const String &word, int x, int y, int focusIndex,
                                   uint8_t scalePercent);
  void drawWordLine(const String &word, int y, uint16_t color);
  void drawMenuItem(const String &item, int y, bool selected);
  void applyBrightness();
  void flushScaledFrame(int scale, int virtualWidth, int virtualHeight);
  void flushFullWidthLogicalBand(int yStart, int yEnd);
  int logicalWidth() const;
  int logicalHeight() const;
  uint16_t focusTimerBreakColor() const;

  uint16_t *virtualFrame_ = nullptr;
  uint16_t *txBuffer_ = nullptr;
  size_t txBufferBytes_ = 0;
  bool initialized_ = false;
  uint8_t brightnessPercent_ = 100;
  uint8_t focusColorIndex_ = 1;  // 0=red, 1=blue, 2=green, 3=yellow, 4=orange, 5=purple
  bool darkMode_ = true;
  bool nightMode_ = false;
  BoardConfig::UiOrientation uiOrientation_ =
      BoardConfig::UI_ROTATED_180 ? BoardConfig::UiOrientation::LandscapeFlipped
                                  : BoardConfig::UiOrientation::Landscape;
  bool tickerPlaybackFrameActive_ = false;
  String lastRenderKey_;
  String batteryLabel_;
  // Word-wrap is comparatively expensive (String concatenation in a loop)
  // and renderArticleReader() must return the fresh total-line count on
  // every call (even when lastRenderKey_ skips the redraw), so the wrap
  // itself is cached separately keyed on the raw title+body, not on the
  // scroll position.
  String articleReaderSourceCache_;
  std::vector<String> articleReaderLinesCache_;
  uint8_t scrollFontSize_ = 4;
  uint8_t scrollLineSpacing_ = 1;
  uint8_t scrollMargin_ = 1;

  int scrollLineHeightPx() const;
  int scrollMarginPx() const;
  int scrollSerifDivisor() const;
  uint8_t scrollScalePercent() const;
};
