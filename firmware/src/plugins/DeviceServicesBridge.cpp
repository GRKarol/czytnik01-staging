// firmware/src/plugins/DeviceServicesBridge.cpp
#include "plugins/DeviceServicesBridge.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <string.h>
#include <stdio.h>

#include "audio/AudioManager.h"
#include "audio/AudioRecorder.h"
#include "audio/AudioVolume.h"
#include "board/BoardConfig.h"
#include "display/DisplayManager.h"

static const char* TAG = "DeviceServicesBridge";

// ─── Static state (allows C function pointers to reach C++ objects) ─────────

static DisplayManager* sDisplay = nullptr;
static AudioManager* sAudio = nullptr;
static AudioRecorder* sRecorder = nullptr;
static String sStorageRoot;  // e.g. "/plugins/focus-timer/"

// IMU register constants (QMI8658 on Wire1)
namespace {
constexpr uint8_t kImuAddress = 0x6B;
constexpr uint8_t kImuAccelStartReg = 0x35;
constexpr float kAccelScale = 4.0f / 32768.0f;
}  // namespace

// ─── Path Validation (sandbox enforcement) ──────────────────────────────────

/// Returns true if the relative path is safe (no traversal).
/// Rejects any path containing ".." as a component.
static bool isPathSafe(const char* relativePath) {
    if (relativePath == nullptr || relativePath[0] == '\0') {
        return false;
    }

    // Reject absolute paths
    if (relativePath[0] == '/') {
        return false;
    }

    // Reject any occurrence of ".." as a path component
    const char* p = relativePath;
    while (*p) {
        // Check if we're at the start of a ".." component
        if (p[0] == '.' && p[1] == '.') {
            // It's a traversal if it's at start, after '/', or followed by '/' or end
            if ((p == relativePath || *(p - 1) == '/') &&
                (p[2] == '\0' || p[2] == '/')) {
                return false;
            }
        }
        ++p;
    }

    return true;
}

/// Build the full SD path from a relative path, with sandbox validation.
/// Returns empty String on failure.
static String resolveSandboxedPath(const char* relativePath) {
    if (!isPathSafe(relativePath)) {
        ESP_LOGW(TAG, "Path traversal rejected: %s", relativePath ? relativePath : "(null)");
        return String();
    }
    return sStorageRoot + relativePath;
}

// ─── Display Service Wrappers ───────────────────────────────────────────────

static void bridgeRenderFocusTimerScreen(const char* mode, const char* genre,
                                          const char* timer, const char* instruction,
                                          const char* footer, int progressPercent,
                                          bool breakAccent) {
    if (!sDisplay) return;
    sDisplay->renderFocusTimerScreen(
        mode ? mode : "", genre ? genre : "",
        timer ? timer : "", instruction ? instruction : "",
        footer ? footer : "", progressPercent, breakAccent);
}

static void bridgeRenderStatus(const char* title, const char* line1, const char* line2) {
    if (!sDisplay) return;
    sDisplay->renderStatus(title ? title : "", line1 ? line1 : "", line2 ? line2 : "");
}

static void bridgeRenderProgress(const char* title, const char* line1,
                                  const char* line2, int progressPercent) {
    if (!sDisplay) return;
    sDisplay->renderProgress(title ? title : "", line1 ? line1 : "",
                             line2 ? line2 : "", progressPercent);
}

static void bridgeRenderMenu(const char* const* items, uint8_t itemCount,
                              uint8_t selectedIndex) {
    if (!sDisplay || !items) return;
    sDisplay->renderMenu(items, static_cast<size_t>(itemCount),
                         static_cast<size_t>(selectedIndex));
}

static void bridgeRenderCenteredWord(const char* word) {
    if (!sDisplay) return;
    sDisplay->renderCenteredWord(word ? word : "");
}

static void bridgeSetDarkMode(bool dark) {
    if (!sDisplay) return;
    sDisplay->setDarkMode(dark);
}

static ui::IconId mapPluginIcon(PluginIconId icon) {
    switch (icon) {
        case PLUGIN_ICON_RECORD: return ui::IconId::Record;
        case PLUGIN_ICON_STOP:   return ui::IconId::Stop;
        case PLUGIN_ICON_PLAY:   return ui::IconId::Play;
        case PLUGIN_ICON_DELETE: return ui::IconId::Delete;
        case PLUGIN_ICON_BOOK:   return ui::IconId::Book;
        default:                 return ui::IconId::None;
    }
}

