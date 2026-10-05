// firmware/src/plugins/builtin/DictaphonePlugin.cpp
#include "plugins/builtin/DictaphonePlugin.h"
#include <Arduino.h>

#include <string.h>
#include <stdio.h>
#include <algorithm>

namespace {

// Menu items for library context menu (shown via touch actions)
static constexpr uint8_t kLibraryVisibleRows = 5;

// Left-edge tap width reserved as a "back to Main" zone on every row, so
// the library is always escapable without deleting anything or hunting
// for the boot button. Comfortably smaller than the right-edge delete
// zone (120px) since it doesn't need to be the primary target.
static constexpr uint16_t kLibraryBackZoneWidth = 64;

// Playing-screen layout — must match the constants of the same names in
// DeviceServicesBridge.cpp's bridgeRenderPlaybackControls(), since that is
// what actually draws this screen and these are the numbers used to hit-test
// it. Kept as separate constants rather than shared ones because a plugin
// binary and the firmware bridge are built and shipped independently.
static constexpr uint16_t kPlayingRowY = 20;
static constexpr uint16_t kPlayingRowH = 46;
static constexpr uint16_t kPlayingButtonW = 145;
static constexpr uint16_t kPlayingButtonGap = 4;
static constexpr uint16_t kPlayingButtonX0 = 4;
static constexpr uint16_t kPlayingSliderX = 4;
static constexpr uint16_t kPlayingSliderY = 68;
static constexpr uint16_t kPlayingSliderW = 632;
static constexpr uint16_t kPlayingSliderH = 104;

// Mirrors DisplayManager::sliderTrackRectFor()'s fixed formula for the
// track's horizontal extent (trackX = button.x + 24, trackW = button.width
// - 48) so applySeekTouchX() can convert a touch x into a slider value
// without the plugin SDK needing to expose display internals. Only the x
// axis matters here since dragging is horizontal.
static constexpr int kSliderTrackMarginX = 24;

// Singleton instance
DictaphoneCore* s_instance = nullptr;

// ─── Localization ───────────────────────────────────────────────────────────
//
// The plugin can't include app/Localization.h (it's built to stay decoupled
// from the app's own headers — see PluginDisplayService::languageIndex(),
// which is how it learns the current language without that dependency), so
// this enum is its own small copy of the same 6-language key ordering
// (0=English, 1=Spanish, 2=French, 3=German, 4=Romanian, 5=Polish). The
// actual strings live in tools/translations.csv (as DictStr.* rows) and are
// looked up through PluginDisplayService::pluginTr() -- the bridge resolves
// `key` against the same generated table the app's tr()/tr2()/tr3() use, so
// there is only one place to edit a translation.
enum class DictStr : uint8_t {
    Record,
    Library,
    LibraryTitle,
    NoRecordings,
    TapToGoBack,
    Rename,
    Save,
    Backspace,
    Cancel,
    Delete,
    ErrorTitle,
    MicUnavailable,
    RecordingFailed,
    TryAgain,
    PeakAbbrev,
    Stop,
    CardWriteFailed,
    Recording,
};

const char* dictText(PluginDisplayService* display, DictStr key, int lang) {
    return display->pluginTr(static_cast<uint16_t>(key), lang);
}

}  // namespace

// ─── DictaphoneCore Implementation ─────────────────────────────────────────

DictaphoneCore::DictaphoneCore(PluginDisplayService* display,
                               PluginAudioService* audio,
                               PluginStorageService* storage)
    : display_(display), audio_(audio), storage_(storage) {}

bool DictaphoneCore::begin() {
    // Ensure recordings directory exists
    if (storage_ && storage_->mkdir) {
        storage_->mkdir("recordings");
    }

    scanRecordings();
    return true;
}

void DictaphoneCore::update(uint32_t nowMs) {
    lastUpdateMs_ = nowMs;

    // Auto-stop recording at max duration (handled by AudioRecorder but also check here)
    if (screen_ == Screen::Recording && audio_ && audio_->isRecording) {
        if (!audio_->isRecording()) {
            // Stopped on its own: the length limit, or the recorder gave up
            // (no file on the card, codec silent). Only a take that reached
            // the card goes on the list.
            finishRecording(nowMs);
        }
    }

    if (screen_ == Screen::Error && nowMs - errorShownMs_ >= kErrorShowMs) {
        goToScreen(Screen::Main);
    }

    // Auto-return when playback finishes
    if (screen_ == Screen::Playing && audio_ && audio_->isPlaying) {
        if (!audio_->isPlaying()) {
            goToScreen(Screen::Library);
        }
    }
}

void DictaphoneCore::handleButton(const PluginButtonEvent* event) {
    if (!event || !event->pressed) return;

    // Only the boot button (id 0) ever reaches a plugin: App.cpp treats a
    // short press of the power button as a global "exit plugin" and
    // consumes it before it is ever forwarded (see PluginLoader::forwardButton
    // callers in App::update()). Every real action here must therefore be
    // reachable by touch — the boot button is a convenience shortcut only.
    if (event->buttonId != 0) return;

    switch (screen_) {
        case Screen::Main:
            // Boot — open library
            goToScreen(Screen::Library);
            break;

        case Screen::Recording:
            // Boot — stop recording
            stopRecording();
            break;

        case Screen::Library:
            // Boot — back to the record screen. Direct row taps already
            // cover selection, so there's nothing useful left for Boot to
            // cycle — and without this, a non-empty library had no way
            // back to Main short of deleting every recording (see the
            // touch-based back zone in handleTouch for the primary path).
            goToScreen(Screen::Main);
            break;

        case Screen::Playing:
            // Boot — stop playback
            stopPlayback();
            break;

        case Screen::ConfirmDelete:
            // Boot — cancel (confirm is touch-only, right half of screen)
            goToScreen(Screen::Library);
            break;

        default:
            break;
    }
}

