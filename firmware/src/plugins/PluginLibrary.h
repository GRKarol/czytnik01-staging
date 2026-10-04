// firmware/src/plugins/PluginLibrary.h
#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <vector>

/**
 * PluginLibrary — local registry of built-in plugins with enable/disable
 * state and the user's order. Plugin code is always compiled into the
 * firmware; there is no network install/download step anymore. Enabling a
 * plugin just flips a persisted flag so it shows up on the Aktywne (active)
 * screen — disabling it removes it from Aktywne without touching RAM or
 * flash.
 *
 * The phone app changes the same state over Wi-Fi through its own short-
 * lived instance (CompanionSyncManager). Every write bumps a shared
 * revision, and refreshIfStale() lets the App's long-lived instance pick
 * the change up.
 */
class PluginLibrary {
 public:
    struct Entry {
        String id;
        String name;
        String description;
        bool enabled;
    };

    bool begin();

    /// All built-in plugins in the user's order, each annotated with its
    /// current enabled state.
    const std::vector<Entry>& all() const { return entries_; }

    /// Subset of all() where enabled == true, in the user's order.
    std::vector<Entry> enabledEntries() const;

    bool isEnabled(const char* id) const;
    void setEnabled(const char* id, bool enabled);

    /// New order from a comma-separated id list. Unknown ids are skipped;
    /// plugins missing from the list keep their relative order after it.
    void setOrder(const String& csvIds);

    /// Reloads if another instance wrote since this one last read.
    void refreshIfStale();

 private:
    void refresh();

    Preferences prefs_;
    std::vector<Entry> entries_;
    uint32_t loadedRevision_ = 0;
};
