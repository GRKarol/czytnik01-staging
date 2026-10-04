// Card check at boot, after the first-run wizard: a card the reader can't
// read gets the format offer, a new card without the reader's files gets
// "download them" or "not now". "Not now" is the card-reader mode: card
// details, sending files over USB or from the phone, nothing else until the
// files are downloaded, the card is formatted or swapped. Drawn as wizard
// pages (paintWizard). Included at the end of App.cpp, after AppWizard.inl
// (uses its kWizard* ids).

namespace {

// Marks a card that already had (or got) the reader's files; NVS keeps the
// id it should hold. A card without the mark, or with another id, is new.
constexpr const char *kSdCardMarkPath = "/.flower-card";
constexpr const char *kPrefSdCardId = "sd_card_id";
constexpr uint32_t kCardReaderDimAfterMs = 30000;
constexpr uint8_t kCardReaderDimPercent = 5;
constexpr uint32_t kSdSetupDoneRestartMs = 1500;
constexpr uint32_t kSdSetupRenderMs = 400;

enum : int {
  kCardReaderUsb = kWizardChipBase,
  kCardReaderPhone,
  kCardReaderAssets,
  kCardReaderFormat,
};

String sdSizeLabel(uint64_t bytes) {
  const uint32_t tenthsGb = static_cast<uint32_t>((bytes * 10ULL) / (1024ULL * 1024ULL * 1024ULL));
  if (tenthsGb >= 10) {
    return String(tenthsGb / 10) + "," + String(tenthsGb % 10) + " GB";
  }
  return String(static_cast<uint32_t>(bytes / (1024ULL * 1024ULL))) + " MB";
}

uint8_t sdFontsOnCard() {
  uint8_t count = 0;
  for (uint8_t i = static_cast<uint8_t>(DisplayManager::ReaderTypeface::Literata);
       i < static_cast<uint8_t>(DisplayManager::ReaderTypeface::Count); ++i) {
    if (DisplayManager::isTypefaceAvailableOnSd(static_cast<DisplayManager::ReaderTypeface>(i))) {
      ++count;
    }
  }
  return count;
}

uint8_t sdFontsTotal() {
  return static_cast<uint8_t>(DisplayManager::ReaderTypeface::Count) -
         static_cast<uint8_t>(DisplayManager::ReaderTypeface::Literata);
}

}  // namespace

uint32_t App::readCardMarkId() {
  File mark = SD_MMC.open(kSdCardMarkPath, FILE_READ);
  if (!mark) {
    return 0;
  }
  const String text = mark.readStringUntil('\n');
  mark.close();
  return static_cast<uint32_t>(strtoul(text.c_str(), nullptr, 16));
}

void App::markCardKnown() {
  uint32_t id = preferences_.getULong(kPrefSdCardId, 0);
  if (id == 0) {
    id = esp_random() | 1U;
    preferences_.putULong(kPrefSdCardId, id);
  }
  if (readCardMarkId() == id) {
    return;
  }
  File mark = SD_MMC.open(kSdCardMarkPath, FILE_WRITE);
  if (mark) {
    mark.printf("%08lx\n", static_cast<unsigned long>(id));
    mark.close();
  }
}

// Returns true when a card screen took over the boot.
bool App::checkSdCardAtBoot(uint32_t nowMs) {
  sdSetupChecked_ = true;
  if (!storageReady_) {
    // No card at all: the reader runs on its built-in text as before.
    if (storage_.probeCard() == StorageManager::CardProbe::Missing) {
      return false;
    }
    Serial.println("[sdsetup] card unreadable, offering a format");
    openSdCardSetup(SdSetupState::NeedsFormat, nowMs);
    return true;
  }
  const uint32_t savedId = preferences_.getULong(kPrefSdCardId, 0);
  // First boot with this check (savedId 0) or the card seen before: the
  // background download fills in whatever is missing, as it always did.
  if (fontPackComplete_ || savedId == 0 || readCardMarkId() == savedId) {
    markCardKnown();
    return false;
  }
  Serial.printf("[sdsetup] new card, %u/%u fonts: asking to download\n", sdFontsOnCard(), sdFontsTotal());
  openSdCardSetup(SdSetupState::NeedsAssets, nowMs);
  return true;
}

