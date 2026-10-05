// Nano UI — NavMode::Modern: rsvpnano's "regular" presentation for this
// 640x172 panel (tab rail, flat tiles, bookshelf, chapter wheel) plus our
// own screens (Motywy, Urzadzenie, the reader panel before reading).
//
// Included at the bottom of App.cpp instead of compiled on its own: it
// shares that file's anonymous-namespace index constants (kSettingsHome*,
// kPressFlashMs, storedOrFallbackLabel(), ...). Screens keep running on the
// MenuScreen state machine and the existing select*Item() handlers; this
// file builds a view for each screen and hands it to ui/NanoScreens.cpp,
// which lays it out and paints it with DisplayManager's Nano skin
// (display/NanoSkin.inl). The same painters run in tools/nanosim.

#if RSVP_USB_TRANSFER_ENABLED && CONFIG_TINYUSB_MSC_ENABLED && !ARDUINO_USB_MODE
#include <tusb.h>
#endif
#include "text/LatinText.h"

namespace {

using NanoRole = DisplayManager::NanoRole;
using NanoAlign = DisplayManager::NanoAlign;
using NanoIcon = DisplayManager::NanoIcon;

// Offsets from kNanoActionBase (App.cpp) — canonical indices at or above
// the base are these actions; smaller ones are the screen's own item
// indices, handled by its select*Item() as usual.
enum NanoAction : int {
  kNanoTabRead = 1,
  kNanoTabSettings,
  kNanoTabDevice,
  kNanoTabPlugins,
  kNanoPowerOff,
  kNanoPagePrev,
  kNanoPageNext,
  kNanoReadResume,
  kNanoReadChapters,
  kNanoReadSavePoints,
  kNanoReadLibrary,
  kNanoReadFonts,
  kNanoSettingsScreensaver,
  kNanoPluginLibrary,
  kNanoTabThemes,
  kNanoThemePagePrev,
  kNanoThemePageNext,
  kNanoThemeOwnAccent,
  kNanoLibrarySort,
  kNanoLaunchPlugin = 100,   // + index into pluginLibrary_.enabledEntries()
  kNanoThemePalette = 200,   // + palette index
  kNanoThemeLayout = 300,    // + App::NanoLayout
  kNanoThemeSection = 400,   // + 0 colors / 1 font / 2 layout
  kNanoThemeFont = 500,      // + 0 follow the reading font, + 1 + family
  kNanoThemeReading = 600,   // + 0 dark / 1 light / 2 night reading theme
  kNanoThemeLetterColor = 610,
  kNanoThemeLayoutType = 620,  // + 0 icons only / 1 icons + labels
  kNanoTypoSection = 630,      // + 0 colors / 1 text / 2 guide
  kNanoTypoBack = 640,
};

// DeviceHome rows (App::deviceHomeActions_).
enum DeviceHomeAction : uint8_t {
  kDeviceNone,
  kDeviceSdCard,
  kDeviceVersion,
  kDeviceUpdateNow,
  kDeviceUsb,
  kDeviceSync,
  kDeviceBluetooth,
  kDeviceWifi,
  kDeviceFirmware,
  kDeviceTutorial,
  kDevicePowerOff,
};

// Reader panel buttons (not menu items: the panel lives in Paused).
enum NanoPanelAction : int {
  kPanelMenu = 1,
  kPanelChapters,
  kPanelBookmark,
  kPanelWpmMinus,
  kPanelWpmPlus,
  kPanelStart,
  kPanelRewind,
  kPanelGoTo,
  kPanelLook,
};

// Library order (NVS lib_sort).
enum LibrarySort : uint8_t {
  kLibrarySortRecent = 0,
  kLibrarySortTitle,
  kLibrarySortAuthor,
  kLibrarySortProgress,
  kLibrarySortCount,
};

constexpr uint32_t kNanoDragFrameMs = 40;
// Horizontal travel before a touch on a slider tile becomes a drag instead
// of a tap (the tap still opens the row's own editor or cycles it).
constexpr int kNanoSliderDragThreshold = 10;
// Lists: rows per page.
constexpr int kNanoListRows = 3;

// "Tryb zaawansowany: " -> "Tryb zaawansowany".
String nanoStripColon(const String &text) {
  String out = text;
  out.trim();
  while (out.endsWith(":")) {
    out.remove(out.length() - 1);
    out.trim();
  }
  return out;
}

// Settings rows are built as "<Name>: <value>" — split at the last ": ".
bool nanoSplitSetting(const String &item, String &label, String &value) {
  const int sep = item.lastIndexOf(": ");
  if (sep <= 0) {
    return false;
  }
  label = item.substring(0, sep);
  value = item.substring(sep + 2);
  value.trim();
  if (value.endsWith(" >")) {
    value.remove(value.length() - 2);
  }
  return true;
}

bool nanoIsDeleteLabel(const String &label) {
  // German and Romanian labels carry ö / ș (single-byte glyph codes), and
  // "Buch löschen" puts the verb last, so match the ASCII tail of the verb.
  return label.startsWith("Usu") || label.startsWith("Delete") || label.startsWith("Eliminar") ||
         label.startsWith("Supprimer") || label.indexOf("schen") >= 0 || label.indexOf("terge") >= 0;
}

// Cover of the current book on the Czytaj card: a stable color per book.
uint16_t nanoCoverColor(const String &path) {
  constexpr uint16_t kColors[] = {0x99E3, 0x1AF5, 0x0B6A, 0x7B98, 0x4490, 0xB4CD, 0x9A49, 0x32FA};
  // Without the extension: an EPUB and its converted .rsvp are one book.
  String key = path;
  const int dot = key.lastIndexOf('.');
  if (dot > key.lastIndexOf('/')) {
    key.remove(dot);
  }
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < key.length(); ++i) {
    hash = (hash ^ static_cast<uint8_t>(key[i])) * 16777619u;
  }
  return kColors[hash % 8];
}

// Up to two capitals from the first words of a title ("Pan Tadeusz" ->
// "PT"), Polish letters folded to ASCII for the cover.
String nanoInitials(const String &title) {
  String out;
  bool wordStart = true;
  for (size_t i = 0; i < title.length() && out.length() < 2; ++i) {
    uint8_t value = LatinText::byteValue(title[i]);
    if (value >= 0x80 || value < 0x20) {
      value = LatinText::fallbackAsciiByte(value);
    }
    const char c = static_cast<char>(value);
    const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (letter && wordStart) {
      out += static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    }
    wordStart = !letter;
  }
  return out.isEmpty() ? String("?") : out;
}

NanoIcon nanoPluginIcon(const String &id) {
  if (id == "dictaphone") return NanoIcon::Record;
  if (id == "focus-timer") return NanoIcon::Hourglass;
  if (id == "rss") return NanoIcon::List;
  return NanoIcon::Apps;
}

// Reading typeface -> the UI family closest to it (Motywy > Czcionka >
// "Jak czytanie"). Every book face we ship is a serif except these two.
uint8_t nanoFamilyForTypeface(DisplayManager::ReaderTypeface typeface) {
  switch (typeface) {
    case DisplayManager::ReaderTypeface::AtkinsonHyperlegible:
      return 2;  // Atkinson
    case DisplayManager::ReaderTypeface::OpenDyslexic:
      return 5;  // OpenDyslexic
    default:
      return 4;  // Literata
  }
}

}  // namespace

// nano::Sink for menu screens: targets go into currentGridButtons_ (the
// usual handleGridTap() flash/debounce/destructive-confirm path), sliders
// into nanoSliderTargets_. `labels` are the screen's items by canonical
// index — handleGridTap() reads a target's label to decide whether it needs
// the two-tap confirm, and index 0 is Back on every screen that has one.
struct NanoSinkAdapter : nano::Sink {
  App &app;
  const std::vector<String> *labels = nullptr;
  size_t labelOffset = 0;
  bool zeroIsBack = false;

  explicit NanoSinkAdapter(App &owner) : app(owner) {}

  void target(const ui::Rect &rect, int id) override {
    String label;
    if (labels != nullptr && id >= 0 && id < kNanoActionBase &&
        static_cast<size_t>(id) + labelOffset < labels->size()) {
      label = (*labels)[static_cast<size_t>(id) + labelOffset];
    }
    app.nanoAddTarget(rect, id, label, id == 0 && zeroIsBack ? ui::IconId::Back : ui::IconId::None);
  }
  bool pressed(int id) const override { return app.nanoPressed(id); }
  bool armed(int id) const override { return app.nanoArmed(id); }
  void slider(const ui::Rect &rect, int id) override {
    if (id < 0) {
      return;
    }
    App::NanoSliderTarget target;
    target.rect = rect;
    target.index = static_cast<size_t>(id);
    app.nanoSliderTargets_.push_back(target);
  }
};

// Reader panel sink: its own target list, pressed state from the panel.
struct NanoPanelSink : nano::Sink {
  std::vector<std::pair<ui::Rect, int>> &targets;
  int pressedId;
  NanoPanelSink(std::vector<std::pair<ui::Rect, int>> &out, int pressed) : targets(out), pressedId(pressed) {}
  void target(const ui::Rect &rect, int id) override { targets.push_back({rect, id}); }
  bool pressed(int id) const override { return id >= 0 && id == pressedId; }
};

// ─── Mode / screen classification ───────────────────────────────────────────

bool App::nanoUiActive() const {
  if (isExtraScreen()) {
    return true;
  }
  if (navMode_ != NavMode::Modern) {
    return false;
  }
  switch (menuScreen_) {
    case MenuScreen::WelcomeLanguage:
    case MenuScreen::WelcomeSdCard:
    case MenuScreen::WelcomeTheme:
    case MenuScreen::WelcomeHighlightColor:
    case MenuScreen::WelcomeLoading:
    case MenuScreen::WelcomeSuper:
    case MenuScreen::WelcomeConfigureIntro:
    case MenuScreen::WelcomeReadingMode:
    case MenuScreen::WelcomeReadingModePreview:
    case MenuScreen::WelcomeConnect:
    case MenuScreen::WelcomeAppPairing:
    case MenuScreen::WelcomeConfigureInApp:
    case MenuScreen::WelcomeMenuTheme:
    case MenuScreen::WelcomeFont:
    case MenuScreen::WelcomeLibrary:
    case MenuScreen::TutorialStep1:
    case MenuScreen::TutorialStep2:
    case MenuScreen::TutorialStep3:
    case MenuScreen::TutorialStep4:
    case MenuScreen::TutorialStep5:
      return false;
    case MenuScreen::TypographyFontPicker:
      return !wizardFontPickerActive_;
    case MenuScreen::BookPicker:
      return !wizardBookPickerActive_;
    default:
      return true;
  }
}

bool App::nanoRailScreen() const {
  switch (menuScreen_) {
    case MenuScreen::Main:
    case MenuScreen::BookPicker:
    case MenuScreen::ChapterPicker:
    case MenuScreen::SavePointsList:
    case MenuScreen::SettingsHome:
    case MenuScreen::DeviceHome:
    case MenuScreen::NanoThemes:
    case MenuScreen::PluginsHome:
      return true;
    default:
      return false;
  }
}

App::NanoTab App::nanoActiveTab() const {
  switch (menuScreen_) {
    case MenuScreen::SettingsHome:
    case MenuScreen::SettingsDisplay:
    case MenuScreen::SettingsPacing:
    case MenuScreen::ScreensaverSettings:
    case MenuScreen::Presets:
    case MenuScreen::PresetsDeleteConfirm:
    case MenuScreen::TypographyTuning:
    case MenuScreen::TypographyResetConfirm:
      return NanoTab::Settings;
    case MenuScreen::NanoThemes:
      return NanoTab::Themes;
    case MenuScreen::DeviceHome:
    case MenuScreen::WifiSettings:
    case MenuScreen::WifiNetworks:
    case MenuScreen::SettingsConnectivity:
    case MenuScreen::SettingsAbout:
    case MenuScreen::SdCardRepairConfirm:
    case MenuScreen::UpdateConfirm:
      return NanoTab::Device;
    case MenuScreen::PluginsHome:
    case MenuScreen::PluginsActive:
    case MenuScreen::PluginLibraryScreen:
    case MenuScreen::PluginDetail:
      return NanoTab::Plugins;
    case MenuScreen::TypographyFontPicker:
      return nanoFontPickerFromRead_ ? NanoTab::Read : NanoTab::Settings;
    default:
      return NanoTab::Read;
  }
}

