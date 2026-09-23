#include "update/OtaUpdater.h"

#include <algorithm>

#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#ifndef RSVP_FIRMWARE_VERSION
#define RSVP_FIRMWARE_VERSION "dev"
#endif

// font_dl, book_dl i ekran Wi-Fi (main task) mogą wywołać connectWiFi()/
// disconnectWiFi() z różnych zadań FreeRTOS w tym samym czasie. Bez tego
// mutexa jedno zadanie robi WiFi.mode(WIFI_OFF) (pełny teardown sterownika)
// dokładnie w chwili, gdy drugie ma otwarty WiFiClientSecure/HTTPClient —
// destruktor tego drugiego wywołuje wtedy stop() na już zwolnionych
// buforach sterownika Wi-Fi, co daje PANIC/LoadProhibited (potwierdzone
// w coredumpie). Mutex trzyma całą sesję connect->...->disconnect jako
// jeden blok, więc druga sesja czeka, zamiast wchodzić w kolizję. Zewnętrzna
// (nie w anonimowej przestrzeni nazw) bo CompanionSyncManager też musi
// wejść w tę samą sesję zamiast ją omijać — patrz deklaracja w OtaUpdater.h.
SemaphoreHandle_t wifiSessionMutex() {
  static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
  return mutex;
}

namespace {

constexpr const char *kConfigPaths[] = {
    "/config/ota.conf",
    "/ota.conf",
};
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint32_t kWifiConnectPollMs = 250;
constexpr size_t kMaxReleaseJsonBytes = 32768;
constexpr const char *kStatusTitle = "OTA";
const char *kRedirectHeaderKeys[] = {
    "Location",
};

bool isAsciiWhitespace(char c) {
  switch (c) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case '\f':
    case '\v':
      return true;
    default:
      return false;
  }
}

String trimCopy(String value) {
  value.trim();
  return value;
}

bool parseBoolValue(const String &value) {
  String lowered = trimCopy(value);
  lowered.toLowerCase();
  return lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on";
}

String jsonUnescape(const String &input) {
  String output;
  output.reserve(input.length());

  bool escaping = false;
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    if (escaping) {
      switch (c) {
        case '"':
        case '\\':
        case '/':
          output += c;
          break;
        case 'b':
          output += '\b';
          break;
        case 'f':
          output += '\f';
          break;
        case 'n':
          output += '\n';
          break;
        case 'r':
          output += '\r';
          break;
        case 't':
          output += '\t';
          break;
        default:
          output += c;
          break;
      }
      escaping = false;
      continue;
    }

    if (c == '\\') {
      escaping = true;
      continue;
    }

    output += c;
  }

  return output;
}

bool parseJsonStringAt(const String &json, int quoteIndex, String &value) {
  if (quoteIndex < 0 || static_cast<size_t>(quoteIndex) >= json.length() ||
      json[quoteIndex] != '"') {
    return false;
  }

  String raw;
  raw.reserve(64);
  bool escaping = false;
  for (size_t i = static_cast<size_t>(quoteIndex) + 1; i < json.length(); ++i) {
    const char c = json[i];
    if (!escaping && c == '"') {
      value = jsonUnescape(raw);
      return true;
    }

    raw += c;
    if (escaping) {
      escaping = false;
    } else if (c == '\\') {
      escaping = true;
    }
  }

  return false;
}

bool extractJsonStringValue(const String &json, const char *key, size_t searchStart, String &value,
                            int *keyPosition = nullptr) {
  const String pattern = "\"" + String(key) + "\"";
  const int keyIndex = json.indexOf(pattern, static_cast<unsigned int>(searchStart));
  if (keyIndex < 0) {
    return false;
  }

  const int colonIndex = json.indexOf(':', keyIndex + pattern.length());
  if (colonIndex < 0) {
    return false;
  }

  int quoteIndex = colonIndex + 1;
  while (static_cast<size_t>(quoteIndex) < json.length() && isAsciiWhitespace(json[quoteIndex])) {
    ++quoteIndex;
  }
  if (static_cast<size_t>(quoteIndex) >= json.length() || json[quoteIndex] != '"') {
    return false;
  }

  if (keyPosition != nullptr) {
    *keyPosition = keyIndex;
  }
  return parseJsonStringAt(json, quoteIndex, value);
}

