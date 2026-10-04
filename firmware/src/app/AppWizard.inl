// First-run wizard in the Nano skin (ui/NanoScreens.cpp paintWizard). Every
// step is a wizard page drawn here, except the Wi-Fi list and the password
// keyboard, which are the regular Nano screens flagged with
// wifiFlowFromWizard_. The step order and the older open*/select* functions
// live in App.cpp; this file draws the pages, routes their taps, and runs
// the loading step (update, fonts, books) and the resume after a restart.
// Included at the end of App.cpp.

namespace {

constexpr int kWizardNext = 1;
constexpr int kWizardBack = 2;
constexpr int kWizardExtra = 3;
constexpr int kWizardPagePrev = 4;
constexpr int kWizardPageNext = 5;
constexpr int kWizardChipBase = 100;
// Language, theme, color, Wi-Fi, loading, menu look, menu font, reading
// font, reading mode, app download, pairing, library.
constexpr size_t kWizardStepCount = 12;
constexpr size_t kWizardStepWifi = 3;
// Reading theme shown by each Motyw chip (wizard order Light, Dark, Night;
// DisplayManager themes 0 dark, 1 light, 2 night).
constexpr uint8_t kWizardThemeChip[] = {1, 0, 2};
// Chip grids: palettes 2x4, typefaces 2x3, books 1x3.
constexpr size_t kWizardPaletteColumns = 4;
constexpr size_t kWizardPalettesPerPage = 8;
constexpr size_t kWizardFontColumns = 3;
constexpr size_t kWizardFontsPerPage = 6;
// Menu fonts 2x3, all on one page.
constexpr size_t kWizardMenuFontColumns = 3;
// Starter library: the six titles of the language, 2x3 on one page.
constexpr size_t kWizardBookColumns = 3;
// Loading step: the big line changes every kWelcomeLoadingPhraseCycleMs,
// the tip under it every kWizardTipCycleMs.
constexpr uint32_t kWizardTipCycleMs = 8000;
// While a chosen starter book is still downloading, how often the card is
// checked for it.
constexpr uint32_t kWizardBookWaitPollMs = 500;
// How long "restarting" stays on screen before the new firmware boots.
constexpr uint32_t kWizardRestartDelayMs = 2000;
// A check that hangs (GitHub unreachable, no DNS) stops holding the
// loading screen after this long; a running download is left alone.
constexpr uint32_t kWizardUpdateCheckMaxMs = 90000;

// Firmware update run from the loading step: written by wizardUpdateTask,
// read by the main loop.
enum : uint8_t {
  kWizUpdateIdle,
  kWizUpdateChecking,
  kWizUpdateDownloading,
  kWizUpdateNone,   // already newest, or the check/download failed
  kWizUpdateReady,  // new firmware written, restart to run it
};
std::atomic<uint8_t> g_wizUpdateStage{kWizUpdateIdle};
std::atomic<int8_t> g_wizUpdatePercent{-1};

struct WizardUpdateParams {
  OtaUpdater::Config config;
};

void wizardUpdateStatus(void *, const char *, const char *, const char *, int progressPercent) {
  // checkAndInstall reports 30..95 while the image downloads.
  if (progressPercent >= 30) {
    g_wizUpdateStage = kWizUpdateDownloading;
    g_wizUpdatePercent = static_cast<int8_t>(std::min(100, (progressPercent - 30) * 100 / 65));
  }
}

size_t wizardPageCount(size_t items, size_t perPage) {
  return items == 0 ? 1 : (items + perPage - 1) / perPage;
}

}  // namespace

bool App::wizardNanoScreen() const {
  switch (menuScreen_) {
    case MenuScreen::WelcomeLanguage:
    case MenuScreen::WelcomeSdCard:
    case MenuScreen::WelcomeTheme:
    case MenuScreen::WelcomeHighlightColor:
    case MenuScreen::WelcomeLoading:
    case MenuScreen::WelcomeSuper:
    case MenuScreen::WelcomeConfigureIntro:
    case MenuScreen::WelcomeMenuTheme:
    case MenuScreen::WelcomeMenuFont:
    case MenuScreen::WelcomeFont:
    case MenuScreen::WelcomeReadingMode:
    case MenuScreen::WelcomeReadingModePreview:
    case MenuScreen::WelcomeConnect:
    case MenuScreen::WelcomeAppPairing:
    case MenuScreen::WelcomeConfigureInApp:
    case MenuScreen::WelcomeLibrary:
      return true;
    default:
      return false;
  }
}

size_t App::wizardStepIndex() const {
  switch (menuScreen_) {
    case MenuScreen::WelcomeLanguage:
      return 0;
    case MenuScreen::WelcomeSdCard:
    case MenuScreen::WelcomeTheme:
      return 1;
    case MenuScreen::WelcomeHighlightColor:
      return 2;
    case MenuScreen::WelcomeLoading:
    case MenuScreen::WelcomeSuper:
    case MenuScreen::WelcomeConfigureIntro:
      return 4;
    case MenuScreen::WelcomeMenuTheme:
      return 5;
    case MenuScreen::WelcomeMenuFont:
      return 6;
    case MenuScreen::WelcomeFont:
      return 7;
    case MenuScreen::WelcomeReadingModePreview:
      return welcomePreviewFromFont_ ? 7 : 8;
    case MenuScreen::WelcomeReadingMode:
      return 8;
    case MenuScreen::WelcomeConnect:
      return 9;
    case MenuScreen::WelcomeAppPairing:
      return 10;
    case MenuScreen::WelcomeConfigureInApp:
    case MenuScreen::WelcomeLibrary:
    default:
      return 11;
  }
}

