#pragma once

#include <Arduino.h>
#include <vector>

#include "board/BoardConfig.h"
#include "display/Icons.h"
#include "ui/UiGrid.h"

// An RGB565 picture held in memory: book covers and spines sent from the
// Flower app (storage/BookExtras.h).
struct NanoImage {
  uint16_t width = 0;
  uint16_t height = 0;
  const uint16_t *pixels = nullptr;
  bool valid() const { return pixels != nullptr && width > 0 && height > 0; }
};

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
          showSavePointButton(false),
          savePointAtCurrentPosition(false) {}

    bool showBattery;
    bool showChapter;
    bool showProgress;
    bool showPreviousSentenceHint;
    bool showSavePointButton;
    // Current reading position exactly matches an existing save point —
    // drawSavePointButton() draws a solid ribbon instead of a hollow one.
    bool savePointAtCurrentPosition;
  };

  struct LibraryItem {
    String title;
    String subtitle;
    // 0-100 (0 also covers "no saved progress yet") — only read by the
    // Modern nav mode's bookshelf (App::renderNanoLibrary()); every other
    // library rendering path ignores it. No default value: a member
    // initializer here would make this a non-aggregate under the firmware's
    // C++ standard, breaking every existing `push_back({title, subtitle})`
    // call site across App.cpp. Callers that don't care leave it
    // zero-initialized by that same aggregate-init rule; libraryItemForBook()
    // sets it explicitly either way.
    uint8_t progressPercent;
  };

  // ─── Nano skin (App::NavMode::Modern) ────────────────────────────────────
  // Modern UI for this 640x172 panel, grown out of rsvpnano's "regular"
  // layout: flat rounded tiles, a tab rail, one accent color, and
  // proportional anti-aliased UI fonts (display/NanoUiFonts.h, picked in
  // Motywy). App lays every screen out and does all hit-testing; these
  // functions only paint. A screen is drawn between nanoBeginFrame() and
  // nanoEndFrame().
  enum class NanoRole : uint8_t {
    Background,
    Foreground,
    Muted,
    Subtle,
    Accent,
    OnAccent,
    SurfaceMuted,
    SurfaceActive,
    Outline,
    ProgressTrack,
  };

  enum class NanoAlign : uint8_t {
    Start,
    Center,
    End,
  };

  enum class NanoIcon : uint8_t {
    None,
    Bookmark,
    Books,
    Edit,
    Device,
    Language,
    Hourglass,
    Power,
    Apps,
    Palette,
    ChevronLeft,
    ChevronRight,
    Play,
    Pause,
    Plus,
    Minus,
    List,
    Trash,
    Record,
    Stop,
    Wifi,
    Bluetooth,
    Usb,
    Phone,
    SdCard,
    Info,
    Download,
    Sort,
    Check,
    Font,
    Help,
    Sun,
    Sliders,
    Book,
    Restart,
    Rewind,
    Target,
    Moon,
    Image,
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
  // Raw battery reading for the Nano skin's battery icon (fill level +
  // charging bolt); the text badge keeps using setBatteryLabel().
  void setBatteryState(bool present, uint8_t percent, bool charging);
  // How every battery indicator (rail footer, reader panel, reading screen)
  // shows the charge: icon + "76%", the number inside the icon, the bare
  // number, or the icon alone. Stored in NVS by App, so only append.
  static constexpr uint8_t kBatteryStyleIconPercent = 0;
  static constexpr uint8_t kBatteryStyleNumberInIcon = 1;
  static constexpr uint8_t kBatteryStyleNumberOnly = 2;
  static constexpr uint8_t kBatteryStyleIconOnly = 3;
  static constexpr uint8_t kBatteryStyleCount = 4;
  void setBatteryStyle(uint8_t style);
  uint8_t batteryStyle() const { return batteryStyle_; }
  void setBrightnessPercent(uint8_t percent);
  void setFocusColorIndex(uint8_t index);
  uint8_t focusColorIndex() const;
  // Letter color picked from the full palette (Motywy > Czytanie > Kolor
  // litery): focusColorIndex() then reads kFocusColorCustom.
  static constexpr uint8_t kFocusColorCustom = 0xFE;
  void setCustomFocusColor(uint16_t color);
  uint16_t customFocusColor() const { return customFocusColor_; }
  static uint16_t presetFocusColor(uint8_t index);
  static uint8_t presetFocusColorCount();
  // Reading-screen colors of a theme (0 dark, 1 light, 2 night) with the
  // current letter color, for theme previews.
  void readerThemeColors(uint8_t theme, uint16_t &background, uint16_t &word, uint16_t &focus) const;
  // Letter color as the day or the night theme draws it.
  uint16_t focusColorFor(bool night) const;
  void setDarkMode(bool darkMode);
  void setNightMode(bool nightMode);
  // Nano skin for the screens App still draws through the generic
  // Button-list renderers (renderButtonGrid()/renderTextEntry(): slider
  // editors, keyboard) — same geometry and hit-testing, only drawButtons()
  // and the surrounding chrome paint in the Nano style. See NanoRole.
  void setModernCardStyle(bool enabled);
  // Nano skin palette (Motywy tab). 0 = Classic: follows the Ciemny/Jasny/
  // Nocny theme and the highlight color like the rest of the firmware; the
  // others are fixed color sets (rsvpnano's themes/*.toml plus a few of
  // ours). `ownAccent` swaps a fixed palette's accent for the user's
  // highlight color. Only Nano-skin drawing reads this — the reading
  // screen keeps the Classic colors.
  static constexpr uint8_t kNanoPaletteClassic = 0;
  static uint8_t nanoPaletteCount();
  // English/brand name; App localizes the few that aren't proper names.
  static const char *nanoPaletteName(uint8_t palette);
  // Background, foreground and accent of a fixed palette (for the Flower
  // app's swatches); false for Classic, which follows the reading theme.
  static bool nanoPaletteSwatch(uint8_t palette, uint16_t &background, uint16_t &foreground, uint16_t &accent);
  void setNanoPalette(uint8_t palette, bool ownAccent);
  uint8_t nanoPalette() const { return nanoPalette_; }
  bool nanoOwnAccent() const { return nanoOwnAccent_; }
  void setUiOrientation(BoardConfig::UiOrientation orientation);
  void setUiRotated180(bool rotated180);
  void setTypographyConfig(const TypographyConfig &config);
  // True at most once per failed/missing SD font load (see SdFontLoader) —
  // clears itself on read so callers show the warning exactly once, right
  // after the user action that triggered the load attempt.
  bool consumeFontLoadFailure();
  // True when the last failed load deleted a damaged .fnt (re-downloaded
  // by the font pack retry), false when the file was simply missing.
  static bool lastFontLoadDamaged();
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

  // Status and progress screens pass their text through this, so fixed
  // English phrases (book opening, EPUB conversion, SD check) show in the
  // UI language. Returns nullptr for text it doesn't know.
  using PhraseLocalizer = const char *(*)(void *context, const char *text);
  void setPhraseLocalizer(PhraseLocalizer localizer, void *context) {
    phraseLocalizer_ = localizer;
    phraseLocalizerContext_ = context;
  }
  String localizedPhrase(const String &text) const;
  void setScrollFontSize(uint8_t level);
  void setScrollLineSpacing(uint8_t level);
  void setScrollMargin(uint8_t level);
  TypographyConfig typographyConfig() const;
  bool darkMode() const;
  bool nightMode() const;
  void prepareForSleep();
  bool wakeFromSleep();
  void renderCenteredWord(const String &word, uint16_t color = 0xFFFF);
  void renderBootSplash(uint32_t blackMs, uint32_t fadeMs);
  void fadeInBacklight(uint32_t fadeMs);
  void fadeOutBacklight(uint32_t fadeMs);
  // Fades the picture itself (not the backlight) between black and the
  // frame last drawn. Works at every brightness setting; fadeFrameOut()
  // leaves the backlight off, fadeFrameIn() turns it on.
  void fadeFrameIn(uint32_t fadeMs);
  void fadeFrameOut(uint32_t fadeMs);
  // Cross-fade from the picture on the panel to the next screen: after
  // beginCrossfade() renders still fill the frame buffer but nothing
  // reaches the panel; finishCrossfade() blends the old picture into the
  // last rendered frame over fadeMs, then shows it as is.
  void beginCrossfade();
  void finishCrossfade(uint32_t fadeMs);
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

  // ─── Nano skin primitives and widgets (see NanoRole above) ──────────────
  // Clears the frame to the background color.
  void nanoBeginFrame();
  // Pushes the frame to the panel, unless it is pixel-identical to the last
  // Nano frame and nothing else has drawn in between.
  void nanoEndFrame();
  uint16_t nanoColor(NanoRole role) const;
  // `role` of an arbitrary palette (theme chips preview palettes that are
  // not active yet).
  uint16_t nanoPaletteColor(uint8_t palette, NanoRole role) const;
  uint16_t nanoBlend(NanoRole role, uint8_t alpha) const;
  // UI font family (index into kNanoUiFamilies, NanoUiFonts.h).
  static uint8_t nanoUiFontCount();
  static const char *nanoUiFontName(uint8_t family);
  void setNanoUiFont(uint8_t family);
  uint8_t nanoUiFont() const;
  // Text metrics of the active UI font. `size` 1 = small, 2 = body,
  // 3 and 4 = large (one strike each, see NanoUiFonts.h).
  static int nanoTextWidth(const String &text, uint8_t size);
  static int nanoLineHeight(uint8_t size);
  static int nanoCapHeight(uint8_t size);
  // `text` cut to `maxWidth` px with a trailing ellipsis (unchanged when it
  // already fits).
  static String nanoFitText(const String &text, int maxWidth, uint8_t size);
  // Every line of `text` wrapped to `maxWidth` (\n starts a paragraph).
  static std::vector<String> nanoWrapText(const String &text, int maxWidth, uint8_t size);
  // Logical frame (landscape, RGB565 byte-swapped as the panel takes it) --
  // for the serial screenshot command and tools/nanosim.
  const uint16_t *frameBuffer() const { return virtualFrame_; }
  // Key of the last screen drawn (plugins check whether theirs is still up).
  const String &lastRenderKey() const { return lastRenderKey_; }
  static int frameStride();
  // Every primitive below clips to this rect (default: whole screen).
  void nanoSetClip(int x, int y, int w, int h);
  void nanoResetClip();
  void nanoFillRect(int x, int y, int w, int h, uint16_t color);
  void nanoDrawRect(int x, int y, int w, int h, uint16_t color);
  void nanoFillRoundRect(int x, int y, int w, int h, int radius, uint16_t color);
  void nanoDrawRoundRect(int x, int y, int w, int h, int radius, uint16_t color);
  void nanoDrawLine(int x0, int y0, int x1, int y1, uint16_t color);
  void nanoFillCircle(int cx, int cy, int radius, uint16_t color);
  void nanoDrawCircle(int cx, int cy, int radius, uint16_t color);
  void nanoFillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color);
  // `image` scaled to fill `rect` (centre-cropped to its aspect, pixels
  // averaged when shrinking) with the corners rounded like nanoFillRoundRect.
  void nanoImage(const ui::Rect &rect, int radius, const NanoImage &image);
  // Wraps `text` at spaces into at most `maxLines` (1-3) lines of the rect
  // and ends the last one with an ellipsis if the rest doesn't fit -- never
  // shrinks the type (only when the rect is too short for even one line of
  // `size`). The block is centered vertically on the cap height, so a line
  // sits optically in the middle of a button.
  void nanoText(const ui::Rect &rect, const String &text, uint8_t size, uint16_t color,
                NanoAlign align = NanoAlign::Start, uint8_t maxLines = 1);
  // Same, in a specific UI font family (font chips preview themselves).
  void nanoTextInFamily(const ui::Rect &rect, const String &text, uint8_t family, uint8_t size,
                        uint16_t color, NanoAlign align = NanoAlign::Start);
  // One line at a signed x (it may start off-screen, pixels are clipped),
  // centred vertically on `centreY` -- for text sliding across the edge.
  void nanoTextLineAt(int x, int centreY, const String &text, uint8_t size, uint16_t color);
  // Text in a reader typeface (anti-aliased onto whatever is underneath),
  // `y` = top of the glyph box. Count = the active reading typeface.
  void nanoTypefaceText(int x, int y, const String &text, uint16_t color, uint8_t scalePercent,
                        ReaderTypeface typeface = ReaderTypeface::Count);
  int nanoTypefaceTextWidth(const String &text, uint8_t scalePercent,
                            ReaderTypeface typeface = ReaderTypeface::Count) const;
  // The reading position as the reader will show it: the current word with
  // its focus letter in the accent, anchored where the reading screen
  // anchors it, the neighbouring words dimmed. `area` spans the full width
  // so the word doesn't jump when reading starts.
  void nanoReaderPreview(const ui::Rect &area, const String &before, const String &word,
                         const String &after, uint8_t fontSizeLevel);
  // The reading screen itself inside `area`: reading background, word and
  // letter colors, typeface, size level, letter spacing, anchor and guide
  // lines exactly as set, shrunk only when the size does not fit. Used by
  // Wyglad czytania so every change shows as it will look while reading.
  void nanoReaderSample(const ui::Rect &area, const String &before, const String &word,
                        const String &after, uint8_t fontSizeLevel);
  // Scroll-mode counterpart: three lines of the page around the current
  // word (words[currentLocal]), what was read above dimmed, the word itself
  // marked, in the reading typeface.
  void nanoScrollPreview(const ui::Rect &area, const std::vector<ContextWord> &words, size_t currentLocal);
  // Switches the Nano palette for the next frame(s) without forcing a
  // repaint (reader panel: reading colors; restored right after).
  void overrideNanoPalette(uint8_t palette, bool ownAccent);
  // The classic 5x7 glyph set, 1x, clipped — used for book-spine lettering.
  void nanoSmallGlyph(int x, int y, char c, uint16_t color);
  void nanoIcon(const ui::Rect &rect, NanoIcon icon, uint16_t ink, uint16_t surface);
  void nanoBatteryIcon(int x, int y, int w, int h, uint8_t percent, bool charging, uint16_t ink,
                       uint16_t surface);
  // Lightning glyph drawn next to the battery while it charges.
  void nanoChargingBolt(int cx, int cy, int h, uint16_t color);
  // Width nanoBatteryInline() will take for the current style and state.
  int nanoBatteryIndicatorWidth(bool compact = false) const;

  void nanoLabel(const ui::Rect &rect, const String &text, uint8_t size, NanoRole role,
                 NanoAlign align = NanoAlign::Start, uint8_t maxLines = 1);
  void nanoSeparator(const ui::Rect &rect, const String &text);
  // `pressed` is the short tap flash, `armed` the red "tap again to
  // confirm" state of destructive rows. `previewTypeface` draws the label
  // in that reader typeface instead of the pixel font (font picker).
  void nanoButton(const ui::Rect &rect, const String &text, bool enabled = true,
                  NanoIcon icon = NanoIcon::None, uint8_t textLines = 1,
                  const String &detailLeft = "", const String &detailRight = "",
                  bool pressed = false, bool armed = false,
                  ReaderTypeface previewTypeface = ReaderTypeface::Count);
  void nanoIconButton(const ui::Rect &rect, NanoIcon icon, bool pressed = false);
  // Icon over a label (launcher tile). `detail` = small second line (state,
  // count). `accent` fills the tile with the accent (primary action).
  void nanoTile(const ui::Rect &rect, const String &label, NanoIcon icon, const String &detail = "",
                bool pressed = false, bool accent = false, bool enabled = true);
  // Rounded pill (sort switch, pager, filter): optional leading icon.
  void nanoPill(const ui::Rect &rect, const String &text, NanoIcon icon = NanoIcon::None,
                bool pressed = false, bool active = false);
  // Rail backdrop behind the tabs.
  void nanoRailBackground(const ui::Rect &rect);
  // Left-rail navigation tab. `badge` adds a small accent dot (pending
  // firmware update on the Device tab).
  // Empty `text` = icon-only tab (compact rail). `markerRight` puts the
  // active-tab accent bar on the right edge (rail docked on the right).
  void nanoTab(const ui::Rect &rect, const String &text, bool active, NanoIcon icon,
               bool pressed = false, bool badge = false, bool markerRight = false);
  void nanoSetting(const ui::Rect &rect, const String &label, const String &value,
                   bool inlineLayout, bool pressed = false);
  void nanoToggle(const ui::Rect &rect, const String &label, bool on, bool pressed = false);
  void nanoProgress(const ui::Rect &rect, int value, int minimum, int maximum);
  // Setting tile that doubles as a slider: the accent fill grows from the
  // left edge to show where `value` sits between minimum and maximum; label
  // and value flip to the on-accent color where the fill runs under them.
  // `dragging` = a finger is moving the value right now.
  void nanoSlider(const ui::Rect &rect, const String &label, const String &valueText, int value,
                  int minimum, int maximum, bool pressed = false, bool dragging = false);
  // Palette preview for the Motywy tab: the tile is painted in `palette`'s
  // own background/text/accent, ringed in the active accent when selected.
  void nanoPaletteChip(const ui::Rect &rect, uint8_t palette, const String &name, bool selected,
                       bool pressed = false);
  // UI-font preview for Motywy > Czcionka: the name set in that family.
  void nanoFontChip(const ui::Rect &rect, uint8_t family, const String &name, const String &sample,
                    bool selected, bool pressed = false);
  // Rail layout preview for Motywy > Uklad: a sketch of the screen with the
  // rail (icons only or icons + labels) on the chosen side. `selected`
  // adds the swap badge (tapping again moves the rail to the other side).
  void nanoLayoutChip(const ui::Rect &rect, bool compact, bool railRight, const String &name,
                      const String &detail, bool selected, bool pressed = false);
  // Reading theme preview (0 dark, 1 light, 2 night): a word in that
  // theme's colors with the focus letter, as the reading screen shows it.
  // sampleHead/Focus/Tail: the word drawn on the chip, split around its
  // focus letter (in the UI language).
  void nanoReadingThemeChip(const ui::Rect &rect, uint8_t theme, const String &name, const String &sampleHead,
                            const String &sampleFocus, const String &sampleTail, bool selected, bool pressed);
  // Low version of the chip for one row of controls: the theme's
  // background with its name in the theme's word color.
  void nanoReadingThemePill(const ui::Rect &rect, uint8_t theme, const String &name, bool selected,
                            bool pressed = false);
  // Round swatch of the letter color with a label next to it.
  void nanoLetterColorTile(const ui::Rect &rect, uint16_t color, const String &label, bool pressed = false);
  // One color of the letter-color palette.
  void nanoColorSwatch(const ui::Rect &rect, uint16_t color, bool selected, bool pressed = false);
  // Battery icon with the percent label stacked under it.
  void nanoBatteryStack(const ui::Rect &rect);
  // Battery icon and percent side by side, centered in `rect` (rail footer).
  // `iconOnly` = the compact rail (narrow: "Ikona + %" becomes the number in
  // the icon). `align` places the indicator inside `rect`.
  void nanoBatteryInline(const ui::Rect &rect, bool iconOnly = false, NanoAlign align = NanoAlign::Center);
  // line1ScalePercent/line2ScalePercent domyślnie 36/28 (dotychczasowy
  // rozmiar) — ekrany kreatora pierwszego uruchomienia proszą o większe
  // wartości, żeby tekst był czytelny dla osób 40+, bez zmiany rozmiaru na
  // pozostałych ~50 ekranach reużywających renderStatus().
  void renderStatus(const String &title, const String &line1 = "", const String &line2 = "",
                    uint8_t line1ScalePercent = 36, uint8_t line2ScalePercent = 28);
  // `hint` to trzecia, przygaszona linijka pod QR-em. Domyślnie zdanie dla
  // ekranu parowania z telefonem; ekran „zainstaluj aplikację" podaje swoje.
  // `cornerHint`, jeśli niepuste, rysuje mały wypełniony przycisk w prawym
  // dolnym rogu (np. "Dalej") — używane przez kreator, żeby dać dotykowe
  // wyjście z ekranu QR bez zostawiania całego ekranu jako jeden wielki
  // przycisk "dalej" (patrz App::renderWelcomeConnect()).
  // Tap target of renderStatusWithQr()'s `cornerHint` button (depends on
  // the skin, so callers hit-test what is actually drawn).
  ui::Rect qrCornerButtonRect() const;
  void renderStatusWithQr(const String &title, const String &line1, const bool *qrData,
                          uint8_t qrSize, const String &hint = "Scan to connect",
                          const String &cornerHint = "");
  void renderProgress(const String &title, const String &line1 = "", const String &line2 = "",
                      int progressPercent = -1, uint8_t line1ScalePercent = 36,
                      uint8_t line2ScalePercent = 28);
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
  // filled=true draws a solid bookmark ribbon (SavePoint icon only — other
  // icons ignore this flag); filled=false draws it hollow, outline only.
  void drawIcon(ui::IconId id, int x, int y, int size, uint16_t color, bool filled = true);
  void blitIconBitmap(const uint16_t *bitmap, uint8_t w, uint8_t h, int x, int y);
  // Straight-line helper for the vector icon placeholders in drawIcon() —
  // linear-interpolation stepping (not true Bresenham, but plenty for
  // icons a few dozen pixels across) so glyphs aren't limited to
  // axis-aligned rects.
  void drawIconLine(int x0, int y0, int x1, int y1, uint16_t color, int thickness = 1);
  void drawFilledCircle(int cx, int cy, int radius, uint16_t color);
  // Filled rounded rect built from fillVirtualRect() + drawFilledCircle() —
  // the renderer has no native rounded-rect primitive. radius is clamped to
  // half the shorter side; radius 0 falls back to a plain fillVirtualRect().
  void fillRoundedRect(int x, int y, int w, int h, int radius, uint16_t color);
  // Nano-skin painting of one generic Button (drawButtons() delegates here
  // while modernCardStyle_ is set).
  void drawNanoButton(const Button &button);
  void nanoSpan(int x, int y, int w, uint16_t color);
  void nanoPixel(int x, int y, uint16_t color);
  void nanoCircleHelper(int x0, int y0, int r, uint8_t corners, uint16_t color);
  void nanoFillCircleHelper(int x0, int y0, int r, uint8_t corners, int delta, uint16_t color);
  // `alpha` 0-255 of `color` over the pixel already in the frame.
  void nanoBlendPixel(int x, int y, uint16_t color, uint8_t alpha);
  void nanoThickLine(int x0, int y0, int x1, int y1, uint16_t color);
  // One line of UI text in `family`/`strike`, pen starting at x, baseline y.
  void nanoDrawRun(int x, int baseline, const String &text, uint8_t family, uint8_t strike,
                   uint16_t color);
  void nanoTextWithFamily(const ui::Rect &rect, const String &text, uint8_t family, uint8_t size,
                          uint16_t color, NanoAlign align, uint8_t maxLines);
  void nanoTypefaceGlyph(int x, int y, char c, uint16_t color, uint8_t scalePercent,
                         ReaderTypeface typeface);
  // Nano-skin versions of the full-screen status/progress/QR/menu renderers
  // (used while modernCardStyle_ is on, i.e. in the Modern nav mode).
  void renderNanoStatusScreen(const String &title, const String &line1, const String &line2,
                              int progressPercent);
  void renderNanoQrScreen(const String &title, const String &line1, const bool *qrData,
                          uint8_t qrSize, const String &hint, const String &cornerHint);
  void renderNanoMenuList(const std::vector<String> &items, size_t selectedIndex);
  void renderNanoFocusTimer(const String &mode, const String &timer, const String &instruction,
                            int progressPercent, bool breakAccent);
  void nanoGenericIcon(ui::IconId id, const ui::Rect &rect, uint16_t ink, uint16_t surface);
  // Fills the whole frame with the Nano background (palette-aware; the
  // generic clearVirtualBuffer() uses the reader's background).
  void nanoClearBackground(int width, int height);
  void drawBatteryBadge();
  void drawBatteryBadge(int logicalWidth, int logicalHeight);
  void drawPreviousSentenceHint();
  // filled: whether the current reading position exactly matches an
  // existing save point (solid ribbon) or not (hollow ribbon).
  void drawSavePointButton(bool filled = false);
  void drawSavePointButton(int logicalWidth, int logicalHeight, bool filled = false);
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
  bool fadeFrame(bool fadeIn, uint32_t fadeMs);
  // Panel-order copy (native rows) of the frame last flushed, for
  // finishCrossfade().
  uint16_t *captureNativeFrame() const;
  int logicalWidth() const;
  int logicalHeight() const;
  uint16_t focusTimerBreakColor() const;
  uint16_t nanoBatteryLevelColor(uint8_t percent, bool charging) const;
  String batteryNumberLabel() const;
  uint8_t batteryStyleFor(bool compact) const;

  uint16_t *virtualFrame_ = nullptr;
  uint16_t *txBuffer_ = nullptr;
  // beginCrossfade(): the picture being faded out, and drawBitmap() holding
  // panel writes back until finishCrossfade().
  uint16_t *crossfadeFrom_ = nullptr;
  bool panelHold_ = false;
  size_t txBufferBytes_ = 0;
  bool initialized_ = false;
  uint8_t brightnessPercent_ = 100;
  PhraseLocalizer phraseLocalizer_ = nullptr;
  void *phraseLocalizerContext_ = nullptr;
  uint8_t focusColorIndex_ = 1;  // 0=red, 1=blue, 2=green, 3=yellow, 4=orange, 5=purple, kFocusColorCustom
  uint16_t customFocusColor_ = 0x001F;
  bool darkMode_ = true;
  bool nightMode_ = false;
  bool modernCardStyle_ = false;
  uint8_t nanoPalette_ = kNanoPaletteClassic;
  bool nanoOwnAccent_ = false;
  uint8_t nanoUiFont_ = 0;
  BoardConfig::UiOrientation uiOrientation_ =
      BoardConfig::UI_ROTATED_180 ? BoardConfig::UiOrientation::LandscapeFlipped
                                  : BoardConfig::UiOrientation::Landscape;
  bool tickerPlaybackFrameActive_ = false;
  int lastFlushScale_ = 0;
  int lastFlushWidth_ = 0;
  int lastFlushHeight_ = 0;
  String lastRenderKey_;
  String batteryLabel_;
  bool batteryPresent_ = false;
  uint8_t batteryPercent_ = 0;
  bool batteryCharging_ = false;
  uint8_t batteryStyle_ = kBatteryStyleIconPercent;
  // Incremented by every drawBitmap() — nanoEndFrame() may only skip an
  // unchanged frame if nothing else reached the panel since it last flushed.
  uint32_t panelWriteCount_ = 0;
  uint32_t nanoFrameHash_ = 0;
  uint32_t nanoFramePanelWrites_ = 0;
  int nanoClipX0_ = 0;
  int nanoClipY0_ = 0;
  int nanoClipX1_ = 0;
  int nanoClipY1_ = 0;
  // Word-wrap is comparatively expensive (String concatenation in a loop)
  // and renderArticleReader() must return the fresh total-line count on
  // every call (even when lastRenderKey_ skips the redraw), so the wrap
  // itself is cached separately keyed on the raw title+body, not on the
  // scroll position.
  String articleReaderSourceCache_;
  std::vector<String> articleReaderLinesCache_;
  uint8_t scrollFontSize_ = 1;
  uint8_t scrollLineSpacing_ = 1;
  uint8_t scrollMargin_ = 1;

  int scrollLineHeightPx() const;
  int scrollMarginPx() const;
  int scrollSerifDivisor() const;
  uint8_t scrollScalePercent() const;
};