String App::nanoScreenTitle() const {
  switch (menuScreen_) {
    case MenuScreen::SettingsDisplay:
      return uiText(UiText::Display);
    case MenuScreen::SettingsPacing:
      return tr3(TrKey3::ReadingSettings);
    case MenuScreen::ScreensaverSettings:
      return nanoStripColon(tr(TrKey::Screensaver));
    case MenuScreen::WifiSettings:
      return "Wi-Fi";
    case MenuScreen::WifiNetworks:
      return nanoStripColon(tr(TrKey::ChooseNetwork));
    case MenuScreen::SettingsConnectivity:
      return tr(TrKey::Connectivity);
    case MenuScreen::SettingsAbout:
      return tr(TrKey::AboutHelp);
    case MenuScreen::Presets:
      return tr3(TrKey3::PresetsLabel);
    case MenuScreen::PluginsActive:
      return tr3(TrKey3::ActivePlugins);
    case MenuScreen::PluginLibraryScreen:
      return tr2(TrKey2::PluginLibrary);
    case MenuScreen::PluginDetail: {
      const auto &all = pluginLibrary_.all();
      return pluginDetailIndex_ < all.size() ? all[pluginDetailIndex_].name : uiText(UiText::Plugins);
    }
    case MenuScreen::TypographyFontPicker:
      return uiText(UiText::Typeface);
    case MenuScreen::BookDetails:
      return storage_.bookDisplayName(bookDetailsBookIndex_);
    case MenuScreen::ChapterPicker:
      return uiText(UiText::Chapters);
    case MenuScreen::SavePointsList:
      return uiText(UiText::SavePoints);
    default:
      return "";
  }
}

// ─── Tap targets ────────────────────────────────────────────────────────────

void App::nanoAddTarget(const ui::Rect &rect, int canonicalIndex, const String &label, ui::IconId icon) {
  DisplayManager::Button target;
  target.x = rect.x;
  target.y = rect.y;
  target.width = rect.w;
  target.height = rect.h;
  target.label = label;
  target.icon = icon;
  currentGridButtons_.push_back(target);
  currentGridItemIndices_.push_back(static_cast<size_t>(canonicalIndex));
}

bool App::nanoPressed(int canonicalIndex) const {
  return canonicalIndex >= 0 && isGridItemFlashing(static_cast<size_t>(canonicalIndex), millis());
}

bool App::nanoArmed(int canonicalIndex) const {
  return canonicalIndex >= 0 && isGridItemArmed(static_cast<size_t>(canonicalIndex), millis());
}

// ─── Entry points (called by the generic menu renderers) ────────────────────

void App::renderNanoScreen(const String &title, const std::vector<String> &items, size_t selectedIndex,
                           size_t headerRows) {
  (void)selectedIndex;
  applyReaderUiOrientation();
  display_.setModernCardStyle(true);
  nanoSyncLayout();
  currentGridButtons_.clear();
  currentGridItemIndices_.clear();
  nanoSliderTargets_.clear();
  // Paging is Nano's own (header arrows / nanoChangePage()); keep the
  // Buttons-grid swipe pager from acting on these screens.
  gridHeaderRows_ = headerRows;
  gridHasBack_ = false;
  gridItemsPerPage_ = 1;
  gridPageCount_ = 1;
  gridPage_ = 0;
  gridPagesVertically_ = false;
  nanoPage_ = 0;
  nanoPageFirstIndex_.clear();

  NanoSinkAdapter sink(*this);
  sink.labels = &items;
  sink.labelOffset = headerRows;
  sink.zeroIsBack = items.size() > headerRows && items[headerRows] == uiText(UiText::Back);

  display_.nanoBeginFrame();
  switch (menuScreen_) {
    case MenuScreen::Main:
      renderNanoRead();
      break;
    case MenuScreen::SettingsHome:
      renderNanoSettingsHome();
      break;
    case MenuScreen::DeviceHome:
      renderNanoDeviceHome();
      break;
    case MenuScreen::NanoThemes:
      renderNanoThemes();
      break;
    case MenuScreen::PluginsHome:
      renderNanoPluginsHome();
      break;
    case MenuScreen::BookPicker:
      renderNanoShelf();
      break;
    case MenuScreen::ChapterPicker:
      renderNanoChapters();
      break;
    case MenuScreen::SavePointsList:
      renderNanoSavePoints();
      break;
    case MenuScreen::BookDetails:
      renderNanoBookDetails();
      break;
    case MenuScreen::RestartConfirm:
    case MenuScreen::TypographyResetConfirm:
    case MenuScreen::SdCardRepairConfirm:
    case MenuScreen::UpdateConfirm:
    case MenuScreen::BookDeleteConfirm:
    case MenuScreen::SavePointDeleteConfirm:
    case MenuScreen::PresetsDeleteConfirm:
      renderNanoConfirm(title, items, headerRows);
      break;
    default:
      renderNanoList(title, items, headerRows);
      break;
  }
  if (nanoRailScreen()) {
    renderNanoRail();
  }
  display_.nanoEndFrame();
}

void App::renderNanoLibraryList(const std::vector<DisplayManager::LibraryItem> &items, size_t selectedIndex,
                                const String &title) {
  if (menuScreen_ == MenuScreen::BookPicker) {
    renderNanoScreen(title, {}, selectedIndex, 0);
    return;
  }
  // Wi-Fi network list: SSID left, signal/security right.
  std::vector<String> titles;
  std::vector<String> subtitles;
  titles.reserve(items.size());
  subtitles.reserve(items.size());
  for (const DisplayManager::LibraryItem &item : items) {
    titles.push_back(item.title);
    subtitles.push_back(item.subtitle);
  }
  applyReaderUiOrientation();
  display_.setModernCardStyle(true);
  nanoSyncLayout();
  currentGridButtons_.clear();
  currentGridItemIndices_.clear();
  nanoSliderTargets_.clear();
  gridHeaderRows_ = 0;
  gridHasBack_ = false;
  gridItemsPerPage_ = 1;
  gridPageCount_ = 1;
  gridPage_ = 0;
  gridPagesVertically_ = false;
  nanoPage_ = 0;
  nanoPageFirstIndex_.clear();
  display_.nanoBeginFrame();
  renderNanoList(title, titles, 0, subtitles);
  display_.nanoEndFrame();
}

// ─── Rail ───────────────────────────────────────────────────────────────────

std::vector<nano::RailTab> App::nanoRailTabs() {
  std::vector<nano::RailTab> tabs;
  tabs.reserve(5);
  auto add = [&tabs](int action, const String &label, NanoIcon icon) {
    nano::RailTab tab;
    tab.id = kNanoActionBase + action;
    tab.label = label;
    tab.icon = icon;
    tabs.push_back(tab);
  };
  add(kNanoTabRead, uiText(UiText::Read), NanoIcon::Books);
  add(kNanoTabSettings, uiText(UiText::Settings), NanoIcon::Sliders);
  add(kNanoTabThemes, tr3(TrKey3::NanoThemesTab), NanoIcon::Palette);
  add(kNanoTabDevice, tr3(TrKey3::NanoDeviceTab), NanoIcon::Device);
  // Plugins are an advanced-mode feature everywhere else in the UI too.
  if (devModeEnabled()) {
    add(kNanoTabPlugins, uiText(UiText::Plugins), NanoIcon::Apps);
  }
  return tabs;
}

void App::renderNanoRail() {
  std::vector<nano::RailTab> tabs = nanoRailTabs();
  const NanoTab active = nanoActiveTab();
  const NanoTab order[] = {NanoTab::Read, NanoTab::Settings, NanoTab::Themes, NanoTab::Device, NanoTab::Plugins};
  for (size_t i = 0; i < tabs.size() && i < 5; ++i) {
    tabs[i].active = order[i] == active;
    tabs[i].badge = order[i] == NanoTab::Device && otaUpdatePromptPending_;
  }
  NanoSinkAdapter sink(*this);
  nano::paintRail(display_, sink, tabs);
}

void App::nanoSyncLayout() {
  nano::Layout &layout = nano::layout();
  layout.railRight = nanoLayout_ == kNanoLayoutRight || nanoLayout_ == kNanoLayoutCompactRight;
  layout.compact = nanoLayout_ == kNanoLayoutCompact || nanoLayout_ == kNanoLayoutCompactRight;
  std::vector<String> labels;
  for (const nano::RailTab &tab : nanoRailTabs()) {
    labels.push_back(tab.label);
  }
  layout.railWidth = nano::railWidthFor(labels);
}

// ─── Czytaj ─────────────────────────────────────────────────────────────────

void App::renderNanoRead() {
  nanoFontPickerFromRead_ = false;
  nano::ReadHome view;
  view.hasBook = usingStorageBook_;
  view.resumeId = kNanoActionBase + kNanoReadResume;
  view.fontsId = kNanoActionBase + kNanoReadFonts;
  view.fontsLabel = uiText(UiText::Typeface);
  if (view.hasBook) {
    view.title = storage_.bookDisplayName(currentBookIndex_);
    view.author = storage_.bookAuthorName(currentBookIndex_);
    if (view.author.isEmpty()) {
      view.author = tr3(TrKey3::NanoUnknownAuthor);
    }
    view.progressPercent = readingProgressPercent();
    view.progressLabel = String(static_cast<unsigned>(view.progressPercent)) + "%";
    view.coverColor = nanoCoverColor(currentBookPath_);
    view.coverInitials = nanoInitials(view.title);
    view.cover = BookExtras::picture(currentBookPath_, BookExtras::Picture::Cover);
    view.hint = tr3(TrKey3::NanoResumeHint);
  } else {
    view.title = tr3(TrKey3::NanoNoBook);
    view.hint = tr3(TrKey3::NanoPickBookHint);
  }

  nano::Tile chapters;
  chapters.id = kNanoActionBase + kNanoReadChapters;
  chapters.label = uiText(UiText::Chapters);
  chapters.icon = NanoIcon::List;
  chapters.enabled = view.hasBook;
  if (view.hasBook && !chapterMarkers_.empty()) {
    chapters.detail = String(static_cast<unsigned>(chapterMarkers_.size()));
  }
  nano::Tile savePoints;
  savePoints.id = kNanoActionBase + kNanoReadSavePoints;
  savePoints.label = uiText(UiText::SavePoints);
  savePoints.icon = NanoIcon::Bookmark;
  if (!savePoints_.empty()) {
    savePoints.detail = String(static_cast<unsigned>(savePoints_.size()));
  }
  nano::Tile library;
  library.id = kNanoActionBase + kNanoReadLibrary;
  library.label = uiText(UiText::Library);
  library.icon = NanoIcon::Books;
  library.detail = String(static_cast<unsigned>(storage_.bookCount())) + " " + tr3(TrKey3::NanoBooksCount);
  view.tiles = {chapters, savePoints, library};

  NanoSinkAdapter sink(*this);
  nano::paintReadHome(display_, sink, view);
}

// ─── Ustawienia ─────────────────────────────────────────────────────────────

void App::renderNanoSettingsHome() {
  nanoScreensaverFromSettingsHome_ = false;
  auto item = [](int id, const String &label, NanoIcon icon) {
    nano::SectionItem entry;
    entry.id = id;
    entry.label = label;
    entry.icon = icon;
    return entry;
  };
  std::vector<nano::Section> sections(3);
  sections[0].title = tr3(TrKey3::NanoReadingSection);
  sections[0].items = {item(kSettingsHomeReadingIndex, tr3(TrKey3::NanoPacingTile), NanoIcon::Sliders),
                       item(kSettingsHomeTypographyIndex, tr4(TrKey4::TypoTitle), NanoIcon::Font)};
  sections[1].title = tr3(TrKey3::NanoSystemSection);
  sections[1].items = {item(kSettingsHomeDisplayIndex, uiText(UiText::Display), NanoIcon::Sun),
                       item(kNanoActionBase + kNanoSettingsScreensaver, nanoStripColon(tr(TrKey::Screensaver)),
                            NanoIcon::Hourglass)};
  nano::SectionItem advanced = item(kSettingsHomeAdvancedIndex, nanoStripColon(tr3(TrKey3::AdvancedModeColon)),
                                    NanoIcon::None);
  advanced.toggle = true;
  advanced.on = devModeEnabled();
  sections[2].items.push_back(advanced);
  if (devModeEnabled()) {
    sections[2].items.push_back(item(kSettingsHomePresetsIndex, tr3(TrKey3::PresetsLabel), NanoIcon::Edit));
  } else {
    sections[2].items[0].fullWidth = true;
  }
  for (nano::Section &section : sections) {
    for (nano::SectionItem &entry : section.items) {
      entry.helpId = helpActionFor(MenuScreen::SettingsHome, entry.id);
    }
  }
  NanoSinkAdapter sink(*this);
  sink.labels = &settingsMenuItems_;
  nano::paintSections(display_, sink, nano::tabContent(), sections);
}

// ─── Urzadzenie ─────────────────────────────────────────────────────────────

void App::openDeviceHome() {
  menuScreen_ = MenuScreen::DeviceHome;
  settingsSelectedIndex_ = 1;
  rebuildSettingsMenuItems();
  renderSettings();
}

void App::rebuildDeviceHomeItems() {
  deviceHomeActions_.clear();
  settingsMenuItems_.push_back(uiText(UiText::Back));
  deviceHomeActions_.push_back(kDeviceNone);
  auto add = [this](uint8_t action, const String &label) {
    settingsMenuItems_.push_back(label);
    deviceHomeActions_.push_back(action);
  };
  add(kDeviceSdCard, tr3(TrKey3::NanoSdCard));
  if (otaUpdatePromptPending_) {
    add(kDeviceUpdateNow, tr2(TrKey2::Update));
  } else {
    add(kDeviceVersion, tr3(TrKey3::NanoVersionTile));
  }
#if RSVP_USB_TRANSFER_ENABLED
  add(kDeviceUsb, uiText(UiText::UsbTransfer));
#endif
  add(kDeviceSync, tr3(TrKey3::NanoAppTile));
#if FLOWER_BLE_ENABLED
  add(kDeviceBluetooth, "Bluetooth");
#endif
  add(kDeviceWifi, "Wi-Fi");
  add(kDeviceTutorial, tr3(TrKey3::TutorialLabel));
  add(kDevicePowerOff, tr3(TrKey3::NanoPowerOffTile));
}

