// firmware/src/plugins/builtin/DictaphonePlugin.h
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "plugins/sdk/PluginSdk.h"
#include "plugins/sdk/PluginDisplayService.h"
#include "plugins/sdk/PluginAudioService.h"
#include "plugins/sdk/PluginStorageService.h"

static constexpr uint8_t kDictMaxRecordings = 64;
static constexpr uint16_t kDictMaxFilenameLen = 48;

class DictaphoneCore {
 public:
    enum class Screen : uint8_t {
        Main,           // Idle screen — big record button
        Recording,      // Recording in progress
        Library,        // List of recordings
        Playing,        // Playback in progress
        Rename,         // Rename dialog
        ConfirmDelete,  // Delete confirmation
        Error,          // A take that never reached the card
    };

    DictaphoneCore(PluginDisplayService* display, PluginAudioService* audio,
                   PluginStorageService* storage);

    bool begin();
    void update(uint32_t nowMs);
    void handleButton(const PluginButtonEvent* event);
    void handleTouch(const PluginTouchEvent* event);
    void draw();

    // Called right before this instance is destroyed (plugin unload — the
    // power button's global "exit plugin" doesn't know or care what screen
    // we're on). Stops any recording/playback still in flight so the
    // AudioRecorder isn't left running orphaned. See the .cpp for why that
    // otherwise breaks the mic until a full power cycle.
    void shutdown();

 private:
    // Touch handling for the Playing screen — split out because it, alone,
    // needs to react to every touch phase (drag) instead of just the
    // release, to make the seek slider draggable.
    void handlePlayingTouch(const PluginTouchEvent* event);
    void applySeekTouchX(uint16_t x);

    // File management
    bool scanRecordings();
    bool generateFilename(char* buf, size_t bufSize);
    bool deleteRecording(uint8_t index);
    bool renameRecording(uint8_t index, const char* newName);
    void saveIndex();

    // Rename screen (character-by-character picker over renderMenu(), the
    // plugin SDK's only scrollable-list primitive — see handleRenameTouch()
    // for why the hit-test mirrors App::hitTestMenuListRow()'s layout math).
    void openRename(uint8_t index);
    void handleRenameTouch(const PluginTouchEvent* event);
    void appendRenameChar(char c);

    // Navigation
    void goToScreen(Screen screen);
    void startRecording();
    void stopRecording();
    void startPlayback(uint8_t index);
    void stopPlayback();
    void adjustVolume(int delta);
    void togglePausePlayback();
    void seekPlayback(int32_t deltaMs);

    // Drawing helpers
    void drawMain();
    void drawRecording();
    void drawLibrary();
    void drawPlaying();
    void drawRename();
    void drawConfirmDelete();
    void drawError();
    void showError(uint32_t nowMs);
    // Recording has ended (tap or on its own): lists the take or shows the
    // error.
    void finishRecording(uint32_t nowMs);

    // Format time as MM:SS
    void formatTime(uint32_t ms, char* buf, size_t bufSize);

    // Device services
    PluginDisplayService* display_;
    PluginAudioService* audio_;
    PluginStorageService* storage_;

    // State
    Screen screen_ = Screen::Main;
    uint32_t lastUpdateMs_ = 0;

    // Recording list
    uint8_t recordingCount_ = 0;
    char recordingNames_[kDictMaxRecordings][kDictMaxFilenameLen];

    // Library navigation
    uint8_t librarySelected_ = 0;
    uint8_t libraryScrollTop_ = 0;

    // Start position of the touch currently down, captured on phase==0 so
    // the Library screen can tell a vertical swipe (scroll the list) from a
    // tap (play/delete/back) at release time. See handleTouch()'s
    // Screen::Library case.
    uint16_t touchStartX_ = 0;
    uint16_t touchStartY_ = 0;

    // Currently playing index
    uint8_t playingIndex_ = 0;

    // True while a finger is down inside the Playing screen's seek slider —
    // lets handlePlayingTouch() keep tracking the drag across move events
    // even though PluginTouchEvent gives no "which control did this touch
    // start on" of its own.
    bool draggingSeek_ = false;

    // Rename state
    uint8_t renameIndex_ = 0;
    char renameBuffer_[kDictMaxFilenameLen] = {};
    uint8_t renameCursorPos_ = 0;
    // Scroll/selection position in the Rename screen's key list (Save/
    // Backspace/Cancel + A-Z/0-9/space/underscore) — same role as
    // librarySelected_ for the Library screen.
    uint8_t renameKeySelected_ = 0;

    // Delete confirmation
    uint8_t deleteIndex_ = 0;
    bool deleteConfirmed_ = false;

    // Recording counter for auto-naming
    uint16_t recordingCounter_ = 0;

    // Currently recording filename (for adding to index after stop)
    char currentRecordingName_[kDictMaxFilenameLen] = {};

    // Timestamp (event->timestampMs) of the last accepted tap/press. The
    // touch controller can report a second phantom press-release cycle
    // right after a real one — contact bounce on release, not a deliberate
    // second tap — which was firing actions twice (e.g. two recordings
    // deleted for one confirm tap, or a stray seek jump on the playback
    // slider). Any new tap within kActionCooldownMs of the last one is
    // treated as that bounce and ignored.
    uint32_t lastActionMs_ = 0;
    uint32_t errorShownMs_ = 0;
    static constexpr uint32_t kErrorShowMs = 3000;
    static constexpr uint32_t kActionCooldownMs = 350;
};

/// Plugin SDK vtable entry points for the Dictaphone built-in plugin.
namespace DictaphonePlugin {
    PluginVTable vtable();
}