void App::openSdCardSetup(SdSetupState state, uint32_t nowMs) {
  menuScreen_ = MenuScreen::SdCardSetup;
  sdSetupState_ = state;
  sdSetupStateMs_ = nowMs;
  sdSetupLastRenderMs_ = 0;
  cardReaderMode_ = state == SdSetupState::CardReader ||
                    (cardReaderMode_ && state != SdSetupState::Downloading && state != SdSetupState::Done);
  lastActivityMs_ = nowMs;
  if (state_ == AppState::Menu) {
    renderWizardPage();
  }
}

void App::fillSdCardSetupView(nano::WizardView &view) {
  view.stepCount = 0;
  view.body = nano::WizardBody::Message;
  view.backId = nano::kNoTarget;
  view.nextId = kWizardNext;
  view.extraId = nano::kNoTarget;
  switch (sdSetupState_) {
    case SdSetupState::NeedsFormat:
      view.title = tr4(TrKey4::WizSdFormatTitle);
      view.subtitle = tr4(TrKey4::WizSdFormatSub);
      view.nextLabel = tr4(TrKey4::WizSdFormat);
      view.extraId = kWizardExtra;
      view.extraLabel = tr4(TrKey4::WizSdSkip);
      break;
    case SdSetupState::ConfirmFormat:
      view.title = tr4(TrKey4::WizSdFormatTitle);
      view.subtitle = tr4(TrKey4::WizSdConfirmSub);
      view.nextLabel = tr4(TrKey4::WizSdFormatYes);
      view.backId = kWizardBack;
      break;
    case SdSetupState::Formatting:
      view.body = nano::WizardBody::Loading;
      view.title = tr4(TrKey4::WizSdFormatting);
      view.subtitle = tr4(TrKey4::WizSdFormattingSub);
      view.phase = millis() / 40;
      view.nextId = nano::kNoTarget;
      break;
    case SdSetupState::FormatFailed:
      view.title = tr4(TrKey4::WizSdFailedTitle);
      view.subtitle = tr4(TrKey4::WizSdFailedSub);
      view.nextLabel = tr4(TrKey4::WizSdFormat);
      view.extraId = kWizardExtra;
      view.extraLabel = tr4(TrKey4::WizSdSkip);
      break;
    case SdSetupState::NeedsAssets:
      view.title = tr4(TrKey4::SdSetupAssetsTitle);
      view.subtitle = tr4(TrKey4::SdSetupAssetsSub);
      view.nextLabel = tr4(TrKey4::SdSetupDownload);
      view.extraId = kWizardExtra;
      view.extraLabel = tr4(TrKey4::SdSetupDecline);
      break;
    case SdSetupState::NoWifi:
      view.title = tr4(TrKey4::SdSetupNoWifiTitle);
      view.subtitle = tr4(TrKey4::SdSetupNoWifiSub);
      view.nextLabel = tr4(TrKey4::CardReaderTitleShort);
      break;
    case SdSetupState::Downloading: {
      view.body = nano::WizardBody::Loading;
      view.title = tr4(TrKey4::SdSetupDownloading);
      view.subtitle = tr4(TrKey4::SdSetupDownloadingSub);
      view.nextId = nano::kNoTarget;
      view.phase = millis() / 40;
      if (bookDownloadInProgress_) {
        view.status = tr4(TrKey4::SdSetupBooks);
      } else {
        const uint8_t total = g_fontDlTotal.load();
        const uint8_t done = g_fontDlDone.load();
        view.status = String(tr4(TrKey4::SdSetupFonts)) + " " + String(done) + "/" + String(total);
        if (total > 0) {
          view.percent = done * 100 / total;
        }
      }
      break;
    }
    case SdSetupState::DownloadFailed:
      view.title = tr4(TrKey4::SdSetupFailedTitle);
      view.subtitle = tr4(TrKey4::SdSetupFailedSub);
      view.nextLabel = tr4(TrKey4::SdSetupRetry);
      view.extraId = kWizardExtra;
      view.extraLabel = tr4(TrKey4::SdSetupDecline);
      break;
    case SdSetupState::Done:
      view.title = tr4(TrKey4::SdSetupDoneTitle);
      view.subtitle = tr4(TrKey4::SdSetupDoneSub);
      view.nextId = nano::kNoTarget;
      break;
    case SdSetupState::CardReader: {
      view.body = nano::WizardBody::Chips;
      view.title = tr4(TrKey4::CardReaderTitle);
      String details;
      if (storageReady_) {
        const uint64_t total = SD_MMC.totalBytes();
        const uint64_t used = SD_MMC.usedBytes();
        details = String(tr4(TrKey4::CardReaderCard)) + " " + sdSizeLabel(total) + ", " +
                  tr4(TrKey4::CardReaderFree) + " " + sdSizeLabel(total > used ? total - used : 0) + "  |  " +
                  tr4(TrKey4::CardReaderBooks) + " " + String(storage_.bookCount()) + "  |  " +
                  tr4(TrKey4::CardReaderFonts) + " " + String(sdFontsOnCard()) + "/" + String(sdFontsTotal());
      }
      view.subtitle = details;
      view.footer = tr4(TrKey4::CardReaderFooter);
      view.nextId = nano::kNoTarget;
      auto addChip = [&](int id, const String &label) {
        nano::WizardChip chip;
        chip.id = id;
        chip.label = label;
        view.chips.push_back(chip);
      };
#if RSVP_USB_TRANSFER_ENABLED
      addChip(kCardReaderUsb, tr4(TrKey4::CardReaderUsb));
#endif
      addChip(kCardReaderPhone, tr4(TrKey4::CardReaderPhone));
      addChip(kCardReaderAssets, tr4(TrKey4::SdSetupDownload));
      addChip(kCardReaderFormat, tr4(TrKey4::WizSdFormat));
      break;
    }
  }
}