void App::renderNanoDeviceHome() {
  std::vector<nano::Tile> tiles;
  for (size_t i = 1; i < deviceHomeActions_.size() && i < settingsMenuItems_.size(); ++i) {
    nano::Tile tile;
    tile.id = static_cast<int>(i);
    tile.label = settingsMenuItems_[i];
    switch (deviceHomeActions_[i]) {
      case kDeviceSdCard:
        tile.icon = NanoIcon::SdCard;
        tile.detail = String(static_cast<unsigned>(storage_.bookCount())) + " " + tr3(TrKey3::NanoBooksCount);
        break;
      case kDeviceVersion:
        tile.icon = NanoIcon::Info;
        tile.detail = otaUpdater_.currentVersion();
        break;
      case kDeviceUpdateNow:
        tile.icon = NanoIcon::Download;
        tile.detail = pendingUpdateNewVersion_;
        tile.accent = true;
        break;
      case kDeviceUsb:
        tile.icon = NanoIcon::Usb;
        tile.detail = tr3(TrKey3::NanoTransferFiles);
        break;
      case kDeviceSync:
        tile.icon = NanoIcon::Phone;
        tile.detail = "Wi-Fi + QR";
        break;
      case kDeviceBluetooth:
        tile.icon = NanoIcon::Bluetooth;
#if FLOWER_BLE_ENABLED
        tile.detail = ble_.isActive() ? (ble_.isConnected() ? tr3(TrKey3::NanoConnectedState)
                                                            : tr3(TrKey3::NanoOnState))
                                      : tr3(TrKey3::NanoOffState);
#endif
        break;
      case kDeviceWifi:
        tile.icon = NanoIcon::Wifi;
        tile.detail = storedOrFallbackLabel(configuredWifiSsid(), tr(TrKey::NotSet));
        break;
      case kDeviceTutorial:
        tile.icon = NanoIcon::Help;
        break;
      case kDevicePowerOff:
        tile.icon = NanoIcon::Power;
        break;
      default:
        break;
    }
    tiles.push_back(tile);
  }
  const int columns = tiles.size() > 6 ? 4 : 3;
  const int rows = tiles.size() > static_cast<size_t>(columns * 2) ? 3 : 2;
  NanoSinkAdapter sink(*this);
  sink.labels = &settingsMenuItems_;
  nano::paintTileGrid(display_, sink, nano::tabContent(), tiles, columns, rows);
}

void App::selectDeviceHomeItem(uint32_t nowMs) {
  if (settingsSelectedIndex_ == 0 || settingsSelectedIndex_ >= deviceHomeActions_.size()) {
    menuScreen_ = MenuScreen::Main;
    renderMainMenu();
    return;
  }
  switch (deviceHomeActions_[settingsSelectedIndex_]) {
    case kDeviceSdCard:
      runSdCardCheck(nowMs);
      return;
    case kDeviceVersion: {
      if (devModeEnabled()) {
        // Advanced mode: the version tile checks for an update.
        runFirmwareUpdate(preferredOtaConfig(), false, nowMs);
        return;
      }
      // Same 10-tap developer unlock as Informacje > Wersja.
      constexpr uint32_t kTapWindowMs = 1500;
      constexpr uint8_t kTapsToUnlock = 10;
      if (aboutLastTapMs_ != 0 && nowMs - aboutLastTapMs_ > kTapWindowMs) {
        aboutTapCount_ = 0;
      }
      aboutLastTapMs_ = nowMs;
      ++aboutTapCount_;
      if (!devModeEnabled() && aboutTapCount_ >= kTapsToUnlock) {
        setDevModeEnabled(true);
        aboutTapCount_ = 0;
      }
      rebuildSettingsMenuItems();
      renderSettings();
      return;
    }
    case kDeviceUpdateNow:
      otaUpdatePromptPending_ = false;
      runFirmwareUpdate(preferredOtaConfig(), false, nowMs);
      return;
    case kDeviceUsb:
#if RSVP_USB_TRANSFER_ENABLED
      enterUsbTransfer(nowMs);
#endif
      return;
    case kDeviceSync:
      if (state_ == AppState::CompanionSync) {
        exitCompanionSync(nowMs);
      } else {
        enterCompanionSync(nowMs);
      }
      return;
    case kDeviceBluetooth:
#if FLOWER_BLE_ENABLED
      if (ble_.isActive()) {
        ble_.stop();
        preferences_.putBool(kPrefBleEnabled, false);
        Serial.println("[app] BLE turned OFF by user");
      } else {
        ble_.begin(this);
        preferences_.putBool(kPrefBleEnabled, true);
        Serial.printf("[app] BLE turned ON by user (name=%s)\n", ble_.deviceName().c_str());
      }
      rebuildSettingsMenuItems();
      renderSettings();
#endif
      return;
    case kDeviceWifi:
      openWifiSettings();
      return;
    case kDeviceFirmware:
      runFirmwareUpdate(preferredOtaConfig(), false, nowMs);
      return;
    case kDeviceTutorial:
      openTutorialStep1();
      return;
    case kDevicePowerOff:
      enterPowerOff(nowMs);
      return;
    default:
      return;
  }
}

// ─── Motywy ─────────────────────────────────────────────────────────────────
// Three sections behind a segmented control: palette chips that preview
// themselves (+ the own-accent switch), the UI font (drawn in each face),
// and where the tab rail sits.

void App::openNanoThemes() {
  menuScreen_ = MenuScreen::NanoThemes;
  settingsSelectedIndex_ = 0;
  rebuildSettingsMenuItems();
  renderSettings();
}

void App::setNanoTheme(uint8_t palette, bool ownAccent, uint8_t layout) {
  if (palette >= DisplayManager::nanoPaletteCount()) {
    palette = DisplayManager::kNanoPaletteClassic;
  }
  if (layout >= kNanoLayoutCount) {
    layout = kNanoLayoutLeft;
  }
  nanoPalette_ = palette;
  nanoOwnAccent_ = ownAccent;
  nanoLayout_ = layout;
  preferences_.putUChar(kPrefNanoPalette, nanoPalette_);
  preferences_.putBool(kPrefNanoOwnAccent, nanoOwnAccent_);
  preferences_.putUChar(kPrefNanoLayout, nanoLayout_);
  display_.setNanoPalette(nanoPalette_, nanoOwnAccent_);
  Serial.printf("[nano] palette=%s ownAccent=%d layout=%u\n", DisplayManager::nanoPaletteName(nanoPalette_),
                nanoOwnAccent_ ? 1 : 0, static_cast<unsigned>(nanoLayout_));
  rebuildSettingsMenuItems();
  renderSettings();
}

String App::nanoPaletteLabel(uint8_t palette) const {
  const String name = DisplayManager::nanoPaletteName(palette);
  if (palette == DisplayManager::kNanoPaletteClassic) return tr3(TrKey3::NanoPaletteClassic);
  if (name == "Cream") return tr3(TrKey3::NanoPaletteCream);
  if (name == "Graphite") return tr3(TrKey3::NanoPaletteGraphite);
  if (name == "Forest") return tr3(TrKey3::NanoPaletteForest);
  return name;
}

void App::renderNanoThemes() {

  nano::ThemesView view;
  view.section = std::min<int>(nanoThemeSection_, nano::ThemesView::kSections - 1);
  const String segmentLabels[nano::ThemesView::kSections] = {tr4(TrKey4::ThemeMenuColors), tr4(TrKey4::ThemeMenuFont),
                                                             tr3(TrKey3::NanoLayoutTab)};
  for (int i = 0; i < nano::ThemesView::kSections; ++i) {
    view.segmentIds[i] = kNanoActionBase + kNanoThemeSection + i;
    view.segmentLabels[i] = segmentLabels[i];
  }

  if (view.section == 0) {
    for (uint8_t palette = 0; palette < DisplayManager::nanoPaletteCount(); ++palette) {
      nano::ThemesView::PaletteChip chip;
      chip.id = kNanoActionBase + kNanoThemePalette + palette;
      chip.palette = palette;
      chip.name = nanoPaletteLabel(palette);
      chip.selected = palette == nanoPalette_;
      view.palettes.push_back(chip);
    }
    // "Jak czytanie" already uses the letter color; the switch only
    // matters for the fixed palettes.
    if (nanoPalette_ != DisplayManager::kNanoPaletteClassic) {
      view.ownAccentId = kNanoActionBase + kNanoThemeOwnAccent;
      view.ownAccentLabel = tr3(TrKey3::NanoOwnAccent);
      view.ownAccentOn = nanoOwnAccent_;
    }
  } else if (view.section == 1) {
    nano::ThemesView::FontChip follow;
    follow.id = kNanoActionBase + kNanoThemeFont;
    follow.family = nanoFamilyForTypeface(typographyConfig_.typeface);
    follow.name = tr3(TrKey3::NanoFollowReader);
    follow.sample = typefaceDisplayName(typographyConfig_.typeface);
    follow.selected = nanoUiFontChoice_ == kNanoUiFontFollowReader;
    view.fonts.push_back(follow);
    for (uint8_t family = 0; family < DisplayManager::nanoUiFontCount(); ++family) {
      nano::ThemesView::FontChip chip;
      chip.id = kNanoActionBase + kNanoThemeFont + 1 + family;
      chip.family = family;
      chip.name = DisplayManager::nanoUiFontName(family);
      chip.sample = tr3(TrKey3::NanoFontSample);
      chip.selected = nanoUiFontChoice_ == family;
      view.fonts.push_back(chip);
    }
  } else {
    const bool compact = nanoLayout_ == kNanoLayoutCompact || nanoLayout_ == kNanoLayoutCompactRight;
    const bool right = nanoLayout_ == kNanoLayoutRight || nanoLayout_ == kNanoLayoutCompactRight;
    const String side = right ? tr4(TrKey4::LayoutSideRight) : tr4(TrKey4::LayoutSideLeft);
    for (int type = 0; type < 2; ++type) {
      nano::ThemesView::LayoutChip chip;
      chip.id = kNanoActionBase + kNanoThemeLayoutType + type;
      chip.compact = type == 0;
      chip.selected = chip.compact == compact;
      chip.railRight = right;
      chip.name = chip.compact ? tr4(TrKey4::LayoutIconsOnly) : tr4(TrKey4::LayoutIconsLabels);
      chip.detail = chip.selected ? side : String(tr4(TrKey4::LayoutTapToPick));
      view.layouts.push_back(chip);
    }
    view.layoutHint = tr4(TrKey4::LayoutTapAgainHint);
  }
  NanoSinkAdapter sink(*this);
  nano::paintThemes(display_, sink, view);
}

uint8_t App::nanoResolvedUiFont() const {
  if (nanoUiFontChoice_ == kNanoUiFontFollowReader || nanoUiFontChoice_ >= DisplayManager::nanoUiFontCount()) {
    return nanoFamilyForTypeface(typographyConfig_.typeface);
  }
  return nanoUiFontChoice_;
}

void App::applyNanoUiFont() { display_.setNanoUiFont(nanoResolvedUiFont()); }

void App::setNanoUiFontChoice(uint8_t choice) {
  if (choice != kNanoUiFontFollowReader && choice >= DisplayManager::nanoUiFontCount()) {
    choice = kNanoUiFontFollowReader;
  }
  nanoUiFontChoice_ = choice;
  preferences_.putUChar(kPrefNanoUiFont, nanoUiFontChoice_);
  applyNanoUiFont();
  Serial.printf("[nano] ui font choice=%u -> %s\n", static_cast<unsigned>(nanoUiFontChoice_),
                DisplayManager::nanoUiFontName(nanoResolvedUiFont()));
  rebuildSettingsMenuItems();
  renderSettings();
}

// ─── Wyglad czytania ────────────────────────────────────────────────────────
// Reading colors and typography on one screen, over the reading screen
// itself (the current word of the open book, or the demo text), so every
// change shows as it will look while reading. The menu palette and menu
// font stay on Motywy.

void App::openNanoTypography(uint8_t section, uint8_t returnTo, uint32_t nowMs) {
  nanoTypographySection_ = static_cast<uint8_t>(std::min<int>(section, nano::TypographyView::kSections - 1));
  nanoTypographyReturn_ = returnTo;
  typographyTuningSelectedIndex_ = TypographyTuningFontSize;
  menuScreen_ = MenuScreen::TypographyTuning;
  if (state_ != AppState::Menu) {
    setState(AppState::Menu, nowMs);
  } else {
    renderMenu();
  }
}

