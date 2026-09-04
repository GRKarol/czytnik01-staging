// firmware/src/plugins/sdk/PluginDisplayService.h
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Small icon set exposed to plugins — mapped to the app's internal
/// ui::IconId by the device services bridge, so plugins never need to know
/// about the app's own icon enum.
typedef enum PluginIconId {
    PLUGIN_ICON_NONE = 0,
    PLUGIN_ICON_RECORD,
    PLUGIN_ICON_STOP,
    PLUGIN_ICON_PLAY,
    PLUGIN_ICON_DELETE,
    PLUGIN_ICON_BOOK,
} PluginIconId;

typedef struct PluginDisplayService {
    void (*renderFocusTimerScreen)(const char* mode, const char* genre,
                                   const char* timer, const char* instruction,
                                   const char* footer, int progressPercent,
                                   bool breakAccent);
    void (*renderStatus)(const char* title, const char* line1, const char* line2);
    void (*renderProgress)(const char* title, const char* line1,
                           const char* line2, int progressPercent);
    void (*renderMenu)(const char* const* items, uint8_t itemCount,
                       uint8_t selectedIndex);
    void (*renderCenteredWord)(const char* word);
    void (*setDarkMode)(bool dark);
    int (*logicalWidth)(void);
    int (*logicalHeight)(void);

    /// Two big side-by-side buttons filling the whole screen (left/right
    /// halves) — e.g. a dictaphone's record + library home screen.
    /// `leftActive` tints the left button as "on" (e.g. currently recording).
    void (*renderButtonPair)(const char* leftLabel, PluginIconId leftIcon, bool leftActive,
                             const char* rightLabel, PluginIconId rightIcon);

    /// Full-width rows, each a tappable button with a trailing delete icon
    /// docked to its right edge (e.g. a recordings library). `selectedIndex`
    /// highlights that row. Tap hit-testing for the delete zone is the
    /// caller's job — see logicalWidth() and match the same right-edge
    /// width the bridge uses to draw it.
    void (*renderDeletableList)(const char* const* items, uint8_t itemCount,
                                uint8_t selectedIndex);

    /// Real, tappable playback controls (e.g. a dictaphone's playing
    /// screen): a row of square buttons — Stop, volume down, pause/resume,
    /// volume up — plus a full-width draggable position slider below them.
    /// `title` is drawn as small text at the very top (e.g. filename plus
    /// elapsed/total time). The caller owns hit-testing for both the button
    /// row and the slider's drag — see logicalWidth()/logicalHeight() and
    /// match the same geometry the bridge draws with.
    void (*renderPlaybackControls)(const char* title, bool paused, uint8_t volumePercent,
                                   uint32_t elapsedSec, uint32_t totalSec);
} PluginDisplayService;

#ifdef __cplusplus
}
#endif
