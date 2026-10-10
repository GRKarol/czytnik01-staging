// Screensaver scenes that are not cell grids: Book, Words, Waves.
// Included at the end of App.cpp (after AppNano.inl, whose cover helpers
// the Book scene reuses). Painting lives in ui/NanoScreens.cpp so
// tools/nanosim renders the same frames.

App::ScreensaverMode App::screensaverModeFromStored(uint8_t stored) {
  switch (stored) {
    case static_cast<uint8_t>(ScreensaverMode::Life):
      return ScreensaverMode::Life;
    case static_cast<uint8_t>(ScreensaverMode::Maze):
      return ScreensaverMode::Maze;
    case static_cast<uint8_t>(ScreensaverMode::Voronoi):
      return ScreensaverMode::Voronoi;
    case static_cast<uint8_t>(ScreensaverMode::ScreenOff):
      return ScreensaverMode::ScreenOff;
    case static_cast<uint8_t>(ScreensaverMode::Words):
      return ScreensaverMode::Words;
    case static_cast<uint8_t>(ScreensaverMode::Waves):
      return ScreensaverMode::Waves;
    case static_cast<uint8_t>(ScreensaverMode::Book):
    default:
      // Also the retired Stars (4) and Matrix (5).
      return ScreensaverMode::Book;
  }
}

App::ScreensaverMode App::nextScreensaverMode(ScreensaverMode mode) {
  switch (mode) {
    case ScreensaverMode::Book:
      return ScreensaverMode::Words;
    case ScreensaverMode::Words:
      return ScreensaverMode::Waves;
    case ScreensaverMode::Waves:
      return ScreensaverMode::Life;
    case ScreensaverMode::Life:
      return ScreensaverMode::Maze;
    case ScreensaverMode::Maze:
      return ScreensaverMode::Voronoi;
    case ScreensaverMode::Voronoi:
      return ScreensaverMode::ScreenOff;
    case ScreensaverMode::ScreenOff:
    default:
      return ScreensaverMode::Book;
  }
}

bool App::screensaverIsScene() const {
  return screensaverMode_ == ScreensaverMode::Book || screensaverMode_ == ScreensaverMode::Words ||
         screensaverMode_ == ScreensaverMode::Waves;
}

uint32_t App::standbyFrameIntervalMs() const {
  switch (screensaverMode_) {
    case ScreensaverMode::Waves:
      return 80;
    case ScreensaverMode::Words:
      return 100;
    case ScreensaverMode::Book:
      return 250;
    default:
      return kStandbyFrameMs;
  }
}

namespace {

// Splits `text` at spaces into `out` (the Words fallback without a book).
void appendSaverWords(const String &text, std::vector<String> &out) {
  int start = 0;
  const int length = static_cast<int>(text.length());
  while (start < length) {
    int end = text.indexOf(' ', start);
    if (end < 0) end = length;
    if (end > start) out.push_back(text.substring(start, end));
    start = end + 1;
  }
}

}  // namespace

void App::seedStandbyScene(uint32_t nowMs) {
  (void)nowMs;
  standbyLifeCells_.clear();
  standbyLifeNextCells_.clear();
  standbyScreensaverDimCells_.clear();
  standbyMazeVisited_.clear();
  standbyMazeStack_.clear();
  standbyVoronoiX_.clear();
  standbyVoronoiY_.clear();
  standbyVoronoiDx_.clear();
  standbyVoronoiDy_.clear();
  standbyLaneWords_.clear();
  standbySceneFrame_ = 0;
  standbySceneSeeded_ = true;

  if (screensaverMode_ != ScreensaverMode::Words) {
    return;
  }

  // Three lanes: the middle one continues the book from the reading
  // position, the outer two carry the passages right after it.
  constexpr size_t kLaneCount = 3;
  constexpr size_t kWordsPerLane = 48;
  constexpr size_t kMaxWordBytes = 18;
  std::vector<String> pool;
  pool.reserve(kLaneCount * kWordsPerLane);
  if (usingStorageBook_ && reader_.wordCount() > 0) {
    const size_t total = reader_.wordCount();
    for (size_t i = reader_.currentIndex(); i < total && pool.size() < kLaneCount * kWordsPerLane; ++i) {
      String word = reader_.wordAt(i);
      word.trim();
      if (!word.isEmpty() && word.length() <= kMaxWordBytes) pool.push_back(word);
    }
  }
  if (pool.size() < kLaneCount * 4) {
    pool.clear();
    appendSaverWords(tr4(TrKey4::SaverWordsFallback), pool);
  }

  standbyLaneWords_.assign(kLaneCount, {});
  const size_t perLane = std::max<size_t>(1, pool.size() / kLaneCount);
  // Lane order in the pool: middle (index 1) first, then top, then bottom.
  const size_t laneForSlice[kLaneCount] = {1, 0, 2};
  for (size_t slice = 0; slice < kLaneCount; ++slice) {
    const size_t begin = slice * perLane;
    const size_t end = slice + 1 == kLaneCount ? pool.size() : std::min(pool.size(), begin + perLane);
    std::vector<String> &lane = standbyLaneWords_[laneForSlice[slice]];
    for (size_t i = begin; i < end; ++i) lane.push_back(pool[i]);
    if (lane.empty()) lane = pool;
  }
}