void App::renderWizardPage() {
  if (wizardRenderSuppressed_) {
    return;
  }
  const uint32_t nowMs = millis();
  const uint32_t elapsed = nowMs - welcomeScreenEnteredMs_;
  nano::WizardView view;
  view.step = wizardStepIndex();
  view.stepCount = kWizardStepCount;
  view.backId = kWizardBack;
  view.backLabel = uiText(UiText::Back);
  view.nextId = kWizardNext;
  view.nextLabel = tr3(TrKey3::NextLabel);

  auto addChips = [&](nano::WizardChipArt art) {
    for (size_t i = 0; i < settingsMenuItems_.size(); ++i) {
      nano::WizardChip chip;
      chip.id = kWizardChipBase + static_cast<int>(i);
      chip.label = settingsMenuItems_[i];
      chip.selected = i == settingsSelectedIndex_;
      chip.art = art;
      view.chips.push_back(chip);
    }
  };
  // Arrows either side of a paged chip grid; none on the first/last page.
  auto setPaging = [&](size_t pages) {
    if (welcomeChipPage_ >= pages) {
      welcomeChipPage_ = pages - 1;
    }
    if (pages > 1) {
      view.pagePrevId = welcomeChipPage_ > 0 ? kWizardPagePrev : nano::kNoTarget;
      view.pageNextId = welcomeChipPage_ + 1 < pages ? kWizardPageNext : nano::kNoTarget;
    }
  };

  switch (menuScreen_) {
    case MenuScreen::WelcomeLanguage:
      view.title = tr3(TrKey3::WelcomeLanguageTitle);
      view.subtitle = tr4(TrKey4::WizLanguageSub);
      view.backId = nano::kNoTarget;
      view.footer = tr3(TrKey3::WelcomePowerBackHint);
      addChips(nano::WizardChipArt::Label);
      break;
    case MenuScreen::WelcomeTheme:
      view.title = tr3(TrKey3::WelcomeThemeTitle);
      view.subtitle = tr4(TrKey4::WizThemeSub);
      addChips(nano::WizardChipArt::Theme);
      for (size_t i = 0; i < view.chips.size() && i < 3; ++i) {
        view.chips[i].theme = kWizardThemeChip[i];
      }
      break;
    case MenuScreen::WelcomeHighlightColor:
      view.title = tr3(TrKey3::WelcomeHighlightColorTitle);
      view.subtitle = tr4(TrKey4::WizColorSub);
      addChips(nano::WizardChipArt::Swatch);
      for (size_t i = 0; i < view.chips.size(); ++i) {
        view.chips[i].swatch = DisplayManager::presetFocusColor(static_cast<uint8_t>(i));
      }
      break;
    case MenuScreen::WelcomeSdCard:
      view.body = nano::WizardBody::Message;
      view.extraId = kWizardExtra;
      view.extraLabel = tr4(TrKey4::WizSdSkip);
      switch (welcomeSdState_) {
        case WelcomeSdState::Missing:
          view.title = tr4(TrKey4::WizSdMissingTitle);
          view.subtitle = tr4(TrKey4::WizSdMissingSub);
          view.nextLabel = tr4(TrKey4::WizSdCheck);
          break;
        case WelcomeSdState::Unreadable:
          view.title = tr4(TrKey4::WizSdFormatTitle);
          view.subtitle = tr4(TrKey4::WizSdFormatSub);
          view.nextLabel = tr4(TrKey4::WizSdFormat);
          break;
        case WelcomeSdState::ConfirmFormat:
          view.title = tr4(TrKey4::WizSdFormatTitle);
          view.subtitle = tr4(TrKey4::WizSdConfirmSub);
          view.nextLabel = tr4(TrKey4::WizSdFormatYes);
          break;
        case WelcomeSdState::Formatting:
          view.body = nano::WizardBody::Loading;
          view.title = tr4(TrKey4::WizSdFormatting);
          view.subtitle = tr4(TrKey4::WizSdFormattingSub);
          view.backId = nano::kNoTarget;
          view.nextId = nano::kNoTarget;
          view.extraId = nano::kNoTarget;
          break;
        case WelcomeSdState::Failed:
          view.title = tr4(TrKey4::WizSdFailedTitle);
          view.subtitle = tr4(TrKey4::WizSdFailedSub);
          view.nextLabel = tr4(TrKey4::WizSdFormat);
          break;
      }
      break;
    case MenuScreen::WelcomeMenuTheme: {
      view.title = tr4(TrKey4::WizMenuThemeTitle);
      view.subtitle = tr4(TrKey4::WizMenuThemeSub);
      const size_t count = DisplayManager::nanoPaletteCount();
      setPaging(wizardPageCount(count, kWizardPalettesPerPage));
      view.chipRows = 2;
      view.chipColumns = static_cast<int>(kWizardPaletteColumns);
      for (size_t i = welcomeChipPage_ * kWizardPalettesPerPage;
           i < count && i < (welcomeChipPage_ + 1) * kWizardPalettesPerPage; ++i) {
        nano::WizardChip chip;
        chip.id = kWizardChipBase + static_cast<int>(i);
        chip.art = nano::WizardChipArt::Palette;
        chip.palette = static_cast<uint8_t>(i);
        chip.label = nanoPaletteLabel(chip.palette);
        chip.selected = chip.palette == nanoPalette_;
        view.chips.push_back(chip);
      }
      break;
    }
    case MenuScreen::WelcomeMenuFont: {
      view.title = tr4(TrKey4::WizMenuFontTitle);
      view.subtitle = tr4(TrKey4::WizMenuFontSub);
      view.chipRows = 2;
      view.chipColumns = static_cast<int>(kWizardMenuFontColumns);
      const uint8_t current = nanoResolvedUiFont();
      for (uint8_t family = 0; family < DisplayManager::nanoUiFontCount(); ++family) {
        nano::WizardChip chip;
        chip.id = kWizardChipBase + family;
        chip.art = nano::WizardChipArt::UiFont;
        chip.family = family;
        chip.label = DisplayManager::nanoUiFontName(family);
        chip.selected = family == current;
        view.chips.push_back(chip);
      }
      break;
    }
    case MenuScreen::WelcomeFont: {
      view.title = tr4(TrKey4::WizReadFontTitle);
      view.subtitle = tr4(TrKey4::WizFontSub);
      view.extraId = kWizardExtra;
      view.extraLabel = tr4(TrKey4::WizPreview);
      if (fontDownloadInProgress_) {
        view.footer = tr4(TrKey4::WizFontsStillLoading);
      }
      setPaging(wizardPageCount(welcomeFontFaces_.size(), kWizardFontsPerPage));
      view.chipRows = 2;
      view.chipColumns = static_cast<int>(kWizardFontColumns);
      for (size_t i = welcomeChipPage_ * kWizardFontsPerPage;
           i < welcomeFontFaces_.size() && i < (welcomeChipPage_ + 1) * kWizardFontsPerPage; ++i) {
        nano::WizardChip chip;
        chip.id = kWizardChipBase + static_cast<int>(i);
        chip.art = nano::WizardChipArt::Typeface;
        chip.typeface = welcomeFontFaces_[i];
        chip.label = typefaceDisplayName(chip.typeface);
        chip.selected = chip.typeface == typographyConfig_.typeface;
        view.chips.push_back(chip);
      }
      break;
    }
    case MenuScreen::WelcomeReadingMode:
      view.title = tr3(TrKey3::WelcomeReadingModeTitle);
      view.subtitle = tr4(TrKey4::WizModeSub);
      addChips(nano::WizardChipArt::Rsvp);
      for (nano::WizardChip &chip : view.chips) {
        chip.word = tr4(TrKey4::TutWord);
      }
      if (view.chips.size() > 1) {
        view.chips[1].art = nano::WizardChipArt::Scroll;
      }
      view.extraId = kWizardExtra;
      view.extraLabel = tr4(TrKey4::WizPreview);
      break;
    case MenuScreen::WelcomeReadingModePreview:
      view.body = nano::WizardBody::Preview;
      view.nextLabel = tr4(TrKey4::WizPick);
      view.previewMode = welcomeReadingModePreviewMode_;
      if (welcomePreviewFromFont_) {
        view.footer = typefaceDisplayName(typographyConfig_.typeface);
      }
      if (welcomeReadingModePreviewMode_ == 0 && kTypographyPreviewWordCount > 0) {
        const size_t current = welcomeReadingModePreviewWordIndex_ % kTypographyPreviewWordCount;
        view.previewWord = kTypographyPreviewWords[current];
        if (kTypographyPreviewWordCount > 1) {
          view.previewBefore =
              kTypographyPreviewWords[current == 0 ? kTypographyPreviewWordCount - 1 : current - 1];
          view.previewAfter = kTypographyPreviewWords[(current + 1) % kTypographyPreviewWordCount];
        }
        view.previewSizeLevel = static_cast<uint8_t>(readerFontSizeIndex_);
      } else if (!welcomeScrollPreviewWords_.empty()) {
        view.scrollWords = &welcomeScrollPreviewWords_;
        view.scrollCurrent = welcomeReadingModePreviewWordIndex_ % welcomeScrollPreviewWords_.size();
      }
      break;
    case MenuScreen::WelcomeLoading: {
      view.body = nano::WizardBody::Loading;
      view.title = wizardLoadingPhrase(elapsed);
      view.subtitle = wizardLoadingTip(elapsed);
      view.phase = elapsed / 50UL;
      view.backId = nano::kNoTarget;
      view.nextId = nano::kNoTarget;
      switch (welcomeLoadPhase_) {
        case WelcomeLoadPhase::Start:
          break;
        case WelcomeLoadPhase::Update:
          if (g_wizUpdateStage.load() == kWizUpdateDownloading) {
            const int percent = g_wizUpdatePercent.load();
            view.status = String(tr4(TrKey4::WizUpdateInstalling)) + "  " + String(std::max(0, percent)) + "%";
            view.percent = std::max(0, percent);
          } else {
            view.status = tr4(TrKey4::WizUpdateChecking);
          }
          break;
        case WelcomeLoadPhase::Restarting:
          view.status = tr4(TrKey4::WizUpdateRestart);
          view.percent = 100;
          break;
        case WelcomeLoadPhase::Assets: {
          const unsigned fontsDone = g_fontDlDone.load();
          const unsigned fontsTotal = g_fontDlTotal.load();
          const unsigned booksDone = g_bookDlDone.load();
          const unsigned booksTotal = g_bookDlTotal.load();
          if (fontDownloadInProgress_ && fontsTotal > 0) {
            view.status = String(tr4(TrKey4::WizFontsProgress)) + "  " + String(fontsDone) + "/" + String(fontsTotal);
          } else if (bookDownloadInProgress_ && booksTotal > 0) {
            view.status = String(tr4(TrKey4::WizBooksProgress)) + "  " + String(booksDone) + "/" + String(booksTotal);
          }
          const unsigned total = (fontDownloadInProgress_ ? fontsTotal : 0) + (bookDownloadInProgress_ ? booksTotal : 0);
          if (total > 0) {
            const unsigned done = (fontDownloadInProgress_ ? fontsDone : 0) + (bookDownloadInProgress_ ? booksDone : 0);
            view.percent = static_cast<int>(done * 100 / total);
          }
          break;
        }
      }
      break;
    }
    case MenuScreen::WelcomeSuper:
      view.body = nano::WizardBody::Message;
      view.title = tr3(TrKey3::WelcomeSuperTitle);
      view.subtitle = tr4(TrKey4::WizSuperSub);
      view.backId = nano::kNoTarget;
      view.autoPercent = static_cast<int>(std::min<uint32_t>(100, elapsed * 100UL / kWelcomeTimedMessageMs));
      break;
    case MenuScreen::WelcomeConfigureIntro:
      view.body = nano::WizardBody::Message;
      view.title = tr3(TrKey3::WelcomeConfigureTitle);
      view.subtitle = tr4(TrKey4::WizConfigureSub);
      view.backId = nano::kNoTarget;
      view.autoPercent = static_cast<int>(std::min<uint32_t>(100, elapsed * 100UL / kWelcomeTimedMessageMs));
      break;
    case MenuScreen::WelcomeConfigureInApp:
      view.body = nano::WizardBody::Message;
      view.title = tr3(TrKey3::WelcomeConfigureInAppLine1);
      view.subtitle = tr3(TrKey3::WelcomeConfigureInAppLine2);
      view.autoPercent = static_cast<int>(std::min<uint32_t>(100, elapsed * 100UL / kWelcomeTimedMessageMs));
      break;
    case MenuScreen::WelcomeLibrary: {
      const uint8_t language = std::min<uint8_t>(static_cast<uint8_t>(uiLanguage_), StarterTitles::kLanguages - 1);
      view.extraId = kWizardExtra;
      view.extraLabel = tr2(TrKey2::SkipForNow);
      if (welcomeLibraryWaiting_) {
        // The chosen title is still on its way: a bar until it lands.
        view.body = nano::WizardBody::Loading;
        view.title = tr4(TrKey4::WizBookDownloading);
        view.subtitle = StarterTitles::kTitles[language][welcomeLibrarySelected_].title;
        view.phase = elapsed / 50UL;
        view.nextId = nano::kNoTarget;
        view.backId = nano::kNoTarget;
        break;
      }
      view.title = tr3(TrKey3::WelcomeBookPickerTitle);
      view.nextLabel = uiText(UiText::Read);
      view.tallChips = true;
      view.chipRows = 2;
      view.chipColumns = static_cast<int>(kWizardBookColumns);
      if (welcomeLibraryFailed_) {
        view.footer = tr4(TrKey4::WizBookFailed);
      } else if (bookDownloadInProgress_) {
        view.footer = tr4(TrKey4::WizBooksStillLoading);
      }
      for (uint8_t slot = 0; slot < StarterTitles::kPerLanguage; ++slot) {
        const StarterTitles::Title &title = StarterTitles::kTitles[language][slot];
        if (title.title[0] == '\0') {
          continue;
        }
        nano::WizardChip chip;
        chip.id = kWizardChipBase + slot;
        chip.art = nano::WizardChipArt::Book;
        chip.label = title.title;
        chip.detail = title.author;
        chip.selected = slot == welcomeLibrarySelected_;
        view.chips.push_back(chip);
      }
      break;
    }
    case MenuScreen::WelcomeConnect: {
      ensureInstallAppQr();
      view.body = nano::WizardBody::Qr;
      view.title = tr3(TrKey3::WelcomeConnectTitle);
      view.subtitle = tr3(TrKey3::WelcomeConnectLine1);
      view.qr = g_installAppQrSize > 0 ? g_installAppQrData : nullptr;
      view.qrSize = g_installAppQrSize;
      view.qrLine = "flower.theworkpc.com/appdownload";
      // Dalej only after a few seconds with the code on screen, as before.
      const bool showNext = g_installAppQrSize == 0 || elapsed >= kWelcomeConnectNextDelayMs;
      if (!showNext) {
        view.nextId = nano::kNoTarget;
        view.qrHint = tr4(TrKey4::WizConnectLook);
      }
      break;
    }
    case MenuScreen::WelcomeAppPairing:
      view.body = nano::WizardBody::Qr;
      view.title = tr3(TrKey3::WelcomeAppPairingTitle);
      view.subtitle = tr4(TrKey4::WizPairSub);
      if (companionSync_.hasQrCode()) {
        view.qr = companionSync_.qrCodeData();
        view.qrSize = companionSync_.qrCodeSize();
      }
      view.qrLine = companionSync_.statusLine1();
      view.qrHint = autoSyncClientConnected_ ? tr3(TrKey3::WelcomeConnectHintConnected)
                                             : tr3(TrKey3::WelcomeConnectHintWaiting);
      break;
    default:
      return;
  }

  const bool classicColors = navMode_ != NavMode::Modern;
  if (classicColors) {
    display_.overrideNanoPalette(DisplayManager::kNanoPaletteClassic, false);
  }
  wizardTargets_.clear();
  NanoPanelSink sink(wizardTargets_, -1);
  nano::paintWizard(display_, sink, view);
  if (classicColors) {
    display_.overrideNanoPalette(nanoPalette_, nanoOwnAccent_);
  }
}