bool extractAssetDownloadUrl(const String &json, const String &assetName, String &assetUrl) {
  size_t searchStart = 0;
  String candidateName;
  int nameKeyIndex = -1;
  while (extractJsonStringValue(json, "name", searchStart, candidateName, &nameKeyIndex)) {
    if (candidateName == assetName &&
        extractJsonStringValue(json, "browser_download_url",
                               static_cast<size_t>(std::max(0, nameKeyIndex)), assetUrl)) {
      return true;
    }

    searchStart = static_cast<size_t>(nameKeyIndex) + 1;
  }

  return false;
}

String readBodyLimited(HTTPClient &http, size_t maxBytes) {
  WiFiClient *stream = http.getStreamPtr();
  if (stream == nullptr) {
    return "";
  }

  const int reportedSize = http.getSize();
  String body;
  const size_t reserveBytes =
      reportedSize > 0 ? std::min(static_cast<size_t>(reportedSize), maxBytes) : 1024;
  body.reserve(reserveBytes);

  uint8_t buffer[512];
  size_t totalRead = 0;
  while (http.connected() || stream->available()) {
    if (reportedSize > 0 && totalRead >= static_cast<size_t>(reportedSize)) {
      break;
    }

    const int available = stream->available();
    if (available <= 0) {
      delay(1);
      continue;
    }

    const size_t remaining = maxBytes - totalRead;
    if (remaining == 0) {
      break;
    }

    const size_t chunkSize =
        std::min(remaining, std::min(sizeof(buffer), static_cast<size_t>(available)));
    const int bytesRead = stream->readBytes(buffer, chunkSize);
    if (bytesRead <= 0) {
      break;
    }

    totalRead += static_cast<size_t>(bytesRead);
    for (int i = 0; i < bytesRead; ++i) {
      body += static_cast<char>(buffer[i]);
    }
  }

  return body;
}

String userAgentForVersion(const String &version) {
  return String("Flower/") + (version.isEmpty() ? "dev" : version);
}

String versionDetail(const String &currentVersion, const String &latestVersion) {
  if (latestVersion.isEmpty()) {
    return currentVersion;
  }
  if (currentVersion.isEmpty()) {
    return latestVersion;
  }
  return currentVersion + " -> " + latestVersion;
}

}  // namespace

bool OtaUpdater::loadConfig(Config &config) const {
  config = Config();
  for (const char *path : kConfigPaths) {
    if (loadConfigFromPath(path, config)) {
      return true;
    }
  }

  return false;
}

bool OtaUpdater::isConfigured(const Config &config) const {
  return !trimCopy(config.wifiSsid).isEmpty();
}

String OtaUpdater::currentVersion() const { return RSVP_FIRMWARE_VERSION; }

bool OtaUpdater::loadConfigFromPath(const char *path, Config &config) const {
  File file = SD_MMC.open(path);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    return false;
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.isEmpty() || line.startsWith("#")) {
      continue;
    }

    const int equalsIndex = line.indexOf('=');
    if (equalsIndex <= 0) {
      continue;
    }

    String key = line.substring(0, equalsIndex);
    String value = line.substring(equalsIndex + 1);
    key.trim();
    value.trim();
    key.toLowerCase();

    if (key == "wifi_ssid") {
      config.wifiSsid = value;
    } else if (key == "wifi_password") {
      config.wifiPassword = value;
    } else if (key == "github_owner") {
      config.githubOwner = value;
    } else if (key == "github_repo") {
      config.githubRepo = value;
    } else if (key == "asset_name") {
      config.assetName = value;
    } else if (key == "auto_check") {
      config.autoCheck = parseBoolValue(value);
    }
  }

  file.close();
  return true;
}