static void bridgeRenderButtonPair(const char* leftLabel, PluginIconId leftIcon, bool leftActive,
                                    const char* rightLabel, PluginIconId rightIcon) {
    if (!sDisplay) return;

    const int width = BoardConfig::DISPLAY_WIDTH;
    const int height = BoardConfig::DISPLAY_HEIGHT;
    const int halfWidth = width / 2;

    std::vector<DisplayManager::Button> buttons;
    buttons.reserve(2);

    DisplayManager::Button left;
    left.label = leftLabel ? leftLabel : "";
    left.x = 0;
    left.y = 0;
    left.width = static_cast<uint16_t>(halfWidth);
    left.height = static_cast<uint16_t>(height);
    left.icon = mapPluginIcon(leftIcon);
    left.active = leftActive;
    buttons.push_back(left);

    DisplayManager::Button right;
    right.label = rightLabel ? rightLabel : "";
    right.x = static_cast<uint16_t>(halfWidth);
    right.y = 0;
    right.width = static_cast<uint16_t>(width - halfWidth);
    right.height = static_cast<uint16_t>(height);
    right.icon = mapPluginIcon(rightIcon);
    buttons.push_back(right);

    sDisplay->renderButtonGrid("", buttons, 0, 1);
}

// Width of the trailing delete-icon zone in renderDeletableList rows — kept
// in sync with the hit-test zone in DictaphonePlugin.cpp's touch handler,
// since that's the only consumer today.
static constexpr int kDeletableListIconZoneWidth = 120;

// Width reserved for the visible Back button carved out of row 0 (the only
// row it visually overlaps) — kept in sync with kLibraryBackZoneWidth in
// DictaphonePlugin.cpp's touch handler, which already treats this whole
// left strip as "go back" on every row, invisibly. This just makes that
// existing behavior visible instead of adding a new one.
static constexpr int kDeletableListBackZoneWidth = 64;

static void bridgeRenderDeletableList(const char* const* items, uint8_t itemCount,
                                       uint8_t selectedIndex) {
    if (!sDisplay || !items || itemCount == 0) return;

    const int width = BoardConfig::DISPLAY_WIDTH;
    const int height = BoardConfig::DISPLAY_HEIGHT;
    const int rowHeight = height / itemCount;

    std::vector<DisplayManager::Button> buttons;
    buttons.reserve(static_cast<size_t>(itemCount) * 2 + 1);

    for (uint8_t i = 0; i < itemCount; ++i) {
        DisplayManager::Button row;
        row.label = items[i] ? items[i] : "";
        row.y = static_cast<uint16_t>(i * rowHeight);
        row.height = static_cast<uint16_t>(rowHeight);
        row.active = (i == selectedIndex);
        if (i == 0) {
            // Leave room on the left for the Back button drawn below.
            row.x = static_cast<uint16_t>(kDeletableListBackZoneWidth);
            row.width = static_cast<uint16_t>(width - kDeletableListBackZoneWidth -
                                              kDeletableListIconZoneWidth);
        } else {
            row.x = 0;
            row.width = static_cast<uint16_t>(width - kDeletableListIconZoneWidth);
        }
        buttons.push_back(row);

        DisplayManager::Button del;
        del.x = static_cast<uint16_t>(width - kDeletableListIconZoneWidth);
        del.y = row.y;
        del.width = static_cast<uint16_t>(kDeletableListIconZoneWidth);
        del.height = static_cast<uint16_t>(rowHeight);
        del.icon = ui::IconId::Delete;
        // The zone is a wide, short tap target (120px x row height) so it
        // stays easy to hit, but the glyph itself should stay a normal
        // small trash-can size, not balloon to fill that whole box.
        del.iconMaxSize = 22;
        buttons.push_back(del);
    }

    // Same corner geometry as the app's own back buttons (see
    // App::applyBackButtonCornerLayout) — a small icon-only tile, not a
    // full-height zone, even though the tap zone it sits over is taller.
    DisplayManager::Button back;
    back.icon = ui::IconId::Back;
    back.x = 0;
    back.y = 2;
    back.width = 44;
    back.height = 26;
    buttons.push_back(back);

    sDisplay->renderButtonGrid("", buttons, 0, 1);
}

// ─── Dictaphone Playback Controls ───────────────────────────────────────────
//
// Layout constants below must stay in sync with DictaphonePlugin.cpp's
// handlePlayingTouch()/applySeekTouchX(), which derive their own hit-test
// geometry from the same numbers instead of reading anything back — same
// convention as the library's delete-icon zone above.
static constexpr uint16_t kPlayingRowY = 20;
static constexpr uint16_t kPlayingRowH = 46;
static constexpr uint16_t kPlayingButtonW = 145;
static constexpr uint16_t kPlayingButtonGap = 4;
static constexpr uint16_t kPlayingButtonX0 = 4;
static constexpr uint16_t kPlayingSliderX = 4;
static constexpr uint16_t kPlayingSliderY = 68;
static constexpr uint16_t kPlayingSliderW = 632;
static constexpr uint16_t kPlayingSliderH = 104;