void App::renderWizardBusy(const String &title, const String &subtitle) {
  // Blocking work inside the wizard (Wi-Fi scan, connecting): a still page,
  // no animation that would freeze while the radio holds the loop.
  nano::WizardView view;
  view.step = kWizardStepWifi;
  view.stepCount = kWizardStepCount;
  view.body = nano::WizardBody::Message;
  view.title = title;
  view.subtitle = subtitle;
  wizardTargets_.clear();
  NanoPanelSink sink(wizardTargets_, -1);
  nano::paintWizard(display_, sink, view);
}

void App::renderWifiStatus(const String &line1, const String &line2) {
  if (wifiFlowFromWizard_) {
    renderWizardBusy(line1, line2);
    return;
  }
  display_.renderStatus("Wi-Fi", line1, line2);
}

void App::handleWizardTouchAt(uint16_t x, uint16_t y, uint32_t nowMs) {
  int hit = nano::kNoTarget;
  for (const auto &target : wizardTargets_) {
    const ui::Rect &r = target.first;
    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
      hit = target.second;
      break;
    }
  }
  if (hit == nano::kNoTarget) {
    return;
  }
  if (hit == kWizardPagePrev || hit == kWizardPageNext) {
    if (hit == kWizardPagePrev && welcomeChipPage_ > 0) {
      --welcomeChipPage_;
    } else if (hit == kWizardPageNext) {
      ++welcomeChipPage_;  // clamped by renderWizardPage
    }
    renderWizardPage();
    return;
  }
  if (hit >= kWizardChipBase) {
    const size_t index = static_cast<size_t>(hit - kWizardChipBase);
    switch (menuScreen_) {
      case MenuScreen::WelcomeMenuTheme:
        // Applied at once: the page itself turns into the chosen palette.
        nanoPalette_ = static_cast<uint8_t>(index);
        preferences_.putUChar(kPrefNanoPalette, nanoPalette_);
        display_.setNanoPalette(nanoPalette_, nanoOwnAccent_);
        break;
      case MenuScreen::WelcomeMenuFont:
        // The page itself switches to the tapped font.
        if (index < DisplayManager::nanoUiFontCount()) {
          nanoUiFontChoice_ = static_cast<uint8_t>(index);
          preferences_.putUChar(kPrefNanoUiFont, nanoUiFontChoice_);
          applyNanoUiFont();
        }
        break;
      case MenuScreen::WelcomeFont:
        if (index < welcomeFontFaces_.size() && welcomeFontFaces_[index] != typographyConfig_.typeface) {
          typographyConfig_.typeface = welcomeFontFaces_[index];
          preferences_.putUChar(kPrefReaderTypeface, static_cast<uint8_t>(typographyConfig_.typeface));
          applyTypographySettings(nowMs, false);
          display_.consumeFontLoadFailure();
        }
        break;
      case MenuScreen::WelcomeLibrary:
        if (welcomeLibraryWaiting_) {
          return;
        }
        welcomeLibrarySelected_ = static_cast<uint8_t>(index);
        welcomeLibraryFailed_ = false;
        break;
      default:
        settingsSelectedIndex_ = index;
        previewWizardPickerSelection(nowMs);
        break;
    }
    renderWizardPage();
    return;
  }
  if (hit == kWizardBack) {
    wizardStepBack(nowMs);
    return;
  }
  if (hit == kWizardExtra) {
    if (menuScreen_ == MenuScreen::WelcomeReadingMode) {
      welcomePreviewFromFont_ = false;
      openWelcomeReadingModePreview(static_cast<uint8_t>(settingsSelectedIndex_ == 1 ? 1 : 0));
    } else if (menuScreen_ == MenuScreen::WelcomeFont) {
      // The chosen typeface running as RSVP words at the reading size.
      welcomePreviewFromFont_ = true;
      openWelcomeReadingModePreview(0);
    } else if (menuScreen_ == MenuScreen::WelcomeSdCard) {
      openWelcomeTheme();
    } else if (menuScreen_ == MenuScreen::WelcomeLibrary) {
      finishWelcomeWizard(nowMs);
    }
    return;
  }
  switch (menuScreen_) {
    case MenuScreen::WelcomeLanguage:
      selectWelcomeLanguageItem(nowMs);
      return;
    case MenuScreen::WelcomeSdCard:
      selectWelcomeSdCardNext(nowMs);
      return;
    case MenuScreen::WelcomeTheme:
      selectWelcomeThemeItem(nowMs);
      return;
    case MenuScreen::WelcomeHighlightColor:
      selectWelcomeHighlightColorItem(nowMs);
      return;
    case MenuScreen::WelcomeMenuTheme:
      openWelcomeMenuFont(nowMs);
      return;
    case MenuScreen::WelcomeMenuFont:
      // From here on the menu keeps this font whatever the books use.
      nanoUiFontChoice_ = nanoResolvedUiFont();
      preferences_.putUChar(kPrefNanoUiFont, nanoUiFontChoice_);
      openWelcomeFont(nowMs);
      return;
    case MenuScreen::WelcomeFont:
      openWelcomeReadingMode();
      return;
    case MenuScreen::WelcomeReadingMode:
      selectWelcomeReadingModeItem(nowMs);
      return;
    case MenuScreen::WelcomeReadingModePreview:
      if (welcomePreviewFromFont_) {
        // "Wybieram" on the typeface preview: this font, on to the mode.
        welcomePreviewFromFont_ = false;
        openWelcomeReadingMode();
        return;
      }
      // "Wybieram": the previewed mode is the choice.
      settingsSelectedIndex_ = welcomeReadingModePreviewMode_ == 1 ? 1 : 0;
      selectWelcomeReadingModeItem(nowMs);
      return;
    case MenuScreen::WelcomeSuper:
      openWelcomeConfigureIntro(nowMs);
      return;
    case MenuScreen::WelcomeConfigureIntro:
      openWelcomeMenuTheme(nowMs);
      return;
    case MenuScreen::WelcomeConnect:
      selectWelcomeConnectTap(nowMs);
      return;
    case MenuScreen::WelcomeAppPairing:
      selectWelcomeAppPairingTap(nowMs);
      return;
    case MenuScreen::WelcomeConfigureInApp:
      openWelcomeBookPicker(nowMs);
      return;
    case MenuScreen::WelcomeLibrary:
      openWelcomeLibraryBook(nowMs);
      return;
    default:
      return;
  }
}