bool OtaUpdater::connectWiFi(const Config &config, StatusCallback callback,
                             void *context) const {
  // Blokuje, aż zwolni się poprzednia sesja (font_dl/book_dl/ekran Wi-Fi) —
  // patrz komentarz przy wifiSessionMutex() na początku pliku.
  xSemaphoreTake(wifiSessionMutex(), portMAX_DELAY);

  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.wifiSsid.c_str(), config.wifiPassword.c_str());

  const uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < kWifiConnectTimeoutMs) {
    const uint32_t elapsedMs = millis() - startMs;
    const int progress = 5 + static_cast<int>((elapsedMs * 15) / kWifiConnectTimeoutMs);
    reportStatus(callback, context, kStatusTitle, "Connecting Wi-Fi", config.wifiSsid, progress);
    delay(kWifiConnectPollMs);
  }

  const bool connected = WiFi.status() == WL_CONNECTED;
  if (!connected) {
    // Sesja się nie zaczęła, ale WiFi.begin() już uruchomił sterownik —
    // trzeba go zgasić tutaj, POD mutexem, zanim go zwolnimy. Wcześniej ten
    // teardown był zostawiony wywołującemu (checkOnly/checkAndInstall/
    // installAsset zawsze wołały potem disconnectWiFi() też przy błędzie),
    // co dawało podwójne zwolnienie mutexa: to zwolnienie tutaj, a chwilę
    // później drugie w disconnectWiFi(). W tej szczelinie między nimi inne
    // zadanie czekające na mutex (np. font_dl na drugim rdzeniu) mogło
    // złapać go i odpalić własne WiFi.begin() dokładnie w momencie, gdy to
    // zadanie robiło WiFi.mode(WIFI_OFF) — ten sam crash LoadProhibited co
    // przy zwykłym rozłączeniu (patrz disconnectWiFi()), tylko wyzwolony
    // nieudanym połączeniem zamiast udanego. Niektórzy wywołujący
    // (font_dl/book_dl) nadal nie wołają disconnectWiFi() po błędzie — teraz
    // nie muszą, bo pełny teardown+release dzieje się już tutaj.
    WiFi.disconnect(false, false);
    delay(100);
    WiFi.mode(WIFI_OFF);
    xSemaphoreGive(wifiSessionMutex());
  }
  return connected;
}

void OtaUpdater::disconnectWiFi() const {
  // wifioff=false: zostawia sterownik WiFi żywym na czas zdarzenia
  // WIFI_EVENT_STA_DISCONNECTED (wątek sys_evt wysyła DHCP-release przez
  // sieć). Natychmiastowe WiFi.mode(WIFI_OFF) po disconnect(true, ...)
  // ubijało sterownik w trakcie tej transmisji (LoadProhibited w
  // ieee80211_output_do, boot #53).
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  xSemaphoreGive(wifiSessionMutex());
}

bool OtaUpdater::fetchLatestRelease(const Config &config, LatestRelease &release,
                                    String &errorDetail, StatusCallback callback,
                                    void *context) const {
  const String version = currentVersion();
  const String url = "https://api.github.com/repos/" + config.githubOwner + "/" +
                     config.githubRepo + "/releases/latest";

  reportStatus(callback, context, kStatusTitle, "Checking GitHub", config.githubRepo, 22);

  WiFiClientSecure client;
  // GitHub release metadata and assets can redirect across multiple hosts, so keep the transport
  // flexible for now. A signed manifest is the best follow-up hardening step.
  client.setInsecure();
  client.setHandshakeTimeout(15);

  HTTPClient http;
  http.setUserAgent(userAgentForVersion(version));
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  if (!http.begin(client, url)) {
    errorDetail = "HTTP begin failed";
    return false;
  }

  http.addHeader("Accept", "application/vnd.github+json");
  const int statusCode = http.GET();
  if (statusCode != HTTP_CODE_OK) {
    // Unauthenticated GitHub API requests return 404 for BOTH "no release
    // yet" and "repo is private" (it never confirms a private repo exists) —
    // and 403 almost always means the 60 req/hour unauthenticated rate limit
    // got hit, not a real access problem. Distinguish these from a generic
    // transport failure so the status screen doesn't just say "GitHub HTTP
    // 404" for a case that code alone can't recover from (needs the repo
    // public, or a token this build doesn't send).
    if (statusCode == HTTP_CODE_NOT_FOUND) {
      errorDetail = "No release (or repo is private)";
    } else if (statusCode == HTTP_CODE_FORBIDDEN) {
      errorDetail = "GitHub rate limit (403)";
    } else {
      errorDetail = "GitHub HTTP " + String(statusCode);
    }
    http.end();
    return false;
  }

  const String body = readBodyLimited(http, kMaxReleaseJsonBytes);
  http.end();

  if (!extractJsonStringValue(body, "tag_name", 0, release.tagName) || release.tagName.isEmpty()) {
    errorDetail = "Release tag missing";
    return false;
  }

  if (!extractAssetDownloadUrl(body, config.assetName, release.assetUrl) ||
      release.assetUrl.isEmpty()) {
    errorDetail = config.assetName + " missing";
    return false;
  }

  return true;
}

