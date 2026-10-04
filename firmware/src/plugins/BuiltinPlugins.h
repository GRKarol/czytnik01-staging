// firmware/src/plugins/BuiltinPlugins.h
#pragma once

#include "plugins/sdk/PluginSdk.h"

/**
 * BuiltinPlugins — registry of all plugins compiled into the firmware binary.
 *
 * Plugin code is always present in the firmware. A plugin is considered
 * "installed" (activated) only when its manifest exists on SD card at
 * /plugins/{id}/manifest.json. Launching a plugin calls the built-in code
 * directly — no PSRAM binary loading needed.
 */

// Files a plugin keeps on the card that the phone app may list, download
// and delete (Dyktafon: its recordings). Paths are relative to the
// plugin's own folder, /plugins/{id}/.
struct BuiltinPluginFiles {
    const char* dir;    // e.g. "recordings"
    const char* ext;    // only names ending with this are listed
    const char* index;  // plugin's own list of names, kept in step on delete
    const char* mime;   // served with this content type
};

struct BuiltinPlugin {
    const char* id;           // e.g. "focus-timer", "rss"
    const char* name;         // human-readable name
    const char* description;  // shown on the plugin detail screen
    PluginVTable vtable;       // function pointers to built-in code
    const BuiltinPluginFiles* files;  // nullptr: nothing for the app to manage
};

namespace BuiltinPlugins {

/// Find a built-in plugin by its ID. Returns nullptr if not found.
const BuiltinPlugin* find(const char* pluginId);

/// Get the full list of built-in plugins and count.
const BuiltinPlugin* all();
size_t count();

}  // namespace BuiltinPlugins