// ─── Wygląd menu, czcionka, biblioteka ──────────────────────────────────────

void App::openWelcomeMenuTheme(uint32_t nowMs) {
  (void)nowMs;
  menuScreen_ = MenuScreen::WelcomeMenuTheme;
  saveWizardStep(kWizStepMenuTheme);
  welcomeChipPage_ = nanoPalette_ / kWizardPalettesPerPage;
  renderWizardPage();
}

void App::rebuildWelcomeFontFaces() {
  // Built-in faces always; the SD ones once both .fnt files are on the card.
  welcomeFontFaces_.clear();
  for (uint8_t i = 0; i < static_cast<uint8_t>(DisplayManager::ReaderTypeface::Count); ++i) {
    const auto typeface = static_cast<DisplayManager::ReaderTypeface>(i);
    if (DisplayManager::isTypefaceAvailableOnSd(typeface)) {
      welcomeFontFaces_.push_back(typeface);
    }
  }
}

void App::openWelcomeMenuFont(uint32_t nowMs) {
  (void)nowMs;
  menuScreen_ = MenuScreen::WelcomeMenuFont;
  saveWizardStep(kWizStepMenuFont);
  welcomeChipPage_ = 0;
  renderWizardPage();
}

void App::openWelcomeFont(uint32_t nowMs) {
  (void)nowMs;
  menuScreen_ = MenuScreen::WelcomeFont;
  saveWizardStep(kWizStepFont);
  rebuildWelcomeFontFaces();
  welcomeFontsSeen_ = g_fontDlDone.load();
  welcomeChipPage_ = 0;
  for (size_t i = 0; i < welcomeFontFaces_.size(); ++i) {
    if (welcomeFontFaces_[i] == typographyConfig_.typeface) {
      welcomeChipPage_ = i / kWizardFontsPerPage;
    }
  }
  renderWizardPage();
}