bool OtaUpdater::resolveDownloadUrl(const String &assetUrl, const String &version,
                                    String &resolvedUrl, String &errorDetail,
                                    StatusCallback callback, void *context) const {
  reportStatus(callback, context, kStatusTitle, "Resolving asset", version, 29);

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(15);

  HTTPClient http;
  http.collectHeaders(kRedirectHeaderKeys, 1);
  http.setUserAgent(userAgentForVersion(version));
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  if (!http.begin(client, assetUrl)) {
    errorDetail = "Asset URL failed";
    return false;
  }

  http.addHeader("Accept", "application/octet-stream");
  const int statusCode = http.GET();
  if (statusCode == HTTP_CODE_OK) {
    resolvedUrl = assetUrl;
    http.end();
    return true;
  }

  if (statusCode == HTTP_CODE_MOVED_PERMANENTLY || statusCode == HTTP_CODE_FOUND ||
      statusCode == HTTP_CODE_SEE_OTHER || statusCode == HTTP_CODE_TEMPORARY_REDIRECT ||
      statusCode == HTTP_CODE_PERMANENT_REDIRECT) {
    resolvedUrl = http.header("Location");
    http.end();
    if (!resolvedUrl.isEmpty()) {
      return true;
    }
    errorDetail = "Asset redirect missing";
    return false;
  }

  errorDetail = "Asset HTTP " + String(statusCode);
  http.end();
  return false;
}

void OtaUpdater::reportStatus(StatusCallback callback, void *context, const char *title,
                              const String &line1, const String &line2,
                              int progressPercent) const {
  if (callback == nullptr) {
    return;
  }

  callback(context, title, line1.c_str(), line2.c_str(), progressPercent);
}

OtaUpdater::Result OtaUpdater::checkOnly(const Config &config, StatusCallback callback,
                                         void *context) const {
  Result result;
  result.currentVersion = currentVersion();

  if (!isConfigured(config)) {
    result.code = ResultCode::NotConfigured;
    result.summary = "Wi-Fi not set";
    result.detail = "Settings -> Wi-Fi";
    return result;
  }

  if (!connectWiFi(config, callback, context)) {
    // connectWiFi() już zgasiła sterownik i zwolniła mutex sama, w środku,
    // pod osłoną tego samego mutexa — patrz komentarz w connectWiFi().
    // Drugie wołanie disconnectWiFi() tutaj było zbędne i otwierało okno na
    // wyścig z inną sesją WiFi.
    result.code = ResultCode::ConnectFailed;
    result.summary = "Wi-Fi failed";
    result.detail = "Check credentials";
    return result;
  }

  LatestRelease release;
  String metadataError;
  if (!fetchLatestRelease(config, release, metadataError, callback, context)) {
    disconnectWiFi();
    result.code = ResultCode::MetadataFailed;
    result.summary = "GitHub failed";
    result.detail = metadataError;
    return result;
  }

  disconnectWiFi();
  result.latestVersion = release.tagName;
  if (release.tagName == result.currentVersion) {
    result.code = ResultCode::NoUpdate;
    result.summary = "Already current";
    result.detail = release.tagName;
    return result;
  }

  if (release.assetUrl.isEmpty()) {
    result.code = ResultCode::AssetMissing;
    result.summary = "Asset missing";
    result.detail = config.assetName;
    return result;
  }

  result.code = ResultCode::UpdateAvailable;
  result.summary = "Update available";
  result.detail = release.tagName;
  return result;
}

