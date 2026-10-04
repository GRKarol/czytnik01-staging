#include "ble/BleApi.h"
#include "text/UnicodeFold.h"

#if FLOWER_BLE_ENABLED

#include <NimBLEDevice.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>

#include <atomic>
#include <algorithm>
#include <vector>

#include "app/App.h"

namespace {

// --- UUIDs (shared with Android app) ---
constexpr const char *kServiceUuid = "f10e7e10-f10e-7e10-f10e-7e10f10e7e10";
constexpr const char *kCmdCharUuid = "f10e7e11-f10e-7e10-f10e-7e10f10e7e10";
constexpr const char *kEvtCharUuid = "f10e7e12-f10e-7e10-f10e-7e10f10e7e10";

// --- NVS keys ---
constexpr const char *kNvsNamespace = "ble";
constexpr const char *kNvsTokenKey = "auth_token";

// --- Limits ---
constexpr uint16_t kPreferredMtu = 512;
constexpr size_t kPendingCommandsMax = 8;
constexpr size_t kMaxReassemblySize = 32768;  // 32 KB max inbound message (for upload chunks)

// --- File upload limits ---
constexpr size_t kUploadMaxFileSize = 10 * 1024 * 1024;  // 10 MB max file
constexpr size_t kUploadChunkMaxBase64 = 16384;           // max base64 chars per chunk

// --- Chunked framing flags ---
constexpr uint8_t kFlagMore = 0x01;   // bit 0
constexpr uint8_t kFlagStart = 0x02;  // bit 1
// 0x02 = single complete message (START=1, MORE=0)
// 0x03 = first of multi-chunk   (START=1, MORE=1)
// 0x01 = middle chunk            (START=0, MORE=1)
// 0x00 = last chunk              (START=0, MORE=0)

String deviceSuffix() {
  uint64_t chip = ESP.getEfuseMac();
  char buf[7];
  snprintf(buf, sizeof(buf), "%02X%02X%02X", static_cast<unsigned>((chip >> 16) & 0xFF),
           static_cast<unsigned>((chip >> 8) & 0xFF), static_cast<unsigned>(chip & 0xFF));
  return String(buf);
}

// --- Minimal JSON helpers (no ArduinoJson dependency) ---

int jsonFieldStart(const String &body, const char *key) {
  String pattern = String("\"") + key + "\"";
  int p = body.indexOf(pattern);
  if (p < 0) return -1;
  p += pattern.length();
  while (p < static_cast<int>(body.length()) &&
         (body[p] == ' ' || body[p] == '\t' || body[p] == ':')) {
    p++;
  }
  return p;
}

String jsonReadString(const String &body, const char *key) {
  int p = jsonFieldStart(body, key);
  if (p < 0 || p >= static_cast<int>(body.length()) || body[p] != '"') return "";
  int end = body.indexOf('"', p + 1);
  if (end < 0) return "";
  return body.substring(p + 1, end);
}

bool jsonStringEquals(const String &body, const char *key, const char *expected) {
  String val = jsonReadString(body, key);
  return val == expected;
}

String jsonReadRawValue(const String &body, const char *key) {
  int p = jsonFieldStart(body, key);
  if (p < 0) return "";
  // Find end: next comma or closing brace at same nesting level
  int depth = 0;
  int start = p;
  bool inStr = false;
  for (int i = p; i < static_cast<int>(body.length()); i++) {
    char c = body[i];
    if (c == '"' && (i == 0 || body[i - 1] != '\\')) inStr = !inStr;
    if (inStr) continue;
    if (c == '{' || c == '[') depth++;
    else if (c == '}' || c == ']') {
      if (depth == 0) return body.substring(start, i);
      depth--;
    }
    else if (c == ',' && depth == 0) return body.substring(start, i);
  }
  return body.substring(start);
}

}  // namespace

// Base64 decode helper (no external dependency)
static const int8_t kBase64DecTable[256] = {
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
  52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
  -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
  15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
  -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
  41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

static size_t base64Decode(const char *src, size_t srcLen, uint8_t *dst, size_t dstCap) {
  size_t written = 0;
  uint32_t accum = 0;
  int bits = 0;
  for (size_t i = 0; i < srcLen; ++i) {
    uint8_t c = static_cast<uint8_t>(src[i]);
    if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
    int8_t val = kBase64DecTable[c];
    if (val < 0) continue;
    accum = (accum << 6) | static_cast<uint32_t>(val);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (written < dstCap) {
        dst[written++] = static_cast<uint8_t>((accum >> bits) & 0xFF);
      }
    }
  }
  return written;
}