String App::starterBookPath(uint8_t slot) const {
  return "/books/books/starter-" + starterBookLanguageCode(uiLanguage_) + "-" + String(slot + 1) + ".rsvp";
}

void App::openWelcomeLibrary(uint32_t nowMs) {
  // The six starter titles of the chosen language, named from the firmware
  // (StarterTitles.h), so the page is complete even while some are still
  // downloading; a title not on the card yet is fetched when picked. With
  // no card and no Wi-Fi there is nothing to read: straight to the menu.
  bool anyOnCard = false;
  for (uint8_t slot = 0; slot < StarterTitles::kPerLanguage && storageReady_; ++slot) {
    anyOnCard = anyOnCard || SD_MMC.exists(starterBookPath(slot));
  }
  const bool canFetch = storageReady_ && otaUpdater_.isConfigured(preferredOtaConfig());
  if (!anyOnCard && !canFetch && !bookDownloadInProgress_) {
    Serial.println("[welcome] no starter books and no way to fetch them, wizard done");
    finishWelcomeWizard(nowMs);
    return;
  }
  menuScreen_ = MenuScreen::WelcomeLibrary;
  saveWizardStep(kWizStepLibrary);
  welcomeScreenEnteredMs_ = nowMs;
  welcomeLibrarySelected_ = 0;
  welcomeLibraryWaiting_ = false;
  welcomeLibraryFailed_ = false;
  welcomeChipPage_ = 0;
  renderWizardPage();
}