void App::renderStandbyScene(uint32_t nowMs, const String &hint, uint8_t hintAlpha, const String &label,
                             uint8_t labelAlpha) {
  nano::SaverOverlay overlay;
  overlay.hint = hint;
  overlay.hintAlpha = hintAlpha;
  overlay.label = label;
  overlay.labelAlpha = labelAlpha;

  switch (screensaverMode_) {
    case ScreensaverMode::Words: {
      nano::SaverWordsView view;
      view.overlay = overlay;
      // Top: small and faint, middle: large with the centre word in the
      // accent (the reading screen's focus spot), bottom: in between.
      const int laneY[] = {32, 84, 132};
      const uint8_t laneSize[] = {1, 3, 2};
      const uint8_t laneAlpha[] = {90, 235, 140};
      const uint8_t laneSpeed[] = {1, 3, 2};
      for (size_t i = 0; i < standbyLaneWords_.size() && i < 3; ++i) {
        nano::SaverLane lane;
        lane.words = standbyLaneWords_[i];
        lane.y = laneY[i];
        lane.size = laneSize[i];
        lane.alpha = laneAlpha[i];
        lane.offset = standbySceneFrame_ * laneSpeed[i];
        lane.markCentre = i == 1;
        view.lanes.push_back(lane);
      }
      nano::paintSaverWords(display_, view);
      return;
    }
    case ScreensaverMode::Waves: {
      nano::SaverWavesView view;
      view.overlay = overlay;
      view.phase = standbySceneFrame_;
      nano::paintSaverWaves(display_, view);
      return;
    }
    case ScreensaverMode::Book:
    default: {
      nano::SaverBookView view;
      view.overlay = overlay;
      view.hasBook = usingStorageBook_;
      if (view.hasBook) {
        view.title = storage_.bookDisplayName(currentBookIndex_);
        view.author = storage_.bookAuthorName(currentBookIndex_);
        if (view.author.isEmpty()) view.author = tr3(TrKey3::NanoUnknownAuthor);
        view.progressPercent = readingProgressPercent();
        view.progressLabel = String(static_cast<unsigned>(view.progressPercent)) + "%";
        view.coverColor = nanoCoverColor(currentBookPath_);
        view.coverInitials = nanoInitials(view.title);
        view.cover = BookExtras::picture(currentBookPath_, BookExtras::Picture::Cover);
      } else {
        view.title = tr4(TrKey4::SaverNoBook);
      }
      // Slow Lissajous drift (periods ~50 s and ~31 s) so the card never
      // sits on the same pixels for long.
      const float t = static_cast<float>(nowMs - standbyEnteredMs_) / 1000.0f;
      view.driftX = static_cast<int>(sinf(t * 6.2831853f / 50.0f) * 80.0f);
      view.driftY = static_cast<int>(sinf(t * 6.2831853f / 31.0f) * 10.0f);
      nano::paintSaverBook(display_, view);
      return;
    }
  }
}

// ─── Screen off (PWR) and the screensaver question ──────────────────────────

namespace {
constexpr int kStandbyAskNo = 1;
constexpr int kStandbyAskYes = 2;
}  // namespace

void App::turnScreenOffFromPower(uint32_t nowMs) {
  if (state_ == AppState::Booting || state_ == AppState::UsbTransfer || state_ == AppState::Sleeping ||
      state_ == AppState::Standby || powerOffStarted_) {
    return;
  }
  // The first-run wizard and the card set-up run downloads and the pairing
  // network that a dark screen would stop half way.
  if (savedWizardStep_ != 0xFF || menuScreen_ == MenuScreen::SdCardSetup || cardReaderMode_) {
    Serial.println("[app] PWR: screen stays on during set-up");
    return;
  }
  if (showingHelpPopup_) {
    dismissHelpPopup(nowMs);
  }
  Serial.println("[app] PWR: screen off");
  enterStandby(nowMs, true);
}