// JSON string escape helper — escapes quotes, backslashes, and control chars.
// UTF-8 multi-byte sequences pass through unchanged (valid JSON).
static String jsonEscape(const String &s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = static_cast<uint8_t>(s[i]);
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (c < 0x20) {
      // Control char — skip
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

// ============================================================================
// BleApi::Impl
// ============================================================================

struct BleApi::Impl : public NimBLEServerCallbacks, public NimBLECharacteristicCallbacks {
  App *app = nullptr;
  NimBLEServer *server = nullptr;
  NimBLEService *service = nullptr;
  NimBLECharacteristic *cmdChar = nullptr;
  NimBLECharacteristic *evtChar = nullptr;
  String name;
  String authToken;  // loaded from NVS on begin()

  std::atomic<bool> active{false};
  std::atomic<bool> clientConnected{false};
  std::atomic<bool> authenticated{false};
  std::atomic<bool> shuttingDown{false};
  std::atomic<bool> menuDirty{false};
  std::atomic<bool> notifyReady{false};

  // Deferred command queue (BLE host task → main task)
  std::vector<String> pendingCommands;
  portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED;
  std::atomic<bool> pendingNonEmpty{false};

  // Inbound reassembly buffer (BLE host task only)
  String reassemblyBuf;
  bool reassemblyInProgress = false;

  uint16_t negotiatedMtu = 23;  // default BLE 4.0 MTU

  // --- File upload state ---
  File uploadFile;
  String uploadFilename;
  size_t uploadExpectedSize = 0;
  size_t uploadReceivedBytes = 0;
  bool uploadInProgress = false;

  // --- NimBLEServerCallbacks ---

  void onConnect(NimBLEServer *, ble_gap_conn_desc *desc) override {
    if (shuttingDown.load(std::memory_order_acquire)) return;
    clientConnected.store(true, std::memory_order_release);
    authenticated.store(false, std::memory_order_release);
    notifyReady.store(false, std::memory_order_release);
    menuDirty.store(true, std::memory_order_release);
    reassemblyBuf = "";
    reassemblyInProgress = false;
    negotiatedMtu = 23;
    Serial.println("[ble] client connected");
  }

  void onDisconnect(NimBLEServer *) override {
    if (shuttingDown.load(std::memory_order_acquire)) return;
    clientConnected.store(false, std::memory_order_release);
    authenticated.store(false, std::memory_order_release);
    notifyReady.store(false, std::memory_order_release);
    menuDirty.store(true, std::memory_order_release);
    reassemblyBuf = "";
    reassemblyInProgress = false;
    // Abort any in-progress upload on disconnect
    if (uploadInProgress) {
      abortUpload();
    }
    Serial.println("[ble] client disconnected, restart advertising");
    NimBLEDevice::startAdvertising();
  }

  void onMTUChange(uint16_t mtu, ble_gap_conn_desc *) override {
    negotiatedMtu = mtu;
    Serial.printf("[ble] MTU changed to %u\n", mtu);
  }

  // --- NimBLECharacteristicCallbacks ---

  void onWrite(NimBLECharacteristic *ch) override {
    if (shuttingDown.load(std::memory_order_acquire)) return;
    const std::string &v = ch->getValue();
    if (v.empty()) return;

    // First byte is framing flags
    uint8_t flags = static_cast<uint8_t>(v[0]);
    const char *payload = v.c_str() + 1;
    size_t payloadLen = v.length() - 1;

    bool isStart = (flags & kFlagStart) != 0;
    bool isMore = (flags & kFlagMore) != 0;

    if (isStart) {
      // Start of new message — reset buffer
      reassemblyBuf = "";
      reassemblyInProgress = true;
    }

    if (!reassemblyInProgress) {
      // Got continuation without start — discard
      Serial.println("[ble] chunk without START, discarding");
      return;
    }

    // Append payload
    if (reassemblyBuf.length() + payloadLen > kMaxReassemblySize) {
      Serial.println("[ble] reassembly overflow, discarding");
      reassemblyBuf = "";
      reassemblyInProgress = false;
      return;
    }
    reassemblyBuf += String(payload, payloadLen);

    if (!isMore) {
      // Message complete — enqueue for main task processing
      reassemblyInProgress = false;
      String msg = reassemblyBuf;
      reassemblyBuf = "";
      msg.trim();
      if (msg.length() > 0) {
        enqueueCommand(msg);
      }
    }
  }

  void onSubscribe(NimBLECharacteristic *ch, ble_gap_conn_desc *, uint16_t subValue) override {
    if (shuttingDown.load(std::memory_order_acquire)) return;
    if (ch != evtChar) return;
    const bool wantsNotify = (subValue & 0x0001) != 0;
    notifyReady.store(wantsNotify, std::memory_order_release);
    if (wantsNotify) {
      Serial.println("[ble] client subscribed to events");
    }
  }

  // --- Queue management ---

  void enqueueCommand(const String &line) {
    portENTER_CRITICAL(&pendingMux);
    if (pendingCommands.size() < kPendingCommandsMax) {
      pendingCommands.push_back(line);
      pendingNonEmpty.store(true, std::memory_order_release);
    }
    portEXIT_CRITICAL(&pendingMux);
  }

  // --- Chunked send (EVT notify) ---

  void sendChunkedEvent(const String &json) {
    if (!evtChar) return;
    if (!clientConnected.load(std::memory_order_acquire)) return;
    if (!notifyReady.load(std::memory_order_acquire)) return;

    String msg = json + "\n";
    uint16_t maxPayload = (negotiatedMtu > 4) ? (negotiatedMtu - 4) : 20;
    // -4: 3 bytes ATT header + 1 byte our framing flag

    if (msg.length() <= maxPayload) {
      // Single packet
      uint8_t flags = kFlagStart;  // 0x02 = START=1, MORE=0
      sendOneChunk(flags, reinterpret_cast<const uint8_t *>(msg.c_str()), msg.length());
    } else {
      // Multi-chunk
      size_t offset = 0;
      size_t remaining = msg.length();
      bool first = true;
      while (remaining > 0) {
        size_t chunkSize = (remaining > maxPayload) ? maxPayload : remaining;
        bool more = (remaining - chunkSize) > 0;
        uint8_t flags = 0;
        if (first) flags |= kFlagStart;
        if (more) flags |= kFlagMore;
        sendOneChunk(flags, reinterpret_cast<const uint8_t *>(msg.c_str() + offset), chunkSize);
        offset += chunkSize;
        remaining -= chunkSize;
        first = false;
        // Small delay between chunks to avoid BLE congestion
        if (more) delay(5);
      }
    }
  }

  void sendOneChunk(uint8_t flags, const uint8_t *data, size_t len) {
    // Build packet: [flags][payload]
    std::vector<uint8_t> packet(1 + len);
    packet[0] = flags;
    memcpy(packet.data() + 1, data, len);
    evtChar->setValue(packet.data(), packet.size());
    evtChar->notify();
  }

  // --- Command processing (main task) ---

  void drainPendingCommands() {
    if (!pendingNonEmpty.load(std::memory_order_acquire)) return;

    std::vector<String> local;
    portENTER_CRITICAL(&pendingMux);
    local.swap(pendingCommands);
    pendingNonEmpty.store(false, std::memory_order_release);
    portEXIT_CRITICAL(&pendingMux);

    for (const String &line : local) {
      handleCommand(line);
    }
  }

  void handleCommand(const String &line) {
    Serial.printf("[ble] cmd: %s\n", line.c_str());

    String cmd = jsonReadString(line, "cmd");

    // Auth command is always allowed (even before authentication)
    if (cmd == "auth") {
      handleAuth(line);
      return;
    }

    // All other commands require authentication
    if (!authenticated.load(std::memory_order_acquire)) {
      sendChunkedEvent("{\"ev\":\"auth-required\"}");
      return;
    }

    if (cmd == "ping") {
      sendChunkedEvent("{\"ev\":\"pong\",\"ts\":" + String(millis()) + "}");
    } else if (cmd == "get-settings") {
      handleGetSettings();
    } else if (cmd == "set-settings") {
      handleSetSettings(line);
    } else if (cmd == "get-books") {
      handleGetBooks();
    } else if (cmd == "get-status") {
      handleGetStatus();
    } else if (cmd == "start-wifi") {
      handleStartWifi(line);
    } else if (cmd == "stop-wifi") {
      handleStopWifi();
    } else if (cmd == "get-wifi") {
      handleGetWifi();
    } else if (cmd == "set-wifi") {
      handleSetWifi(line);
    } else if (cmd == "reboot") {
      sendChunkedEvent("{\"ev\":\"reboot-ack\"}");
      delay(200);
      ESP.restart();
    } else if (cmd == "get-version") {
      String v = app ? app->firmwareVersionLabel() : String("dev");
      sendChunkedEvent("{\"ev\":\"version\",\"value\":\"" + v + "\",\"buildDate\":\"" + String(__DATE__) + "\"}");
    } else if (cmd == "upload-begin") {
      handleUploadBegin(line);
    } else if (cmd == "upload-chunk") {
      handleUploadChunk(line);
    } else if (cmd == "upload-end") {
      handleUploadEnd();
    } else {
      sendChunkedEvent("{\"ev\":\"error\",\"reason\":\"unknown-cmd\",\"cmd\":\"" + cmd + "\"}");
    }
  }

  // --- Auth ---

  void handleAuth(const String &line) {
    String token = jsonReadString(line, "token");

    if (authToken.isEmpty()) {
      // No token generated yet — device not set up for pairing
      sendChunkedEvent("{\"ev\":\"auth-fail\",\"reason\":\"not-paired\"}");
      return;
    }

    if (token == authToken) {
      authenticated.store(true, std::memory_order_release);
      menuDirty.store(true, std::memory_order_release);
      sendChunkedEvent("{\"ev\":\"auth-ok\",\"api\":2}");
      Serial.println("[ble] auth OK");
    } else {
      sendChunkedEvent("{\"ev\":\"auth-fail\",\"reason\":\"invalid-token\"}");
      Serial.println("[ble] auth FAIL — wrong token");
    }
  }

  // --- Settings ---

  void handleGetSettings() {
    if (!app) return;
    String json = buildSettingsJson();
    sendChunkedEvent("{\"ev\":\"settings\",\"data\":" + json + "}");
  }

  void handleSetSettings(const String &line) {
    if (!app) return;
    // Accept both "data" and "settings" field names (app sends "settings")
    String dataStr = jsonReadRawValue(line, "data");
    if (dataStr.isEmpty()) {
      dataStr = jsonReadRawValue(line, "settings");
    }
    if (dataStr.isEmpty()) {
      sendChunkedEvent("{\"ev\":\"error\",\"reason\":\"missing-data\"}");
      return;
    }
    String error;
    // Reuse existing settings apply logic from CompanionSyncManager
    if (app->companionSync_.applySettingsFromBle(dataStr, error)) {
      // The current values back, as the HTTP PUT answers.
      sendChunkedEvent("{\"ev\":\"settings-ok\",\"data\":" + buildSettingsJson() + "}");
    } else {
      sendChunkedEvent("{\"ev\":\"error\",\"reason\":\"" + error + "\"}");
    }
  }

  // --- Books ---

  void handleGetBooks() {
    if (!app) return;
    // Build books array JSON
    String json = "[";
    auto &storage = app->storage_;
    size_t count = storage.bookCount();
    for (size_t i = 0; i < count; i++) {
      if (i > 0) json += ",";
      json += buildBookJson(i);
    }
    json += "]";
    sendChunkedEvent("{\"ev\":\"books\",\"data\":" + json + "}");
  }

  String buildBookJson(size_t index) {
    auto &storage = app->storage_;
    String path = storage.bookPath(index);
    // Use raw UTF-8 title from file (not display-normalized version)
    String title = readRawRsvpTitle(path);
    if (title.isEmpty()) {
      title = storage.bookDisplayName(index);  // fallback to display name
    }
    String author = readRawRsvpAuthor(path);
    if (author.isEmpty()) {
      author = storage.bookAuthorName(index);
    }

    // Category
    String category = "book";
    if (path.startsWith("articles/")) category = "article";
    else if (!path.startsWith("books/")) category = "legacy";

    // Progress
    uint8_t progress = 0;
    app->bookProgressPercent(index, progress);

    String json = "{\"name\":\"" + jsonEscape(path) + "\",\"category\":\"" + category +
                  "\",\"title\":\"" + jsonEscape(UnicodeFold::toUtf8(title)) + "\",\"author\":\"" + jsonEscape(UnicodeFold::toUtf8(author)) +
                  "\",\"progressPercent\":" + String(progress) + "}";
    return json;
  }

  // Read raw @title from .rsvp file without display normalization (preserves UTF-8)
  String readRawRsvpTitle(const String &path) {
    if (!path.endsWith(".rsvp")) return "";
    File file = SD_MMC.open(path);
    if (!file || file.isDirectory()) { if (file) file.close(); return ""; }
    String result;
    String line;
    while (file.available() && line.length() < 512) {
      char c = static_cast<char>(file.read());
      if (c == '\r') continue;
      if (c != '\n') { line += c; continue; }
      line.trim();
      if (line.length() == 0) { line = ""; continue; }
      String lower = line; lower.toLowerCase();
      if (lower.startsWith("@title")) {
        result = line.substring(6);
        result.trim();
        if (result.length() > 0 && (result[0] == ':' || result[0] == ' ')) {
          result = result.substring(1);
          result.trim();
        }
        break;
      }
      if (!line.startsWith("@")) break;
      line = "";
    }
    file.close();
    return result;
  }

  String readRawRsvpAuthor(const String &path) {
    if (!path.endsWith(".rsvp")) return "";
    File file = SD_MMC.open(path);
    if (!file || file.isDirectory()) { if (file) file.close(); return ""; }
    String result;
    String line;
    while (file.available() && line.length() < 512) {
      char c = static_cast<char>(file.read());
      if (c == '\r') continue;
      if (c != '\n') { line += c; continue; }
      line.trim();
      if (line.length() == 0) { line = ""; continue; }
      String lower = line; lower.toLowerCase();
      if (lower.startsWith("@author")) {
        result = line.substring(7);
        result.trim();
        if (result.length() > 0 && (result[0] == ':' || result[0] == ' ')) {
          result = result.substring(1);
          result.trim();
        }
        break;
      }
      if (!line.startsWith("@")) break;
      line = "";
    }
    file.close();
    return result;
  }

  // --- Status ---

  void handleGetStatus() {
    if (!app) return;
    String version = app->firmwareVersionLabel();
    uint8_t battery = app->batteryDisplayedPercent_;
    uint16_t wpm = app->reader_.wpm();
    String book = app->currentBookTitle_;
    size_t wordIndex = app->reader_.currentIndex();

    String json = "{\"ev\":\"status\",\"battery\":" + String(battery) +
                  ",\"version\":\"" + version +
                  "\",\"wpm\":" + String(wpm) +
                  ",\"book\":\"" + jsonEscape(book) +
                  "\",\"wordIndex\":" + String(wordIndex) + "}";
    sendChunkedEvent(json);
  }

  // --- WiFi burst ---

  void handleStartWifi(const String &line) {
    if (!app) return;
    String reason = jsonReadString(line, "reason");

    // Use CompanionSyncManager which starts AP + HTTP server + all endpoints
    CompanionSyncManager::Config syncConfig;
    syncConfig.wifiSsid = "";
    syncConfig.wifiPassword = "";

    if (!app->companionSync_.begin(syncConfig)) {
      sendChunkedEvent("{\"ev\":\"error\",\"reason\":\"wifi-start-failed\"}");
      return;
    }

    // Get the AP details from CompanionSyncManager
    String ssid = app->companionSync_.statusLine1();  // returns SSID
    String ip = "192.168.4.1";

    // Generate a simple password info (the AP is open in current impl,
    // but we report it for protocol compliance)
    // Note: CompanionSyncManager currently creates open AP. For security
    // per protocol v2, we should add WPA2 — but for now report no password.
    Serial.printf("[ble] WiFi burst started: %s ip=%s reason=%s\n",
                  ssid.c_str(), ip.c_str(), reason.c_str());

    sendChunkedEvent("{\"ev\":\"wifi-ready\",\"ssid\":\"" + ssid +
                     "\",\"pass\":\"\",\"ip\":\"" + ip + "\"}");
  }

  void handleStopWifi() {
    if (app) {
      app->companionSync_.end();
    }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[ble] WiFi AP stopped");
    sendChunkedEvent("{\"ev\":\"wifi-stopped\"}");
  }

  // --- WiFi credentials ---

  void handleGetWifi() {
    if (!app) return;
    Preferences prefs;
    prefs.begin("rsvp", true);
    String ssid = prefs.getString("wifi_ssid", "");
    prefs.end();
    bool configured = ssid.length() > 0;
    sendChunkedEvent("{\"ev\":\"wifi\",\"configured\":" +
                     String(configured ? "true" : "false") +
                     ",\"ssid\":\"" + ssid + "\"}");
  }

  void handleSetWifi(const String &line) {
    if (!app) return;
    String ssid = jsonReadString(line, "ssid");
    String password = jsonReadString(line, "password");
    if (ssid.isEmpty()) {
      sendChunkedEvent("{\"ev\":\"error\",\"reason\":\"missing-ssid\"}");
      return;
    }
    Preferences prefs;
    prefs.begin("rsvp", false);
    prefs.putString("wifi_ssid", ssid);
    prefs.putString("wifi_pass", password);
    prefs.end();
    Serial.printf("[ble] WiFi credentials saved: ssid=%s\n", ssid.c_str());
    sendChunkedEvent("{\"ev\":\"wifi-ok\",\"configured\":true,\"ssid\":\"" + ssid + "\"}");
  }

  // --- File Upload via BLE ---

  void handleUploadBegin(const String &line) {
    if (!app) return;

    if (uploadInProgress) {
      // Abort previous incomplete upload
      if (uploadFile) {
        uploadFile.close();
        SD_MMC.remove(("/books/books/" + uploadFilename).c_str());
      }
      uploadInProgress = false;
    }

    String name = jsonReadString(line, "name");
    String sizeStr = jsonReadRawValue(line, "size");
    size_t fileSize = static_cast<size_t>(sizeStr.toInt());

    if (name.isEmpty()) {
      sendChunkedEvent("{\"ev\":\"upload-error\",\"reason\":\"missing-name\"}");
      return;
    }
    if (fileSize == 0 || fileSize > kUploadMaxFileSize) {
      sendChunkedEvent("{\"ev\":\"upload-error\",\"reason\":\"invalid-size\"}");
      return;
    }

    // Sanitize filename — keep only last path component
    int lastSlash = name.lastIndexOf('/');
    if (lastSlash >= 0) name = name.substring(lastSlash + 1);
    lastSlash = name.lastIndexOf('\\');
    if (lastSlash >= 0) name = name.substring(lastSlash + 1);

    if (name.isEmpty()) {
      sendChunkedEvent("{\"ev\":\"upload-error\",\"reason\":\"invalid-name\"}");
      return;
    }

    // Open file for writing in /books/books/
    String path = "/books/books/" + name;
    uploadFile = SD_MMC.open(path.c_str(), FILE_WRITE);
    if (!uploadFile) {
      sendChunkedEvent("{\"ev\":\"upload-error\",\"reason\":\"cannot-create-file\"}");
      Serial.printf("[ble-upload] cannot create file: %s\n", path.c_str());
      return;
    }

    uploadFilename = name;
    uploadExpectedSize = fileSize;
    uploadReceivedBytes = 0;
    uploadInProgress = true;

    Serial.printf("[ble-upload] started: %s (%u bytes)\n", name.c_str(),
                  static_cast<unsigned int>(fileSize));
    sendChunkedEvent("{\"ev\":\"upload-ready\",\"name\":\"" + jsonEscape(name) +
                     "\",\"size\":" + String(fileSize) + "}");
  }

  void handleUploadChunk(const String &line) {
    if (!uploadInProgress || !uploadFile) {
      sendChunkedEvent("{\"ev\":\"upload-error\",\"reason\":\"no-upload-active\"}");
      return;
    }

    // Extract base64 data from "d" field
    String b64 = jsonReadString(line, "d");
    if (b64.isEmpty()) {
      sendChunkedEvent("{\"ev\":\"upload-error\",\"reason\":\"empty-chunk\"}");
      return;
    }

    // Decode base64 in place
    size_t decodedMaxSize = (b64.length() * 3) / 4 + 4;
    uint8_t *decodeBuf = static_cast<uint8_t *>(malloc(decodedMaxSize));
    if (!decodeBuf) {
      sendChunkedEvent("{\"ev\":\"upload-error\",\"reason\":\"out-of-memory\"}");
      abortUpload();
      return;
    }

    size_t decodedLen = base64Decode(b64.c_str(), b64.length(), decodeBuf, decodedMaxSize);
    if (decodedLen == 0) {
      free(decodeBuf);
      sendChunkedEvent("{\"ev\":\"upload-error\",\"reason\":\"decode-failed\"}");
      abortUpload();
      return;
    }

    // Write to file
    size_t written = uploadFile.write(decodeBuf, decodedLen);
    free(decodeBuf);

    if (written != decodedLen) {
      sendChunkedEvent("{\"ev\":\"upload-error\",\"reason\":\"write-failed\"}");
      abortUpload();
      return;
    }

    uploadReceivedBytes += written;

    // Send progress every chunk
    uint8_t percent = 0;
    if (uploadExpectedSize > 0) {
      percent = static_cast<uint8_t>(
          std::min(static_cast<size_t>(100),
                   (uploadReceivedBytes * 100) / uploadExpectedSize));
    }
    sendChunkedEvent("{\"ev\":\"upload-progress\",\"received\":" +
                     String(uploadReceivedBytes) + ",\"percent\":" + String(percent) + "}");
  }

  void handleUploadEnd() {
    if (!uploadInProgress || !uploadFile) {
      sendChunkedEvent("{\"ev\":\"upload-error\",\"reason\":\"no-upload-active\"}");
      return;
    }

    uploadFile.close();
    uploadInProgress = false;

    Serial.printf("[ble-upload] complete: %s (%u/%u bytes)\n",
                  uploadFilename.c_str(),
                  static_cast<unsigned int>(uploadReceivedBytes),
                  static_cast<unsigned int>(uploadExpectedSize));

    // Refresh the book library
    if (app) {
      app->storage_.refreshBooks();
    }

    sendChunkedEvent("{\"ev\":\"upload-complete\",\"name\":\"" + jsonEscape(uploadFilename) +
                     "\",\"bytes\":" + String(uploadReceivedBytes) + "}");

    uploadFilename = "";
    uploadExpectedSize = 0;
    uploadReceivedBytes = 0;
  }

  void abortUpload() {
    if (uploadFile) {
      uploadFile.close();
      if (!uploadFilename.isEmpty()) {
        String path = "/books/books/" + uploadFilename;
        SD_MMC.remove(path.c_str());
      }
    }
    uploadInProgress = false;
    uploadFilename = "";
    uploadExpectedSize = 0;
    uploadReceivedBytes = 0;
    Serial.println("[ble-upload] aborted");
  }

  // --- Settings JSON builder ---

  String buildSettingsJson() {
    if (!app) return "{}";
    // Use CompanionSyncManager's existing settingsJson() method
    return app->companionSync_.settingsJsonForBle();
  }

  // --- Token NVS ---

  void loadTokenFromNvs() {
    Preferences prefs;
    prefs.begin(kNvsNamespace, true);
    authToken = prefs.getString(kNvsTokenKey, "");
    prefs.end();
    if (authToken.length() > 0) {
      Serial.printf("[ble] token loaded from NVS (%u chars)\n",
                    static_cast<unsigned>(authToken.length()));
    } else {
      Serial.println("[ble] no token in NVS (not yet paired)");
    }
  }

  void saveTokenToNvs() {
    Preferences prefs;
    prefs.begin(kNvsNamespace, false);
    prefs.putString(kNvsTokenKey, authToken);
    prefs.end();
    Serial.println("[ble] token saved to NVS");
  }

  void clearTokenFromNvs() {
    Preferences prefs;
    prefs.begin(kNvsNamespace, false);
    prefs.remove(kNvsTokenKey);
    prefs.end();
    authToken = "";
    Serial.println("[ble] token cleared from NVS");
  }

  void generateToken() {
    char hex[65];
    for (int i = 0; i < 32; i++) {
      uint8_t b = esp_random() & 0xFF;
      snprintf(hex + i * 2, 3, "%02x", b);
    }
    hex[64] = '\0';
    authToken = String(hex);
    saveTokenToNvs();
    Serial.printf("[ble] new token generated: %s...%s\n",
                  authToken.substring(0, 8).c_str(),
                  authToken.substring(56).c_str());
  }
};

// ============================================================================
// BleApi public methods
// ============================================================================

BleApi::BleApi() : impl_(nullptr) {}

BleApi::~BleApi() { stop(); }

void BleApi::begin(App *app) {
  if (impl_ != nullptr) return;

  impl_ = new Impl();
  impl_->app = app;
  impl_->name = "Flower-" + deviceSuffix();

  // Load auth token from NVS
  impl_->loadTokenFromNvs();

  NimBLEDevice::init(impl_->name.c_str());
  NimBLEDevice::setPower(ESP_PWR_LVL_N12, ESP_BLE_PWR_TYPE_ADV);
  NimBLEDevice::setPower(ESP_PWR_LVL_N12, ESP_BLE_PWR_TYPE_SCAN);
  NimBLEDevice::setPower(ESP_PWR_LVL_N12, ESP_BLE_PWR_TYPE_DEFAULT);
  NimBLEDevice::setMTU(kPreferredMtu);

  impl_->server = NimBLEDevice::createServer();
  impl_->server->setCallbacks(impl_);

  impl_->service = impl_->server->createService(kServiceUuid);

  // CMD: write with response (WRITE) + write without response (WRITE_NR)
  impl_->cmdChar = impl_->service->createCharacteristic(
      kCmdCharUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  impl_->cmdChar->setCallbacks(impl_);

  // EVT: notify + read (read for initial value / debug)
  impl_->evtChar = impl_->service->createCharacteristic(
      kEvtCharUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  impl_->evtChar->setCallbacks(impl_);

  impl_->service->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(kServiceUuid);
  adv->setScanResponse(true);
  // Force service UUID into advertising data (not just scan response).
  // Android ScanFilter.setServiceUuid() only matches advData, not scan response.
  // NimBLE with setScanResponse(true) may split data — ensure UUID is in advData.
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.setCompleteServices(NimBLEUUID(kServiceUuid));
  adv->setAdvertisementData(advData);
  // Put full name in scan response (saves advData space for UUID)
  NimBLEAdvertisementData scanResp;
  scanResp.setName(impl_->name.c_str());
  adv->setScanResponseData(scanResp);
  adv->setMinPreferred(0x06);
  adv->setMaxPreferred(0x12);
  // 1000ms advertising interval — optimized for battery life.
  // Device is always advertising so slow interval is fine; phone will
  // still discover it within 2-3 seconds.
  adv->setMinInterval(0x640);   // 1000ms
  adv->setMaxInterval(0x6C0);   // 1075ms
  NimBLEDevice::startAdvertising();

  impl_->active.store(true, std::memory_order_release);
  Serial.printf("[ble] advertising as %s (token: %s)\n",
                impl_->name.c_str(),
                impl_->authToken.isEmpty() ? "none" : "set");
}

void BleApi::stop() {
  if (impl_ == nullptr) return;

  impl_->shuttingDown.store(true, std::memory_order_release);

  if (impl_->server != nullptr) impl_->server->setCallbacks(nullptr);
  if (impl_->cmdChar != nullptr) impl_->cmdChar->setCallbacks(nullptr);
  if (impl_->evtChar != nullptr) impl_->evtChar->setCallbacks(nullptr);

  NimBLEDevice::stopAdvertising();
  if (impl_->server != nullptr) {
    auto peers = impl_->server->getPeerDevices();
    for (auto handle : peers) impl_->server->disconnect(handle);
  }

  NimBLEDevice::deinit(true);
  delete impl_;
  impl_ = nullptr;
  Serial.println("[ble] stopped");
}

void BleApi::update() {
  if (impl_ == nullptr) return;
  impl_->drainPendingCommands();
}

bool BleApi::isActive() const {
  return impl_ != nullptr && impl_->active.load(std::memory_order_acquire);
}

bool BleApi::isConnected() const {
  return impl_ != nullptr && impl_->clientConnected.load(std::memory_order_acquire);
}

bool BleApi::isAuthenticated() const {
  return impl_ != nullptr && impl_->authenticated.load(std::memory_order_acquire);
}

String BleApi::deviceName() const {
  return impl_ != nullptr ? impl_->name : String("");
}

void BleApi::emitEvent(const String &json) {
  if (impl_) impl_->sendChunkedEvent(json);
}

bool BleApi::consumeMenuDirty() {
  if (impl_ == nullptr) return false;
  return impl_->menuDirty.exchange(false, std::memory_order_acq_rel);
}

void BleApi::generateNewToken() {
  if (impl_) impl_->generateToken();
}

String BleApi::currentToken() const {
  return impl_ ? impl_->authToken : String("");
}

bool BleApi::hasToken() const {
  return impl_ != nullptr && impl_->authToken.length() > 0;
}

void BleApi::clearToken() {
  if (impl_) impl_->clearTokenFromNvs();
}

String BleApi::qrPayload() const {
  if (!impl_ || impl_->authToken.isEmpty()) return "";
  return "flower://pair?t=" + impl_->authToken + "&n=" + impl_->name;
}

#else  // FLOWER_BLE_ENABLED == 0 — stub

BleApi::BleApi() {}
BleApi::~BleApi() {}
void BleApi::begin(App *) {}
void BleApi::stop() {}
void BleApi::update() {}
bool BleApi::isActive() const { return false; }
bool BleApi::isConnected() const { return false; }
bool BleApi::isAuthenticated() const { return false; }
String BleApi::deviceName() const { return String(""); }
void BleApi::emitEvent(const String &) {}
bool BleApi::consumeMenuDirty() { return false; }
void BleApi::generateNewToken() {}
String BleApi::currentToken() const { return String(""); }
bool BleApi::hasToken() const { return false; }
void BleApi::clearToken() {}
String BleApi::qrPayload() const { return String(""); }

#endif  // FLOWER_BLE_ENABLED