void DictaphoneCore::handleTouch(const PluginTouchEvent* event) {
    if (!event) return;

    // The Playing screen alone needs every touch phase (press + move +
    // release) to make the seek slider draggable — everything else here
    // only ever acts on tap-release.
    if (screen_ == Screen::Playing) {
        handlePlayingTouch(event);
        return;
    }

    // Remember where this touch started so the Library screen can tell a
    // vertical swipe (scroll) from a tap (play/delete/back) once it ends —
    // see the Screen::Library case below.
    if (event->phase == 0) {
        touchStartX_ = event->x;
        touchStartY_ = event->y;
        return;
    }

    // Only handle touch end (tap)
    if (event->phase != 2) return;

    // See lastActionMs_'s comment — this drops the phantom second release
    // a real tap's contact bounce can produce, which was deleting two
    // recordings per confirm and occasionally re-triggering the delete
    // confirm screen on its own right after the library redraws.
    if (event->timestampMs - lastActionMs_ < kActionCooldownMs) return;
    lastActionMs_ = event->timestampMs;

    const uint16_t x = event->x;
    const uint16_t y = event->y;

    switch (screen_) {
        case Screen::Main: {
            // Left half = record button, right half = library button.
            int width = display_ && display_->logicalWidth ? display_->logicalWidth() : 640;
            if (x < static_cast<uint16_t>(width / 2)) {
                startRecording();
            } else {
                goToScreen(Screen::Library);
            }
            break;
        }

        case Screen::Recording: {
            // Tap anywhere to stop
            stopRecording();
            break;
        }

        case Screen::Error:
            goToScreen(Screen::Main);
            break;

        case Screen::Library: {
            if (recordingCount_ == 0) {
                // No recordings — tap anywhere to go back
                goToScreen(Screen::Main);
                return;
            }

            int height = display_ && display_->logicalHeight ? display_->logicalHeight() : 172;
            int width = display_ && display_->logicalWidth ? display_->logicalWidth() : 640;

            // A vertical swipe scrolls the list a page at a time instead of
            // being read as a tap on whatever row/zone it happened to land
            // on — without this, the library had no way to reach recordings
            // past the first kLibraryVisibleRows (5): libraryScrollTop_ was
            // set at construction and never touched again, so a 6th
            // recording was simply unreachable.
            {
                int deltaX = static_cast<int>(x) - static_cast<int>(touchStartX_);
                int deltaY = static_cast<int>(y) - static_cast<int>(touchStartY_);
                int absDeltaX = deltaX < 0 ? -deltaX : deltaX;
                int absDeltaY = deltaY < 0 ? -deltaY : deltaY;
                constexpr int kLibrarySwipeThresholdPx = 30;

                if (absDeltaY >= kLibrarySwipeThresholdPx && absDeltaY > absDeltaX) {
                    if (recordingCount_ > kLibraryVisibleRows) {
                        int maxScrollTop = recordingCount_ - kLibraryVisibleRows;
                        int next = static_cast<int>(libraryScrollTop_) +
                                   (deltaY < 0 ? kLibraryVisibleRows : -kLibraryVisibleRows);
                        if (next < 0) next = 0;
                        if (next > maxScrollTop) next = maxScrollTop;
                        libraryScrollTop_ = static_cast<uint8_t>(next);
                    }
                    return;
                }

                // A horizontal swipe on a row opens Rename for that
                // recording — the row-tap zone already means play, and the
                // right/left edges mean delete/back, so there was no gesture
                // left for rename until now. Uses the row under where the
                // swipe *started*, matching how a real finger drag reads.
                if (absDeltaX >= kLibrarySwipeThresholdPx && absDeltaX > absDeltaY &&
                    touchStartX_ >= kLibraryBackZoneWidth &&
                    touchStartX_ <= static_cast<uint16_t>(width - 120)) {
                    uint8_t swipeVisibleRows = static_cast<uint8_t>(recordingCount_ - libraryScrollTop_);
                    if (swipeVisibleRows > kLibraryVisibleRows) swipeVisibleRows = kLibraryVisibleRows;
                    if (swipeVisibleRows > 0) {
                        uint16_t swipeRowHeight = static_cast<uint16_t>(height / swipeVisibleRows);
                        uint8_t swipeRow = static_cast<uint8_t>(touchStartY_ / swipeRowHeight);
                        if (swipeRow >= swipeVisibleRows) swipeRow = swipeVisibleRows - 1;
                        uint8_t swipeIndex = libraryScrollTop_ + swipeRow;
                        if (swipeIndex < recordingCount_) {
                            openRename(swipeIndex);
                        }
                    }
                    return;
                }
            }

            // Left edge = back to the record screen. Without this, a
            // library with at least one recording had no way back to Main
            // except deleting recordings until the list was empty (the
            // only other exit) — tap-to-play and the right-edge delete
            // zone covered the rest of each row, but nothing covered "I
            // don't want to play or delete anything, just go back".
            if (x < kLibraryBackZoneWidth) {
                goToScreen(Screen::Main);
                return;
            }

            // Must match the row count drawLibrary() actually hands to
            // renderDeletableList() — that's how many rows are drawn on
            // screen, which can be fewer than kLibraryVisibleRows on the
            // last (partial) page.
            uint8_t visibleRows = static_cast<uint8_t>(recordingCount_ - libraryScrollTop_);
            if (visibleRows > kLibraryVisibleRows) visibleRows = kLibraryVisibleRows;
            if (visibleRows == 0) {
                goToScreen(Screen::Main);
                return;
            }

            uint16_t rowHeight = static_cast<uint16_t>(height / visibleRows);
            uint8_t tappedRow = static_cast<uint8_t>(y / rowHeight);
            if (tappedRow >= visibleRows) tappedRow = visibleRows - 1;
            uint8_t tappedIndex = libraryScrollTop_ + tappedRow;

            // Right edge = delete
            if (x > static_cast<uint16_t>(width - 120) && tappedIndex < recordingCount_) {
                deleteIndex_ = tappedIndex;
                goToScreen(Screen::ConfirmDelete);
                return;
            }

            // Tap on row = play
            if (tappedIndex < recordingCount_) {
                librarySelected_ = tappedIndex;
                startPlayback(tappedIndex);
            }
            break;
        }

        case Screen::ConfirmDelete: {
            int width = display_ && display_->logicalWidth ? display_->logicalWidth() : 640;
            // Left half = cancel, right half = confirm
            if (x < static_cast<uint16_t>(width / 2)) {
                goToScreen(Screen::Library);
            } else {
                deleteRecording(deleteIndex_);
                if (librarySelected_ >= recordingCount_ && recordingCount_ > 0) {
                    librarySelected_ = recordingCount_ - 1;
                }
                // A delete can shrink the list below the current scroll
                // position (e.g. deleting the last item on the last page) —
                // without this, the library would render empty rows past
                // the new end instead of snapping back to show what's left.
                if (recordingCount_ <= kLibraryVisibleRows) {
                    libraryScrollTop_ = 0;
                } else if (libraryScrollTop_ > recordingCount_ - kLibraryVisibleRows) {
                    libraryScrollTop_ = static_cast<uint8_t>(recordingCount_ - kLibraryVisibleRows);
                }
                goToScreen(Screen::Library);
            }
            break;
        }

        case Screen::Rename:
            handleRenameTouch(event);
            break;

        default:
            break;
    }
}