static void bridgeRenderPlaybackControls(const char* title, bool paused, uint8_t volumePercent,
                                          uint32_t elapsedSec, uint32_t totalSec) {
    if (!sDisplay) return;

    std::vector<DisplayManager::Button> buttons;
    buttons.reserve(5);

    auto addSquare = [&](const char* label, ui::IconId icon, uint8_t col) {
        DisplayManager::Button b;
        b.label = label;
        b.icon = icon;
        b.x = static_cast<uint16_t>(kPlayingButtonX0 + col * (kPlayingButtonW + kPlayingButtonGap));
        b.y = kPlayingRowY;
        b.width = kPlayingButtonW;
        b.height = kPlayingRowH;
        buttons.push_back(b);
    };

    addSquare("Stop", ui::IconId::Stop, 0);
    addSquare("Gl -", ui::IconId::None, 1);
    addSquare(paused ? "Wznow" : "Pauza", paused ? ui::IconId::Play : ui::IconId::None, 2);
    addSquare("Gl +", ui::IconId::None, 3);

    DisplayManager::Button slider;
    slider.kind = DisplayManager::Button::ButtonKind::Slider;
    char label[24];
    snprintf(label, sizeof(label), "Pozycja  Gl:%u%%", static_cast<unsigned>(volumePercent));
    slider.label = label;
    slider.x = kPlayingSliderX;
    slider.y = kPlayingSliderY;
    slider.width = kPlayingSliderW;
    slider.height = kPlayingSliderH;
    slider.sliderMin = 0;
    slider.sliderMax = static_cast<uint16_t>(totalSec > 0xFFFFu ? 0xFFFFu : totalSec);
    uint32_t clampedElapsed = elapsedSec > slider.sliderMax ? slider.sliderMax : elapsedSec;
    slider.sliderValue = static_cast<uint16_t>(clampedElapsed);
    slider.sliderUnit = "s";
    buttons.push_back(slider);

    sDisplay->renderButtonGrid(title ? title : "", buttons, 0, 1);
}

static int bridgeLogicalWidth() {
    // Return fixed display dimensions based on panel config
    // In landscape mode (default): 640 wide
    return BoardConfig::DISPLAY_WIDTH;
}

static int bridgeLogicalHeight() {
    // In landscape mode (default): 172 tall
    return BoardConfig::DISPLAY_HEIGHT;
}

// ─── Audio Service Wrappers ─────────────────────────────────────────────────

static bool bridgeAudioBeep() {
    if (!sAudio) return false;
    return sAudio->beep();
}

static bool bridgeAudioAvailable() {
    if (!sAudio) return false;
    return sAudio->available();
}

// ─── Audio Recording/Playback Wrappers ──────────────────────────────────────

static bool bridgeAudioStartRecording(const char* relativePath) {
    if (!sRecorder) return false;
    String fullPath = resolveSandboxedPath(relativePath);
    if (fullPath.isEmpty()) return false;
    return sRecorder->startRecording(fullPath.c_str());
}

static bool bridgeAudioStopRecording() {
    if (!sRecorder) return false;
    return sRecorder->stopRecording();
}

static bool bridgeAudioIsRecording() {
    if (!sRecorder) return false;
    return sRecorder->isRecording();
}

static uint32_t bridgeAudioRecordingElapsedMs() {
    if (!sRecorder) return 0;
    return sRecorder->recordingElapsedMs();
}

static uint8_t bridgeAudioRecordingPeakLevel() {
    if (!sRecorder) return 0;
    return sRecorder->recordingPeakLevel();
}

static bool bridgeAudioStartPlayback(const char* relativePath) {
    if (!sRecorder) return false;
    String fullPath = resolveSandboxedPath(relativePath);
    if (fullPath.isEmpty()) return false;
    return sRecorder->startPlayback(fullPath.c_str());
}

static bool bridgeAudioStopPlayback() {
    if (!sRecorder) return false;
    return sRecorder->stopPlayback();
}

static bool bridgeAudioIsPlaying() {
    if (!sRecorder) return false;
    return sRecorder->isPlaying();
}

static uint32_t bridgeAudioPlaybackElapsedMs() {
    if (!sRecorder) return 0;
    return sRecorder->playbackElapsedMs();
}