OtaUpdater::Result OtaUpdater::checkAndInstall(const Config &config, StatusCallback callback,
                                               void *context) const {
  Result result;
  result.currentVersion = currentVersion();

  if (!isConfigured(config)) {
    result.code = ResultCode::NotConfigured;
    result.summary = "Wi-Fi not set";
    result.detail = "Settings -> Wi-Fi";
    return result;
  }

  if (!connectWiFi(config, callback, context)) {
    // Patrz komentarz w checkOnly() — connectWiFi() już posprzątała po sobie.
    result.code = ResultCode::ConnectFailed;
    result.summary = "Wi-Fi failed";
    result.detail = "Check credentials";
    return result;
  }

  LatestRelease release;
  String metadataError;
  if (!fetchLatestRelease(config, release, metadataError, callback, context)) {
    disconnectWiFi();
    result.code = ResultCode::MetadataFailed;
    result.summary = "GitHub failed";
    result.detail = metadataError;
    return result;
  }

  result.latestVersion = release.tagName;
  if (release.tagName == result.currentVersion) {
    disconnectWiFi();
    result.code = ResultCode::NoUpdate;
    result.summary = "Already current";
    result.detail = release.tagName;
    return result;
  }

  if (release.assetUrl.isEmpty()) {
    disconnectWiFi();
    result.code = ResultCode::AssetMissing;
    result.summary = "Asset missing";
    result.detail = config.assetName;
    return result;
  }

  reportStatus(callback, context, kStatusTitle, "Preparing update",
               versionDetail(result.currentVersion, result.latestVersion), 28);

  String resolvedAssetUrl;
  String resolveError;
  if (!resolveDownloadUrl(release.assetUrl, result.latestVersion, resolvedAssetUrl, resolveError,
                          callback, context)) {
    disconnectWiFi();
    result.code = ResultCode::InstallFailed;
    result.summary = "Asset failed";
    result.detail = resolveError;
    return result;
  }

  WiFiClientSecure client;
  // Match the metadata request behavior until the update path gains certificate pinning or
  // signature verification above the transport layer.
  client.setInsecure();
  client.setHandshakeTimeout(15);

  HTTPUpdate updater;
  updater.rebootOnUpdate(false);
  updater.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int lastReportedProgress = -1;
  updater.onProgress([this, callback, context, &result, &lastReportedProgress](int current,
                                                                                int total) {
    if (total <= 0) {
      reportStatus(callback, context, kStatusTitle, "Downloading update", result.latestVersion,
                   -1);
      return;
    }

    const int progress = 30 + static_cast<int>((static_cast<int64_t>(current) * 65) / total);
    if (progress == lastReportedProgress) {
      return;
    }

    lastReportedProgress = progress;
    reportStatus(callback, context, kStatusTitle, "Downloading update", result.latestVersion,
                 progress);
  });

  const String version = result.currentVersion;
  const t_httpUpdate_return updateResult =
      updater.update(client, resolvedAssetUrl, version, [version](HTTPClient *http) {
        http->setUserAgent(userAgentForVersion(version));
        http->addHeader("Accept", "application/octet-stream");
      });

  disconnectWiFi();

  switch (updateResult) {
    case HTTP_UPDATE_OK:
      result.code = ResultCode::Success;
      result.summary = "Update ready";
      result.detail = result.latestVersion;
      result.rebootRequired = true;
      return result;
    case HTTP_UPDATE_NO_UPDATES:
      result.code = ResultCode::NoUpdate;
      result.summary = "Already current";
      result.detail = result.latestVersion;
      return result;
    case HTTP_UPDATE_FAILED:
    default:
      result.code = ResultCode::InstallFailed;
      result.summary = "Update failed";
      result.detail = updater.getLastErrorString();
      return result;
  }
}

// ─── Plugin / variant install ────────────────────────────────────────────────