void App::markWelcomeWizardDone() {
  wifiFlowFromWizard_ = false;
  wizardFontPickerActive_ = false;
  wizardBookPickerActive_ = false;
  welcomeLibraryWaiting_ = false;
  preferences_.putBool(kPrefSetupDone, true);
  preferences_.remove(kPrefWizardStep);
  savedWizardStep_ = 0xFF;
  // The old on-device tutorial stays under Ustawienia > O aplikacji; without
  // tut_done every later boot would force it.
  preferences_.putBool(kPrefTutorialDone, true);
  tutorialCompleted_ = true;
  // The phone network and Bluetooth from the pairing step stay up for the
  // rest of this first session (firstSessionSyncHold_), so the app keeps
  // working while the new owner looks around. From the next boot the phone
  // connects from Urządzenie > Aplikacja.
}

bool App::openStarterBook(uint8_t slot, uint32_t nowMs) {
  const String path = starterBookPath(slot);
  if (!SD_MMC.exists(path)) {
    return false;
  }
  int bookIndex = -1;
  for (int pass = 0; pass < 2 && bookIndex < 0; ++pass) {
    if (pass == 1) {
      // Downloaded after the library was last scanned.
      storage_.refreshBooks();
    }
    for (size_t i = 0; i < storage_.bookCount(); ++i) {
      if (storage_.bookPath(i).endsWith(path.substring(path.lastIndexOf('/')))) {
        bookIndex = static_cast<int>(i);
        break;
      }
    }
  }
  if (bookIndex < 0) {
    return false;
  }
  Serial.printf("[welcome] first book: %s\n", storage_.bookPath(static_cast<size_t>(bookIndex)).c_str());
  markWelcomeWizardDone();
  saveReadingPosition(true);
  if (loadBookAtIndex(static_cast<size_t>(bookIndex), nowMs, true, true, true, true)) {
    menuScreen_ = MenuScreen::Main;
    setState(AppState::Paused, nowMs);
    return true;
  }
  finishWelcomeWizard(nowMs);
  return true;
}

void App::openWelcomeLibraryBook(uint32_t nowMs) {
  if (welcomeLibrarySelected_ >= StarterTitles::kPerLanguage) {
    finishWelcomeWizard(nowMs);
    return;
  }
  if (openStarterBook(welcomeLibrarySelected_, nowMs)) {
    return;
  }
  // Not on the card yet: wait for the running download, or start one (it
  // fetches every title still missing, this one included).
  if (!bookDownloadInProgress_) {
    OtaUpdater::Config config = preferredOtaConfig();
    if (!storageReady_ || !otaUpdater_.isConfigured(config) || !startBackgroundBookDownload(config)) {
      welcomeLibraryFailed_ = true;
      renderWizardPage();
      return;
    }
  }
  welcomeLibraryWaiting_ = true;
  welcomeLibraryFailed_ = false;
  welcomeLibraryWaitPollMs_ = nowMs;
  welcomeScreenEnteredMs_ = nowMs;
  renderWizardPage();
}