static uint32_t bridgeAudioPlaybackTotalMs() {
    if (!sRecorder) return 0;
    return sRecorder->playbackTotalMs();
}

static void bridgeAudioPausePlayback() {
    if (!sRecorder) return;
    sRecorder->pausePlayback();
}

static void bridgeAudioResumePlayback() {
    if (!sRecorder) return;
    sRecorder->resumePlayback();
}

static bool bridgeAudioIsPaused() {
    if (!sRecorder) return false;
    return sRecorder->isPaused();
}

static void bridgeAudioSeekPlaybackBy(int32_t deltaMs) {
    if (!sRecorder) return;
    sRecorder->seekPlaybackBy(deltaMs);
}

static uint8_t bridgeAudioGetVolume() {
    return AudioVolume::percent();
}

static void bridgeAudioSetVolume(uint8_t percent) {
    AudioVolume::setPercent(percent);
    if (sRecorder) sRecorder->applyVolume();
}

// ─── IMU Service Wrappers ───────────────────────────────────────────────────

static bool bridgeImuReadAccelerometer(float* x, float* y, float* z) {
    if (!x || !y || !z) return false;

    // Direct I2C read from QMI8658 on Wire1 (same approach as FocusTimer)
    BoardConfig::I2cBusLock lock;
    Wire1.beginTransmission(kImuAddress);
    Wire1.write(kImuAccelStartReg);
    if (Wire1.endTransmission(false) != 0) {
        return false;
    }

    if (Wire1.requestFrom(static_cast<int>(kImuAddress), 6, 1) != 6) {
        return false;
    }

    uint8_t buffer[6];
    for (int i = 0; i < 6; ++i) {
        buffer[i] = Wire1.read();
    }

    const int16_t rawX = static_cast<int16_t>((buffer[1] << 8) | buffer[0]);
    const int16_t rawY = static_cast<int16_t>((buffer[3] << 8) | buffer[2]);
    const int16_t rawZ = static_cast<int16_t>((buffer[5] << 8) | buffer[4]);

    *x = rawX * kAccelScale;
    *y = rawY * kAccelScale;
    *z = rawZ * kAccelScale;
    return true;
}

static bool bridgeImuAvailable() {
    // Probe the IMU address on Wire1
    BoardConfig::I2cBusLock lock;
    Wire1.beginTransmission(kImuAddress);
    return Wire1.endTransmission(true) == 0;
}

// ─── Orientation Service Wrappers ───────────────────────────────────────────

static PluginOrientation bridgeCurrentOrientation() {
    // The default orientation for this device is LandscapeFlipped
    // (UI_ROTATED_180 = true means buttons are at top in flipped mode).
    return PLUGIN_ORIENTATION_LANDSCAPE_FLIPPED;
}

static void bridgeSetUiOrientation(PluginOrientation orientation) {
    if (!sDisplay) return;

    BoardConfig::UiOrientation mapped;
    switch (orientation) {
        case PLUGIN_ORIENTATION_LANDSCAPE:
            // Plugin says "landscape" — map to the device's default (flipped)
            mapped = BoardConfig::UiOrientation::LandscapeFlipped;
            break;
        case PLUGIN_ORIENTATION_PORTRAIT_A:
            mapped = BoardConfig::UiOrientation::PortraitFlipped;
            break;
        case PLUGIN_ORIENTATION_PORTRAIT_B:
            mapped = BoardConfig::UiOrientation::Portrait;
            break;
        case PLUGIN_ORIENTATION_LANDSCAPE_FLIPPED:
            mapped = BoardConfig::UiOrientation::Landscape;
            break;
        default:
            mapped = BoardConfig::UiOrientation::LandscapeFlipped;
            break;
    }

    sDisplay->setUiOrientation(mapped);
}

// ─── Storage Service Wrappers ───────────────────────────────────────────────

static bool bridgeStorageFileExists(const char* relativePath) {
    String fullPath = resolveSandboxedPath(relativePath);
    if (fullPath.isEmpty()) return false;
    return SD_MMC.exists(fullPath);
}

static int bridgeStorageReadFile(const char* relativePath, uint8_t* buffer, uint32_t maxLen) {
    if (!buffer || maxLen == 0) return -1;

    String fullPath = resolveSandboxedPath(relativePath);
    if (fullPath.isEmpty()) return -1;

    File file = SD_MMC.open(fullPath, FILE_READ);
    if (!file) return -1;

    int bytesRead = file.read(buffer, maxLen);
    file.close();
    return bytesRead;
}

