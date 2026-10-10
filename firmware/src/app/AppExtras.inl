// Screens added by the 2026-09-27 update: Przejdz do (jump to %, page or
// chapter), the bookmark-name choice, the help page, the letter-color
// palette. All of them are drawn with the Nano painters (ui/NanoScreens.cpp)
// in every navigation mode; their buttons are Nano actions at
// kNanoActionBase + kExtraActionBase + n, dispatched by runExtraAction().
//
// Included at the bottom of App.cpp after AppNano.inl (shares its
// anonymous-namespace helpers and NanoSinkAdapter).

namespace {

enum ExtraAction : int {
  kExtraGoToSegment = kExtraActionBase,  // + 0 percent / 1 page / 2 chapter
  kExtraGoToMinus = kExtraActionBase + 10,
  kExtraGoToPlus,
  kExtraGoToRead,
  kExtraBookmarkDefault,
  kExtraBookmarkCustom,
  kExtraColorSwatch = kExtraActionBase + 100,  // + swatch index
  kExtraHelp = kExtraActionBase + 200,         // + HelpTopic
  kExtraHelpUp = kExtraActionBase + 199,
  kExtraHelpDown = kExtraActionBase + 198,
};

constexpr size_t kGoToWordsPerPage = 250;

// Letter-color palette: 12 hues around the wheel, a light and a deep row.
constexpr size_t kLetterColorHues = 12;
constexpr size_t kLetterColorSwatchCount = kLetterColorHues * 3;
constexpr uint8_t kLetterColorHueRgb[kLetterColorHues][3] = {
    {235, 30, 40},   {250, 120, 20}, {250, 185, 20}, {245, 230, 30}, {150, 220, 30},  {30, 200, 70},
    {20, 190, 160},  {30, 180, 240}, {20, 80, 255},  {100, 60, 240}, {170, 50, 235}, {240, 50, 150},
};

uint16_t letterColorRgb565(int r, int g, int b) {
  r = std::max(0, std::min(255, r));
  g = std::max(0, std::min(255, g));
  b = std::max(0, std::min(255, b));
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

uint16_t letterColorSwatch(size_t index) {
  const size_t hue = index % kLetterColorHues;
  const size_t row = index / kLetterColorHues;
  const int r = kLetterColorHueRgb[hue][0];
  const int g = kLetterColorHueRgb[hue][1];
  const int b = kLetterColorHueRgb[hue][2];
  if (row == 1) {  // light: 45 % toward white
    return letterColorRgb565(r + (255 - r) * 45 / 100, g + (255 - g) * 45 / 100, b + (255 - b) * 45 / 100);
  }
  if (row == 2) {  // deep: 62 % brightness
    return letterColorRgb565(r * 62 / 100, g * 62 / 100, b * 62 / 100);
  }
  return letterColorRgb565(r, g, b);
}

}  // namespace

bool App::isExtraScreen() const {
  switch (menuScreen_) {
    case MenuScreen::GoToPosition:
    case MenuScreen::SavePointNameChoice:
    case MenuScreen::FocusColorPicker:
    case MenuScreen::HelpPage:
      return true;
    default:
      return false;
  }
}

bool App::renderExtraScreen() {
  if (!isExtraScreen()) {
    return false;
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
  switch (menuScreen_) {
    case MenuScreen::GoToPosition:
      renderGoToPosition();
      break;
    case MenuScreen::SavePointNameChoice:
      renderSavePointNameChoice();
      break;
    case MenuScreen::FocusColorPicker:
      renderFocusColorPicker();
      break;
    case MenuScreen::HelpPage:
      renderHelpPage();
      break;
    default:
      break;
  }
  display_.nanoEndFrame();
  return true;
}

void App::extraScreenBack(uint32_t nowMs) {
  switch (menuScreen_) {
    case MenuScreen::GoToPosition:
      if (goToFromBookDetails_ && !bookDetailsMenuItems_.empty()) {
        menuScreen_ = MenuScreen::BookDetails;
        renderBookDetails();
        return;
      }
      menuScreen_ = MenuScreen::Main;
      setState(AppState::Paused, nowMs);
      return;
    case MenuScreen::SavePointNameChoice:
      if (savePointQuickSaveFromReader_) {
        savePointQuickSaveFromReader_ = false;
        menuScreen_ = MenuScreen::Main;
        setState(AppState::Paused, nowMs);
        return;
      }
      openSavePointsList();
      return;
    case MenuScreen::FocusColorPicker:
      // Back to Wyglad czytania > Kolory, keeping where that screen came from.
      openNanoTypography(0, nanoTypographyReturn_, nowMs);
      return;
    case MenuScreen::HelpPage:
      menuScreen_ = helpReturnScreen_;
      if (isSettingsListScreen()) {
        rebuildSettingsMenuItems();
      }
      renderMenu();
      return;
    default:
      menuScreen_ = MenuScreen::Main;
      renderMenu();
      return;
  }
}

void App::runExtraAction(int action, uint32_t nowMs) {
  if (action >= kExtraHelp && action < kExtraHelp + static_cast<int>(HelpTopic::Count)) {
    openHelpPage(static_cast<HelpTopic>(action - kExtraHelp), nowMs);
    return;
  }
  if (action == kExtraHelpUp || action == kExtraHelpDown) {
    const int page = nano::helpBodyRect().h - DisplayManager::nanoLineHeight(2);
    helpScroll_ += action == kExtraHelpDown ? page : -page;
    renderMenu();
    return;
  }
  if (action >= kExtraColorSwatch && action < kExtraColorSwatch + static_cast<int>(kLetterColorSwatchCount)) {
    const uint16_t color = letterColorSwatch(static_cast<size_t>(action - kExtraColorSwatch));
    display_.setCustomFocusColor(color);
    preferences_.putUChar(kPrefFocusColorIndex, DisplayManager::kFocusColorCustom);
    preferences_.putUShort(kPrefFocusColorRgb, color);
    Serial.printf("[display] letter color=0x%04X\n", static_cast<unsigned>(color));
    applyDisplayPreferences(nowMs, false);
    renderMenu();
    return;
  }
  if (action >= kExtraGoToSegment && action < kExtraGoToSegment + 3) {
    goToSegment_ = static_cast<uint8_t>(action - kExtraGoToSegment);
    if (goToSegment_ == 2 && chapterMarkers_.empty()) {
      goToSegment_ = 0;
      showGridToast(tr4(TrKey4::GoToNoChapters), nowMs);
    }
    renderMenu();
    return;
  }
  switch (action) {
    case kExtraGoToMinus:
    case kExtraGoToPlus: {
      int minimum = 0;
      int maximum = 0;
      const int value = goToSliderValue(minimum, maximum);
      goToSetSliderValue(std::max(minimum, std::min(maximum, value + (action == kExtraGoToPlus ? 1 : -1))));
      renderMenu();
      return;
    }
    case kExtraGoToRead: {
      const size_t wordCount = reader_.wordCount();
      if (wordCount == 0) {
        extraScreenBack(nowMs);
        return;
      }
      saveReadingPosition(true);
      reader_.seekTo(std::min(goToTargetWord_, wordCount - 1));
      invalidateContextPreviewWindow();
      bookDetailsMenuItems_.clear();
      goToFromBookDetails_ = false;
      menuScreen_ = MenuScreen::Main;
      setState(AppState::Paused, nowMs);
      saveReadingPosition(true);
      Serial.printf("[goto] jumped to word %u / %u\n", static_cast<unsigned>(reader_.currentIndex()),
                    static_cast<unsigned>(wordCount));
      return;
    }
    case kExtraBookmarkDefault:
      finishSavePointCreation(savePointDefaultName(), nowMs);
      return;
    case kExtraBookmarkCustom:
      openTextEntry(TextEntryPurpose::SavePointName, tr3(TrKey3::NameBookmark), tr3(TrKey3::EnterNamePrompt), "",
                    savePointDefaultName(), "", false, 30, MenuScreen::SavePointsList);
      return;
    default:
      return;
  }
}

// ─── Przejdz do ─────────────────────────────────────────────────────────────

void App::openGoToPosition(bool fromBookDetails, uint32_t nowMs) {
  if (!usingStorageBook_ || reader_.wordCount() == 0) {
    return;
  }
  goToFromBookDetails_ = fromBookDetails;
  goToTargetWord_ = reader_.currentIndex();
  goToDragging_ = false;
  if (goToSegment_ == 2 && chapterMarkers_.empty()) {
    goToSegment_ = 0;
  }
  menuScreen_ = MenuScreen::GoToPosition;
  if (state_ != AppState::Menu) {
    setState(AppState::Menu, nowMs);
  } else {
    renderMenu();
  }
}

size_t App::chapterIndexForWord(size_t wordIndex) const {
  size_t found = 0;
  for (size_t i = 0; i < chapterMarkers_.size(); ++i) {
    if (chapterMarkers_[i].wordIndex <= wordIndex) {
      found = i;
    } else {
      break;
    }
  }
  return found;
}

int App::goToSliderValue(int &minimum, int &maximum) const {
  const size_t wordCount = std::max<size_t>(1, reader_.wordCount());
  const size_t target = std::min(goToTargetWord_, wordCount - 1);
  switch (goToSegment_) {
    case 1: {
      const size_t pages = std::max<size_t>(1, (wordCount + kGoToWordsPerPage - 1) / kGoToWordsPerPage);
      minimum = 1;
      maximum = static_cast<int>(pages);
      return static_cast<int>(target / kGoToWordsPerPage) + 1;
    }
    case 2:
      minimum = 0;
      maximum = chapterMarkers_.empty() ? 0 : static_cast<int>(chapterMarkers_.size()) - 1;
      return static_cast<int>(chapterIndexForWord(target));
    default:
      minimum = 0;
      maximum = 100;
      return static_cast<int>((static_cast<uint64_t>(target) * 100U) / wordCount);
  }
}

void App::goToSetSliderValue(int value) {
  const size_t wordCount = std::max<size_t>(1, reader_.wordCount());
  switch (goToSegment_) {
    case 1:
      goToTargetWord_ = static_cast<size_t>(std::max(0, value - 1)) * kGoToWordsPerPage;
      break;
    case 2:
      if (!chapterMarkers_.empty()) {
        const size_t index = std::min(static_cast<size_t>(std::max(0, value)), chapterMarkers_.size() - 1);
        goToTargetWord_ = chapterMarkers_[index].wordIndex;
      }
      break;
    default:
      goToTargetWord_ = static_cast<size_t>((static_cast<uint64_t>(std::max(0, value)) * wordCount) / 100U);
      break;
  }
  goToTargetWord_ = std::min(goToTargetWord_, wordCount - 1);
}

void App::renderGoToPosition() {
  const size_t wordCount = std::max<size_t>(1, reader_.wordCount());
  const size_t target = std::min(goToTargetWord_, wordCount - 1);
  const size_t pages = std::max<size_t>(1, (wordCount + kGoToWordsPerPage - 1) / kGoToWordsPerPage);
  const unsigned page = static_cast<unsigned>(target / kGoToWordsPerPage) + 1;
  const unsigned percent = static_cast<unsigned>((static_cast<uint64_t>(target) * 100U) / wordCount);
  const String pageText = String(tr4(TrKey4::GoToPage)) + " " + String(page) + " " + tr4(TrKey4::GoToPageOf) + " " +
                          String(static_cast<unsigned>(pages));

  nano::GoToView view;
  view.header.backId = 0;
  view.header.title = tr4(TrKey4::GoToTitle);
  const String segments[3] = {tr4(TrKey4::GoToPercent), tr4(TrKey4::GoToPage), tr4(TrKey4::GoToChapter)};
  for (int i = 0; i < 3; ++i) {
    view.segmentIds[i] = kNanoActionBase + kExtraGoToSegment + i;
    view.segmentLabels[i] = segments[i];
  }
  view.segment = goToSegment_;
  view.sliderValue = goToSliderValue(view.sliderMin, view.sliderMax);
  view.dragging = goToDragging_;
  if (!chapterMarkers_.empty()) {
    const size_t chapter = chapterIndexForWord(target);
    view.detail = chapterMenuLabel(chapter);
  }
  switch (goToSegment_) {
    case 1:
      view.value = String(page) + " / " + String(static_cast<unsigned>(pages));
      view.hint = String(tr4(TrKey4::GoToPageSize)) + "  -  " + String(percent) + "%";
      break;
    case 2:
      view.value = String(static_cast<unsigned>(chapterIndexForWord(target) + 1)) + " / " +
                   String(static_cast<unsigned>(chapterMarkers_.size()));
      view.hint = pageText + "  -  " + String(percent) + "%";
      break;
    default:
      view.value = String(percent) + "%";
      view.hint = pageText;
      break;
  }
  const String toast = activeGridToastText(millis());
  if (!toast.isEmpty()) {
    view.hint = toast;
  }
  view.minusId = kNanoActionBase + kExtraGoToMinus;
  view.plusId = kNanoActionBase + kExtraGoToPlus;
  view.readId = kNanoActionBase + kExtraGoToRead;
  view.readLabel = tr4(TrKey4::GoToReadHere);
  NanoSinkAdapter sink(*this);
  sink.zeroIsBack = true;
  nano::paintGoTo(display_, sink, view);
}

bool App::handleGoToTouch(const TouchEvent &event, uint32_t nowMs) {
  const ui::Rect bar = nano::goToBarRect();
  // A little extra height: the bar is a thin strip under a thumb.
  const bool onBar = event.x >= bar.x && event.x < bar.x + bar.w && event.y + 12 >= bar.y &&
                     event.y < bar.y + bar.h + 12;
  if (event.phase == TouchPhase::Start) {
    goToDragging_ = onBar;
    if (!goToDragging_) {
      return false;
    }
    pausedTouch_.active = false;
  } else if (!goToDragging_) {
    return false;
  }
  int minimum = 0;
  int maximum = 0;
  goToSliderValue(minimum, maximum);
  const int x = std::max<int>(bar.x, std::min<int>(bar.x + bar.w, event.x));
  const int value = minimum + ((x - bar.x) * (maximum - minimum) + bar.w / 2) / std::max<int>(1, bar.w);
  goToSetSliderValue(value);
  if (event.phase == TouchPhase::End) {
    goToDragging_ = false;
    renderMenu();
    return true;
  }
  if (event.phase == TouchPhase::Start || nowMs - goToLastRenderMs_ >= kNanoDragFrameMs) {
    goToLastRenderMs_ = nowMs;
    renderMenu();
  }
  return true;
}

// ─── Nazwa zakladki ─────────────────────────────────────────────────────────

void App::beginSavePointNaming(uint32_t nowMs) {
  if (!savePointUseCustomName_) {
    finishSavePointCreation(savePointDefaultName(), nowMs);
    return;
  }
  menuScreen_ = MenuScreen::SavePointNameChoice;
  if (state_ != AppState::Menu) {
    setState(AppState::Menu, nowMs);
  } else {
    renderMenu();
  }
}

void App::renderSavePointNameChoice() {
  nano::ChoiceView view;
  view.header.backId = 0;
  view.header.title = tr3(TrKey3::NameBookmark);
  view.question = tr4(TrKey4::BookmarkNameQuestion);
  nano::ChoiceView::Option standard;
  standard.id = kNanoActionBase + kExtraBookmarkDefault;
  standard.label = tr4(TrKey4::BookmarkNameDefault);
  standard.detail = savePointDefaultName();
  standard.icon = NanoIcon::Bookmark;
  standard.accent = true;
  nano::ChoiceView::Option custom;
  custom.id = kNanoActionBase + kExtraBookmarkCustom;
  custom.label = tr4(TrKey4::BookmarkNameCustom);
  custom.detail = tr4(TrKey4::BookmarkNameCustomDetail);
  custom.icon = NanoIcon::Edit;
  view.options = {standard, custom};
  NanoSinkAdapter sink(*this);
  sink.zeroIsBack = true;
  nano::paintChoice(display_, sink, view);
}

// ─── Kolor litery ───────────────────────────────────────────────────────────

void App::openFocusColorPicker(uint32_t nowMs) {
  menuScreen_ = MenuScreen::FocusColorPicker;
  if (state_ != AppState::Menu) {
    setState(AppState::Menu, nowMs);
  } else {
    renderMenu();
  }
}

void App::renderFocusColorPicker() {
  nano::ColorPickerView view;
  view.header.backId = 0;
  view.header.title = tr4(TrKey4::LetterColorTitle);
  view.columns = static_cast<int>(kLetterColorHues);
  view.rows = 3;
  const uint16_t current = display_.focusColorFor(false);
  for (size_t i = 0; i < kLetterColorSwatchCount; ++i) {
    nano::ColorPickerView::Swatch swatch;
    swatch.id = kNanoActionBase + kExtraColorSwatch + static_cast<int>(i);
    swatch.color = letterColorSwatch(i);
    swatch.selected = display_.focusColorIndex() == DisplayManager::kFocusColorCustom && swatch.color == current;
    view.swatches.push_back(swatch);
  }
  const uint8_t theme = nightMode_ ? 2 : (darkMode_ ? 0 : 1);
  display_.readerThemeColors(theme, view.previewBackground, view.previewWord, view.previewFocus);
  view.sampleWord = tr4(TrKey4::TutWord);
  NanoSinkAdapter sink(*this);
  sink.zeroIsBack = true;
  nano::paintColorPicker(display_, sink, view);
}

// ─── Pomoc (?) ──────────────────────────────────────────────────────────────

bool App::helpTopicFor(MenuScreen screen, int id, HelpTopic &topic) const {
  auto pick = [&topic](HelpTopic value) {
    topic = value;
    return true;
  };
  if (screen == MenuScreen::SettingsDisplay) {
    switch (id) {
      case kSettingsDisplayBrightnessIndex: return pick(HelpTopic::Brightness);
      case kSettingsDisplayHandednessIndex: return pick(HelpTopic::Handedness);
      case kSettingsDisplayBatteryStyleIndex: return pick(HelpTopic::BatteryStyle);
      case kSettingsDisplayFooterIndex: return pick(HelpTopic::Footer);
      case kSettingsDisplayBatteryIndex: return pick(HelpTopic::BatteryLabel);
      case kSettingsDisplayScreensaverIndex: return pick(HelpTopic::Screensaver);
      case kSettingsDisplayReaderBatteryIndex: return pick(HelpTopic::ReaderBattery);
      case kSettingsDisplayReaderChapterIndex: return pick(HelpTopic::ReaderChapter);
      case kSettingsDisplayReaderProgressIndex: return pick(HelpTopic::ReaderProgress);
      case kSettingsDisplayLanguageIndex: return pick(HelpTopic::Language);
      case kSettingsDisplayHelpHintsIndex: return pick(HelpTopic::HelpHints);
      case kSettingsDisplayNavModeIndex: return pick(HelpTopic::NavMode);
      case kSettingsDisplaySavePointNameModeIndex: return pick(HelpTopic::SavePointNameMode);
      default: return false;
    }
  }
  if (screen == MenuScreen::SettingsPacing) {
    if (id == kSettingsPacingReadingModeIndex) return pick(HelpTopic::ReadingMode);
    if (readerMode_ == ReaderMode::Scroll) {
      switch (id) {
        case kSettingsPacingScrollFontSizeIndex: return pick(HelpTopic::ScrollFontSize);
        case kSettingsPacingScrollLineSpacingIndex: return pick(HelpTopic::ScrollLineSpacing);
        case kSettingsPacingScrollMarginIndex: return pick(HelpTopic::ScrollMargins);
        case kSettingsPacingScrollAlignIndex: return pick(HelpTopic::ScrollAlign);
        case kSettingsPacingScrollPreviewIndex: return pick(HelpTopic::ScrollPreview);
        default: return false;
      }
    }
    switch (id) {
      case kSettingsPacingPauseModeIndex: return pick(HelpTopic::PauseMode);
      case kSettingsPacingWpmIndex: return pick(HelpTopic::Wpm);
      case kSettingsPacingLongWordsIndex: return pick(HelpTopic::LongWords);
      case kSettingsPacingComplexityIndex: return pick(HelpTopic::Complexity);
      case kSettingsPacingPunctuationIndex: return pick(HelpTopic::Punctuation);
      case kSettingsPacingResetIndex: return pick(HelpTopic::ResetPacing);
      default: return false;
    }
  }
  if (screen == MenuScreen::ScreensaverSettings) {
    switch (id) {
      case kScreensaverSettingsStyleIndex: return pick(HelpTopic::SaverStyle);
      case kScreensaverSettingsTimeoutIndex: return pick(HelpTopic::SaverTimeout);
      case kScreensaverSettingsAutoOffIndex: return pick(HelpTopic::SaverAutoOff);
      case kScreensaverSettingsSleepGuardIndex: return pick(HelpTopic::SaverSleepGuard);
      case kScreensaverSettingsPreviewIndex: return pick(HelpTopic::SaverPreview);
      default: return false;
    }
  }
  if (screen == MenuScreen::SettingsHome) {
    if (id == static_cast<int>(kSettingsHomeReadingIndex)) return pick(HelpTopic::HomePacing);
    if (id == static_cast<int>(kSettingsHomeTypographyIndex)) return pick(HelpTopic::HomeTypography);
    if (id == static_cast<int>(kSettingsHomeDisplayIndex)) return pick(HelpTopic::HomeDisplay);
    if (id == static_cast<int>(kSettingsHomeAdvancedIndex)) return pick(HelpTopic::HomeAdvanced);
    if (id == static_cast<int>(kSettingsHomePresetsIndex)) return pick(HelpTopic::HomePresets);
    if (id == kNanoActionBase + kNanoSettingsScreensaver) return pick(HelpTopic::Screensaver);
  }
  return false;
}

int App::helpActionFor(MenuScreen screen, int id) const {
  HelpTopic topic;
  if (!showHelpHints_ || !helpTopicFor(screen, id, topic)) {
    return nano::kNoTarget;
  }
  return kNanoActionBase + kExtraHelp + static_cast<int>(topic);
}

void App::openHelpPage(HelpTopic topic, uint32_t nowMs) {
  if (menuScreen_ != MenuScreen::HelpPage) {
    helpReturnScreen_ = menuScreen_;
  }
  helpTopic_ = topic;
  helpScroll_ = 0;
  helpDragging_ = false;
  helpLines_ = DisplayManager::nanoWrapText(HelpData::body(topic, static_cast<uint8_t>(uiLanguage_)),
                                            nano::helpTextWidth(), 2);
  menuScreen_ = MenuScreen::HelpPage;
  if (state_ != AppState::Menu) {
    setState(AppState::Menu, nowMs);
  } else {
    renderMenu();
  }
}

void App::renderHelpPage() {
  nano::HelpView view;
  view.header.backId = 0;
  view.header.title = HelpData::title(helpTopic_, static_cast<uint8_t>(uiLanguage_));
  view.lines = helpLines_;
  const int maxScroll = std::max(0, nano::helpContentHeight(view) - nano::helpBodyRect().h);
  helpScroll_ = std::max(0, std::min(helpScroll_, maxScroll));
  view.scroll = helpScroll_;
  if (maxScroll > 0) {
    const int pageH = std::max(1, nano::helpBodyRect().h - DisplayManager::nanoLineHeight(2));
    view.header.pageCount = static_cast<size_t>(maxScroll / pageH + 2);
    view.header.page = helpScroll_ >= maxScroll ? view.header.pageCount - 1 : static_cast<size_t>(helpScroll_ / pageH);
    view.header.prevId = kNanoActionBase + kExtraHelpUp;
    view.header.nextId = kNanoActionBase + kExtraHelpDown;
  }
  NanoSinkAdapter sink(*this);
  sink.zeroIsBack = true;
  nano::paintHelp(display_, sink, view);
}

bool App::handleHelpTouch(const TouchEvent &event, uint32_t nowMs) {
  const ui::Rect body = nano::helpBodyRect();
  if (event.phase == TouchPhase::Start) {
    helpDragging_ = body.contains(event.x, event.y);
    helpMoved_ = false;
    if (!helpDragging_) {
      return false;
    }
    helpDragStartY_ = event.y;
    helpDragStartScroll_ = helpScroll_;
    pausedTouch_.active = false;
    return true;
  }
  if (!helpDragging_) {
    return false;
  }
  const int dy = static_cast<int>(event.y) - helpDragStartY_;
  helpMoved_ = helpMoved_ || std::abs(dy) > 6;
  if (helpMoved_) {
    helpScroll_ = helpDragStartScroll_ - dy;
  }
  if (event.phase == TouchPhase::End) {
    helpDragging_ = false;
    renderMenu();
    return true;
  }
  if (helpMoved_ && nowMs - helpLastRenderMs_ >= kNanoDragFrameMs) {
    helpLastRenderMs_ = nowMs;
    renderMenu();
  }
  return true;
}