void App::nanoTypographyBack(uint32_t nowMs) {
  switch (nanoTypographyReturn_) {
    case kNanoTypographyFromRead:
      menuScreen_ = MenuScreen::Main;
      renderMainMenu();
      return;
    case kNanoTypographyFromPanel:
      menuScreen_ = MenuScreen::Main;
      setState(AppState::Paused, nowMs);
      return;
    default:
      settingsSelectedIndex_ = kSettingsHomeTypographyIndex;
      menuScreen_ = MenuScreen::SettingsHome;
      rebuildSettingsMenuItems();
      renderSettings();
      return;
  }
}

void App::renderNanoTypography() {
  applyReaderUiOrientation();
  display_.setModernCardStyle(true);
  nanoSyncLayout();
  currentGridButtons_.clear();
  currentGridItemIndices_.clear();
  nanoSliderTargets_.clear();
  gridHeaderRows_ = 0;
  gridHasBack_ = false;
  gridItemsPerPage_ = 1;
  gridPageCount_ = 1;
  gridPage_ = 0;
  gridPagesVertically_ = false;
  nanoPage_ = 0;
  nanoPageFirstIndex_.clear();

  nano::TypographyView view;
  view.backId = kNanoActionBase + kNanoTypoBack;
  view.section = std::min<int>(nanoTypographySection_, nano::TypographyView::kSections - 1);
  const String segmentLabels[nano::TypographyView::kSections] = {tr4(TrKey4::TypoColors), tr4(TrKey4::TypoText),
                                                                 tr4(TrKey4::TypoGuide)};
  for (int i = 0; i < nano::TypographyView::kSections; ++i) {
    view.segmentIds[i] = kNanoActionBase + kNanoTypoSection + i;
    view.segmentLabels[i] = segmentLabels[i];
  }
  view.word = reader_.currentWord();
  if (view.word.isEmpty()) {
    view.word = tr4(TrKey4::TutWord);
  } else if (phantomWordsEnabled_) {
    view.before = phantomBeforeText();
    view.after = phantomAfterText();
  }
  view.fontSizeLevel = readerFontSizeIndex_;

  auto slider = [this](int index, const String &label, const String &value) {
    nano::ListItem item;
    item.kind = nano::ListItem::Kind::Slider;
    item.id = index;
    item.label = label;
    item.value = value;
    NanoSliderSpec spec;
    if (nanoSliderSpec(static_cast<size_t>(index), spec)) {
      item.sliderMin = spec.minimum;
      item.sliderMax = spec.maximum;
      item.sliderValue = spec.value;
    }
    item.dragging = nanoSliderDragging_ && nanoSliderIndex_ == index;
    return item;
  };
  auto toggle = [](int index, const String &label, bool on) {
    nano::ListItem item;
    item.kind = nano::ListItem::Kind::Toggle;
    item.id = index;
    item.label = label;
    item.on = on;
    return item;
  };

  if (view.section == 0) {
    const uint8_t current = nightMode_ ? 2 : (darkMode_ ? 0 : 1);
    const String names[3] = {uiText(UiText::Dark), uiText(UiText::Light), uiText(UiText::Night)};
    for (uint8_t theme = 0; theme < 3; ++theme) {
      nano::TypographyView::ThemeChip chip;
      chip.id = kNanoActionBase + kNanoThemeReading + theme;
      chip.theme = theme;
      chip.name = names[theme];
      chip.selected = theme == current;
      view.themes.push_back(chip);
    }
    view.letterColorId = kNanoActionBase + kNanoThemeLetterColor;
    view.letterColorLabel = tr4(TrKey4::LetterColorTitle);
    view.letterColor = display_.focusColorFor(nightMode_);
    view.items.push_back(
        toggle(TypographyTuningFocusHighlight, tr4(TrKey4::TypoHighlight), typographyConfig_.focusHighlight));
  } else if (view.section == 1) {
    nano::ListItem face;
    face.kind = nano::ListItem::Kind::Button;
    face.id = TypographyTuningTypeface;
    face.label = typefaceDisplayName(typographyConfig_.typeface);
    face.icon = NanoIcon::Font;
    face.typeface = typographyConfig_.typeface;
    view.items.push_back(face);
    view.items.push_back(slider(TypographyTuningFontSize, uiText(UiText::FontSize), readerFontSizeLabel()));
    view.items.push_back(slider(TypographyTuningTracking, tr4(TrKey4::TypoSpacing),
                                String(typographyConfig_.trackingPx > 0 ? "+" : "") +
                                    String(static_cast<int>(typographyConfig_.trackingPx)) + " px"));
    view.items.push_back(toggle(TypographyTuningPhantomWords, tr4(TrKey4::TypoNeighbours), phantomWordsEnabled_));
  } else {
    view.items.push_back(slider(TypographyTuningAnchor, tr4(TrKey4::TypoPosition),
                                String(static_cast<unsigned>(effectiveAnchorPercent())) + "%"));
    view.items.push_back(slider(TypographyTuningGuideWidth, tr4(TrKey4::TypoLineLength),
                                String(static_cast<unsigned>(typographyConfig_.guideHalfWidth)) + " px"));
    view.items.push_back(slider(TypographyTuningGuideGap, tr4(TrKey4::TypoLineGap),
                                String(static_cast<unsigned>(typographyConfig_.guideGap)) + " px"));
    nano::ListItem reset;
    reset.kind = nano::ListItem::Kind::Button;
    reset.id = TypographyTuningReset;
    reset.label = tr4(TrKey4::TypoDefaults);
    reset.icon = NanoIcon::Restart;
    view.items.push_back(reset);
  }

  NanoSinkAdapter sink(*this);
  display_.nanoBeginFrame();
  nano::paintTypography(display_, sink, view);
  display_.nanoEndFrame();
}

// ─── Pluginy ────────────────────────────────────────────────────────────────

void App::renderNanoPluginsHome() {
  const ui::Rect content = nano::tabContent();
  const auto enabled = pluginLibrary_.enabledEntries();
  std::vector<nano::Tile> tiles;
  for (size_t i = 0; i < enabled.size() && i < 7; ++i) {
    nano::Tile tile;
    tile.id = kNanoActionBase + kNanoLaunchPlugin + static_cast<int>(i);
    tile.label = enabled[i].name;
    tile.icon = nanoPluginIcon(enabled[i].id);
    tiles.push_back(tile);
  }
  nano::Tile library;
  library.id = kNanoActionBase + kNanoPluginLibrary;
  library.label = tr2(TrKey2::PluginLibrary);
  library.icon = NanoIcon::Books;
  library.detail = String(static_cast<unsigned>(pluginLibrary_.all().size())) + " " + tr3(TrKey3::NanoAvailable);

  NanoSinkAdapter sink(*this);
  if (tiles.empty()) {
    const int labelH = content.h / 2;
    display_.nanoLabel(ui::Rect(content.x, content.y, content.w, labelH), tr3(TrKey3::NoActivePlugins), 2,
                       NanoRole::Muted, NanoAlign::Center, 2);
    nano::paintTileGrid(display_, sink,
                        ui::Rect(content.x, content.y + labelH, content.w, content.h - labelH), {library}, 1, 1);
    return;
  }
  tiles.push_back(library);
  // 2x2 up to four tiles, then three or four columns over two rows.
  const int columns = tiles.size() <= 2 ? static_cast<int>(tiles.size()) : tiles.size() <= 4 ? 2 : tiles.size() <= 6 ? 3 : 4;
  const int rows = tiles.size() <= 2 ? 1 : 2;
  nano::paintTileGrid(display_, sink, content, tiles, columns, rows);
}

// ─── Biblioteka: bookshelf ──────────────────────────────────────────────────

String App::librarySortLabel() const {
  switch (librarySort_) {
    case kLibrarySortTitle:
      return tr3(TrKey3::NanoSortTitle);
    case kLibrarySortAuthor:
      return tr3(TrKey3::NanoSortAuthor);
    case kLibrarySortProgress:
      return tr3(TrKey3::NanoSortProgress);
    default:
      return tr3(TrKey3::NanoSortRecent);
  }
}

void App::sortLibraryIndices(std::vector<size_t> &indices) {
  // Recent first is also the tie-breaker of every other order.
  std::vector<uint32_t> recent(storage_.bookCount(), 0);
  for (size_t index : indices) {
    if (index < recent.size()) {
      recent[index] = bookRecentSequence(storage_.bookPath(index));
    }
  }
  auto byRecent = [&](size_t left, size_t right) {
    const bool leftCurrent = usingStorageBook_ && left == currentBookIndex_;
    const bool rightCurrent = usingStorageBook_ && right == currentBookIndex_;
    if (leftCurrent != rightCurrent) {
      return leftCurrent;
    }
    const uint32_t l = left < recent.size() ? recent[left] : 0;
    const uint32_t r = right < recent.size() ? recent[right] : 0;
    if ((l > 0) != (r > 0)) {
      return l > 0;
    }
    return l > r;
  };
  auto folded = [](const String &text) {
    String out;
    out.reserve(text.length());
    for (size_t i = 0; i < text.length(); ++i) {
      uint8_t value = LatinText::byteValue(text[i]);
      if (value >= 0x80 || value < 0x20) {
        value = LatinText::fallbackAsciiByte(value);
      }
      char c = static_cast<char>(value);
      if (c >= 'A' && c <= 'Z') {
        c = static_cast<char>(c - 'A' + 'a');
      }
      out += c;
    }
    return out;
  };

  switch (librarySort_) {
    case kLibrarySortTitle:
    case kLibrarySortAuthor: {
      const bool byAuthor = librarySort_ == kLibrarySortAuthor;
      std::vector<String> keys(storage_.bookCount());
      for (size_t index : indices) {
        if (index < keys.size()) {
          String key = byAuthor ? storage_.bookAuthorName(index) : String();
          // Books without an author go last in the author order.
          keys[index] = folded(byAuthor ? (key.isEmpty() ? String("~") : key) + " " + storage_.bookDisplayName(index)
                                        : storage_.bookDisplayName(index));
        }
      }
      std::stable_sort(indices.begin(), indices.end(), [&](size_t left, size_t right) {
        const int cmp = keys[left].compareTo(keys[right]);
        return cmp != 0 ? cmp < 0 : byRecent(left, right);
      });
      return;
    }
    case kLibrarySortProgress: {
      std::vector<int> progress(storage_.bookCount(), -1);
      for (size_t index : indices) {
        uint8_t percent = 0;
        if (index < progress.size() && bookProgressPercent(index, percent)) {
          progress[index] = percent;
        }
      }
      // Started books first, furthest along first; unread after them.
      std::stable_sort(indices.begin(), indices.end(), [&](size_t left, size_t right) {
        if (progress[left] != progress[right]) {
          return progress[left] > progress[right];
        }
        return byRecent(left, right);
      });
      return;
    }
    default:
      std::stable_sort(indices.begin(), indices.end(), byRecent);
      return;
  }
}

void App::renderNanoShelf() {
  const nano::ShelfGeometry g = nano::shelfGeometry();
  const size_t count = bookMenuItems_.size() > 1 ? bookMenuItems_.size() - 1 : 0;
  nano::ShelfView view;
  view.header.backId = 0;
  view.header.title = uiText(UiText::Library);
  view.header.trailing = String(static_cast<unsigned>(count));
  view.header.pillId = kNanoActionBase + kNanoLibrarySort;
  view.header.pillLabel = librarySortLabel();
  view.header.pillIcon = NanoIcon::Sort;
  view.emptyLabel = tr3(TrKey3::NanoNoLibraryItems);

  NanoSinkAdapter sink(*this);
  sink.zeroIsBack = true;
  if (count > 0) {
    const size_t selected =
        bookPickerSelectedIndex_ > 0 ? std::min(bookPickerSelectedIndex_ - 1, count - 1) : 0;
    if (!nanoShelfDragging_) {
      nanoShelfOffset_ = nano::shelfCenteredOffset(count, selected, g.viewport.w);
    }
    view.selected = selected;
    view.offset = nanoShelfOffset_;
    view.books.reserve(count);
    // Spine pictures only for the books on screen (same range paintShelf
    // draws), so a long library doesn't churn the picture cache.
    const size_t firstVisible = nano::shelfSpineIndexAt(-nanoShelfOffset_, count);
    const size_t lastVisible = nano::shelfSpineIndexAt(-nanoShelfOffset_ + g.viewport.w, count);
    for (size_t i = 0; i < count; ++i) {
      nano::ShelfBook book;
      book.title = bookMenuItems_[i + 1].title;
      book.progress = bookMenuItems_[i + 1].progressPercent;
      if (i >= firstVisible && i <= lastVisible && i < bookPickerBookIndices_.size()) {
        book.spine = BookExtras::picture(storage_.bookPath(bookPickerBookIndices_[i]), BookExtras::Picture::Spine);
      }
      view.books.push_back(book);
    }
    const size_t bookIndex = selected < bookPickerBookIndices_.size() ? bookPickerBookIndices_[selected] : 0;
    view.detailTitle = bookMenuItems_[selected + 1].title;
    const String author = storage_.bookAuthorName(bookIndex);
    view.detailAuthor = author.isEmpty() ? String(tr3(TrKey3::NanoUnknownAuthor)) : author;
    if (usingStorageBook_ && bookIndex == currentBookIndex_) {
      view.detailStatus = uiText(UiText::CurrentBook);
    }
    uint8_t percent = 0;
    const bool hasProgress = bookProgressPercent(bookIndex, percent);
    view.detailPercent = String(static_cast<unsigned>(hasProgress ? percent : 0)) + "%";
  }
  nano::paintShelf(display_, sink, view);
}