void DictaphoneCore::handlePlayingTouch(const PluginTouchEvent* event) {
    const uint16_t x = event->x;
    const uint16_t y = event->y;

    const bool onSlider =
        y >= kPlayingSliderY && y < static_cast<uint16_t>(kPlayingSliderY + kPlayingSliderH);

    if (onSlider) {
        if (event->phase == 0) {
            // Same contact-bounce guard as the tap-release path below,
            // applied to the *start* of a drag — without it, a phantom
            // press right after a real button tap immediately jumped
            // playback to whatever x it landed on, since a single touch
            // here is enough to seek (see applySeekTouchX() below).
            if (event->timestampMs - lastActionMs_ < kActionCooldownMs) {
                return;
            }
            draggingSeek_ = true;
            lastActionMs_ = event->timestampMs;
        }
        if (draggingSeek_) {
            applySeekTouchX(x);
        }
        if (event->phase == 2) {
            draggingSeek_ = false;
        }
        return;
    }

    draggingSeek_ = false;

    // Buttons act on release only, same as every other screen.
    if (event->phase != 2) return;
    if (event->timestampMs - lastActionMs_ < kActionCooldownMs) return;
    lastActionMs_ = event->timestampMs;

    if (y < kPlayingRowY || y >= static_cast<uint16_t>(kPlayingRowY + kPlayingRowH)) return;
    if (x < kPlayingButtonX0) return;

    uint16_t rel = static_cast<uint16_t>(x - kPlayingButtonX0);
    uint8_t col = static_cast<uint8_t>(rel / (kPlayingButtonW + kPlayingButtonGap));
    if (col > 3) col = 3;

    switch (col) {
        case 0: stopPlayback(); break;
        case 1: adjustVolume(-10); break;
        case 2: togglePausePlayback(); break;
        case 3: adjustVolume(10); break;
    }
}

void DictaphoneCore::applySeekTouchX(uint16_t x) {
    if (!audio_ || !audio_->playbackTotalMs || !audio_->playbackElapsedMs) return;

    const int trackX = kPlayingSliderX + kSliderTrackMarginX;
    const int trackW = static_cast<int>(kPlayingSliderW) - kSliderTrackMarginX * 2;
    if (trackW <= 0) return;

    int clampedX = static_cast<int>(x);
    if (clampedX < trackX) clampedX = trackX;
    if (clampedX > trackX + trackW) clampedX = trackX + trackW;

    uint32_t total = audio_->playbackTotalMs();
    if (total == 0) return;

    float ratio = static_cast<float>(clampedX - trackX) / static_cast<float>(trackW);
    uint32_t targetMs = static_cast<uint32_t>(ratio * static_cast<float>(total) + 0.5f);

    uint32_t current = audio_->playbackElapsedMs();
    int32_t delta = static_cast<int32_t>(targetMs) - static_cast<int32_t>(current);
    seekPlayback(delta);
}