OtaUpdater::Result OtaUpdater::installAsset(const Config &config, const String &assetName,
                                             const String &tagName, StatusCallback callback,
                                             void *context) const {
  Result result;
  result.currentVersion = currentVersion();

  if (!isConfigured(config)) {
    result.code = ResultCode::NotConfigured;
    result.summary = "Wi-Fi not set";
    result.detail = "Settings -> Wi-Fi";
    return result;
  }

  if (!connectWiFi(config, callback, context)) {
    // Patrz komentarz w checkOnly() — connectWiFi() już posprzątała po sobie.
    result.code = ResultCode::ConnectFailed;
    result.summary = "Wi-Fi failed";
    result.detail = "Check credentials";
    return result;
  }

  // Build release API URL — either specific tag or latest
  const String releaseUrl = tagName.isEmpty()
      ? "https://api.github.com/repos/" + config.githubOwner + "/" + config.githubRepo +
            "/releases/latest"
      : "https://api.github.com/repos/" + config.githubOwner + "/" + config.githubRepo +
            "/releases/tags/" + tagName;

  reportStatus(callback, context, "Plugin", "Checking release", config.githubRepo, 22);

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(15);

  HTTPClient http;
  http.setUserAgent(userAgentForVersion(currentVersion()));
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  if (!http.begin(client, releaseUrl)) {
    disconnectWiFi();
    result.code = ResultCode::MetadataFailed;
    result.summary = "HTTP begin failed";
    result.detail = releaseUrl;
    return result;
  }

  http.addHeader("Accept", "application/vnd.github+json");
  const int statusCode = http.GET();
  if (statusCode != HTTP_CODE_OK) {
    http.end();
    disconnectWiFi();
    result.code = ResultCode::MetadataFailed;
    result.summary = "GitHub HTTP " + String(statusCode);
    result.detail = assetName;
    return result;
  }

  const String body = readBodyLimited(http, kMaxReleaseJsonBytes);
  http.end();

  // Extract tag name from response
  String resolvedTag;
  if (!extractJsonStringValue(body, "tag_name", 0, resolvedTag) || resolvedTag.isEmpty()) {
    disconnectWiFi();
    result.code = ResultCode::MetadataFailed;
    result.summary = "Release tag missing";
    result.detail = assetName;
    return result;
  }
  result.latestVersion = resolvedTag;

  // Find the asset URL
  String assetUrl;
  if (!extractAssetDownloadUrl(body, assetName, assetUrl) || assetUrl.isEmpty()) {
    disconnectWiFi();
    result.code = ResultCode::AssetMissing;
    result.summary = "Asset missing";
    result.detail = assetName;
    return result;
  }

  reportStatus(callback, context, "Plugin", "Resolving asset", assetName, 28);

  String resolvedUrl;
  String resolveError;
  if (!resolveDownloadUrl(assetUrl, resolvedTag, resolvedUrl, resolveError, callback, context)) {
    disconnectWiFi();
    result.code = ResultCode::InstallFailed;
    result.summary = "Asset failed";
    result.detail = resolveError;
    return result;
  }

  WiFiClientSecure flashClient;
  flashClient.setInsecure();
  flashClient.setHandshakeTimeout(15);

  HTTPUpdate updater;
  updater.rebootOnUpdate(false);
  updater.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int lastReportedProgress = -1;
  const String &assetNameRef = assetName;
  updater.onProgress([this, callback, context, &assetNameRef,
                      &lastReportedProgress](int current, int total) {
    if (total <= 0) {
      reportStatus(callback, context, "Plugin", "Downloading", assetNameRef, -1);
      return;
    }
    const int progress = 30 + static_cast<int>((static_cast<int64_t>(current) * 65) / total);
    if (progress == lastReportedProgress) {
      return;
    }
    lastReportedProgress = progress;
    reportStatus(callback, context, "Plugin", "Downloading", assetNameRef, progress);
  });

  const String version = result.currentVersion;
  const t_httpUpdate_return updateResult =
      updater.update(flashClient, resolvedUrl, version, [version](HTTPClient *http) {
        http->setUserAgent(userAgentForVersion(version));
        http->addHeader("Accept", "application/octet-stream");
      });

  disconnectWiFi();

  switch (updateResult) {
    case HTTP_UPDATE_OK:
      result.code = ResultCode::Success;
      result.summary = "Plugin ready";
      result.detail = resolvedTag;
      result.rebootRequired = true;
      return result;
    case HTTP_UPDATE_NO_UPDATES:
      result.code = ResultCode::NoUpdate;
      result.summary = "No change";
      result.detail = resolvedTag;
      return result;
    case HTTP_UPDATE_FAILED:
    default:
      result.code = ResultCode::InstallFailed;
      result.summary = "Install failed";
      result.detail = updater.getLastErrorString();
      return result;
  }
}

// ─── Asset-to-SD download (font pack) ────────────────────────────────────────