// ─── Rozdzialy: chapter wheel ───────────────────────────────────────────────

void App::renderNanoChapters() {
  const size_t count = chapterMarkers_.size();
  if (nanoWheelCentered_ >= count && count > 0) {
    nanoWheelCentered_ = count - 1;
  }
  nano::WheelView view;
  view.header.backId = 0;
  view.header.title = uiText(UiText::Chapters);
  view.header.trailing = String(static_cast<unsigned>(count == 0 ? 0 : nanoWheelCentered_ + 1)) + " / " +
                         String(static_cast<unsigned>(count));
  view.count = count;
  view.centered = nanoWheelCentered_;
  view.offset = nanoWheelOffset_;
  view.readingIndex = currentChapterIndex();
  view.emptyId = static_cast<int>(kChapterPickerFallbackIndex);
  view.emptyLabel = uiText(UiText::StartOfBook);
  view.firstIndex = nanoWheelCentered_ > 4 ? nanoWheelCentered_ - 4 : 0;
  const size_t last = std::min(count, nanoWheelCentered_ + 5);
  for (size_t i = view.firstIndex; i < last; ++i) {
    view.titles.push_back(chapterMarkers_[i].title.isEmpty()
                              ? uiText(UiText::Chapters) + " " + String(static_cast<unsigned>(i + 1))
                              : chapterMarkers_[i].title);
  }
  NanoSinkAdapter sink(*this);
  sink.zeroIsBack = true;
  nano::paintWheel(display_, sink, view);
}

// ─── Punkty zapisu ──────────────────────────────────────────────────────────

void App::renderNanoSavePoints() {
  // Row 0 is "+ Dodaj punkt zapisu" (index 1), then one row per save point
  // (name at 2+2k, its delete at 3+2k — see openSavePointsList()).
  const size_t pointCount = savePointMenuItems_.size() > 2 ? (savePointMenuItems_.size() - 2) / 2 : 0;
  const size_t rowCount = 1 + pointCount;
  const size_t rows = static_cast<size_t>(kNanoListRows);
  const size_t pageCount = std::max<size_t>(1, (rowCount + rows - 1) / rows);
  const size_t selectedRow = savePointSelectedIndex_ <= 1 ? 0 : 1 + (savePointSelectedIndex_ - 2) / 2;
  const size_t page = std::min(selectedRow / rows, pageCount - 1);
  nanoPage_ = page;
  for (size_t p = 0; p < pageCount; ++p) {
    const size_t row = p * rows;
    nanoPageFirstIndex_.push_back(row == 0 ? 1 : 2 + (row - 1) * 2);
  }

  nano::ListView view;
  view.header.backId = 0;
  view.header.title = uiText(UiText::SavePoints);
  view.header.page = page;
  view.header.pageCount = pageCount;
  view.header.prevId = kNanoActionBase + kNanoPagePrev;
  view.header.nextId = kNanoActionBase + kNanoPageNext;
  view.columns = 1;
  view.rows = kNanoListRows;
  const String deleteLabel = nanoStripColon(tr3(TrKey3::DeleteSpace));
  for (size_t r = 0; r < rows; ++r) {
    const size_t row = page * rows + r;
    if (row >= rowCount) {
      break;
    }
    nano::ListItem item;
    if (row == 0) {
      item.id = 1;
      item.label = savePointMenuItems_.size() > 1 ? savePointMenuItems_[1] : String();
      item.icon = NanoIcon::Plus;
      view.items.push_back(item);
      continue;
    }
    const size_t nameIndex = 2 + (row - 1) * 2;
    if (nameIndex + 1 >= savePointMenuItems_.size()) {
      break;
    }
    item.kind = nano::ListItem::Kind::Row;
    item.id = static_cast<int>(nameIndex);
    item.label = savePointMenuItems_[nameIndex];
    item.icon = NanoIcon::Bookmark;
    const size_t pointIndex = row - 1;
    if (pointIndex < savePoints_.size()) {
      item.value = String(static_cast<unsigned>(savePoints_[pointIndex].progressPercent)) + "%";
    }
    item.trailingId = static_cast<int>(nameIndex + 1);
    item.trailingLabel = deleteLabel;
    view.items.push_back(item);
  }
  NanoSinkAdapter sink(*this);
  sink.labels = &savePointMenuItems_;
  sink.zeroIsBack = true;
  nano::paintList(display_, sink, view);
}

// ─── Szczegoly ksiazki ──────────────────────────────────────────────────────

void App::renderNanoBookDetails() {
  nano::BookDetailsView view;
  view.header.backId = 0;
  view.header.title = storage_.bookDisplayName(bookDetailsBookIndex_);
  const String author = storage_.bookAuthorName(bookDetailsBookIndex_);
  view.author = author.isEmpty() ? String(tr3(TrKey3::NanoUnknownAuthor)) : author;
  uint8_t percent = 0;
  bookProgressPercent(bookDetailsBookIndex_, percent);
  view.percent = percent;
  view.percentLabel = String(static_cast<unsigned>(percent)) + "%";
  const String bookPath = storage_.bookPath(bookDetailsBookIndex_);
  view.coverColor = nanoCoverColor(bookPath);
  view.coverInitials = nanoInitials(view.header.title);
  view.cover = BookExtras::picture(bookPath, BookExtras::Picture::Cover);
  // bookDetailsMenuItems_[3..7] = read on / chapters / restart / delete /
  // go to.
  const NanoIcon icons[] = {NanoIcon::Play, NanoIcon::List, NanoIcon::Restart, NanoIcon::Trash, NanoIcon::Target};
  for (size_t i = 0; i < 5; ++i) {
    const size_t index = 3 + i;
    if (index >= bookDetailsMenuItems_.size()) {
      break;
    }
    nano::Tile tile;
    tile.id = static_cast<int>(index);
    tile.label = bookDetailsMenuItems_[index];
    tile.icon = icons[i];
    tile.accent = i == 0;
    view.actions.push_back(tile);
  }
  NanoSinkAdapter sink(*this);
  sink.labels = &bookDetailsMenuItems_;
  sink.zeroIsBack = true;
  nano::paintBookDetails(display_, sink, view);
}

// ─── Confirm dialogs ────────────────────────────────────────────────────────

void App::renderNanoConfirm(const String &title, const std::vector<String> &items, size_t headerRows) {
  (void)title;
  nano::ConfirmView view;
  const bool infoRow = menuScreen_ == MenuScreen::BookDeleteConfirm ||
                       menuScreen_ == MenuScreen::SavePointDeleteConfirm;
  // Question text: the header rows, or for the list-style confirms the info
  // row right after Back ("Usun: <tytul>").
  for (size_t i = 0; i < headerRows && i < items.size(); ++i) {
    view.question += (view.question.isEmpty() ? "" : " ") + items[i];
  }
  for (size_t i = headerRows; i < items.size(); ++i) {
    const size_t canonical = i - headerRows;
    if (canonical == 0 && items[i] == uiText(UiText::Back)) {
      view.backId = 0;
      view.backLabel = uiText(UiText::Back);
      continue;
    }
    if (infoRow && canonical == 1) {
      view.question = items[i];
      continue;
    }
    nano::ConfirmView::Action action;
    action.id = static_cast<int>(canonical);
    action.label = items[i];
    if (menuScreen_ == MenuScreen::PresetsDeleteConfirm) {
      String name;
      String value;
      if (nanoSplitSetting(items[i], name, value)) {
        action.label = name;
        view.question = value;
      }
    }
    action.danger = nanoIsDeleteLabel(action.label) ||
                    (infoRow && action.label == uiText(UiText::On));
    view.actions.push_back(action);
  }
  NanoSinkAdapter sink(*this);
  sink.labels = &items;
  sink.labelOffset = headerRows;
  sink.zeroIsBack = view.backId == 0;
  nano::paintConfirm(display_, sink, view);
}

// ─── Generic list (every other menu screen) ─────────────────────────────────

void App::renderNanoList(const String &title, const std::vector<String> &items, size_t headerRows,
                         const std::vector<String> &subtitles) {
  if (menuScreen_ == MenuScreen::SettingsDisplay) {
    nanoScreensaverFromSettingsHome_ = false;
  }
  const bool rail = nanoRailScreen();
  size_t itemCount = 0;
  const size_t *selectedPtr = currentMenuSelectedIndexPtr(itemCount);
  const size_t selected = selectedPtr != nullptr ? *selectedPtr : 0;

  const size_t actionable = items.size() > headerRows ? items.size() - headerRows : 0;
  const bool hasBack = actionable > 0 && items[headerRows] == uiText(UiText::Back);
  const size_t firstTile = hasBack ? 1 : 0;
  // Rows this screen shows in the Nano UI (some settings live on another
  // tab here, see nanoHiddenRow()).
  std::vector<size_t> visible;
  visible.reserve(actionable);
  for (size_t i = firstTile; i < actionable; ++i) {
    if (!nanoHiddenRow(i)) {
      visible.push_back(i);
    }
  }
  const size_t tileCount = visible.size();

  // Two columns of setting rows when the items carry values ("Motyw:
  // Ciemny"), three columns of plain buttons otherwise (two next to the rail).
  bool settingsLike = !subtitles.empty();
  for (size_t i = firstTile; i < actionable && !settingsLike; ++i) {
    settingsLike = items[headerRows + i].indexOf(": ") > 0;
  }
  const bool detailScreen = menuScreen_ == MenuScreen::PluginDetail;
  const int columns = detailScreen ? 1 : (settingsLike || rail) ? 2 : 3;
  const size_t perPage = static_cast<size_t>(columns * kNanoListRows);
  const size_t pageCount = std::max<size_t>(1, (tileCount + perPage - 1) / perPage);
  size_t selectedTile = 0;
  for (size_t v = 0; v < visible.size(); ++v) {
    if (visible[v] <= selected) {
      selectedTile = v;
    }
  }
  // Paging the font picker must not move its selection (that is the face
  // in use); its page is kept on its own.
  const bool ownPage = nanoListPageScreen_ == menuScreen_ && nanoListPage_ >= 0;
  const size_t page = std::min(ownPage ? static_cast<size_t>(nanoListPage_) : selectedTile / perPage, pageCount - 1);
  nanoPage_ = page;
  for (size_t p = 0; p < pageCount; ++p) {
    nanoPageFirstIndex_.push_back(p * perPage < visible.size() ? visible[p * perPage] : firstTile);
  }

  nano::ListView view;
  view.header.backId = hasBack ? 0 : nano::kNoTarget;
  view.header.title = title.isEmpty() ? nanoScreenTitle() : title;
  view.header.page = page;
  view.header.pageCount = pageCount;
  view.header.prevId = kNanoActionBase + kNanoPagePrev;
  view.header.nextId = kNanoActionBase + kNanoPageNext;
  view.columns = columns;
  view.rows = kNanoListRows;
  view.fullScreen = !rail;

  const size_t pageStart = page * perPage;
  for (size_t i = 0; i < perPage && pageStart + i < tileCount; ++i) {
    const size_t canonical = visible[pageStart + i];
    const String &text = items[headerRows + canonical];
    const int index = static_cast<int>(canonical);
    nano::ListItem item;
    item.id = index;
    item.label = text;
    item.helpId = helpActionFor(menuScreen_, index);

    if (text == "---") {
      item.kind = nano::ListItem::Kind::Separator;
      item.label = "";
      view.items.push_back(item);
      continue;
    }

    // Reuse the Buttons-grid annotations for what each row is.
    DisplayManager::Button info;
    info.label = text;
    if (menuScreen_ == MenuScreen::SettingsDisplay) {
      annotateSettingsDisplayButton(info, canonical);
    } else if (menuScreen_ == MenuScreen::PluginDetail) {
      annotatePluginDetailButton(info, canonical);
    } else if (menuScreen_ == MenuScreen::TypographyFontPicker) {
      annotateTypographyFontPickerButton(info, canonical);
    }

    if (info.kind == DisplayManager::Button::ButtonKind::Label) {
      // Plugin description: plain muted prose, two rows tall.
      item.kind = nano::ListItem::Kind::Label;
      item.label = info.sublabel.isEmpty() ? info.label : info.label + " " + info.sublabel;
      view.items.push_back(item);
      continue;
    }
    NanoSliderSpec slider;
    String sliderLabel;
    String sliderValue;
    if (nanoSliderSpec(canonical, slider) && nanoSplitSetting(text, sliderLabel, sliderValue)) {
      item.kind = nano::ListItem::Kind::Slider;
      item.label = sliderLabel;
      item.value = sliderValue;
      item.sliderValue = slider.value;
      item.sliderMin = slider.minimum;
      item.sliderMax = slider.maximum;
      item.dragging = nanoSliderDragging_ && nanoSliderIndex_ == index;
      view.items.push_back(item);
      continue;
    }
    if (info.kind == DisplayManager::Button::ButtonKind::Toggle) {
      item.kind = nano::ListItem::Kind::Toggle;
      item.label = info.label;
      item.on = info.active;
    } else if (canonical < subtitles.size()) {
      item.kind = nano::ListItem::Kind::Setting;
      item.value = subtitles[canonical];
    } else {
      String label;
      String value;
      if (nanoSplitSetting(text, label, value) && !nanoArmed(index)) {
        item.kind = nano::ListItem::Kind::Setting;
        item.label = label;
        item.value = value;
      } else {
        item.kind = nano::ListItem::Kind::Button;
        item.typeface = info.previewTypeface;
        // Font picker: the selection always equals the active face (set on
        // open, moved by the tap that applies a face), so mark it.
        item.marked = menuScreen_ == MenuScreen::TypographyFontPicker &&
                      canonical < typographyFontPickerTypefaceForIndex_.size() &&
                      typographyFontPickerTypefaceForIndex_[canonical] == typographyConfig_.typeface;
        if (nanoIsDeleteLabel(text)) {
          item.icon = NanoIcon::Trash;
        }
      }
    }
    view.items.push_back(item);
  }

  NanoSinkAdapter sink(*this);
  sink.labels = &items;
  sink.labelOffset = headerRows;
  sink.zeroIsBack = hasBack;
  nano::paintList(display_, sink, view);
}