void App::stopActivityForScreenOff() {
  // Reading pauses in enterStandby(); here the phone network goes too.
  firstSessionSyncHold_ = false;
  if (autoSyncActive_) {
    stopAutoSyncAccessPoint("screen off");
    standbyStoppedWifi_ = true;
  }
}

void App::renderStandbyAsk(uint32_t nowMs) {
  const uint32_t elapsed = nowMs - standbyPhaseMs_;
  const int secondsLeft =
      static_cast<int>((kStandbyAskMs > elapsed ? kStandbyAskMs - elapsed : 0) + 999) / 1000;
  if (secondsLeft == standbyAskShownSeconds_ && standbyAskPressed_ < 0) {
    return;
  }
  standbyAskShownSeconds_ = secondsLeft;

  nano::ConfirmView view;
  view.question = tr4(TrKey4::SaverAskTitle);
  view.detail = String(tr4(TrKey4::SaverAskCountdown)) + " " + String(secondsLeft) + " s";
  nano::ConfirmView::Action no;
  no.id = kStandbyAskNo;
  no.label = tr4(TrKey4::SaverAskNo);
  view.actions.push_back(no);
  nano::ConfirmView::Action yes;
  yes.id = kStandbyAskYes;
  yes.label = tr4(TrKey4::SaverAskYes);
  view.actions.push_back(yes);

  const bool classicColors = navMode_ != NavMode::Modern;
  if (classicColors) {
    display_.overrideNanoPalette(DisplayManager::kNanoPaletteClassic, false);
  }
  display_.setModernCardStyle(true);
  standbyAskTargets_.clear();
  NanoPanelSink sink(standbyAskTargets_, standbyAskPressed_);
  display_.nanoBeginFrame();
  nano::paintConfirm(display_, sink, view);
  display_.nanoEndFrame();
  if (classicColors) {
    display_.overrideNanoPalette(nanoPalette_, nanoOwnAccent_);
  }
}

void App::handleStandbyTouch(const TouchEvent &event, uint32_t nowMs) {
  if (standbyPhase_ == StandbyPhase::Ask) {
    auto targetAt = [this](uint16_t x, uint16_t y) {
      for (const auto &target : standbyAskTargets_) {
        if (target.first.contains(x, y)) {
          return target.second;
        }
      }
      return -1;
    };
    if (event.phase == TouchPhase::Start) {
      standbyAskPressed_ = targetAt(event.x, event.y);
      if (standbyAskPressed_ >= 0) {
        standbyAskShownSeconds_ = -1;
        renderStandbyAsk(nowMs);
      }
      return;
    }
    if (event.phase != TouchPhase::End) {
      return;
    }
    const int pressed = standbyAskPressed_;
    standbyAskPressed_ = -1;
    standbyAskShownSeconds_ = -1;
    if (pressed >= 0 && targetAt(event.x, event.y) == pressed) {
      if (pressed == kStandbyAskYes) {
        Serial.println("[app] screensaver: screen off (asked)");
        standbyPhase_ = StandbyPhase::Off;
        standbyPhaseMs_ = nowMs;
        stopActivityForScreenOff();
        seedStandbyScreenOff(nowMs);
      } else {
        exitStandby(nowMs);
      }
      return;
    }
    renderStandbyAsk(nowMs);
    return;
  }

  if (standbyPhase_ == StandbyPhase::Dim) {
    // The screen is still lit, only dim: one tap brings it back.
    if (event.phase == TouchPhase::End) {
      exitStandby(nowMs);
    }
    return;
  }

  // Dark screen: two quick taps wake it, so a pocket doesn't.
  if (event.phase == TouchPhase::Start) {
    standbyTouchDown_ = true;
    standbyTouchStartMs_ = nowMs;
    return;
  }
  if (event.phase != TouchPhase::End || !standbyTouchDown_) {
    return;
  }
  standbyTouchDown_ = false;
  if (nowMs - standbyTouchStartMs_ > kStandbyTapMaxMs) {
    standbyLastTapMs_ = 0;
    return;
  }
  if (standbyLastTapMs_ != 0 && nowMs - standbyLastTapMs_ <= kStandbyDoubleTapMs) {
    standbyLastTapMs_ = 0;
    Serial.println("[app] double tap: screen on");
    exitStandby(nowMs);
    return;
  }
  standbyLastTapMs_ = nowMs;
}