void DictaphoneCore::draw() {
    if (!display_) return;

    switch (screen_) {
        case Screen::Main:          drawMain(); break;
        case Screen::Recording:     drawRecording(); break;
        case Screen::Library:       drawLibrary(); break;
        case Screen::Playing:       drawPlaying(); break;
        case Screen::Rename:        drawRename(); break;
        case Screen::ConfirmDelete: drawConfirmDelete(); break;
        case Screen::Error:         drawError(); break;
    }
}

// ─── Screen Drawing ─────────────────────────────────────────────────────────

void DictaphoneCore::drawMain() {
    if (!display_->renderButtonPair) return;

    const int lang = display_->languageIndex ? display_->languageIndex() : 0;
    const char* libraryWord = dictText(display_, DictStr::Library, lang);

    char rightLabel[32];
    if (recordingCount_ > 0) {
        snprintf(rightLabel, sizeof(rightLabel), "%s (%d)", libraryWord, recordingCount_);
    } else {
        snprintf(rightLabel, sizeof(rightLabel), "%s", libraryWord);
    }

    display_->renderButtonPair(dictText(display_, DictStr::Record, lang), PLUGIN_ICON_RECORD, false,
                                rightLabel, PLUGIN_ICON_BOOK);
}

void DictaphoneCore::drawRecording() {
    if (!display_->renderButtonPair) return;

    const int lang = display_->languageIndex ? display_->languageIndex() : 0;

    char timeBuf[8];
    uint32_t elapsed = 0;
    if (audio_ && audio_->recordingElapsedMs) {
        elapsed = audio_->recordingElapsedMs();
    }
    formatTime(elapsed, timeBuf, sizeof(timeBuf));

    // Peak input level appended to the label — the only way to tell "mic
    // is actually picking something up" from the device itself, without a
    // serial cable. Stays at "Pzm:0%" (or the equivalent abbreviation in the
    // active language) the whole recording if the ADC path is silent.
    uint8_t peak = 0;
    if (audio_ && audio_->recordingPeakLevel) {
        peak = audio_->recordingPeakLevel();
    }
    // The take is running: both halves stop it, and neither may read like
    // the Record button of the screen before (a second "record" tap ended
    // the take a moment after it began).
    char left[40];
    snprintf(left, sizeof(left), "%s  %s", dictText(display_, DictStr::Stop, lang), timeBuf);
    char right[40];
    snprintf(right, sizeof(right), "%s  %s %u%%", dictText(display_, DictStr::Recording, lang),
             dictText(display_, DictStr::PeakAbbrev, lang), static_cast<unsigned>(peak));

    display_->renderButtonPair(left, PLUGIN_ICON_STOP, true, right, PLUGIN_ICON_RECORD);
}

void DictaphoneCore::drawLibrary() {
    if (!display_->renderDeletableList) return;

    const int lang = display_->languageIndex ? display_->languageIndex() : 0;

    if (recordingCount_ == 0) {
        if (display_->renderStatus) {
            display_->renderStatus(dictText(display_, DictStr::LibraryTitle, lang),
                                    dictText(display_, DictStr::NoRecordings, lang),
                                    dictText(display_, DictStr::TapToGoBack, lang));
        }
        return;
    }

    // Build the row list from the visible range
    const char* items[kLibraryVisibleRows];
    uint8_t visibleCount = 0;

    for (uint8_t i = 0; i < kLibraryVisibleRows && (libraryScrollTop_ + i) < recordingCount_; i++) {
        items[i] = recordingNames_[libraryScrollTop_ + i];
        visibleCount++;
    }

    // librarySelected_ can be outside the currently scrolled-to page (e.g.
    // right after a swipe, before anything on the new page is tapped) — the
    // subtraction below would otherwise wrap to a huge uint8_t and highlight
    // nothing sensible.
    uint8_t selectedInView = (librarySelected_ >= libraryScrollTop_ &&
                               librarySelected_ - libraryScrollTop_ < visibleCount)
                                  ? static_cast<uint8_t>(librarySelected_ - libraryScrollTop_)
                                  : 0xFF;

    // Page dots showing where the current view sits in the whole library —
    // one page per kLibraryVisibleRows-sized chunk, matching how the
    // vertical swipe above scrolls (a full page of rows at a time).
    const uint8_t pageCount = static_cast<uint8_t>(
        (recordingCount_ + kLibraryVisibleRows - 1) / kLibraryVisibleRows);
    const uint8_t currentPage = static_cast<uint8_t>(libraryScrollTop_ / kLibraryVisibleRows);

    display_->renderDeletableList(items, visibleCount, selectedInView, currentPage, pageCount);
}

void DictaphoneCore::drawPlaying() {
    if (!display_->renderPlaybackControls) return;

    uint32_t elapsed = 0;
    uint32_t total = 0;
    uint8_t volume = 0;
    bool paused = false;

    if (audio_) {
        if (audio_->playbackElapsedMs) elapsed = audio_->playbackElapsedMs();
        if (audio_->playbackTotalMs) total = audio_->playbackTotalMs();
        if (audio_->getVolume) volume = audio_->getVolume();
        if (audio_->isPaused) paused = audio_->isPaused();
    }

    char timeBuf[8];
    char totalStr[8];
    formatTime(elapsed, timeBuf, sizeof(timeBuf));
    formatTime(total, totalStr, sizeof(totalStr));

    const char* name = (playingIndex_ < recordingCount_)
        ? recordingNames_[playingIndex_] : "---";

    char title[64];
    snprintf(title, sizeof(title), "%s  %s/%s", name, timeBuf, totalStr);

    display_->renderPlaybackControls(title, paused, volume, elapsed / 1000, total / 1000);
}