void App::selectSdCardSetup(int hit, uint32_t nowMs) {
  if (hit == kWizardBack) {
    sdCardSetupBack(nowMs);
    return;
  }
  switch (sdSetupState_) {
    case SdSetupState::NeedsFormat:
    case SdSetupState::FormatFailed:
      if (hit == kWizardExtra) {
        // Skipped: the reader runs without the card, as before this check.
        Serial.println("[sdsetup] format skipped");
        cardReaderMode_ = false;
        menuScreen_ = MenuScreen::Main;
        state_ = AppState::Booting;
        return;
      }
      if (hit == kWizardNext) {
        sdSetupFormatFromReader_ = false;
        openSdCardSetup(SdSetupState::ConfirmFormat, nowMs);
      }
      return;
    case SdSetupState::ConfirmFormat:
      if (hit != kWizardNext) {
        return;
      }
      openSdCardSetup(SdSetupState::Formatting, nowMs);
      storageReady_ = storage_.formatCard();
      if (!storageReady_) {
        openSdCardSetup(SdSetupState::FormatFailed, nowMs);
        return;
      }
      // A fresh card: nothing of the reader's on it yet.
      refreshFontPackComplete();
      openSdCardSetup(SdSetupState::NeedsAssets, millis());
      return;
    case SdSetupState::NeedsAssets:
    case SdSetupState::DownloadFailed:
      if (hit == kWizardExtra) {
        Serial.println("[sdsetup] download declined: card-reader mode");
        openSdCardSetup(SdSetupState::CardReader, nowMs);
      } else if (hit == kWizardNext) {
        startSdSetupDownload(nowMs);
      }
      return;
    case SdSetupState::NoWifi:
      if (hit == kWizardNext) {
        openSdCardSetup(SdSetupState::CardReader, nowMs);
      }
      return;
    case SdSetupState::CardReader:
      switch (hit) {
#if RSVP_USB_TRANSFER_ENABLED
        case kCardReaderUsb:
          enterUsbTransfer(nowMs);
          return;
#endif
        case kCardReaderPhone:
          enterCompanionSync(nowMs);
          return;
        case kCardReaderAssets:
          startSdSetupDownload(nowMs);
          return;
        case kCardReaderFormat:
          sdSetupFormatFromReader_ = true;
          openSdCardSetup(SdSetupState::ConfirmFormat, nowMs);
          return;
        default:
          return;
      }
    default:
      return;
  }
}

// PWR tap / Wstecz: only the format confirmation has somewhere to go back to.
void App::sdCardSetupBack(uint32_t nowMs) {
  if (sdSetupState_ != SdSetupState::ConfirmFormat) {
    return;
  }
  if (sdSetupFormatFromReader_) {
    openSdCardSetup(SdSetupState::CardReader, nowMs);
  } else {
    openSdCardSetup(storageReady_ ? SdSetupState::NeedsAssets : SdSetupState::NeedsFormat, nowMs);
  }
}