bool App::nanoHiddenRow(size_t canonical) const {
  if (menuScreen_ == MenuScreen::SettingsDisplay) {
    // Reading theme and letter color moved to Motywy > Czytanie (one place
    // for everything that changes how things look).
    if (canonical == kSettingsDisplayThemeIndex || canonical == kSettingsDisplayFocusColorIndex) {
      return true;
    }
    // The legacy navigation modes are an advanced-mode option.
    if (canonical == kSettingsDisplayNavModeIndex && !const_cast<App *>(this)->devModeEnabled()) {
      return true;
    }
  }
  return false;
}

// ─── Actions ────────────────────────────────────────────────────────────────

bool App::nanoChangePage(int delta, bool fromSwipe) {
  if (nanoPageFirstIndex_.size() <= 1) {
    return false;
  }
  const int target = std::max(0, std::min(static_cast<int>(nanoPageFirstIndex_.size()) - 1,
                                          static_cast<int>(nanoPage_) + delta));
  if (static_cast<size_t>(target) != nanoPage_ && menuScreen_ == MenuScreen::TypographyFontPicker) {
    nanoListPageScreen_ = menuScreen_;
    nanoListPage_ = target;
    if (fromSwipe) {
      lastGridPageChangeAtMs_ = millis();
    }
    renderMenu();
    return true;
  }
  if (static_cast<size_t>(target) != nanoPage_) {
    size_t itemCount = 0;
    size_t *selected = currentMenuSelectedIndexPtr(itemCount);
    if (selected != nullptr) {
      *selected = nanoPageFirstIndex_[static_cast<size_t>(target)];
      if (fromSwipe) {
        // Same settle-time guard as handleGridPageSwipe().
        lastGridPageChangeAtMs_ = millis();
      }
      renderMenu();
    }
  }
  return true;
}

void App::runNanoAction(int action, uint32_t nowMs) {
  if (action >= kExtraActionBase) {
    runExtraAction(action, nowMs);
    return;
  }
  if (action == kNanoTypoBack) {
    nanoTypographyBack(nowMs);
    return;
  }
  if (action >= kNanoTypoSection && action < kNanoTypoSection + nano::TypographyView::kSections) {
    nanoTypographySection_ = static_cast<uint8_t>(action - kNanoTypoSection);
    renderMenu();
    return;
  }
  if (action >= kNanoThemeLayoutType) {
    // Same type again = the rail goes to the other side; the other type
    // keeps the current side.
    const bool compact = action - kNanoThemeLayoutType == 0;
    const bool wasCompact = nanoLayout_ == kNanoLayoutCompact || nanoLayout_ == kNanoLayoutCompactRight;
    bool right = nanoLayout_ == kNanoLayoutRight || nanoLayout_ == kNanoLayoutCompactRight;
    if (compact == wasCompact) {
      right = !right;
    }
    const uint8_t layout = compact ? (right ? kNanoLayoutCompactRight : kNanoLayoutCompact)
                                   : (right ? kNanoLayoutRight : kNanoLayoutLeft);
    setNanoTheme(nanoPalette_, nanoOwnAccent_, layout);
    return;
  }
  if (action >= kNanoThemeLetterColor) {
    openFocusColorPicker(nowMs);
    return;
  }
  if (action >= kNanoThemeReading) {
    setReaderTheme(static_cast<uint8_t>(action - kNanoThemeReading), nowMs);
    return;
  }
  if (action >= kNanoThemeFont) {
    const int family = action - kNanoThemeFont - 1;
    setNanoUiFontChoice(family < 0 ? kNanoUiFontFollowReader : static_cast<uint8_t>(family));
    return;
  }
  if (action >= kNanoThemeSection) {
    nanoThemeSection_ = static_cast<uint8_t>(std::min(nano::ThemesView::kSections - 1, action - kNanoThemeSection));
    renderSettings();
    return;
  }
  if (action >= kNanoThemeLayout) {
    setNanoTheme(nanoPalette_, nanoOwnAccent_, static_cast<uint8_t>(action - kNanoThemeLayout));
    return;
  }
  if (action >= kNanoThemePalette) {
    setNanoTheme(static_cast<uint8_t>(action - kNanoThemePalette), nanoOwnAccent_, nanoLayout_);
    return;
  }
  if (action >= kNanoLaunchPlugin) {
    const auto enabled = pluginLibrary_.enabledEntries();
    const size_t index = static_cast<size_t>(action - kNanoLaunchPlugin);
    if (index < enabled.size()) {
      pluginsActiveSelectedIndex_ = index + 1;
      selectPluginsActiveItem(nowMs);
    }
    return;
  }
  switch (action) {
    case kNanoTabRead:
      menuScreen_ = MenuScreen::Main;
      renderMainMenu();
      return;
    case kNanoTabSettings:
      openSettings();
      return;
    case kNanoTabDevice:
      openDeviceHome();
      return;
    case kNanoTabThemes:
      openNanoThemes();
      return;
    case kNanoThemeOwnAccent:
      setNanoTheme(nanoPalette_, !nanoOwnAccent_, nanoLayout_);
      return;
    case kNanoTabPlugins:
      openPluginsHome();
      return;
    case kNanoPowerOff:
      enterPowerOff(nowMs);
      return;
    case kNanoPagePrev:
      nanoChangePage(-1, false);
      return;
    case kNanoPageNext:
      nanoChangePage(1, false);
      return;
    case kNanoReadResume:
      if (!usingStorageBook_ && storage_.bookCount() > 0) {
        openBookPicker(false);
      } else {
        // The reader panel (Paused in this mode): word, speed, Start.
        menuScreen_ = MenuScreen::Main;
        setState(AppState::Paused, nowMs);
      }
      return;
    case kNanoReadChapters:
      // Back from the wheel lands on Czytaj, not on a stale book-details page.
      bookDetailsMenuItems_.clear();
      nanoChaptersFromPanel_ = false;
      openChapterPicker();
      return;
    case kNanoReadSavePoints:
      openSavePointsList();
      return;
    case kNanoReadLibrary:
      openBookPicker(false);
      return;
    case kNanoReadFonts:
      // "Aa" on the Czytaj card: only the reading typeface list. Everything
      // else about the reading screen stays under Ustawienia > Wyglad
      // czytania. Back returns to Czytaj (selectTypographyFontPickerItem).
      nanoFontPickerFromRead_ = true;
      openTypographyFontPicker();
      return;
    case kNanoSettingsScreensaver:
      nanoScreensaverFromSettingsHome_ = true;
      openScreensaverSettings();
      return;
    case kNanoPluginLibrary:
      openPluginLibraryScreen();
      return;
    case kNanoLibrarySort:
      librarySort_ = static_cast<uint8_t>((librarySort_ + 1) % kLibrarySortCount);
      preferences_.putUChar(kPrefLibrarySort, librarySort_);
      Serial.printf("[library] sort=%u\n", static_cast<unsigned>(librarySort_));
      openBookPicker(false);
      return;
    default:
      return;
  }
}

void App::returnFromPlugin() {
  if (navMode_ == NavMode::Modern) {
    openPluginsHome();
  } else {
    openPluginsActive();
  }
}

// ─── Live-drag screens ──────────────────────────────────────────────────────

bool App::handleNanoTouch(const TouchEvent &event, uint32_t nowMs) {
  if (menuScreen_ == MenuScreen::GoToPosition) {
    return handleGoToTouch(event, nowMs);
  }
  if (menuScreen_ == MenuScreen::HelpPage) {
    return handleHelpTouch(event, nowMs);
  }
  if (handleNanoSliderTouch(event, nowMs)) {
    return true;
  }
  if (menuScreen_ == MenuScreen::BookPicker) {
    const nano::ShelfGeometry g = nano::shelfGeometry();
    const size_t count = bookMenuItems_.size() > 1 ? bookMenuItems_.size() - 1 : 0;
    if (event.phase == TouchPhase::Start) {
      nanoShelfDragging_ = count > 0 && g.viewport.contains(event.x, event.y);
      nanoShelfDetailTouch_ = !nanoShelfDragging_ && count > 0 && g.detail.contains(event.x, event.y);
      if (!nanoShelfDragging_ && !nanoShelfDetailTouch_) {
        return false;
      }
      nanoShelfMoved_ = false;
      nanoShelfDragStartX_ = event.x;
      nanoShelfDragStartY_ = event.y;
      nanoShelfDragStartOffset_ = nanoShelfOffset_;
      pausedTouch_.active = false;
      return true;
    }
    const bool trackingShelf = nanoShelfDragging_;
    const bool trackingDetail = nanoShelfDetailTouch_;
    if (!trackingShelf && !trackingDetail) {
      return false;
    }
    const int dx = static_cast<int>(event.x) - nanoShelfDragStartX_;
    const int dy = static_cast<int>(event.y) - nanoShelfDragStartY_;
    nanoShelfMoved_ = nanoShelfMoved_ || std::abs(dx) > nano::kShelfDragThreshold ||
                      std::abs(dy) > nano::kShelfDragThreshold;
    if (trackingShelf && nanoShelfMoved_) {
      nanoShelfOffset_ = nano::shelfClampOffset(count, nanoShelfDragStartOffset_ + dx, g.viewport.w);
      bookPickerSelectedIndex_ = nano::shelfNearest(count, nanoShelfOffset_, g.marker, g.viewport.x) + 1;
    }
    if (event.phase == TouchPhase::Move) {
      if (trackingShelf && nanoShelfMoved_ && nowMs - nanoShelfLastDragRenderMs_ >= kNanoDragFrameMs) {
        nanoShelfLastDragRenderMs_ = nowMs;
        renderBookPicker();
      }
      return true;
    }
    // End.
    nanoShelfDragging_ = false;
    nanoShelfDetailTouch_ = false;
    if (nanoShelfMoved_) {
      renderBookPicker();  // snaps to the selected spine
      return true;
    }
    if (trackingDetail) {
      if (!g.detail.contains(event.x, event.y)) {
        return true;
      }
      // Tap on the title strip: the book's detail page (chapters, restart,
      // delete).
      if (bookPickerSelectedIndex_ >= 1 && bookPickerSelectedIndex_ - 1 < bookPickerBookIndices_.size()) {
        openBookDetails(bookPickerBookIndices_[bookPickerSelectedIndex_ - 1], nowMs);
      }
      return true;
    }
    // Tap on the shelf: pick the spine under the finger (or keep the
    // selected one) and open that book on the reader panel.
    if (lastMenuActionAtMs_ != 0 && nowMs - lastMenuActionAtMs_ < kMenuActionDebounceMs) {
      return true;
    }
    const int32_t contentX = static_cast<int32_t>(event.x) - g.viewport.x - nanoShelfOffset_;
    const size_t tapped = nano::shelfSpineIndexAt(contentX, count);
    const int spineX = g.viewport.x + static_cast<int>(nano::shelfSpineLeft(tapped) + nanoShelfOffset_);
    if (static_cast<int>(event.x) >= spineX && static_cast<int>(event.x) < spineX + nano::shelfSpineWidth(tapped)) {
      if (tapped + 1 != bookPickerSelectedIndex_) {
        // First tap on another spine only brings it forward.
        bookPickerSelectedIndex_ = tapped + 1;
        lastMenuActionAtMs_ = nowMs;
        renderBookPicker();
        return true;
      }
    }
    lastMenuActionAtMs_ = nowMs;
    const size_t row = bookPickerSelectedIndex_ > 0 ? bookPickerSelectedIndex_ - 1 : 0;
    if (row >= bookPickerBookIndices_.size()) {
      return true;
    }
    const size_t bookIndex = bookPickerBookIndices_[row];
    if (!(usingStorageBook_ && bookIndex == currentBookIndex_)) {
      saveReadingPosition(true);
      if (!loadBookAtIndex(bookIndex, nowMs, true, true, true, true)) {
        display_.renderStatus(tr3(TrKey3::ErrorLabel), storage_.bookDisplayName(bookIndex), "");
        delay(1400);
        renderBookPicker();
        return true;
      }
    }
    bookDetailsMenuItems_.clear();
    menuScreen_ = MenuScreen::Main;
    setState(AppState::Paused, nowMs);
    return true;
  }

  if (menuScreen_ == MenuScreen::ChapterPicker && !chapterMarkers_.empty()) {
    const ui::Rect viewport = nano::wheelViewport();
    const size_t count = chapterMarkers_.size();
    if (event.phase == TouchPhase::Start) {
      if (!viewport.contains(event.x, event.y)) {
        return false;
      }
      nanoWheelDragging_ = true;
      nanoWheelMoved_ = false;
      nanoWheelDragStartIndex_ = nanoWheelCentered_;
      nanoWheelDragStartY_ = event.y;
      pausedTouch_.active = false;
      return true;
    }
    if (!nanoWheelDragging_) {
      return false;
    }
    const int delta = static_cast<int>(event.y) - nanoWheelDragStartY_;
    nanoWheelMoved_ = nanoWheelMoved_ || std::abs(delta) > nano::kWheelDragThreshold;
    if (nanoWheelMoved_) {
      // Displacement owns the selection: holding still never advances.
      const int64_t position = std::max<int64_t>(
          0, std::min<int64_t>(static_cast<int64_t>(nanoWheelDragStartIndex_) * nano::kWheelRowStep - delta,
                               static_cast<int64_t>(count - 1) * nano::kWheelRowStep));
      nanoWheelCentered_ = static_cast<size_t>((position + nano::kWheelRowStep / 2) / nano::kWheelRowStep);
      nanoWheelOffset_ =
          static_cast<int16_t>(static_cast<int64_t>(nanoWheelCentered_) * nano::kWheelRowStep - position);
    }
    if (event.phase == TouchPhase::Move) {
      if (nanoWheelMoved_ && nowMs - nanoWheelLastDragRenderMs_ >= kNanoDragFrameMs) {
        nanoWheelLastDragRenderMs_ = nowMs;
        renderChapterPicker();
      }
      return true;
    }
    nanoWheelDragging_ = false;
    if (!nanoWheelMoved_ && viewport.contains(event.x, event.y)) {
      const size_t first = nanoWheelCentered_ > 4 ? nanoWheelCentered_ - 4 : 0;
      const size_t last = std::min(count, nanoWheelCentered_ + 5);
      size_t tappedIndex = count;
      int closest = nano::kWheelRowStep / 2 + 1;
      for (size_t i = first; i < last; ++i) {
        const int y = nano::wheelRowCenter(viewport, static_cast<int>(i) - static_cast<int>(nanoWheelCentered_),
                                           nanoWheelOffset_);
        if (!nano::wheelRowVisible(viewport, y, nano::wheelRowHeight(i == nanoWheelCentered_))) {
          continue;
        }
        const int distance = std::abs(y - static_cast<int>(event.y));
        if (distance < closest) {
          closest = distance;
          tappedIndex = i;
        }
      }
      if (tappedIndex != count) {
        if (tappedIndex != nanoWheelCentered_) {
          // A row off the center scrolls it in; tapping the centered row
          // jumps there.
          nanoWheelCentered_ = tappedIndex;
          nanoWheelOffset_ = 0;
          renderChapterPicker();
          return true;
        }
        nanoWheelOffset_ = 0;
        chapterPickerSelectedIndex_ = tappedIndex + 1;
        selectChapterPickerItem(nowMs);
        return true;
      }
    }
    nanoWheelOffset_ = 0;  // snap to the highlighted chapter
    renderChapterPicker();
    return true;
  }
  return false;
}

