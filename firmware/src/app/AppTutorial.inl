// Samouczek: six pages in the Nano skin (ui/NanoScreens.cpp paintTutorial).
// All pages live on MenuScreen::TutorialStep1; tutorialPage_ picks the
// page. The other TutorialStep* values stay only so older checks keep
// matching. Included at the end of App.cpp.

namespace {

constexpr uint8_t kTutorialPageCount = 6;
constexpr int kTutorialBack = 1;
constexpr int kTutorialNext = 2;
constexpr int kTutorialSkip = 3;

}  // namespace

void App::openTutorialStep1() {
  tutorialPage_ = 0;
  menuScreen_ = MenuScreen::TutorialStep1;
  renderTutorialStep();
}

void App::renderTutorialStep() {
  if (tutorialPage_ >= kTutorialPageCount) {
    tutorialPage_ = kTutorialPageCount - 1;
  }
  menuScreen_ = MenuScreen::TutorialStep1;

  nano::TutorialView view;
  view.caption = tr3(TrKey3::TutorialLabel);
  view.page = tutorialPage_;
  view.pageCount = kTutorialPageCount;
  view.artWord = tr4(TrKey4::TutWord);
  view.artStart = uiText(UiText::Read);
  view.artUnit = tr3(TrKey3::NanoWpmUnit);
  view.artTile = tr3(TrKey3::SpeedLabel);
  const std::vector<nano::RailTab> tabs = nanoRailTabs();
  for (size_t i = 0; i < 4 && i < tabs.size(); ++i) {
    view.artTabs[i] = tabs[i].label;
  }
  switch (tutorialPage_) {
    case 0:
      view.art = nano::TutorialArt::Rsvp;
      view.title = tr4(TrKey4::TutTitleRsvp);
      view.body = tr4(TrKey4::TutBodyRsvp);
      break;
    case 1:
      view.art = nano::TutorialArt::Start;
      view.title = tr4(TrKey4::TutTitleStart);
      view.body = tr4(TrKey4::TutBodyStart);
      break;
    case 2:
      view.art = nano::TutorialArt::Speed;
      view.title = tr4(TrKey4::TutTitleSpeed);
      view.body = tr4(TrKey4::TutBodySpeed);
      break;
    case 3:
      view.art = nano::TutorialArt::Scrub;
      view.title = tr4(TrKey4::TutTitleMove);
      view.body = tr4(TrKey4::TutBodyMove);
      break;
    case 4:
      view.art = nano::TutorialArt::Menu;
      view.title = tr4(TrKey4::TutTitleMenu);
      view.body = tr4(TrKey4::TutBodyMenu);
      break;
    default:
      view.art = nano::TutorialArt::Help;
      view.title = tr4(TrKey4::TutTitleHelp);
      view.body = tr4(TrKey4::TutBodyHelp);
      break;
  }
  const bool last = tutorialPage_ + 1 >= kTutorialPageCount;
  view.backId = tutorialPage_ > 0 ? kTutorialBack : nano::kNoTarget;
  view.backLabel = uiText(UiText::Back);
  view.nextId = kTutorialNext;
  view.nextLabel = last ? String(tr4(TrKey4::TutDone)) : String(tr3(TrKey3::NextLabel));
  view.skipId = last ? nano::kNoTarget : kTutorialSkip;
  view.skipLabel = tr2(TrKey2::SkipForNow);

  // Menu palette in the Official mode, the classic one elsewhere (the
  // tutorial is reachable from every nav mode).
  const bool classicColors = navMode_ != NavMode::Modern;
  if (classicColors) {
    display_.overrideNanoPalette(DisplayManager::kNanoPaletteClassic, false);
  }
  tutorialTargets_.clear();
  NanoPanelSink sink(tutorialTargets_, tutorialPressedId_);
  nano::paintTutorial(display_, sink, view);
  if (classicColors) {
    display_.overrideNanoPalette(nanoPalette_, nanoOwnAccent_);
  }
}

void App::handleTutorialTouchAt(uint16_t x, uint16_t y, uint32_t nowMs) {
  // An off-center tap on the panel's edge can arrive as two or three taps in
  // a row; each one used to turn a page. One turn per 400 ms.
  constexpr uint32_t kTutorialTurnGuardMs = 400;
  // Counted from when the new page finished drawing; a tap queued while it
  // drew is older than that, hence the signed difference.
  if (tutorialTurnMs_ != 0 && static_cast<int32_t>(nowMs - tutorialTurnMs_) < static_cast<int32_t>(kTutorialTurnGuardMs)) {
    return;
  }
  int hit = nano::kNoTarget;
  for (const auto &target : tutorialTargets_) {
    const ui::Rect &r = target.first;
    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
      hit = target.second;
      break;
    }
  }
  switch (hit) {
    case kTutorialBack:
      previousTutorialStep(nowMs);
      return;
    case kTutorialSkip:
      finishTutorial(nowMs);
      return;
    case kTutorialNext:
      handleTutorialTap(nowMs);
      return;
    default:
      // A tap on the page itself also turns it (the whole screen used to
      // be one "next" button; people still expect that).
      if (y > 30 && y < BoardConfig::DISPLAY_HEIGHT - 40) {
        handleTutorialTap(nowMs);
      }
      return;
  }
}

void App::handleTutorialTap(uint32_t nowMs) {
  if (tutorialPage_ + 1 >= kTutorialPageCount) {
    finishTutorial(nowMs);
    return;
  }
  ++tutorialPage_;
  renderTutorialStep();
  tutorialTurnMs_ = millis();
}

void App::previousTutorialStep(uint32_t nowMs) {
  (void)nowMs;
  if (tutorialPage_ > 0) {
    --tutorialPage_;
  }
  renderTutorialStep();
  tutorialTurnMs_ = millis();
}
