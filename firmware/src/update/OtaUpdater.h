#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Guards every WiFi.mode()/WiFi.begin()/WiFi.disconnect() call against
// concurrent use from a different FreeRTOS task — see the comment on the
// definition in OtaUpdater.cpp for the LoadProhibited crash this prevents.
// Exposed (not file-local) so other code that touches the WiFi driver from
// its own task — CompanionSyncManager's AP mode for phone pairing — can join
// the same session instead of racing OtaUpdater's font/book background
// downloads.
SemaphoreHandle_t wifiSessionMutex();

// The task running the UI (App::begin calls this once). It never waits for
// the lock without limit: a font pack download held it for minutes and the
// screen froze on the phone network or on Update.
void markWifiForegroundTask();
// Takes the lock. From the UI task it asks the background downloads to give
// the radio up (they stop at the next chunk) and waits at most timeoutMs;
// background tasks wait as long as it takes.
bool takeWifiSession(uint32_t foregroundTimeoutMs = 30000);
// True while the UI task waits for the radio, or while background downloads
// are paused: background sessions stop.
bool wifiYieldRequested();
// A plugin (the dictaphone) needs the card and core 0 to itself: font, book
// and update downloads stop at the next chunk and none starts until unpaused.
void setBackgroundDownloadsPaused(bool paused);
bool backgroundDownloadsPaused();

// Set by tools/pio_set_version.py from RSVP_OTA_REPO (release CI).
#ifndef RSVP_OTA_DEFAULT_REPO
#define RSVP_OTA_DEFAULT_REPO "czytnik01"
#endif

class OtaUpdater {
 public:
  using StatusCallback = void (*)(void *context, const char *title, const char *line1,
                                  const char *line2, int progressPercent);

  struct Config {
    String wifiSsid;
    String wifiPassword;
    String githubOwner = "GRKarol";
    String githubRepo = RSVP_OTA_DEFAULT_REPO;
    String assetName = "flower-firmware.bin";
    bool autoCheck = false;
  };

  enum class ResultCode : uint8_t {
    Success,
    NoUpdate,
    UpdateAvailable,
    NotConfigured,
    ConnectFailed,
    MetadataFailed,
    AssetMissing,
    InstallFailed,
  };

  struct Result {
    ResultCode code = ResultCode::MetadataFailed;
    String currentVersion;
    String latestVersion;
    String summary;
    String detail;
    bool rebootRequired = false;
  };

  // True when `latest` is a higher release than `current` ("v0.4.01" >
  // "v0.4.00"; a "-N-gHASH" suffix is ignored). Tags that don't parse fall
  // back to "any difference counts", the old behaviour.
  static bool isNewerVersion(const String &latest, const String &current);

  bool loadConfig(Config &config) const;
  bool isConfigured(const Config &config) const;
  String currentVersion() const;
  Result checkOnly(const Config &config, StatusCallback callback = nullptr,
                   void *context = nullptr) const;
  Result checkAndInstall(const Config &config, StatusCallback callback = nullptr,
                         void *context = nullptr) const;

  /**
   * Pobiera i flashuje konkretny plik z GitHub Releases dla podanego tagu.
   * Nie sprawdza wersji — używane przez system pluginów.
   * assetName: np. "flower-firmware-timer.bin"
   * tagName: np. "v0.3.0" (pusty = "latest" release)
   */
  Result installAsset(const Config &config, const String &assetName, const String &tagName,
                      StatusCallback callback = nullptr, void *context = nullptr) const;

  // Connects to Wi-Fi using config.wifiSsid/wifiPassword. Public so callers
  // that need to download several assets in one Wi-Fi session (e.g. the font
  // pack) can connect once, call downloadAsset() repeatedly, then
  // disconnectWiFi() — instead of paying the ~15s connect cost per file.
  bool connectWiFi(const Config &config, StatusCallback callback = nullptr,
                   void *context = nullptr) const;
  void disconnectWiFi() const;

  // Why connectWiFiUserAttempt() failed.
  enum class WifiFailure : uint8_t { None, WrongPassword, NotFound, NoAnswer, Busy };
  // The Wi-Fi screen's "connect to this network": no fallback to other
  // remembered networks, and it gives up as soon as the router has said no
  // twice (wrong password, network gone) instead of waiting out the whole
  // timeout. On success the session is open, as with connectWiFi().
  bool connectWiFiUserAttempt(const Config &config, WifiFailure &failure, StatusCallback callback = nullptr,
                              void *context = nullptr) const;

  /**
   * Pobiera pojedynczy nazwany asset z release'u (jak installAsset) i zapisuje
   * go na SD pod destPath, zamiast flashować przez HTTPUpdate. Nie łączy ani
   * nie rozłącza Wi-Fi — zakłada że wywołujący już wywołał connectWiFi().
   * Zapis jest atomowy (plik tymczasowy + rename), żeby przerwane pobieranie
   * nie zostawiło uszkodzonego .fnt na karcie.
   * tagName: np. "v0.3.38" (pusty = "latest" release).
   */
  bool downloadAsset(const Config &config, const String &assetName, const String &tagName,
                     const String &destPath, String &errorDetail,
                     StatusCallback callback = nullptr, void *context = nullptr) const;

 private:
  struct LatestRelease {
    String tagName;
    String assetUrl;
  };

  bool loadConfigFromPath(const char *path, Config &config) const;
  bool fetchLatestRelease(const Config &config, LatestRelease &release, String &errorDetail,
                          StatusCallback callback, void *context) const;
  bool resolveDownloadUrl(const String &assetUrl, const String &version, String &resolvedUrl,
                          String &errorDetail, StatusCallback callback, void *context) const;
  void reportStatus(StatusCallback callback, void *context, const char *title,
                    const String &line1, const String &line2, int progressPercent) const;
};