// ─── Reader panel (Paused, Modern mode) ─────────────────────────────────────
// rsvpnano's reader screen before Start: the current word with its
// neighbours, chapter / progress / time left on top, and a bottom bar with
// Menu, Chapters, a save-point ribbon, the speed stepper and Start. Start
// (or hold, or a double tap) plays on the classic reading screen; pausing
// brings the panel back.

bool App::nanoReaderPanelActive() const {
  return navMode_ == NavMode::Modern && state_ == AppState::Paused && !pendingBootBookLoad_ &&
         !chapterTransitionVisible_ && !nanoPanelScrubbing_;
}

void App::renderNanoReaderPanel() {
  const SlowStepLog slowLog("renderNanoReaderPanel", 60);
  applyReaderUiOrientation();
  display_.setModernCardStyle(true);
  contextViewVisible_ = false;
  wpmFeedbackVisible_ = false;

  nano::ReaderPanelView view;
  view.chapter = currentChapterLabel();
  view.progressPercent = readingProgressPercent();
  view.progressLabel = String(static_cast<unsigned>(view.progressPercent)) + "%";
  const size_t wordCount = reader_.wordCount();
  if (wordCount > 0) {
    const size_t current = std::min(reader_.currentIndex(), wordCount - 1);
    view.timeLeft = formatReadingTimeRemaining(estimatedReadingTimeRemainingMs(current, wordCount));
    if (scrollModeEnabled()) {
      // Scroll reading: show the page around the word, not an RSVP line.
      updateContextPreviewWindow(current);
      view.scrollMode = true;
      view.words = contextPreviewWords_;
      view.currentLocal = current >= contextPreviewStartIndex_ ? current - contextPreviewStartIndex_ : 0;
    }
  }
  view.before = phantomBeforeText();
  view.word = reader_.currentWord();
  view.after = phantomAfterText();
  view.menuId = kPanelMenu;
  view.menuLabel = tr3(TrKey3::NanoMenuLabel);
  view.chaptersId = kPanelChapters;
  view.bookmarkId = kPanelBookmark;
  view.bookmarkFilled = isCurrentPositionSaved();
  view.rewindId = kPanelRewind;
  view.lookId = kPanelLook;
  view.gotoId = usingStorageBook_ ? kPanelGoTo : nano::kNoTarget;
  view.statusId = usingStorageBook_ ? kPanelGoTo : nano::kNoTarget;
  view.minusId = kPanelWpmMinus;
  view.plusId = kPanelWpmPlus;
  view.wpmLabel = String(static_cast<unsigned>(reader_.wpm())) + " " + tr3(TrKey3::NanoWpmUnit);
  view.startId = kPanelStart;
  view.startLabel = uiText(UiText::Read);
  view.hint = view.scrollMode ? String() : String(tr3(TrKey3::NanoPanelHint));

  // The panel is the doorway to the reading screen, so it wears the
  // reading colors (Motyw czytania + kolor litery), not the menu palette.
  const uint8_t menuPalette = display_.nanoPalette();
  const bool menuOwnAccent = display_.nanoOwnAccent();
  display_.overrideNanoPalette(DisplayManager::kNanoPaletteClassic, false);
  nanoPanelTargets_.clear();
  NanoPanelSink sink(nanoPanelTargets_, nanoPanelPressedAction_);
  display_.nanoBeginFrame();
  nano::paintReaderPanel(display_, sink, view);
  display_.nanoEndFrame();
  display_.overrideNanoPalette(menuPalette, menuOwnAccent);
}

bool App::handleNanoReaderPanelTouch(const TouchEvent &event, uint32_t nowMs) {
  auto targetAt = [this](uint16_t x, uint16_t y) {
    for (const auto &target : nanoPanelTargets_) {
      if (target.first.contains(x, y)) {
        return target.second;
      }
    }
    return -1;
  };
  if (event.phase == TouchPhase::Start) {
    nanoPanelBarTouch_ = nano::readerPanelBar().contains(event.x, event.y) ||
                         (usingStorageBook_ && nano::readerPanelStatusArea().contains(event.x, event.y));
    if (!nanoPanelBarTouch_) {
      return false;
    }
    nanoPanelTouchAction_ = targetAt(event.x, event.y);
    resetReaderTapTracking();
    pausedTouch_.active = false;
    if (nanoPanelTouchAction_ >= 0) {
      nanoPanelPressedAction_ = nanoPanelTouchAction_;
      renderNanoReaderPanel();
    }
    return true;
  }
  if (!nanoPanelBarTouch_) {
    return false;
  }
  if (event.phase == TouchPhase::Move) {
    return true;
  }
  // End: fire when the finger lifts on the button it went down on.
  nanoPanelBarTouch_ = false;
  const int action = nanoPanelTouchAction_;
  nanoPanelTouchAction_ = -1;
  nanoPanelPressedAction_ = -1;
  if (action >= 0 && targetAt(event.x, event.y) == action) {
    runNanoReaderPanelAction(action, nowMs);
  } else if (action >= 0) {
    renderNanoReaderPanel();
  }
  return true;
}

void App::runNanoReaderPanelAction(int action, uint32_t nowMs) {
  switch (action) {
    case kPanelMenu:
      openMainMenu(nowMs);
      return;
    case kPanelChapters:
      bookDetailsMenuItems_.clear();
      nanoChaptersFromPanel_ = true;
      openChapterPicker();  // builds the list and sets menuScreen_
      setState(AppState::Menu, nowMs);
      return;
    case kPanelBookmark:
      quickSavePointFromReader(nowMs);
      return;
    case kPanelWpmMinus:
    case kPanelWpmPlus:
      reader_.adjustWpm(action == kPanelWpmPlus ? 1 : -1);
      preferences_.putUShort(kPrefWpm, reader_.wpm());
      Serial.printf("[app] WPM=%u (panel)\n", reader_.wpm());
      renderNanoReaderPanel();
      return;
    case kPanelRewind:
      reader_.rewindSentence();
      invalidateContextPreviewWindow();
      saveReadingPosition(true);
      Serial.printf("[app] sentence rewind (panel) index=%u\n", static_cast<unsigned>(reader_.currentIndex()));
      renderNanoReaderPanel();
      return;
    case kPanelGoTo:
      openGoToPosition(false, nowMs);
      return;
    case kPanelLook:
      // Reading colors one tap from the page.
      openNanoTypography(0, kNanoTypographyFromPanel, nowMs);
      return;
    case kPanelStart:
      playLocked_ = true;
      pauseAtSentenceEndRequested_ = false;
      wpmFeedbackVisible_ = false;
      setState(AppState::Playing, nowMs);
      return;
    default:
      return;
  }
}

void App::quickSavePointFromReader(uint32_t nowMs) {
  resetReaderTapTracking();
  if (state_ == AppState::Playing) {
    setState(AppState::Paused, nowMs);
  }
  saveReadingPosition(true);
  savePointQuickSaveFromReader_ = true;
  menuScreen_ = MenuScreen::Main;
  beginSavePointNaming(nowMs);
}

// ─── Slider tiles ───────────────────────────────────────────────────────────

bool App::nanoSliderSpec(size_t index, NanoSliderSpec &spec) const {
  auto set = [&spec](int minimum, int maximum, int step, int value) {
    spec.minimum = minimum;
    spec.maximum = maximum;
    spec.step = std::max(1, step);
    spec.value = value;
    return true;
  };
  switch (menuScreen_) {
    case MenuScreen::SettingsDisplay:
      if (index == kSettingsDisplayBrightnessIndex) {
        return set(kBrightnessMinPercent, 100, 1, brightnessPercentSetting_);
      }
      return false;
    case MenuScreen::SettingsPacing:
      if (readerMode_ == ReaderMode::Scroll) {
        switch (index) {
          case kSettingsPacingScrollFontSizeIndex:
            return set(0, 8, 1, scrollFontSize_);
          case kSettingsPacingScrollLineSpacingIndex:
            return set(0, 2, 1, scrollLineSpacing_);
          case kSettingsPacingScrollMarginIndex:
            return set(0, 2, 1, scrollMargin_);
          default:
            return false;
        }
      }
      switch (index) {
        case kSettingsPacingWpmIndex:
          return set(kSettingsWpmMin, kSettingsWpmMax, kWpmSliderStepWpm, reader_.wpm());
        case kSettingsPacingLongWordsIndex:
          return set(kPacingDelayMinMs, kPacingDelayMaxMs, kPacingDelayStepMs, pacingLongWordDelayMs_);
        case kSettingsPacingComplexityIndex:
          return set(kPacingDelayMinMs, kPacingDelayMaxMs, kPacingDelayStepMs, pacingComplexWordDelayMs_);
        case kSettingsPacingPunctuationIndex:
          return set(kPacingDelayMinMs, kPacingDelayMaxMs, kPacingDelayStepMs, pacingPunctuationDelayMs_);
        default:
          return false;
      }
    case MenuScreen::TypographyTuning:
      switch (index) {
        case TypographyTuningFontSize:
          // Small on the left, large on the right (readerFontSizeIndex_ is
          // 0 = large), same flip as the typography value editor.
          return set(0, kReaderFontSizeCount - 1, 1, kReaderFontSizeCount - 1 - readerFontSizeIndex_);
        case TypographyTuningTracking:
          return set(kTypographyTrackingMin, kTypographyTrackingMax, 1, typographyConfig_.trackingPx);
        case TypographyTuningAnchor: {
          const bool left = handednessMode_ == HandednessMode::Left;
          return set(left ? kLeftHandAnchorMin : kTypographyAnchorMin, left ? kLeftHandAnchorMax : kTypographyAnchorMax, 1,
                     effectiveAnchorPercent());
        }
        case TypographyTuningGuideWidth:
          return set(kTypographyGuideWidthMin, kTypographyGuideWidthMax, kTypographyGuideWidthStep,
                     typographyConfig_.guideHalfWidth);
        case TypographyTuningGuideGap:
          return set(kTypographyGuideGapMin, kTypographyGuideGapMax, 1, typographyConfig_.guideGap);
        default:
          return false;
      }
    case MenuScreen::ScreensaverSettings:
      switch (index) {
        case kScreensaverSettingsTimeoutIndex:
          return set(0, kScreensaverTimeoutCount - 1, 1, screensaverTimeoutIndex_);
        case kScreensaverSettingsAutoOffIndex:
          return set(0, kScreensaverAutoOffCount - 1, 1, screensaverAutoOffIndex_);
        case kScreensaverSettingsSleepGuardIndex:
          return set(0, kScreensaverSleepGuardCount - 1, 1, screensaverSleepGuardIndex_);
        default:
          return false;
      }
    default:
      return false;
  }
}