static bool bridgeStorageWriteFile(const char* relativePath, const uint8_t* data, uint32_t len) {
    if (!data && len > 0) return false;

    String fullPath = resolveSandboxedPath(relativePath);
    if (fullPath.isEmpty()) return false;

    File file = SD_MMC.open(fullPath, FILE_WRITE);
    if (!file) return false;

    size_t written = file.write(data, len);
    file.close();
    return written == len;
}

static bool bridgeStorageDeleteFile(const char* relativePath) {
    String fullPath = resolveSandboxedPath(relativePath);
    if (fullPath.isEmpty()) return false;

    return SD_MMC.remove(fullPath);
}

static bool bridgeStorageMkdir(const char* relativePath) {
    String fullPath = resolveSandboxedPath(relativePath);
    if (fullPath.isEmpty()) return false;

    return SD_MMC.mkdir(fullPath);
}

// ─── Public API ─────────────────────────────────────────────────────────────

void DeviceServicesBridge::setup(const char* pluginId,
                                  const char* storageRoot,
                                  DisplayManager* display,
                                  AudioManager* audio,
                                  AudioRecorder* recorder,
                                  PluginDisplayService* displayService,
                                  PluginAudioService* audioService,
                                  PluginImuService* imuService,
                                  PluginStorageService* storageService,
                                  PluginOrientationService* orientationService) {
    // Store manager pointers for static wrappers
    sDisplay = display;
    sAudio = audio;
    sRecorder = recorder;
    sStorageRoot = storageRoot ? storageRoot : "";

    // Populate display service function pointers
    if (displayService) {
        displayService->renderFocusTimerScreen = bridgeRenderFocusTimerScreen;
        displayService->renderStatus = bridgeRenderStatus;
        displayService->renderProgress = bridgeRenderProgress;
        displayService->renderMenu = bridgeRenderMenu;
        displayService->renderCenteredWord = bridgeRenderCenteredWord;
        displayService->setDarkMode = bridgeSetDarkMode;
        displayService->logicalWidth = bridgeLogicalWidth;
        displayService->logicalHeight = bridgeLogicalHeight;
        displayService->renderButtonPair = bridgeRenderButtonPair;
        displayService->renderDeletableList = bridgeRenderDeletableList;
        displayService->renderPlaybackControls = bridgeRenderPlaybackControls;
    }

    // Populate audio service function pointers
    if (audioService) {
        audioService->beep = bridgeAudioBeep;
        audioService->available = bridgeAudioAvailable;
        audioService->startRecording = bridgeAudioStartRecording;
        audioService->stopRecording = bridgeAudioStopRecording;
        audioService->isRecording = bridgeAudioIsRecording;
        audioService->recordingElapsedMs = bridgeAudioRecordingElapsedMs;
        audioService->recordingPeakLevel = bridgeAudioRecordingPeakLevel;
        audioService->startPlayback = bridgeAudioStartPlayback;
        audioService->stopPlayback = bridgeAudioStopPlayback;
        audioService->isPlaying = bridgeAudioIsPlaying;
        audioService->playbackElapsedMs = bridgeAudioPlaybackElapsedMs;
        audioService->playbackTotalMs = bridgeAudioPlaybackTotalMs;
        audioService->pausePlayback = bridgeAudioPausePlayback;
        audioService->resumePlayback = bridgeAudioResumePlayback;
        audioService->isPaused = bridgeAudioIsPaused;
        audioService->seekPlaybackBy = bridgeAudioSeekPlaybackBy;
        audioService->getVolume = bridgeAudioGetVolume;
        audioService->setVolume = bridgeAudioSetVolume;
    }

    // Populate IMU service function pointers
    if (imuService) {
        imuService->readAccelerometer = bridgeImuReadAccelerometer;
        imuService->available = bridgeImuAvailable;
    }

    // Populate storage service function pointers
    if (storageService) {
        storageService->fileExists = bridgeStorageFileExists;
        storageService->readFile = bridgeStorageReadFile;
        storageService->writeFile = bridgeStorageWriteFile;
        storageService->deleteFile = bridgeStorageDeleteFile;
        storageService->mkdir = bridgeStorageMkdir;
    }

    // Populate orientation service function pointers
    if (orientationService) {
        orientationService->currentOrientation = bridgeCurrentOrientation;
        orientationService->setUiOrientation = bridgeSetUiOrientation;
    }

    ESP_LOGI(TAG, "Device services bridge set up for plugin '%s'", pluginId ? pluginId : "");
}

void DeviceServicesBridge::teardown() {
    sDisplay = nullptr;
    sAudio = nullptr;
    sRecorder = nullptr;
    sStorageRoot = "";

    ESP_LOGI(TAG, "Device services bridge torn down");
}