void App::startSdSetupDownload(uint32_t nowMs) {
  const OtaUpdater::Config config = preferredOtaConfig();
  if (!otaUpdater_.isConfigured(config)) {
    openSdCardSetup(SdSetupState::NoWifi, nowMs);
    return;
  }
  // Station mode for the downloads: the phone network has to go.
  stopAutoSyncAccessPoint("card files download");
  sdSetupBooksStarted_ = false;
  g_fontDlDone = 0;
  g_fontDlTotal = sdFontsTotal() - sdFontsOnCard();
  if (!fontDownloadInProgress_ && !startBackgroundFontDownload(config)) {
    openSdCardSetup(SdSetupState::DownloadFailed, nowMs);
    return;
  }
  Serial.println("[sdsetup] downloading the reader's files");
  openSdCardSetup(SdSetupState::Downloading, nowMs);
}

void App::updateSdCardSetup(uint32_t nowMs) {
  if (state_ != AppState::Menu || menuScreen_ != MenuScreen::SdCardSetup) {
    return;
  }
  if (sdSetupState_ == SdSetupState::Done) {
    if (nowMs - sdSetupStateMs_ >= kSdSetupDoneRestartMs) {
      Serial.println("[sdsetup] card ready, restarting");
      delay(100);
      ESP.restart();
    }
    return;
  }
  if (sdSetupState_ != SdSetupState::Downloading) {
    return;
  }
  if (fontDownloadInProgress_ || bookDownloadInProgress_) {
    if (nowMs - sdSetupLastRenderMs_ >= kSdSetupRenderMs) {
      sdSetupLastRenderMs_ = nowMs;
      renderWizardPage();
    }
    return;
  }
  // Both tasks idle: fonts finished (pollFontDownloadResult refreshed
  // fontPackComplete_), maybe the starter books too.
  if (!fontPackComplete_) {
    Serial.printf("[sdsetup] download incomplete, %u/%u fonts\n", sdFontsOnCard(), sdFontsTotal());
    openSdCardSetup(SdSetupState::DownloadFailed, nowMs);
    return;
  }
  // An empty card also gets the starter books of the reader's language.
  if (!sdSetupBooksStarted_ && storage_.bookCount() == 0) {
    sdSetupBooksStarted_ = true;
    if (startBackgroundBookDownload(preferredOtaConfig())) {
      return;
    }
  }
  markCardKnown();
  cardReaderMode_ = false;
  openSdCardSetup(SdSetupState::Done, nowMs);
}

// Back from USB or phone sending in card-reader mode: the files may have
// arrived that way.
void App::returnToCardReader(uint32_t nowMs) {
  if (cardReaderDimmed_) {
    cardReaderDimmed_ = false;
    display_.setBrightnessPercent(currentBrightnessPercent());
  }
  if (!storageReady_) {
    storageReady_ = storage_.begin();
  }
  if (!storageReady_) {
    sdSetupFormatFromReader_ = true;
    menuScreen_ = MenuScreen::SdCardSetup;
    sdSetupState_ = SdSetupState::NeedsFormat;
    setState(AppState::Menu, nowMs);
    return;
  }
  storage_.refreshBooks();
  refreshFontPackComplete();
  menuScreen_ = MenuScreen::SdCardSetup;
  if (fontPackComplete_) {
    markCardKnown();
    cardReaderMode_ = false;
    sdSetupState_ = SdSetupState::Done;
  } else {
    sdSetupState_ = SdSetupState::CardReader;
  }
  sdSetupStateMs_ = nowMs;
  lastActivityMs_ = nowMs;
  setState(AppState::Menu, nowMs);
}

void App::updateCardReaderDim(uint32_t nowMs) {
  if (!cardReaderMode_ || cardReaderDimmed_) {
    return;
  }
  if (nowMs - lastActivityMs_ >= kCardReaderDimAfterMs) {
    cardReaderDimmed_ = true;
    display_.setBrightnessPercent(kCardReaderDimPercent);
    Serial.println("[sdsetup] card-reader mode idle, screen dimmed");
  }
}

// A touch on the dimmed screen only brings the brightness back; the rest
// of that touch (up to the finger lifting) does nothing.
bool App::wakeCardReaderScreen(const TouchEvent &event) {
  if (cardReaderDimmed_) {
    cardReaderDimmed_ = false;
    display_.setBrightnessPercent(currentBrightnessPercent());
    cardReaderSwallowTouch_ = event.phase != TouchPhase::End;
    return true;
  }
  if (cardReaderSwallowTouch_) {
    cardReaderSwallowTouch_ = event.phase != TouchPhase::End;
    return true;
  }
  return false;
}
