#pragma once

// Nano UI screen painters (NavMode::Modern). Each screen is described by a
// plain view struct and painted onto the Nano frame; tap targets go to a
// Sink. Nothing here reads App state, so the same code draws on the
// device (App builds the views in app/AppNano.inl) and on the PC
// (tools/nanosim renders every screen to an image).

#include <Arduino.h>

#include <vector>

#include "display/DisplayManager.h"

namespace nano {

using Icon = DisplayManager::NanoIcon;
using Role = DisplayManager::NanoRole;
using Align = DisplayManager::NanoAlign;
using Rect = ui::Rect;

constexpr int kScreenW = BoardConfig::DISPLAY_WIDTH;
constexpr int kScreenH = BoardConfig::DISPLAY_HEIGHT;
constexpr int kGap = 8;
constexpr int kMargin = 10;
constexpr int kHeaderH = 30;
// Book cover on the Czytaj card; book details draw it twice as large and the
// Book screensaver at the same aspect. The Flower app crops cover pictures to
// kCoverW*2 x kCoverH*2 and spines to kSpineImageW x kSpineImageH (tallest
// spine on the shelf is 68 px, widest 35).
constexpr int kCoverW = 46;
constexpr int kCoverH = 58;
constexpr int kSpineImageW = 36;
constexpr int kSpineImageH = 72;
constexpr int kCompactRailW = 60;
constexpr int kRailFooterH = 24;
constexpr int kNoTarget = -1;

// Rail placement, set by App before every Nano render.
struct Layout {
  bool railRight = false;
  bool compact = false;
  int railWidth = 160;
};
Layout &layout();
// Rail width that fits every tab label at body size in the active UI font.
int railWidthFor(const std::vector<String> &labels);
Rect railRect();
// Content area next to the rail / the whole screen (nested screens).
Rect tabContent();
Rect fullContent();

class Sink {
 public:
  virtual ~Sink() = default;
  virtual void target(const Rect &rect, int id) = 0;
  virtual bool pressed(int id) const = 0;
  virtual bool armed(int id) const {
    (void)id;
    return false;
  }
  // A settings tile that doubles as a slider was drawn here.
  virtual void slider(const Rect &rect, int id) {
    (void)rect;
    (void)id;
  }
};

// ── Shared pieces ──

struct RailTab {
  int id = kNoTarget;
  String label;
  Icon icon = Icon::None;
  bool active = false;
  bool badge = false;
};
void paintRail(DisplayManager &d, Sink &sink, const std::vector<RailTab> &tabs);

struct Header {
  int backId = kNoTarget;
  String title;
  String trailing;  // muted text on the right (count, position)
  size_t page = 0;
  size_t pageCount = 1;
  int prevId = kNoTarget;
  int nextId = kNoTarget;
  // Optional pill on the right (e.g. library sort).
  int pillId = kNoTarget;
  String pillLabel;
  Icon pillIcon = Icon::None;
};
// Paints the header row at the top of `area`; returns the y below it.
int paintHeader(DisplayManager &d, Sink &sink, const Rect &area, const Header &header);

struct Tile {
  int id = kNoTarget;
  String label;
  String detail;
  Icon icon = Icon::None;
  bool accent = false;
  bool enabled = true;
};
// `columns` x `rows` tiles filling `area`, only the first columns*rows used.
void paintTileGrid(DisplayManager &d, Sink &sink, const Rect &area, const std::vector<Tile> &tiles,
                   int columns, int rows);

// ── Czytaj ──

struct ReadHome {
  bool hasBook = false;
  String title;
  String author;
  String progressLabel;  // "42%"
  String hint;           // "Czytaj dalej" / "Wybierz ksiazke"
  int progressPercent = 0;
  uint16_t coverColor = 0x32FA;
  String coverInitials;
  NanoImage cover;  // picture from the Flower app, drawn instead of the colour
  int resumeId = kNoTarget;
  int fontsId = kNoTarget;
  String fontsLabel;
  std::vector<Tile> tiles;  // chapters, save points, library
};
void paintReadHome(DisplayManager &d, Sink &sink, const ReadHome &view);

// ── Sectioned settings (Ustawienia) ──

struct SectionItem {
  int id = kNoTarget;
  String label;
  Icon icon = Icon::None;
  bool toggle = false;
  bool on = false;
  bool fullWidth = false;
  int helpId = kNoTarget;  // round "?" button right of the tile
};
struct Section {
  String title;  // empty = no section label
  std::vector<SectionItem> items;
};
void paintSections(DisplayManager &d, Sink &sink, const Rect &area, const std::vector<Section> &sections);

// ── Motywy ──

// Menu look only: reading colors live on the Wyglad czytania screen.
struct ThemesView {
  int section = 0;  // 0 menu colors, 1 menu font, 2 layout
  static constexpr int kSections = 3;
  int segmentIds[kSections] = {kNoTarget, kNoTarget, kNoTarget};
  String segmentLabels[kSections];
  struct PaletteChip {
    int id = kNoTarget;
    uint8_t palette = 0;
    String name;
    bool selected = false;
  };
  std::vector<PaletteChip> palettes;
  int ownAccentId = kNoTarget;  // kNoTarget = hide the switch
  String ownAccentLabel;
  bool ownAccentOn = false;
  struct FontChip {
    int id = kNoTarget;
    uint8_t family = 0;
    String name;
    String sample;
    bool selected = false;
  };
  std::vector<FontChip> fonts;
  // Layout: two chips (icons only / icons + labels); tapping the selected
  // one again moves the rail to the other side.
  struct LayoutChip {
    int id = kNoTarget;
    bool compact = false;
    bool railRight = false;
    String name;
    String detail;
    bool selected = false;
  };
  std::vector<LayoutChip> layouts;
  String layoutHint;
};
void paintThemes(DisplayManager &d, Sink &sink, const ThemesView &view);

// ── Biblioteka (bookshelf) ──

struct ShelfGeometry {
  Rect header;
  Rect viewport;
  Rect detail;
  int marker = 0;
};
ShelfGeometry shelfGeometry();
int32_t shelfClampOffset(size_t count, int32_t offset, int viewportWidth);
int32_t shelfCenteredOffset(size_t count, size_t index, int viewportWidth);
size_t shelfNearest(size_t count, int32_t offset, int markerX, int viewportX);
size_t shelfSpineIndexAt(int32_t contentX, size_t count);
int32_t shelfSpineLeft(size_t index);
int shelfSpineWidth(size_t index);
constexpr int kShelfDragThreshold = 20;

struct ShelfBook {
  String title;
  uint8_t progress = 0;
  NanoImage spine;  // picture from the Flower app, drawn instead of the colour
};
struct ShelfView {
  Header header;
  std::vector<ShelfBook> books;
  size_t selected = 0;
  int32_t offset = 0;
  String detailTitle;
  String detailAuthor;
  String detailStatus;
  String detailPercent;
  String emptyLabel;
};
void paintShelf(DisplayManager &d, Sink &sink, const ShelfView &view);

// ── Rozdzialy (chapter wheel) ──

constexpr int kWheelRowStep = 30;
constexpr int kWheelDragThreshold = 6;
Rect wheelViewport();
int wheelRowCenter(const Rect &viewport, int row, int offset);
int wheelRowHeight(bool centered);
bool wheelRowVisible(const Rect &viewport, int y, int height);

struct WheelView {
  Header header;
  // Titles of chapters [firstIndex, firstIndex + titles.size()).
  size_t firstIndex = 0;
  std::vector<String> titles;
  size_t count = 0;
  size_t centered = 0;
  int16_t offset = 0;
  size_t readingIndex = 0;
  int emptyId = kNoTarget;
  String emptyLabel;
};
void paintWheel(DisplayManager &d, Sink &sink, const WheelView &view);

// ── Lists (settings sub-screens, save points, plugins, pickers) ──

struct ListItem {
  enum class Kind : uint8_t { Button, Setting, Toggle, Slider, Label, Separator, Row };
  Kind kind = Kind::Button;
  int id = kNoTarget;
  String label;
  String value;
  Icon icon = Icon::None;
  bool on = false;
  bool armed = false;
  bool marked = false;  // current choice (check mark)
  bool fullWidth = false;
  int sliderValue = 0;
  int sliderMin = 0;
  int sliderMax = 1;
  bool dragging = false;
  DisplayManager::ReaderTypeface typeface = DisplayManager::ReaderTypeface::Count;
  // Round "?" button right of the tile (help page for this setting).
  int helpId = kNoTarget;
  // Kind::Row: trailing action (delete) inside the same row.
  int trailingId = kNoTarget;
  String trailingLabel;
  bool trailingArmed = false;
};
struct ListView {
  Header header;
  std::vector<ListItem> items;  // current page only
  int columns = 2;
  int rows = 3;
  bool fullScreen = false;  // no rail
};
void paintList(DisplayManager &d, Sink &sink, const ListView &view);

// ── Wyglad czytania (reading colors + typography) ──
// Top: the reading screen itself with a sample word. Bottom: Back, three
// segments and one row of controls for the chosen segment.

struct TypographyView {
  int backId = kNoTarget;
  static constexpr int kSections = 3;
  int section = 0;  // 0 colors, 1 text, 2 guide
  int segmentIds[kSections] = {kNoTarget, kNoTarget, kNoTarget};
  String segmentLabels[kSections];
  int sampleId = kNoTarget;  // tap on the sample = next sample word
  String before;
  String word;
  String after;
  uint8_t fontSizeLevel = 0;
  // Colors: the three reading themes, then the letter color.
  struct ThemeChip {
    int id = kNoTarget;
    uint8_t theme = 0;  // 0 dark, 1 light, 2 night
    String name;
    bool selected = false;
  };
  std::vector<ThemeChip> themes;
  int letterColorId = kNoTarget;
  String letterColorLabel;
  uint16_t letterColor = 0;
  // The rest of the row (toggles, sliders, buttons), left to right.
  std::vector<ListItem> items;
};
Rect typographySampleRect();
void paintTypography(DisplayManager &d, Sink &sink, const TypographyView &view);

// ── Book details ──

struct BookDetailsView {
  Header header;
  uint16_t coverColor = 0x32FA;
  String coverInitials;
  NanoImage cover;
  String author;
  String percentLabel;
  int percent = 0;
  std::vector<Tile> actions;  // up to 4
};
void paintBookDetails(DisplayManager &d, Sink &sink, const BookDetailsView &view);

// ── Confirm dialog ──

struct ConfirmView {
  String question;
  String detail;
  int backId = kNoTarget;
  String backLabel;
  struct Action {
    int id = kNoTarget;
    String label;
    bool danger = false;
    bool armed = false;
  };
  std::vector<Action> actions;
};
void paintConfirm(DisplayManager &d, Sink &sink, const ConfirmView &view);

// ── Reader panel (paused, before reading starts) ──

struct ReaderPanelView {
  String chapter;
  String progressLabel;
  String timeLeft;
  int progressPercent = 0;
  String before;
  String word;
  String after;
  uint8_t fontSizeLevel = 0;  // RSVP size on the reading screen (0 = largest)
  // Scroll reading mode: the page around the word instead of the RSVP line.
  bool scrollMode = false;
  std::vector<DisplayManager::ContextWord> words;
  size_t currentLocal = 0;
  int menuId = kNoTarget;
  int rewindId = kNoTarget;   // back to the start of the sentence
  int lookId = kNoTarget;     // Wyglad czytania (reading colors, typeface)
  int gotoId = kNoTarget;     // jump to %, page or chapter
  int statusId = kNoTarget;   // the top line + progress bar (also opens "go to")
  int bookmarkId = kNoTarget;
  bool bookmarkFilled = false;
  int minusId = kNoTarget;
  int wpmId = kNoTarget;
  int plusId = kNoTarget;
  String wpmLabel;
  int startId = kNoTarget;
  String startLabel;
  String menuLabel;
  String hint;  // gesture hint under the word, empty = none
};
Rect readerPanelWordArea();
Rect readerPanelBar();
Rect readerPanelStatusArea();
void paintReaderPanel(DisplayManager &d, Sink &sink, const ReaderPanelView &view);

// ── Przejdz do (jump to %, page or chapter) ──

struct GoToView {
  Header header;
  int segmentIds[3] = {kNoTarget, kNoTarget, kNoTarget};
  String segmentLabels[3];
  int segment = 0;
  String value;    // "42%", "Strona 120 / 412", "5 / 12"
  String detail;   // chapter at the target
  String hint;     // "1 strona = 250 slow"
  int sliderMin = 0;
  int sliderMax = 100;
  int sliderValue = 0;
  bool dragging = false;
  int minusId = kNoTarget;
  int plusId = kNoTarget;
  int readId = kNoTarget;
  String readLabel;
};
Rect goToBarRect();
void paintGoTo(DisplayManager &d, Sink &sink, const GoToView &view);

// ── Two big choices under a question (bookmark name, ...) ──

struct ChoiceView {
  Header header;
  String question;
  struct Option {
    int id = kNoTarget;
    String label;
    String detail;
    Icon icon = Icon::None;
    bool accent = false;
  };
  std::vector<Option> options;  // 2 or 3
};
void paintChoice(DisplayManager &d, Sink &sink, const ChoiceView &view);

// ── Help page (the ? buttons) ──

struct HelpView {
  Header header;
  std::vector<String> lines;  // wrapped to helpTextWidth()
  int scroll = 0;             // px from the top
};
Rect helpBodyRect();
int helpTextWidth();
int helpContentHeight(const HelpView &view);
void paintHelp(DisplayManager &d, Sink &sink, const HelpView &view);

// ── Kolor litery (palette) ──

struct ColorPickerView {
  Header header;
  int columns = 12;
  int rows = 3;
  struct Swatch {
    int id = kNoTarget;
    uint16_t color = 0;
    bool selected = false;
  };
  std::vector<Swatch> swatches;
  // Preview word in the reading colors with the chosen letter color.
  uint16_t previewBackground = 0;
  uint16_t previewWord = 0xFFFF;
  uint16_t previewFocus = 0x001F;
  String sampleWord;  // in the UI language
};
void paintColorPicker(DisplayManager &d, Sink &sink, const ColorPickerView &view);

// ── Wygaszacze (App::ScreensaverMode Book / Words / Waves) ──
// Full-screen scenes in the active palette: background, accent, muted
// text. The style name and the "tap to wake" hint fade in and out on top
// (alpha 0 = hidden).
struct SaverOverlay {
  String label;
  uint8_t labelAlpha = 0;
  String hint;
  uint8_t hintAlpha = 0;
};
void paintSaverOverlay(DisplayManager &d, const SaverOverlay &overlay);

// Current book as a card that drifts slowly around the screen.
struct SaverBookView {
  bool hasBook = false;
  String title;   // no book: the "no book open" line
  String author;
  int progressPercent = 0;
  String progressLabel;
  uint16_t coverColor = 0;
  String coverInitials;
  NanoImage cover;
  int driftX = 0;  // offset of the card from the centre, px
  int driftY = 0;
  SaverOverlay overlay;
};
void paintSaverBook(DisplayManager &d, const SaverBookView &view);

// Words of the book gliding past in lanes; each lane loops its words.
struct SaverLane {
  std::vector<String> words;
  int y = 0;        // lane centre
  uint8_t size = 1; // UI font size
  uint8_t alpha = 255;
  uint32_t offset = 0;  // px scrolled so far
  // The word crossing the middle of the screen takes the accent.
  bool markCentre = false;
};
struct SaverWordsView {
  std::vector<SaverLane> lanes;
  SaverOverlay overlay;
};
void paintSaverWords(DisplayManager &d, const SaverWordsView &view);

// A few slow sine waves in the accent, layered from faint to bright.
struct SaverWavesView {
  uint32_t phase = 0;  // advances every frame
  SaverOverlay overlay;
};
void paintSaverWaves(DisplayManager &d, const SaverWavesView &view);

// ── Samouczek ──
// One page: a drawing on the left, title and text on the right, Wstecz /
// dots / Dalej along the bottom, Pomiń in the top-right corner.
enum class TutorialArt : uint8_t {
  Rsvp,     // a word with its focus letter
  Start,    // "Czytaj" button and a held finger
  Speed,    // tempo with - and +
  Scrub,    // words with arrows and the top bar
  Menu,     // the tab rail
  Help,     // a setting tile with its ? button
};
struct TutorialView {
  String caption;  // "Samouczek"
  size_t page = 0;
  size_t pageCount = 1;
  TutorialArt art = TutorialArt::Rsvp;
  String title;
  String body;
  // Words the drawings use (translated): the sample word, the play button,
  // the tempo unit, the four tab names, the tile label.
  String artWord;
  String artStart;
  String artUnit;
  String artTabs[4];
  String artTile;
  int backId = kNoTarget;  // hidden on the first page
  String backLabel;
  int nextId = kNoTarget;
  String nextLabel;        // "Dalej", "Gotowe" on the last page
  int skipId = kNoTarget;  // hidden on the last page
  String skipLabel;
};
void paintTutorial(DisplayManager &d, Sink &sink, const TutorialView &view);


// ── Kreator pierwszego uruchomienia ──
// One step: a segmented step bar on top, title and subtitle, the body
// (option chips, a message, a loading bar or a QR code), Wstecz / Dalej
// along the bottom.
enum class WizardBody : uint8_t {
  Chips,    // options side by side, one selected
  Message,  // big centred title (Super!, Skonfigurujmy...)
  Loading,  // rotating phrase over a sliding bar
  Qr,       // code on the right, text on the left
  Preview,  // the reading screen itself (RSVP word or scrolling page)
};
enum class WizardChipArt : uint8_t {
  Label,     // just the name
  Swatch,    // a color dot before the name
  Theme,     // reading theme preview (DisplayManager::nanoReadingThemeChip)
  Rsvp,      // a word with its focus letter
  Scroll,    // a few lines of text
  Palette,   // menu palette preview (DisplayManager::nanoPaletteChip)
  Typeface,  // the name set in that reader typeface
  Book,      // title over the author (starter library)
  UiFont,    // the name set in that menu font (DisplayManager::nanoFontChip)
};
struct WizardChip {
  int id = kNoTarget;
  String label;
  bool selected = false;
  WizardChipArt art = WizardChipArt::Label;
  uint16_t swatch = 0;  // Swatch
  uint8_t theme = 0;    // Theme: 0 dark, 1 light, 2 night
  String word;          // Rsvp: sample word in the UI language
  uint8_t palette = 0;  // Palette
  DisplayManager::ReaderTypeface typeface = DisplayManager::ReaderTypeface::Count;  // Typeface
  String detail;        // Book: author
  uint8_t family = 0;   // UiFont: DisplayManager::nanoUiFontName index
  bool disabled = false;  // Typeface: still downloading, drawn dimmed, no target
};
struct WizardView {
  size_t step = 0;  // 0-based
  size_t stepCount = 1;
  String title;
  String subtitle;
  WizardBody body = WizardBody::Chips;
  std::vector<WizardChip> chips;
  // Chips: 1 row side by side, or 2 rows (palettes, typefaces). With page
  // ids set, arrows either side of the chips turn the page.
  int chipRows = 1;
  int chipColumns = 0;  // 0 = as many as the chips need
  // Chips take the subtitle's space too (the starter library's 2x3 books).
  bool tallChips = false;
  int pagePrevId = kNoTarget;
  int pageNextId = kNoTarget;
  // Loading: advances every frame; Message: auto-advance progress, -1 none.
  uint32_t phase = 0;
  int autoPercent = -1;
  // Loading: known progress 0-100 fills the bar instead of the glider,
  // `status` names what is happening under it.
  int percent = -1;
  String status;
  // Preview: mode 0 RSVP (before/word/after at the reading size), 1 the
  // scrolling page (scrollWords around scrollCurrent).
  uint8_t previewMode = 0;
  String previewBefore;
  String previewWord;
  String previewAfter;
  uint8_t previewSizeLevel = 0;
  const std::vector<DisplayManager::ContextWord> *scrollWords = nullptr;
  size_t scrollCurrent = 0;
  // Qr
  const bool *qr = nullptr;
  uint8_t qrSize = 0;
  String qrLine;  // e.g. the network name
  String qrHint;  // waiting / connected
  int backId = kNoTarget;
  String backLabel;
  int nextId = kNoTarget;
  String nextLabel;
  int extraId = kNoTarget;  // optional pill next to Dalej (Podgląd)
  String extraLabel;
  String footer;  // muted line in the bottom row
};
void paintWizard(DisplayManager &d, Sink &sink, const WizardView &view);


// ── Aplikacja (phone sync) ──
// Two pages: pairing with the reader's own network, and the app download
// link. Segment buttons on top switch them (so does a sideways swipe),
// Zakończ closes sync.
struct SyncView {
  size_t page = 0;  // 0 pairing, 1 app download
  String pageLabels[2];
  int pageIds[2] = {kNoTarget, kNoTarget};
  int stopId = kNoTarget;
  String stopLabel;
  String title;
  String line;  // network name / link
  String hint;
  const bool *qr = nullptr;
  uint8_t qrSize = 0;
};
void paintSync(DisplayManager &d, Sink &sink, const SyncView &view);


}  // namespace nano