namespace {
// Character set offered by the Rename screen's key list, in the order it's
// drawn after the three action rows (Save/Backspace/Cancel) — see
// DictaphoneCore::drawRename().
constexpr char kRenameKeyChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-";
constexpr size_t kRenameKeyCharCount = sizeof(kRenameKeyChars) - 1;  // drop the '\0'
constexpr uint8_t kRenameActionRowCount = 3;  // Save, Backspace, Cancel
}  // namespace

void DictaphoneCore::drawRename() {
    if (!display_->renderMenu) return;

    const int lang = display_->languageIndex ? display_->languageIndex() : 0;

    char saveLabel[kDictMaxFilenameLen + 8];
    snprintf(saveLabel, sizeof(saveLabel), "%s: %s", dictText(display_, DictStr::Save, lang), renameBuffer_);

    const char* items[kRenameActionRowCount + kRenameKeyCharCount];
    items[0] = saveLabel;
    items[1] = dictText(display_, DictStr::Backspace, lang);
    items[2] = dictText(display_, DictStr::Cancel, lang);

    char keyLabels[kRenameKeyCharCount][2];
    for (size_t i = 0; i < kRenameKeyCharCount; ++i) {
        keyLabels[i][0] = kRenameKeyChars[i];
        keyLabels[i][1] = '\0';
        items[kRenameActionRowCount + i] = keyLabels[i];
    }

    display_->renderMenu(items, static_cast<uint8_t>(kRenameActionRowCount + kRenameKeyCharCount),
                         renameKeySelected_);
}

void DictaphoneCore::openRename(uint8_t index) {
    if (index >= recordingCount_) return;

    renameIndex_ = index;
    // Editing starts from the name without its extension — appendRenameChar
    // et al. only ever add letters/digits/space/underscore, and the
    // extension is re-added on save (see renameRecording()).
    strncpy(renameBuffer_, recordingNames_[index], kDictMaxFilenameLen - 1);
    renameBuffer_[kDictMaxFilenameLen - 1] = '\0';
    char* dot = strrchr(renameBuffer_, '.');
    if (dot) *dot = '\0';

    renameKeySelected_ = kRenameActionRowCount;  // land on the first letter key
    goToScreen(Screen::Rename);
}

void DictaphoneCore::appendRenameChar(char c) {
    size_t len = strlen(renameBuffer_);
    // Leave room for a 4-char extension (".wav") plus the null terminator —
    // renameRecording() re-appends the extension on save.
    if (len + 1 >= kDictMaxFilenameLen - 5) return;
    renameBuffer_[len] = c;
    renameBuffer_[len + 1] = '\0';
}

void DictaphoneCore::handleRenameTouch(const PluginTouchEvent* event) {
    const uint16_t x = event->x;
    const uint16_t y = event->y;

    int height = display_ && display_->logicalHeight ? display_->logicalHeight() : 172;

    constexpr uint8_t kTotalKeys = kRenameActionRowCount + kRenameKeyCharCount;
    constexpr int kRowHeight = 22;  // DisplayManager::kCompactMenuRowHeight
    const uint8_t visibleCount = static_cast<uint8_t>(
        std::min(static_cast<int>(kTotalKeys), std::max(1, height / kRowHeight)));

    // Same swipe-vs-tap split as the Library screen: a vertical drag pages
    // the key list, anything else is read as a tap on whichever row is
    // under the finger.
    int deltaY = static_cast<int>(y) - static_cast<int>(touchStartY_);
    int absDeltaY = deltaY < 0 ? -deltaY : deltaY;
    constexpr int kRenameSwipeThresholdPx = 30;

    if (absDeltaY >= kRenameSwipeThresholdPx) {
        int next = static_cast<int>(renameKeySelected_) + (deltaY < 0 ? visibleCount : -visibleCount);
        if (next < 0) next = 0;
        if (next >= static_cast<int>(kTotalKeys)) next = kTotalKeys - 1;
        renameKeySelected_ = static_cast<uint8_t>(next);
        return;
    }

    // Mirrors App::hitTestMenuListRow()'s exact centered-window math, since
    // this is drawn by the very same DisplayManager::renderMenu().
    size_t firstVisible = 0;
    if (renameKeySelected_ >= visibleCount / 2) {
        firstVisible = renameKeySelected_ - visibleCount / 2;
    }
    if (firstVisible + visibleCount > kTotalKeys) {
        firstVisible = kTotalKeys - visibleCount;
    }
    const int totalHeight = kRowHeight * static_cast<int>(visibleCount);
    const int startY = std::max(0, (height - totalHeight) / 2);
    if (static_cast<int>(y) < startY || static_cast<int>(y) >= startY + totalHeight) {
        return;
    }
    const size_t tappedRow = static_cast<size_t>((static_cast<int>(y) - startY) / kRowHeight);
    const size_t tappedIndex = firstVisible + tappedRow;
    if (tappedIndex >= kTotalKeys) return;

    renameKeySelected_ = static_cast<uint8_t>(tappedIndex);

    if (tappedIndex == 0) {
        // Save
        if (renameBuffer_[0] != '\0') {
            renameRecording(renameIndex_, renameBuffer_);
        }
        goToScreen(Screen::Library);
    } else if (tappedIndex == 1) {
        // Backspace
        size_t len = strlen(renameBuffer_);
        if (len > 0) renameBuffer_[len - 1] = '\0';
    } else if (tappedIndex == 2) {
        // Cancel
        goToScreen(Screen::Library);
    } else {
        appendRenameChar(kRenameKeyChars[tappedIndex - kRenameActionRowCount]);
    }
    (void)x;
}

