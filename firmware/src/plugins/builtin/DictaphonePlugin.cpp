// firmware/src/plugins/builtin/DictaphonePlugin.cpp
#include "plugins/builtin/DictaphonePlugin.h"

#include <string.h>
#include <stdio.h>

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
            // Recording stopped externally (max duration reached or error)
            // Add the file to our recordings list
            if (currentRecordingName_[0] != '\0' && recordingCount_ < kDictMaxRecordings) {
                strncpy(recordingNames_[recordingCount_], currentRecordingName_, kDictMaxFilenameLen - 1);
                recordingNames_[recordingCount_][kDictMaxFilenameLen - 1] = '\0';
                recordingCount_++;
                saveIndex();
            }
            currentRecordingName_[0] = '\0';
            goToScreen(Screen::Library);
        }
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

    // Only handle touch end (tap)
    if (event->phase != 2) return;

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

        case Screen::Library: {
            if (recordingCount_ == 0) {
                // No recordings — tap anywhere to go back
                goToScreen(Screen::Main);
                return;
            }

            int height = display_ && display_->logicalHeight ? display_->logicalHeight() : 172;
            int width = display_ && display_->logicalWidth ? display_->logicalWidth() : 640;

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
                scanRecordings();
                if (librarySelected_ >= recordingCount_ && recordingCount_ > 0) {
                    librarySelected_ = recordingCount_ - 1;
                }
                goToScreen(Screen::Library);
            }
            break;
        }

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
            draggingSeek_ = true;
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
    }
}

// ─── Screen Drawing ─────────────────────────────────────────────────────────

void DictaphoneCore::drawMain() {
    if (!display_->renderButtonPair) return;

    char rightLabel[32];
    if (recordingCount_ > 0) {
        snprintf(rightLabel, sizeof(rightLabel), "Biblioteka (%d)", recordingCount_);
    } else {
        snprintf(rightLabel, sizeof(rightLabel), "Biblioteka");
    }

    display_->renderButtonPair("Nagraj", PLUGIN_ICON_RECORD, false, rightLabel, PLUGIN_ICON_BOOK);
}

void DictaphoneCore::drawRecording() {
    if (!display_->renderButtonPair) return;

    char timeBuf[8];
    uint32_t elapsed = 0;
    if (audio_ && audio_->recordingElapsedMs) {
        elapsed = audio_->recordingElapsedMs();
    }
    formatTime(elapsed, timeBuf, sizeof(timeBuf));

    // Peak input level appended to the label — the only way to tell "mic
    // is actually picking something up" from the device itself, without a
    // serial cable. Stays at "Pzm:0%" the whole recording if the ADC path
    // is silent.
    uint8_t peak = 0;
    if (audio_ && audio_->recordingPeakLevel) {
        peak = audio_->recordingPeakLevel();
    }
    char label[24];
    snprintf(label, sizeof(label), "%s Pzm:%u%%", timeBuf, static_cast<unsigned>(peak));

    display_->renderButtonPair(label, PLUGIN_ICON_STOP, true, "Biblioteka", PLUGIN_ICON_BOOK);
}

void DictaphoneCore::drawLibrary() {
    if (!display_->renderDeletableList) return;

    if (recordingCount_ == 0) {
        if (display_->renderStatus) {
            display_->renderStatus("BIBLIOTEKA", "Brak nagran", "Dotknij, aby wrocic");
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

    uint8_t selectedInView = librarySelected_ - libraryScrollTop_;
    display_->renderDeletableList(items, visibleCount, selectedInView);
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

void DictaphoneCore::drawRename() {
    if (display_->renderStatus) {
        display_->renderStatus("RENAME", renameBuffer_, "");
    }
}

void DictaphoneCore::drawConfirmDelete() {
    if (!display_->renderButtonPair) return;

    // Left/right halves here must match handleTouch()'s Screen::ConfirmDelete
    // hit-test (x < width/2 = cancel, else = confirm) exactly, since that
    // logic isn't derived from these buttons — it's just the same split.
    display_->renderButtonPair("Anuluj", PLUGIN_ICON_NONE, false, "Usun", PLUGIN_ICON_DELETE);
}

// ─── Recording Actions ──────────────────────────────────────────────────────

void DictaphoneCore::startRecording() {
    if (!audio_ || !audio_->startRecording) {
        // Show error if audio service not available
        if (display_ && display_->renderStatus) {
            display_->renderStatus("BLAD", "Mikrofon niedostepny", "");
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
            display_->renderStatus("BLAD", "Nagrywanie nie powiodlo sie", "Sprobuj ponownie");
        }
    }
}

void DictaphoneCore::stopRecording() {
    if (!audio_ || !audio_->stopRecording) return;

    audio_->stopRecording();

    // Add the newly recorded file to our recordings list and index
    if (currentRecordingName_[0] != '\0' && recordingCount_ < kDictMaxRecordings) {
        strncpy(recordingNames_[recordingCount_], currentRecordingName_, kDictMaxFilenameLen - 1);
        recordingNames_[recordingCount_][kDictMaxFilenameLen - 1] = '\0';
        recordingCount_++;
        saveIndex();
    }
    currentRecordingName_[0] = '\0';

    goToScreen(Screen::Library);
}

void DictaphoneCore::startPlayback(uint8_t index) {
    if (!audio_ || !audio_->startPlayback) return;
    if (index >= recordingCount_) return;

    char path[kDictMaxFilenameLen + 16];
    snprintf(path, sizeof(path), "recordings/%s", recordingNames_[index]);

    if (audio_->startPlayback(path)) {
        playingIndex_ = index;
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

    bool result = storage_->deleteFile(path);
    if (result) {
        // Update index
        saveIndex();
    }
    return result;
}

bool DictaphoneCore::renameRecording(uint8_t index, const char* newName) {
    // PluginStorageService doesn't support rename directly, so we'd need
    // read → write → delete. For now, rename is UI-only (rename in index).
    if (index >= recordingCount_ || !newName) return false;

    // Just update the display name in index (file stays the same on SD)
    // This is a simplification — true file rename would need additional API
    strncpy(recordingNames_[index], newName, kDictMaxFilenameLen - 1);
    recordingNames_[index][kDictMaxFilenameLen - 1] = '\0';
    saveIndex();
    return true;
}

void DictaphoneCore::goToScreen(Screen screen) {
    screen_ = screen;
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