void App::nanoSliderSet(size_t index, int value) {
  switch (menuScreen_) {
    case MenuScreen::SettingsDisplay:
      if (index == kSettingsDisplayBrightnessIndex) {
        // Live: the backlight follows the finger.
        setBrightnessSetting(static_cast<uint8_t>(value), false);
      }
      return;
    case MenuScreen::SettingsPacing:
      if (readerMode_ == ReaderMode::Scroll) {
        if (index == kSettingsPacingScrollFontSizeIndex) scrollFontSize_ = static_cast<uint8_t>(value);
        if (index == kSettingsPacingScrollLineSpacingIndex) scrollLineSpacing_ = static_cast<uint8_t>(value);
        if (index == kSettingsPacingScrollMarginIndex) scrollMargin_ = static_cast<uint8_t>(value);
        return;
      }
      if (index == kSettingsPacingWpmIndex) reader_.setWpm(static_cast<uint16_t>(value));
      if (index == kSettingsPacingLongWordsIndex) pacingLongWordDelayMs_ = static_cast<uint16_t>(value);
      if (index == kSettingsPacingComplexityIndex) pacingComplexWordDelayMs_ = static_cast<uint16_t>(value);
      if (index == kSettingsPacingPunctuationIndex) pacingPunctuationDelayMs_ = static_cast<uint16_t>(value);
      return;
    case MenuScreen::TypographyTuning:
      // Live: the sample above the controls follows the finger.
      if (index == TypographyTuningFontSize) {
        readerFontSizeIndex_ = static_cast<uint8_t>(kReaderFontSizeCount - 1 - value);
        return;
      }
      if (index == TypographyTuningTracking) typographyConfig_.trackingPx = static_cast<int8_t>(value);
      if (index == TypographyTuningAnchor) {
        typographyConfig_.anchorPercent = handednessMode_ == HandednessMode::Left
                                              ? static_cast<uint8_t>(value - kLeftHandAnchorOffset)
                                              : static_cast<uint8_t>(value);
      }
      if (index == TypographyTuningGuideWidth) typographyConfig_.guideHalfWidth = static_cast<uint8_t>(value);
      if (index == TypographyTuningGuideGap) typographyConfig_.guideGap = static_cast<uint8_t>(value);
      applyTypographySettings(millis(), false);
      return;
    case MenuScreen::ScreensaverSettings:
      if (index == kScreensaverSettingsTimeoutIndex) screensaverTimeoutIndex_ = static_cast<uint8_t>(value);
      if (index == kScreensaverSettingsAutoOffIndex) screensaverAutoOffIndex_ = static_cast<uint8_t>(value);
      if (index == kScreensaverSettingsSleepGuardIndex) screensaverSleepGuardIndex_ = static_cast<uint8_t>(value);
      return;
    default:
      return;
  }
}

void App::nanoSliderCommit(size_t index, uint32_t nowMs) {
  switch (menuScreen_) {
    case MenuScreen::SettingsDisplay:
      if (index == kSettingsDisplayBrightnessIndex) {
        setBrightnessSetting(brightnessPercentSetting_, true);
        applyDisplayPreferences(nowMs, false);
        Serial.printf("[display] brightness %u%%\n", static_cast<unsigned>(currentBrightnessPercent()));
      }
      return;
    case MenuScreen::SettingsPacing:
      if (readerMode_ == ReaderMode::Scroll) {
        preferences_.putUChar(kPrefScrollFontSize, scrollFontSize_);
        preferences_.putUChar(kPrefScrollLineSpacing, scrollLineSpacing_);
        preferences_.putUChar(kPrefScrollMargin, scrollMargin_);
        display_.setScrollFontSize(scrollFontSize_);
        display_.setScrollLineSpacing(scrollLineSpacing_);
        display_.setScrollMargin(scrollMargin_);
        return;
      }
      if (index == kSettingsPacingWpmIndex) {
        preferences_.putUShort(kPrefWpm, reader_.wpm());
        Serial.printf("[settings] WPM=%u\n", reader_.wpm());
        return;
      }
      preferences_.putUShort(kPrefPacingLongMs, pacingLongWordDelayMs_);
      preferences_.putUShort(kPrefPacingComplexMs, pacingComplexWordDelayMs_);
      preferences_.putUShort(kPrefPacingPunctuationMs, pacingPunctuationDelayMs_);
      applyPacingSettings();
      return;
    case MenuScreen::TypographyTuning:
      if (index == TypographyTuningFontSize) {
        preferences_.putUChar(kPrefReaderFontSize, readerFontSizeIndex_);
        applyDisplayPreferences(nowMs, false);
        return;
      }
      preferences_.putChar(kPrefTypographyTracking, typographyConfig_.trackingPx);
      preferences_.putUChar(kPrefTypographyAnchor, typographyConfig_.anchorPercent);
      preferences_.putUChar(kPrefTypographyGuideWidth, typographyConfig_.guideHalfWidth);
      preferences_.putUChar(kPrefTypographyGuideGap, typographyConfig_.guideGap);
      applyTypographySettings(nowMs, false);
      return;
    case MenuScreen::ScreensaverSettings:
      preferences_.putUChar(kPrefScreensaverTimeout, screensaverTimeoutIndex_);
      preferences_.putUChar(kPrefScreensaverAutoOff, screensaverAutoOffIndex_);
      preferences_.putUChar(kPrefScreensaverSleepGuard, screensaverSleepGuardIndex_);
      return;
    default:
      return;
  }
}

bool App::handleNanoSliderTouch(const TouchEvent &event, uint32_t nowMs) {
  if (event.phase == TouchPhase::Start) {
    nanoSliderIndex_ = -1;
    nanoSliderDragging_ = false;
    for (const NanoSliderTarget &target : nanoSliderTargets_) {
      NanoSliderSpec spec;
      if (target.rect.contains(event.x, event.y) && nanoSliderSpec(target.index, spec)) {
        nanoSliderIndex_ = static_cast<int>(target.index);
        nanoSliderStartValue_ = spec.value;
        nanoSliderStartX_ = event.x;
        nanoSliderStartY_ = event.y;
        nanoSliderWidth_ = std::max<uint16_t>(1, target.rect.w);
        break;
      }
    }
    // Not consumed: the generic handler still records the start, so a tap
    // stays a tap (opens the row's editor / cycles it).
    return false;
  }
  if (nanoSliderIndex_ < 0) {
    return false;
  }
  const size_t index = static_cast<size_t>(nanoSliderIndex_);
  const int dx = static_cast<int>(event.x) - nanoSliderStartX_;
  const int dy = static_cast<int>(event.y) - nanoSliderStartY_;
  if (!nanoSliderDragging_) {
    if (std::abs(dx) > kNanoSliderDragThreshold && std::abs(dx) > std::abs(dy)) {
      nanoSliderDragging_ = true;
      pausedTouch_.active = false;  // the generic tap/swipe handling sits this one out
    } else {
      if (event.phase == TouchPhase::End) {
        nanoSliderIndex_ = -1;
      }
      return false;
    }
  }

  NanoSliderSpec spec;
  if (!nanoSliderSpec(index, spec)) {
    nanoSliderIndex_ = -1;
    nanoSliderDragging_ = false;
    return true;
  }
  // Relative drag: the fill edge moves as far as the finger does, so
  // touching the tile never makes the value jump.
  const int range = spec.maximum - spec.minimum;
  int value = nanoSliderStartValue_ + static_cast<int>(static_cast<int64_t>(dx) * range / nanoSliderWidth_);
  value = std::max(spec.minimum, std::min(value, spec.maximum));
  value = spec.minimum + ((value - spec.minimum + spec.step / 2) / spec.step) * spec.step;
  value = std::max(spec.minimum, std::min(value, spec.maximum));
  if (value != spec.value) {
    nanoSliderSet(index, value);
  }

  if (event.phase == TouchPhase::Move) {
    if (nowMs - nanoSliderLastRenderMs_ >= kNanoDragFrameMs) {
      nanoSliderLastRenderMs_ = nowMs;
      nanoSliderRerender();
    }
    return true;
  }

  nanoSliderCommit(index, nowMs);
  nanoSliderIndex_ = -1;
  nanoSliderDragging_ = false;
  nanoSliderRerender();
  return true;
}

void App::nanoSliderRerender() {
  if (menuScreen_ == MenuScreen::TypographyTuning) {
    renderMenu();
    return;
  }
  rebuildSettingsMenuItems();
  renderSettings();
}

bool App::batteryChargingNow() const {
  if (!batteryPresent_) {
    return false;
  }
#if RSVP_USB_TRANSFER_ENABLED && CONFIG_TINYUSB_MSC_ENABLED && !ARDUINO_USB_MODE
  // Enumerated by a computer = on USB power. A plain wall charger doesn't
  // enumerate; that case is what updateChargeProbe() is for.
  if (tud_inited() && tud_mounted()) {
    return true;
  }
#endif
  return chargingDetected_;
}

// The board has no charger status line, so charging is read off the cell
// voltage: plugging a charger in lifts it by ~0.1 V at once, then it keeps
// climbing; unplugging drops it back. The regular battery sample runs every
// few minutes, far too slow to catch the step, so this probes on its own,
// short cadence (a reading takes ~25 ms, so only rarely while playing).
bool App::updateChargeProbe(uint32_t nowMs) {
  constexpr uint32_t kProbeMs = 6000;
  constexpr uint32_t kProbePlayingMs = 45000;
  constexpr float kStepV = 0.07f;        // plug-in / unplug step
  constexpr float kSettleDropV = 0.035f; // slow drop after unplugging a full cell
  constexpr uint32_t kClimbCheckMs = 240000;
  const uint32_t interval = state_ == AppState::Playing ? kProbePlayingMs : kProbeMs;
  if (chargeProbeLastMs_ != 0 && nowMs - chargeProbeLastMs_ < interval) {
    return false;
  }
  chargeProbeLastMs_ = nowMs;
  BoardConfig::BatteryStatus status;
  if (!BoardConfig::readBatteryStatus(status)) {
    return false;
  }
  const float v = status.voltage;
  if (!chargeProbeReady_) {
    chargeProbeReady_ = true;
    chargeFastV_ = v;
    chargeBaselineV_ = v;
    return false;
  }
  chargeFastV_ = chargeFastV_ * 0.5f + v * 0.5f;
  const bool wasCharging = chargingDetected_;
  if (!chargingDetected_) {
    if (chargeFastV_ - chargeBaselineV_ >= kStepV) {
      chargingDetected_ = true;
      chargeStartV_ = chargeBaselineV_;
      chargeStartMs_ = nowMs;
      chargePeakV_ = chargeFastV_;
      chargeDropCount_ = 0;
    } else {
      // Follow the slow discharge (and ADC drift) without chasing spikes.
      chargeBaselineV_ = chargeBaselineV_ * 0.85f + chargeFastV_ * 0.15f;
    }
  } else {
    chargePeakV_ = std::max(chargePeakV_, chargeFastV_);
    const float drop = chargePeakV_ - chargeFastV_;
    chargeDropCount_ = drop >= kSettleDropV ? static_cast<uint8_t>(chargeDropCount_ + 1) : 0;
    const bool unplugged = drop >= kStepV || chargeDropCount_ >= 3;
    // A step with no climb after it was a load change (Wi-Fi or the
    // backlight going off), not a charger.
    const bool noClimb = nowMs - chargeStartMs_ >= kClimbCheckMs && chargePeakV_ < chargeStartV_ + kStepV + 0.015f &&
                         chargeFastV_ < 4.12f;
    if (unplugged || noClimb) {
      chargingDetected_ = false;
      chargeBaselineV_ = chargeFastV_;
    }
  }
  if (wasCharging == chargingDetected_) {
    return false;
  }
  Serial.printf("[power] charging=%d (v=%.3f base=%.3f)\n", chargingDetected_ ? 1 : 0, static_cast<double>(v),
                static_cast<double>(chargeBaselineV_));
  display_.setBatteryState(batteryPresent_, batteryDisplayedPercent_, batteryChargingNow());
  return true;
}