void DictaphoneCore::drawError() {
    if (!display_->renderStatus) return;
    const int lang = display_->languageIndex ? display_->languageIndex() : 0;
    display_->renderStatus(dictText(display_, DictStr::ErrorTitle, lang),
                           dictText(display_, DictStr::RecordingFailed, lang),
                           dictText(display_, DictStr::CardWriteFailed, lang));
}

void DictaphoneCore::showError(uint32_t nowMs) {
    errorShownMs_ = nowMs;
    goToScreen(Screen::Error);
}

void DictaphoneCore::drawConfirmDelete() {
    if (!display_->renderButtonPair) return;

    const int lang = display_->languageIndex ? display_->languageIndex() : 0;

    // Left/right halves here must match handleTouch()'s Screen::ConfirmDelete
    // hit-test (x < width/2 = cancel, else = confirm) exactly, since that
    // logic isn't derived from these buttons — it's just the same split.
    display_->renderButtonPair(dictText(display_, DictStr::Cancel, lang), PLUGIN_ICON_NONE, false,
                                dictText(display_, DictStr::Delete, lang), PLUGIN_ICON_DELETE);
}

// ─── Recording Actions ──────────────────────────────────────────────────────

void DictaphoneCore::startRecording() {
    if (!audio_ || !audio_->startRecording) {
        // Show error if audio service not available
        if (display_ && display_->renderStatus) {
            const int lang = display_->languageIndex ? display_->languageIndex() : 0;
            display_->renderStatus(dictText(display_, DictStr::ErrorTitle, lang),
                                    dictText(display_, DictStr::MicUnavailable, lang), "");
        }
        return;
    }

    char filename[kDictMaxFilenameLen];
    if (!generateFilename(filename, sizeof(filename))) return;

    char path[kDictMaxFilenameLen + 16];
    snprintf(path, sizeof(path), "recordings/%s", filename);

    if (audio_->startRecording(path)) {
        // Remember filename for index update after stop
        strncpy(currentRecordingName_, filename, kDictMaxFilenameLen - 1);
        currentRecordingName_[kDictMaxFilenameLen - 1] = '\0';
        goToScreen(Screen::Recording);
    } else {
        // Recording failed to start — show feedback
        if (display_ && display_->renderStatus) {
            const int lang = display_->languageIndex ? display_->languageIndex() : 0;
            display_->renderStatus(dictText(display_, DictStr::ErrorTitle, lang),
                                    dictText(display_, DictStr::RecordingFailed, lang),
                                    dictText(display_, DictStr::TryAgain, lang));
        }
    }
}

// Only asks the recorder to stop. The record task closes the take on the
// card; update() lists it once isRecording() turns false. Waiting here held
// the plugin task up to 2 s per stop, and with the card busy the plugin
// watchdog ended the dictaphone.
void DictaphoneCore::stopRecording() {
    if (!audio_ || !audio_->stopRecording) return;

    audio_->stopRecording();
}

void DictaphoneCore::finishRecording(uint32_t nowMs) {
    if (keepFinishedRecording()) {
        goToScreen(Screen::Library);
    } else {
        showError(nowMs);
    }
}

bool DictaphoneCore::keepFinishedRecording() {
    // Listed only when the file is really on the card. A take whose file
    // never opened used to be listed anyway: the row played nothing, and the
    // next delete rescanned the card and wiped every such row at once.
    bool kept = false;
    if (currentRecordingName_[0] != '\0' && recordingCount_ < kDictMaxRecordings) {
        char path[kDictMaxFilenameLen + 16];
        snprintf(path, sizeof(path), "recordings/%s", currentRecordingName_);
        if (storage_ && storage_->fileExists && storage_->fileExists(path)) {
            strncpy(recordingNames_[recordingCount_], currentRecordingName_, kDictMaxFilenameLen - 1);
            recordingNames_[recordingCount_][kDictMaxFilenameLen - 1] = '\0';
            librarySelected_ = recordingCount_;
            recordingCount_++;
            saveIndex();
            kept = true;
        }
    }
    currentRecordingName_[0] = '\0';
    return kept;
}

void DictaphoneCore::startPlayback(uint8_t index) {
    if (!audio_ || !audio_->startPlayback) return;
    if (index >= recordingCount_) return;

    char path[kDictMaxFilenameLen + 16];
    snprintf(path, sizeof(path), "recordings/%s", recordingNames_[index]);

    if (audio_->startPlayback(path)) {
        playingIndex_ = index;
        // Land on the Playing screen already paused, at whatever volume was
        // last set — without this, playback started at full/last volume the
        // instant a library row was tapped, with no chance to turn it down
        // first. The volume +/- buttons here work while paused, so the user
        // sets a safe level and taps play themselves.
        if (audio_->pausePlayback) {
            audio_->pausePlayback();
        }
        goToScreen(Screen::Playing);
    }
}

