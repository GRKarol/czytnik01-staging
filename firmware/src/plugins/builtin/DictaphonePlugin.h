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
    };

    DictaphoneCore(PluginDisplayService* display, PluginAudioService* audio,
                   PluginStorageService* storage);

    bool begin();
    void update(uint32_t nowMs);
    void handleButton(const PluginButtonEvent* event);
    void handleTouch(const PluginTouchEvent* event);
    void draw();

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

    // Delete confirmation
    uint8_t deleteIndex_ = 0;
    bool deleteConfirmed_ = false;

    // Recording counter for auto-naming
    uint16_t recordingCounter_ = 0;

    // Currently recording filename (for adding to index after stop)
    char currentRecordingName_[kDictMaxFilenameLen] = {};
};

/// Plugin SDK vtable entry points for the Dictaphone built-in plugin.
namespace DictaphonePlugin {
    PluginVTable vtable();
}
