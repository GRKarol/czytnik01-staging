// firmware/src/plugins/PluginLibrary.cpp
#include "plugins/PluginLibrary.h"
#include "plugins/BuiltinPlugins.h"

#include <atomic>

namespace {

constexpr const char* kPrefsNamespace = "plugins";
constexpr const char* kEnabledKey = "enabled";
constexpr const char* kOrderKey = "order";

// Bumped by every write from any instance (see refreshIfStale()).
std::atomic<uint32_t> gRevision{1};

std::vector<String> splitCsv(const String& csv) {
    std::vector<String> out;
    int start = 0;
    while (start <= static_cast<int>(csv.length())) {
        int comma = csv.indexOf(',', start);
        if (comma < 0) comma = csv.length();
        String token = csv.substring(start, comma);
        token.trim();
        if (!token.isEmpty()) out.push_back(token);
        start = comma + 1;
    }
    return out;
}

bool containsId(const String& csv, const char* id) {
    if (csv.isEmpty()) return false;
    const String needle = String(",") + id + ",";
    const String haystack = String(",") + csv + ",";
    return haystack.indexOf(needle) >= 0;
}

String withIdRemoved(const String& csv, const char* id) {
    String result;
    for (const String& token : splitCsv(csv)) {
        if (token == id) continue;
        if (!result.isEmpty()) result += ",";
        result += token;
    }
    return result;
}

}  // namespace

bool PluginLibrary::begin() {
    prefs_.begin(kPrefsNamespace, false);
    refresh();
    return true;
}

void PluginLibrary::refresh() {
    loadedRevision_ = gRevision.load();
    entries_.clear();
    const String enabledCsv = prefs_.getString(kEnabledKey, "");

    const BuiltinPlugin* plugins = BuiltinPlugins::all();
    const size_t count = BuiltinPlugins::count();
    std::vector<bool> placed(count, false);
    entries_.reserve(count);
    auto add = [&](size_t i) {
        Entry e;
        e.id = plugins[i].id;
        e.name = plugins[i].name;
        e.description = plugins[i].description;
        e.enabled = containsId(enabledCsv, plugins[i].id);
        entries_.push_back(e);
        placed[i] = true;
    };
    // The saved order first, then whatever it does not name (a plugin new
    // in this firmware) in registry order.
    for (const String& id : splitCsv(prefs_.getString(kOrderKey, ""))) {
        for (size_t i = 0; i < count; ++i) {
            if (!placed[i] && id == plugins[i].id) add(i);
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (!placed[i]) add(i);
    }
}

void PluginLibrary::refreshIfStale() {
    if (loadedRevision_ != gRevision.load()) refresh();
}

std::vector<PluginLibrary::Entry> PluginLibrary::enabledEntries() const {
    std::vector<Entry> out;
    for (const auto& e : entries_) {
        if (e.enabled) out.push_back(e);
    }
    return out;
}

bool PluginLibrary::isEnabled(const char* id) const {
    for (const auto& e : entries_) {
        if (e.id == id) return e.enabled;
    }
    return false;
}

void PluginLibrary::setEnabled(const char* id, bool enabled) {
    String csv = prefs_.getString(kEnabledKey, "");
    csv = withIdRemoved(csv, id);
    if (enabled) {
        csv = csv.isEmpty() ? String(id) : (csv + "," + id);
    }
    prefs_.putString(kEnabledKey, csv);
    ++gRevision;
    refresh();
}

void PluginLibrary::setOrder(const String& csvIds) {
    String csv;
    for (const String& id : splitCsv(csvIds)) {
        if (!BuiltinPlugins::find(id.c_str()) || containsId(csv, id.c_str())) continue;
        if (!csv.isEmpty()) csv += ",";
        csv += id;
    }
    prefs_.putString(kOrderKey, csv);
    ++gRevision;
    refresh();
}