bool OtaUpdater::downloadAsset(const Config &config, const String &assetName,
                               const String &tagName, const String &destPath,
                               String &errorDetail, StatusCallback callback,
                               void *context) const {
  const String releaseUrl = tagName.isEmpty()
      ? "https://api.github.com/repos/" + config.githubOwner + "/" + config.githubRepo +
            "/releases/latest"
      : "https://api.github.com/repos/" + config.githubOwner + "/" + config.githubRepo +
            "/releases/tags/" + tagName;

  WiFiClientSecure metaClient;
  metaClient.setInsecure();
  metaClient.setHandshakeTimeout(15);

  HTTPClient metaHttp;
  metaHttp.setUserAgent(userAgentForVersion(currentVersion()));
  metaHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  metaHttp.setTimeout(15000);
  if (!metaHttp.begin(metaClient, releaseUrl)) {
    errorDetail = "HTTP begin failed";
    return false;
  }

  metaHttp.addHeader("Accept", "application/vnd.github+json");
  const int metaStatus = metaHttp.GET();
  if (metaStatus != HTTP_CODE_OK) {
    errorDetail = "GitHub HTTP " + String(metaStatus);
    metaHttp.end();
    return false;
  }

  const String body = readBodyLimited(metaHttp, kMaxReleaseJsonBytes);
  metaHttp.end();

  String assetUrl;
  if (!extractAssetDownloadUrl(body, assetName, assetUrl) || assetUrl.isEmpty()) {
    errorDetail = assetName + " missing";
    return false;
  }

  String resolvedUrl;
  if (!resolveDownloadUrl(assetUrl, assetName, resolvedUrl, errorDetail, callback, context)) {
    return false;
  }

  WiFiClientSecure dlClient;
  dlClient.setInsecure();
  dlClient.setHandshakeTimeout(15);

  HTTPClient dlHttp;
  dlHttp.setUserAgent(userAgentForVersion(currentVersion()));
  dlHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  dlHttp.setTimeout(15000);
  if (!dlHttp.begin(dlClient, resolvedUrl)) {
    errorDetail = "Download begin failed";
    return false;
  }

  dlHttp.addHeader("Accept", "application/octet-stream");
  const int dlStatus = dlHttp.GET();
  if (dlStatus != HTTP_CODE_OK) {
    errorDetail = "Download HTTP " + String(dlStatus);
    dlHttp.end();
    return false;
  }

  const int lastSlash = destPath.lastIndexOf('/');
  if (lastSlash > 0) {
    SD_MMC.mkdir(destPath.substring(0, lastSlash));
  }

  const String tmpPath = destPath + ".part";
  SD_MMC.remove(tmpPath);
  File out = SD_MMC.open(tmpPath, FILE_WRITE);
  if (!out) {
    errorDetail = "SD open failed";
    dlHttp.end();
    return false;
  }

  WiFiClient *stream = dlHttp.getStreamPtr();
  const int reportedSize = dlHttp.getSize();
  uint8_t buffer[1024];
  size_t totalWritten = 0;
  uint32_t lastDataMs = millis();
  bool stalled = false;
  while (dlHttp.connected() || stream->available()) {
    if (reportedSize > 0 && totalWritten >= static_cast<size_t>(reportedSize)) {
      break;
    }

    const int available = stream->available();
    if (available <= 0) {
      if (millis() - lastDataMs > 15000) {
        stalled = true;
        break;
      }
      delay(1);
      continue;
    }

    const size_t chunkSize = std::min(sizeof(buffer), static_cast<size_t>(available));
    const int bytesRead = stream->readBytes(buffer, chunkSize);
    if (bytesRead <= 0) {
      break;
    }

    out.write(buffer, static_cast<size_t>(bytesRead));
    totalWritten += static_cast<size_t>(bytesRead);
    lastDataMs = millis();

    if (reportedSize > 0 && callback != nullptr) {
      const int progress =
          static_cast<int>((static_cast<int64_t>(totalWritten) * 100) / reportedSize);
      reportStatus(callback, context, "Fonts", "Downloading", assetName, progress);
    }
  }
  out.close();
  dlHttp.end();

  const bool sizeMatches = reportedSize <= 0 || totalWritten == static_cast<size_t>(reportedSize);
  if (stalled || !sizeMatches || totalWritten == 0) {
    SD_MMC.remove(tmpPath);
    errorDetail = stalled ? "Download stalled" : "Incomplete download";
    return false;
  }

  SD_MMC.remove(destPath);
  if (!SD_MMC.rename(tmpPath, destPath)) {
    errorDetail = "SD rename failed";
    return false;
  }

  return true;
}