void DictaphoneCore::stopPlayback() {
    if (!audio_ || !audio_->stopPlayback) return;
    audio_->stopPlayback();
    goToScreen(Screen::Library);
}

void DictaphoneCore::adjustVolume(int delta) {
    if (!audio_ || !audio_->getVolume || !audio_->setVolume) return;

    int next = static_cast<int>(audio_->getVolume()) + delta;
    if (next < 0) next = 0;
    if (next > 100) next = 100;
    audio_->setVolume(static_cast<uint8_t>(next));
}

void DictaphoneCore::togglePausePlayback() {
    if (!audio_ || !audio_->isPaused || !audio_->pausePlayback || !audio_->resumePlayback) return;

    if (audio_->isPaused()) {
        audio_->resumePlayback();
    } else {
        audio_->pausePlayback();
    }
}

void DictaphoneCore::seekPlayback(int32_t deltaMs) {
    if (!audio_ || !audio_->seekPlaybackBy) return;
    audio_->seekPlaybackBy(deltaMs);
}

// ─── File Management ────────────────────────────────────────────────────────

bool DictaphoneCore::scanRecordings() {
    recordingCount_ = 0;

    if (!storage_ || !storage_->readFile) return false;

    // Read directory listing from a special index file or scan pattern
    // Since PluginStorageService doesn't have listDir, we use a counter-based approach:
    // scan for files named REC_0001.wav through REC_9999.wav
    // Also maintain an index file for faster lookup.

    // Try reading index file first
    uint8_t indexBuf[2048];
    int bytesRead = storage_->readFile("recordings/index.txt", indexBuf, sizeof(indexBuf) - 1);

    if (bytesRead > 0) {
        indexBuf[bytesRead] = '\0';
        // Parse index: one filename per line
        char* line = strtok(reinterpret_cast<char*>(indexBuf), "\n");
        while (line && recordingCount_ < kDictMaxRecordings) {
            // Trim whitespace
            while (*line == ' ' || *line == '\r') line++;
            size_t len = strlen(line);
            while (len > 0 && (line[len-1] == ' ' || line[len-1] == '\r' || line[len-1] == '\n')) {
                line[--len] = '\0';
            }
            if (len > 0 && len < kDictMaxFilenameLen) {
                // Verify file still exists
                char checkPath[kDictMaxFilenameLen + 16];
                snprintf(checkPath, sizeof(checkPath), "recordings/%s", line);
                if (storage_->fileExists && storage_->fileExists(checkPath)) {
                    strncpy(recordingNames_[recordingCount_], line, kDictMaxFilenameLen - 1);
                    recordingNames_[recordingCount_][kDictMaxFilenameLen - 1] = '\0';

                    // recordingCounter_ only lives in RAM, so it always
                    // restarts at 0 after a reboot — without this, the next
                    // recording after any power cycle would reuse REC_0001.wav
                    // (or whichever number a previous session already used),
                    // silently overwriting that file on disk. Since multiple
                    // library rows would then point at the same physical
                    // file, deleting any one of them made fileExists() fail
                    // for all of them on the next scan — "delete one, they
                    // all vanish". Resuming the counter from the highest
                    // REC_#### already on disk makes every new filename
                    // unique again.
                    unsigned recNum = 0;
                    if (sscanf(recordingNames_[recordingCount_], "REC_%4u.wav", &recNum) == 1 &&
                        recNum > recordingCounter_) {
                        recordingCounter_ = static_cast<uint16_t>(recNum);
                    }

                    recordingCount_++;
                }
            }
            line = strtok(nullptr, "\n");
        }
        return true;
    }

    // No index file — no recordings yet (first run)
    // Don't scan 9999 files — just start with empty list
    return true;
}

bool DictaphoneCore::generateFilename(char* buf, size_t bufSize) {
    recordingCounter_++;
    if (recordingCounter_ > 9999) recordingCounter_ = 1;  // wrap

    snprintf(buf, bufSize, "REC_%04u.wav", recordingCounter_);
    return true;
}

bool DictaphoneCore::deleteRecording(uint8_t index) {
    if (index >= recordingCount_) return false;
    if (!storage_ || !storage_->deleteFile) return false;

    char path[kDictMaxFilenameLen + 16];
    snprintf(path, sizeof(path), "recordings/%s", recordingNames_[index]);

    // A file already gone from the card still leaves the list. The row goes
    // on its own; the old code rescanned the whole card after a delete, and
    // any row whose file could not be found went with it.
    const bool removed =
        storage_->deleteFile(path) || !(storage_->fileExists && storage_->fileExists(path));
    if (!removed) return false;
    for (uint8_t i = index; i + 1 < recordingCount_; ++i) {
        memcpy(recordingNames_[i], recordingNames_[i + 1], kDictMaxFilenameLen);
    }
    --recordingCount_;
    saveIndex();
    return true;
}