void App::updateWelcomeLibrary(uint32_t nowMs) {
  if (!welcomeLibraryWaiting_) {
    // Footer "still downloading" goes once the downloads end.
    if (welcomeLibraryShownDownloading_ != bookDownloadInProgress_) {
      welcomeLibraryShownDownloading_ = bookDownloadInProgress_;
      renderWizardPage();
    }
    return;
  }
  if (nowMs - welcomeLibraryWaitPollMs_ >= kWizardBookWaitPollMs) {
    welcomeLibraryWaitPollMs_ = nowMs;
    if (SD_MMC.exists(starterBookPath(welcomeLibrarySelected_))) {
      welcomeLibraryWaiting_ = false;
      if (openStarterBook(welcomeLibrarySelected_, nowMs)) {
        return;
      }
    }
    if (!bookDownloadInProgress_ && welcomeLibraryWaiting_) {
      // The download ended without this title.
      welcomeLibraryWaiting_ = false;
      welcomeLibraryFailed_ = true;
      renderWizardPage();
      return;
    }
  }
  if (nowMs - welcomeLoadingLastRenderMs_ >= kWelcomeScreenFrameMs) {
    welcomeLoadingLastRenderMs_ = nowMs;
    renderWizardPage();
  }
}

String App::wizardLoadingPhrase(uint32_t elapsedMs) const {
  static const TrKey4 kMorePhrases[] = {TrKey4::WizPhrase4, TrKey4::WizPhrase5, TrKey4::WizPhrase6,
                                        TrKey4::WizPhrase7, TrKey4::WizPhrase8};
  constexpr size_t kCount = 3 + sizeof(kMorePhrases) / sizeof(kMorePhrases[0]);
  const size_t phrase = static_cast<size_t>((elapsedMs / kWelcomeLoadingPhraseCycleMs) % kCount);
  switch (phrase) {
    case 0:
      return tr3(TrKey3::WelcomeLoadingPhrase1);
    case 1:
      return tr3(TrKey3::WelcomeLoadingPhrase2);
    case 2:
      return tr3(TrKey3::WelcomeLoadingPhrase3);
    default:
      return tr4(kMorePhrases[phrase - 3]);
  }
}

String App::wizardLoadingTip(uint32_t elapsedMs) const {
  static const TrKey4 kTips[] = {TrKey4::WizTip1, TrKey4::WizTip2, TrKey4::WizTip3, TrKey4::WizTip4,
                                 TrKey4::WizTip5, TrKey4::WizTip6, TrKey4::WizTip7, TrKey4::WizTip8,
                                 TrKey4::WizTip9, TrKey4::WizTip10};
  constexpr size_t kCount = sizeof(kTips) / sizeof(kTips[0]);
  const size_t tip = static_cast<size_t>((elapsedMs / kWizardTipCycleMs) % kCount);
  return String(tr4(TrKey4::WizTipLabel)) + " " + tr4(kTips[tip]);
}

// ─── Ładowanie: aktualizacja, potem fonty i książki ─────────────────────────

bool App::startWizardUpdateTask(const OtaUpdater::Config &config) {
  WizardUpdateParams *params = new WizardUpdateParams();
  if (params == nullptr) {
    return false;
  }
  params->config = config;
  g_wizUpdateStage = kWizUpdateChecking;
  g_wizUpdatePercent = -1;
  const BaseType_t created = xTaskCreatePinnedToCore(wizardUpdateTask, "wiz_ota", kOtaCheckTaskStackBytes,
                                                     params, 1, nullptr, 0);
  if (created != pdPASS) {
    Serial.printf("[welcome] update task create failed: %ld\n", static_cast<long>(created));
    g_wizUpdateStage = kWizUpdateIdle;
    delete params;
    return false;
  }
  return true;
}

void App::wizardUpdateTask(void *params) {
  WizardUpdateParams *taskParams = static_cast<WizardUpdateParams *>(params);
  OtaUpdater updater;
  const OtaUpdater::Result result = updater.checkAndInstall(taskParams->config, &wizardUpdateStatus, nullptr);
  Serial.printf("[welcome] update: %s (%s -> %s) %s\n", result.summary.c_str(), result.currentVersion.c_str(),
                result.latestVersion.c_str(), result.detail.c_str());
  g_wizUpdateStage = result.rebootRequired ? kWizUpdateReady : kWizUpdateNone;
  delete taskParams;
  vTaskDelete(nullptr);
}

void App::startWelcomeAssetDownloads() {
  // Books first (a few MB, and the last step lists them), then the font
  // pack (about 8 MB) once they are in: updateWelcomeLoading() starts it.
  g_fontDlDone = 0;
  g_fontDlTotal = 0;
  g_bookDlDone = 0;
  g_bookDlTotal = 0;
  welcomeFontsStarted_ = false;
  welcomeAssetsProgressMark_ = 0;
  welcomeAssetsProgressMs_ = millis();
  if (!storageReady_) {
    welcomeFontsStarted_ = true;
    return;
  }
  OtaUpdater::Config config = preferredOtaConfig();
  if (!otaUpdater_.isConfigured(config)) {
    welcomeFontsStarted_ = true;
    return;
  }
  if (!bookDownloadInProgress_) {
    startBackgroundBookDownload(config);
  }
}

void App::startWelcomeFontDownload() {
  welcomeFontsStarted_ = true;
  if (refreshFontPackComplete() || fontDownloadInProgress_) {
    return;
  }
  OtaUpdater::Config config = preferredOtaConfig();
  if (!otaUpdater_.isConfigured(config)) {
    return;
  }
  lastFontDownloadAttemptMs_ = millis();
  startBackgroundFontDownload(config);
}

void App::updateWelcomeLoading(uint32_t nowMs) {
  const uint32_t elapsed = nowMs - welcomeScreenEnteredMs_;
  if (welcomeLoadPhase_ != WelcomeLoadPhase::Restarting && g_wizUpdateStage.load() == kWizUpdateReady) {
    welcomeLoadPhase_ = WelcomeLoadPhase::Restarting;
    welcomeLoadPhaseMs_ = nowMs;
    renderWelcomeLoading(nowMs);
    return;
  }

  switch (welcomeLoadPhase_) {
    case WelcomeLoadPhase::Start: {
      // One update check per boot: after the restart into the new firmware
      // this runs again, finds it current and goes on to the downloads.
      OtaUpdater::Config config = preferredOtaConfig();
      welcomeLoadPhaseMs_ = nowMs;
      if (!welcomeUpdateChecked_ && otaUpdater_.isConfigured(config) && startWizardUpdateTask(config)) {
        welcomeUpdateChecked_ = true;
        welcomeLoadPhase_ = WelcomeLoadPhase::Update;
        Serial.println("[welcome] checking for a newer firmware");
      } else {
        welcomeLoadPhase_ = WelcomeLoadPhase::Assets;
        startWelcomeAssetDownloads();
      }
      break;
    }
    case WelcomeLoadPhase::Update: {
      const uint8_t stage = g_wizUpdateStage.load();
      const bool checkStuck = stage == kWizUpdateChecking && nowMs - welcomeLoadPhaseMs_ >= kWizardUpdateCheckMaxMs;
      const bool downloadStuck = nowMs - welcomeLoadPhaseMs_ >= kWelcomeUpdateMaxMs;
      if (stage == kWizUpdateNone || checkStuck || downloadStuck) {
        welcomeLoadPhase_ = WelcomeLoadPhase::Assets;
        welcomeLoadPhaseMs_ = nowMs;
        startWelcomeAssetDownloads();
      }
      break;
    }
    case WelcomeLoadPhase::Assets: {
      if (!welcomeFontsStarted_ && !bookDownloadInProgress_) {
        startWelcomeFontDownload();
      }
      // Waits for every file; only a download that stops moving for
      // kWelcomeAssetsStallMs (no network, GitHub down) lets the wizard go
      // on without the rest. Fonts left out keep retrying in the background.
      const unsigned progressMark = g_fontDlDone.load() + g_bookDlDone.load() + (welcomeFontsStarted_ ? 1U : 0U);
      if (progressMark != welcomeAssetsProgressMark_) {
        welcomeAssetsProgressMark_ = progressMark;
        welcomeAssetsProgressMs_ = nowMs;
      }
      const bool done = welcomeFontsStarted_ && !fontDownloadInProgress_ && !bookDownloadInProgress_;
      const bool stalled = nowMs - welcomeAssetsProgressMs_ >= kWelcomeAssetsStallMs;
      if ((done && elapsed >= kWelcomeLoadingMinMs) || stalled || nowMs - welcomeLoadPhaseMs_ >= kWelcomeAssetsMaxMs) {
        Serial.printf("[welcome] loading done after %lu ms (fonts %u/%u, books %u/%u)\n",
                      static_cast<unsigned long>(elapsed), static_cast<unsigned>(g_fontDlDone.load()),
                      static_cast<unsigned>(g_fontDlTotal.load()), static_cast<unsigned>(g_bookDlDone.load()),
                      static_cast<unsigned>(g_bookDlTotal.load()));
        openWelcomeSuper(nowMs);
        return;
      }
      break;
    }
    case WelcomeLoadPhase::Restarting:
      if (nowMs - welcomeLoadPhaseMs_ >= kWizardRestartDelayMs) {
        // The step stays saved as Loading: the new firmware opens here.
        Serial.println("[welcome] restarting into the new firmware");
        Serial.flush();
        delay(100);
        ESP.restart();
      }
      break;
  }

  if (nowMs - welcomeLoadingLastRenderMs_ >= kWelcomeScreenFrameMs) {
    renderWelcomeLoading(nowMs);
  }
}

// ─── Wznowienie po restarcie ────────────────────────────────────────────────

uint8_t App::prepareWizardResume(uint32_t nowMs) {
  uint8_t step = preferences_.getUChar(kPrefWizardStep, kWizStepLanguage);
  if (step >= kWizStepCount) {
    step = kWizStepLanguage;
  }
  if (step > kWizStepLanguage) {
    Serial.printf("[welcome] resuming the wizard at step %u\n", static_cast<unsigned>(step));
  }
  savedWizardStep_ = step;
  welcomeScreenEnteredMs_ = nowMs;
  // Opened without drawing: the boot splash is still up, setState(Menu)
  // draws the page right after it fades.
  wizardRenderSuppressed_ = true;
  switch (step) {
    case kWizStepTheme:
      openWelcomeTheme();
      break;
    case kWizStepColor:
      openWelcomeHighlightColor();
      break;
    case kWizStepMenuTheme:
      openWelcomeMenuTheme(nowMs);
      break;
    case kWizStepMenuFont:
      openWelcomeMenuFont(nowMs);
      break;
    case kWizStepFont:
      openWelcomeFont(nowMs);
      break;
    case kWizStepReadingMode:
      openWelcomeReadingMode();
      break;
    case kWizStepConnect:
      openWelcomeConnect(nowMs);
      break;
    case kWizStepInApp:
      openWelcomeConfigureInApp(nowMs);
      break;
    case kWizStepWifi:
    case kWizStepLoading:
    case kWizStepPairing:
    case kWizStepLibrary:
      // Need the screen (scan, radios, possible finish): placeholder page,
      // the real step opens in finishWizardResume().
      menuScreen_ = MenuScreen::WelcomeLoading;
      welcomeLoadPhase_ = WelcomeLoadPhase::Start;
      break;
    case kWizStepLanguage:
    default:
      openWelcomeLanguage();
      break;
  }
  wizardRenderSuppressed_ = false;
  return step;
}

void App::finishWizardResume(uint8_t step, uint32_t nowMs) {
  switch (step) {
    case kWizStepWifi:
      openWelcomeWifi();
      return;
    case kWizStepLoading:
      openWelcomeLoading(nowMs);
      return;
    case kWizStepPairing:
      openWelcomeAppPairing(nowMs);
      return;
    case kWizStepLibrary:
      openWelcomeLibrary(nowMs);
      return;
    default:
      return;
  }
}