bool DictaphoneCore::renameRecording(uint8_t index, const char* newName) {
    if (index >= recordingCount_ || !newName || newName[0] == '\0') return false;
    if (!storage_ || !storage_->renameFile) return false;

    // recordingNames_ doubles as the actual filename on SD (see
    // startPlayback()/deleteRecording()) — renaming only the index entry
    // without renaming the real file would desync the two, and the next
    // playback attempt would fail with "file not found". Append the
    // extension back on (openRename() stripped it for editing) unless the
    // user already typed one of their own.
    char finalName[kDictMaxFilenameLen];
    bool hasExtension = strrchr(newName, '.') != nullptr;
    if (hasExtension) {
        strncpy(finalName, newName, kDictMaxFilenameLen - 1);
        finalName[kDictMaxFilenameLen - 1] = '\0';
    } else {
        snprintf(finalName, sizeof(finalName), "%s.wav", newName);
    }

    if (strcmp(finalName, recordingNames_[index]) == 0) return true;  // no change

    char fromPath[kDictMaxFilenameLen + 16];
    char toPath[kDictMaxFilenameLen + 16];
    snprintf(fromPath, sizeof(fromPath), "recordings/%s", recordingNames_[index]);
    snprintf(toPath, sizeof(toPath), "recordings/%s", finalName);

    if (!storage_->renameFile(fromPath, toPath)) return false;

    strncpy(recordingNames_[index], finalName, kDictMaxFilenameLen - 1);
    recordingNames_[index][kDictMaxFilenameLen - 1] = '\0';
    saveIndex();
    return true;
}

void DictaphoneCore::goToScreen(Screen screen) {
    screen_ = screen;
}

void DictaphoneCore::shutdown() {
    // The underlying AudioRecorder is a persistent singleton owned by the
    // firmware, not by this plugin instance — it outlives every plugin
    // enter/exit. Its startRecording()/startPlayback() both refuse to run
    // while recording_/playing_ is already true, and the only thing that
    // ever clears those flags is stopRecording()/stopPlayback() finishing.
    // If this plugin gets torn down (power-button exit, or a screen switch
    // mid-recording) without calling either one first, the record/playback
    // FreeRTOS task keeps running orphaned in the background — nothing else
    // is left to stop it — and every future recording attempt in this
    // plugin fails immediately with "BLAD: Nagrywanie nie powiodlo sie"
    // until the device is power-cycled, because that stuck state lives in
    // RAM the plugin reload never touches. Stop whatever's actually still
    // running, independent of what screen_ this instance thinks it's on.
    if (!audio_) return;

    // Leaving mid-take still keeps the take: the stop call no longer waits,
    // so wait here for the file to close, then list it.
    if (audio_->isRecording && audio_->stopRecording && audio_->isRecording()) {
        audio_->stopRecording();
        const uint32_t waitStart = millis();
        while (audio_->isRecording() && millis() - waitStart < 1500) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!audio_->isRecording()) {
            keepFinishedRecording();
        }
    }
    if (audio_->isPlaying && audio_->stopPlayback && audio_->isPlaying()) {
        audio_->stopPlayback();
    }
}

void DictaphoneCore::formatTime(uint32_t ms, char* buf, size_t bufSize) {
    uint32_t totalSec = ms / 1000;
    uint32_t minutes = totalSec / 60;
    uint32_t seconds = totalSec % 60;
    snprintf(buf, bufSize, "%02u:%02u", (unsigned)minutes, (unsigned)seconds);
}

// ─── Index File Management ──────────────────────────────────────────────────

void DictaphoneCore::saveIndex() {
    if (!storage_ || !storage_->writeFile) return;

    // Build index content
    char indexBuf[2048];
    size_t offset = 0;

    for (uint8_t i = 0; i < recordingCount_ && offset < sizeof(indexBuf) - kDictMaxFilenameLen - 2; i++) {
        size_t len = strlen(recordingNames_[i]);
        memcpy(indexBuf + offset, recordingNames_[i], len);
        offset += len;
        indexBuf[offset++] = '\n';
    }

    storage_->writeFile("recordings/index.txt",
                        reinterpret_cast<const uint8_t*>(indexBuf),
                        static_cast<uint32_t>(offset));
}

// ─── Plugin SDK VTable Glue ─────────────────────────────────────────────────

static PluginResult dictaphoneInit(PluginContext* ctx) {
    s_instance = new DictaphoneCore(ctx->display, ctx->audio, ctx->storage);
    if (!s_instance) return PLUGIN_ERROR_MEMORY;
    if (!s_instance->begin()) {
        delete s_instance;
        s_instance = nullptr;
        return PLUGIN_ERROR_INIT;
    }
    return PLUGIN_OK;
}

static void dictaphoneDestroy() {
    if (s_instance) {
        s_instance->shutdown();
        delete s_instance;
        s_instance = nullptr;
    }
}

static void dictaphoneUpdate(uint32_t nowMs) {
    if (s_instance) s_instance->update(nowMs);
}

static void dictaphoneHandleButton(const PluginButtonEvent* event) {
    if (s_instance) s_instance->handleButton(event);
}

static void dictaphoneHandleTouch(const PluginTouchEvent* event) {
    if (s_instance) s_instance->handleTouch(event);
}

static void dictaphoneDraw() {
    if (s_instance) s_instance->draw();
}

static PluginInfo dictaphoneGetInfo() {
    return {"Dictaphone", "1.0.0", PLUGIN_SDK_VERSION};
}

PluginVTable DictaphonePlugin::vtable() {
    return {
        dictaphoneInit,
        dictaphoneDestroy,
        dictaphoneUpdate,
        dictaphoneHandleButton,
        dictaphoneHandleTouch,
        dictaphoneDraw,
        dictaphoneGetInfo,
    };
}
