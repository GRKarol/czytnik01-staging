#include "app/App.h"

#include <SD_MMC.h>
#include <esp_sleep.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <qrcode.h>
#include <WiFi.h>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <iterator>
#include <utility>
#include <vector>

#include "app/Translations.h"
#include "board/BoardConfig.h"
#include "plugins/DeviceServicesBridge.h"

#ifndef RSVP_USB_TRANSFER_ENABLED
#define RSVP_USB_TRANSFER_ENABLED 0
#endif

#ifndef RSVP_USB_TRANSFER_AUTO_START
#define RSVP_USB_TRANSFER_AUTO_START 0
#endif

static const char *kAppTag = "app";
// 10240 B nie starczało: coredump z boot #36 pokazał this=0xcc8251fe (garbage)
// przy konstrukcji HTTPClient wewnątrz OtaUpdater::downloadAsset — realny stack
// overflow zadania, nie wyścig Wi-Fi. mbedTLS handshake + HTTPClient/WiFiClientSecure
// + bufory 1024 B w tej samej ścieżce potrzebują więcej marginesu.
constexpr uint32_t kOtaCheckTaskStackBytes = 20480;
constexpr uint32_t kFontDownloadTaskStackBytes = 20480;
// connectWiFi() and fetchLatestRelease() each cap at 15s, so a healthy check
// finishes well under 30s. If the ota_check task dies mid-flight (crash,
// panic) it never reaches xQueueOverwrite(), so otaCheckInProgress_ would
// otherwise stay stuck true for the rest of the boot — the manual
// "Aktualizacja firmware" button then permanently shows "OTA check running /
// Try again soon" (see blockNetworkActionForOtaCheck) with no way to retry
// short of a restart. Treat a check that's run long past its own internal
// timeouts as dead and release the lock.
constexpr uint32_t kOtaCheckWatchdogTimeoutMs = 45000;
// How often maybeAutoDownloadFonts() re-checks for saved Wi-Fi once the pack
// isn't complete yet — deliberately not "once at boot only", since the user
// may pair the Flower app and save Wi-Fi credentials well after first boot,
// and the whole point is that no explicit action should be required.
constexpr uint32_t kFontDownloadRetryIntervalMs = 60000;
// Starter library (krok 2.4/5-6 kreatora, "Co dziś czytamy?"): ile tytułów na
// jeden język kreator próbuje ściągnąć z GitHuba. Brakujący slot (Karol
// jeszcze nie wgrał tego tytułu dla danego języka) to nie błąd, po prostu
// mniej pozycji w bibliotece na starcie — patrz bookDownloadTask().
constexpr uint8_t kStarterBookCountPerLanguage = 5;
// Minimum time the splash artwork stays up once the main loop starts (on
// top of the black beat below and the two 600 ms fades) — was 5000 ms, an
// artificial floor that had nothing to do with hardware readiness (all real
// init already runs synchronously in setup(), before this is even checked);
// it just made boot feel slow. Trimmed to keep the whole boot sequence
// (black beat + hold + fade-out + fade-in) close to ~2 s.
constexpr uint32_t kBootSplashMs = 800;
constexpr uint32_t kBootSplashBlackMs = 200;
constexpr uint32_t kBootSplashFadeMs = 600;
// Extra budget (from bootStartedMs_, not on top of the splash) to let a
// pending SD-backed typeface load finish before handing off to the reader.
// Almost never fully used — the splash animation itself already burns ~1.8s
// synchronously, and a normal SD mount + font read finishes well under
// that. Only a genuinely slow card eats into this, and even then it just
// falls back to Atkinson and keeps retrying in the background afterward
// (see maybeRetryTypographyFontLoad) instead of freezing indefinitely.
constexpr uint32_t kBootFontWaitBudgetMs = 4000;
constexpr uint32_t kWpmFeedbackMs = 900;
constexpr uint32_t kPowerOffHoldMs = 1600;
constexpr uint32_t kPowerOffReleaseWaitMs = 4000;
// PWR button: single tap toggles the menu, but that only fires once this
// window passes with no second tap — two taps inside it restart the device
// instead.
constexpr uint32_t kPowerDoubleTapWindowMs = 350;
constexpr uint32_t kBatterySampleIntervalMs = 180000;
constexpr uint32_t kTouchPlayHoldMs = 420;
// AXS15231B occasionally reports an empty sample or two mid-hold even while
// a finger sits perfectly still (skin contact/pressure noise) — the touch
// driver's 2-sample debounce (TouchHandler::kReleaseConfirmSamples) already
// filters single-frame dropouts, but a run of several still slips through
// and reads as a real release. Without this grace window that false release
// instantly paused hold-to-read and made the reader wait out
// kTouchPlayHoldMs all over again once the sensor recovered a moment later —
// visible as reading repeatedly stopping/resuming while the finger never
// left the screen. Genuine releases just pay this as extra latency before
// the pause actually lands.
constexpr uint32_t kTouchPlayReleaseGraceMs = 150;
constexpr uint32_t kPreviewBrowseHoldMs = 240;
constexpr uint32_t kReaderDoubleTapWindowMs = 520;
constexpr uint32_t kThemeToggleHoldMs = 900;
constexpr uint32_t kScrollAnimationFrameMs = 16;
constexpr uint16_t kSwipeThresholdPx = 40;
constexpr uint16_t kAxisBiasPx = 12;
constexpr uint16_t kTapSlopPx = 26;
// Two-step tap confirm (arm-then-confirm) window for the immediate-mode
// button grid — see App::handleGridTap()/isGridItemArmed().
constexpr uint32_t kArmedConfirmWindowMs = 2500;
// How long a tapped grid button shows solid focusColor before its action
// actually runs — short enough to feel instant, long enough to register as
// "yes, that tap landed" before the screen changes underneath it.
constexpr uint32_t kPressFlashMs = 140;
// Minimum gap between two fires of the SAME grid button on the SAME
// screen — see lastFiredGridItemIndex_ in App.h for why this exists
// (capacitive-touch contact bounce reads as two quick taps from one
// physical touch).
constexpr uint32_t kGridTapDebounceMs = 200;
// Shorter than kGridTapDebounceMs on purpose: real typing legitimately hits
// the same key twice in a row close together (double letters), so this only
// needs to be long enough to eat capacitive contact-bounce (typically well
// under 100ms) without eating a fast typist's real repeat keystroke.
constexpr uint32_t kTextEntryTapDebounceMs = 90;
// Sentinel canonicalIndex for the wizard-picker Confirm corner button (see
// App::applyConfirmButtonCornerLayout()) — far past any real item count, so
// the `canonicalIndex < itemCount` guards in handleGridTap() never mistake
// it for a real tile and overwrite the current selection.
constexpr size_t kWizardConfirmCanonicalIndex = 100000;
// Visible size of the wizard Confirm corner button — bigger than Back's
// 44x26 (see applyConfirmButtonCornerLayout()) because it's the one button
// that commits a whole wizard step and was reported too small to hit
// reliably with a thumb.
constexpr uint16_t kWizardConfirmButtonWidth = 70;
constexpr uint16_t kWizardConfirmButtonHeight = 40;
// Sentinel canonicalIndex for the WelcomeReadingMode "preview" eye-icon
// corner button (see App::applyReadingModePreviewButtonLayout()) — a
// different sentinel than kWizardConfirmCanonicalIndex so the two never
// collide when both appear together in currentGridItemIndices_.
constexpr size_t kReadingModePreviewCanonicalIndex = 100001;
// General cooldown after any menu action that commits/changes screen (see
// App::selectMenuItem()) — swallows a second commit that lands right after
// the first (fat-finger double tap, or capacitive-touch contact bounce that
// slipped past the per-button guard above because the second bounce landed
// on a *different* button once the screen had already changed underneath
// it, which read as one tap skipping two screens ahead).
constexpr uint32_t kMenuActionDebounceMs = 500;
// Contact-bounce guard for the on-screen virtual D-Pad panel (DPad nav
// mode): one physical tap on the up/down/left/right/OK zones occasionally
// reads back as two quick touch End events, which without this guard moved
// the selection (or committed it) twice for a single press.
constexpr uint32_t kDPadTapDebounceMs = 200;
// Swipe nav mode: a tap-to-confirm gesture must release within this long of
// touch-down, or it's treated as a long hold instead of a tap — see the
// comment above its use in applyMenuTouchGesture().
constexpr uint32_t kSwipeTapMaxHoldMs = 500;
// Button-grid nav mode: after a page-changing horizontal (or vertical, for
// SavePointsList) swipe, ignore taps for this long — a finger still
// settling right after the swipe ends can land on whatever button is now
// under it on the new page and fire it by accident.
constexpr uint32_t kGridPageChangeInputBlackoutMs = 500;
// How long the grid toast (full text of a Toggle/Cycle button's new value,
// see App::showGridToast()) stays on screen before the grid redraws without
// it — same shape as the low-battery overlay's timed restore.
constexpr uint32_t kGridToastVisibleMs = 1100;
constexpr uint16_t kReaderDoubleTapSlopPx = 92;
constexpr uint16_t kPreviousSentenceTapWidthPx = 96;
constexpr uint16_t kPreviousSentenceTapHeightPx = 60;
constexpr uint16_t kFooterMetricTapWidthPx = 220;
constexpr uint16_t kFooterMetricTapHeightPx = 32;
constexpr uint16_t kBatteryBadgeTapWidthPx = 160;
constexpr uint16_t kBatteryBadgeTapHeightPx = 40;
constexpr uint16_t kScrubStepPx = 22;
constexpr uint16_t kBrowseNeutralZonePx = 14;
constexpr int kMaxScrubStepsPerGesture = 96;
constexpr uint32_t kBrowseMinWordsPerSecondPermille = 4000;
constexpr uint32_t kBrowseMaxWordsPerSecondPermille = 72000;
constexpr uint32_t kHelpLongPressMs = 600;
constexpr uint16_t kHelpLongPressMaxDriftPx = 20;
constexpr size_t kContextPreviewWindowWords = 288;
constexpr size_t kContextPreviewAnchorLeadWords = 112;
constexpr size_t kContextPreviewMaxParagraphSnapWords = 48;
constexpr uint32_t kProgressSaveIntervalMs = 10000;
constexpr uint32_t kUsbTransferExitHoldMs = 1200;
constexpr size_t kTimeEstimateBlockWords = 256;
constexpr size_t kTimeEstimateBlocksPerUpdate = 1;
constexpr uint32_t kTimeEstimateProgressLogMs = 5000;
constexpr uint32_t kNominalBatteryRuntimeMinutes = 330;
constexpr uint8_t kBatteryDisplayHysteresisPercent = 2;
constexpr uint8_t kBatteryRuntimeMinDropPercent = 3;
constexpr uint32_t kBatteryRuntimeMinElapsedMs = 10UL * 60UL * 1000UL;
constexpr uint32_t kBatteryPlayingSampleIntervalMs = 10UL * 60UL * 1000UL;
constexpr uint32_t kBatteryLowSampleIntervalMs = 60UL * 1000UL;
constexpr uint32_t kBatteryLowWarningRepeatMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kBatteryWarningVisibleMs = 2500;
constexpr uint32_t kBatteryShutdownNoticeMs = 1500;
constexpr float kBatteryLowWarningVoltage = 3.50f;
constexpr float kBatteryCriticalVoltage = 3.30f;
constexpr uint8_t kBatteryLowWarningPercent = 5;
constexpr uint8_t kBatteryCriticalPercent = 1;
constexpr uint8_t kBatteryCriticalConsecutiveSamples = 2;
constexpr uint32_t kStandbyWakeGraceMs = 900;
constexpr uint32_t kStandbyFrameMs = 500;
constexpr uint16_t kStandbyLifeCellPixels = 2;
constexpr uint16_t kStandbyLifeColumns = BoardConfig::DISPLAY_WIDTH / kStandbyLifeCellPixels;
constexpr uint16_t kStandbyLifeRows = BoardConfig::DISPLAY_HEIGHT / kStandbyLifeCellPixels;
constexpr uint32_t kChapterTransitionMs = 1400;
// Floor raised from the old 40/35 — at that duty the backlight read as
// fully off on the panel, so there was no visible way to tell the reader
// was even powered on at the darkest setting.
constexpr uint8_t kBrightnessLevels[] = {55, 65, 78, 90, 100};
constexpr uint8_t kNightBrightnessLevels[] = {45, 52, 58, 65, 72};
constexpr size_t kBrightnessLevelCount = sizeof(kBrightnessLevels) / sizeof(kBrightnessLevels[0]);

namespace {

// Kolejność tego enuma nie musi już odpowiadać kolejności kafelków na
// ekranie — App::renderMainMenu() buduje mainMenuOrder_ (App.h) w
// faktycznej kolejności push_back, a selectMenuItem() dispatchuje przez tę
// mapę, więc pozycja na ekranie i wartość enuma mogą się różnić bez ryzyka
// trafienia tapem w inną akcję niż narysowana.
enum MenuItem : size_t {
  MenuRead,
  MenuLibrary,
  MenuSavePoints,
  MenuSettings,
  MenuPowerOff,
  MenuPlugins,  // tylko w trybie zaawansowanym; w ustawieniach stoi po Wyłącz
  MenuItemCount,
};

enum SettingsItem : size_t {
  SettingsBack,
  SettingsDisplay,
  SettingsTypography,
  SettingsWordPacing,
  SettingsHandedness,
  SettingsBrightness,
  SettingsTheme,
  SettingsPhantomWords,
  SettingsFontSize,
  SettingsLongWords,
  SettingsComplexWords,
  SettingsPunctuation,
  SettingsReset,
  SettingsItemCount,
};

enum TypographyTuningItem : size_t {
  TypographyTuningBack,
  TypographyTuningFontSize,
  TypographyTuningTypeface,
  TypographyTuningPhantomWords,
  TypographyTuningFocusHighlight,
  TypographyTuningTracking,
  TypographyTuningAnchor,
  TypographyTuningGuideWidth,
  TypographyTuningGuideGap,
  TypographyTuningReset,
  TypographyTuningItemCount,
};

enum RestartConfirmItem : size_t {
  RestartConfirmNo,
  RestartConfirmYes,
  RestartConfirmItemCount,
};

enum TypographyResetConfirmItem : size_t {
  TypographyResetConfirmNo,
  TypographyResetConfirmYes,
  TypographyResetConfirmItemCount,
};

enum SdCardRepairConfirmItem : size_t {
  SdCardRepairConfirmNo,
  SdCardRepairConfirmYes,
  SdCardRepairConfirmItemCount,
};

enum UpdateConfirmItem : size_t {
  UpdateConfirmSkip,
  UpdateConfirmUpdate,
  UpdateConfirmItemCount,
};

constexpr size_t kRestartConfirmHeaderRows = 1;
constexpr size_t kTypographyResetConfirmHeaderRows = 1;
constexpr size_t kSdCardRepairConfirmHeaderRows = 1;
constexpr size_t kUpdateConfirmHeaderRows = 2;
constexpr size_t kSettingsBackIndex = 0;
// SettingsHome — nowy układ, ułożony pod codzienne użycie.
// Indeksy 1–6 są zawsze widoczne; 7–9 tylko gdy `devModeEnabled()`
// (= tryb zaawansowany). Wszystkie pozycje warunkowe leżą NA KOŃCU listy,
// więc włączenie/wyłączenie trybu nigdy nie przesuwa indeksów 0–6.
// Advanced siedzi tuż przed About/Help, zaraz za Connectivity — patrz
// rebuildSettingsMenuItems().
constexpr size_t kSettingsHomeReadingIndex = 1;       // = stare Pacing
constexpr size_t kSettingsHomeDisplayIndex = 2;
constexpr size_t kSettingsHomeTypographyIndex = 3;    // always visible (moved from dev-only)
constexpr size_t kSettingsHomeConnectivityIndex = 4;
constexpr size_t kSettingsHomeAdvancedIndex = 5;      // przełącznik trybu zaawansowanego
constexpr size_t kSettingsHomeAboutIndex = 6;
constexpr size_t kSettingsHomePresetsIndex = 7;       // dev only
constexpr size_t kSettingsHomeWifiIndex = 8;          // dev only
constexpr size_t kSettingsHomeUpdateIndex = 9;        // dev only

// Zachowujemy starą nazwę dla kompatybilności z pozostałymi miejscami,
// które do niej odwołują się dla cofania z głębszych ekranów.
constexpr size_t kSettingsHomePacingIndex = kSettingsHomeReadingIndex;

// SettingsConnectivity items (new layout per menu reorganization).
constexpr size_t kSettingsConnWifiIndex = 1;        // Wi-Fi (opens sub-screen)
#if FLOWER_BLE_ENABLED
constexpr size_t kSettingsConnBluetoothIndex = 2;   // Bluetooth toggle
constexpr size_t kSettingsConnSyncToggleIndex = 3;  // Synchronizacja z telefonem
constexpr size_t kSettingsConnUsbIndex = 4;         // Kopiuj przez USB
#else
constexpr size_t kSettingsConnBluetoothIndex = 99;  // not shown
constexpr size_t kSettingsConnSyncToggleIndex = 2;  // Synchronizacja z telefonem
constexpr size_t kSettingsConnUsbIndex = 3;         // Kopiuj przez USB
#endif

// SettingsAbout items
constexpr size_t kSettingsAboutVersionIndex = 1;   // tap-target dla dev mode
constexpr size_t kSettingsAboutBrandIndex = 2;
constexpr size_t kSettingsAboutSdCardIndex = 3;    // SD card check (moved from main menu)
constexpr size_t kSettingsAboutTutorialIndex = 4;  // restart tutorial
constexpr size_t kSettingsAboutDevModeIndex = 5;   // pokazywane gdy dev mode

constexpr const char *kPrefSetupDone = "setup_done";
constexpr const char *kPrefTutorialDone = "tut_done";
constexpr size_t kSettingsDisplayThemeIndex = 1;
constexpr size_t kSettingsDisplayBrightnessIndex = 2;
constexpr size_t kSettingsDisplayHandednessIndex = 3;
// Moved up front (was slot 12, buried on page 2 of the Ekran grid) — save
// point visibility is a control people reach for often, not a one-time
// setup choice, so it belongs on the first page next to Theme/Brightness.
constexpr size_t kSettingsDisplaySavePointBtnIndex = 4;
constexpr size_t kSettingsDisplayFooterIndex = 5;
constexpr size_t kSettingsDisplayBatteryIndex = 6;
constexpr size_t kSettingsDisplayScreensaverIndex = 7;
constexpr size_t kSettingsDisplayReaderBatteryIndex = 8;
constexpr size_t kSettingsDisplayReaderChapterIndex = 9;
constexpr size_t kSettingsDisplayReaderProgressIndex = 10;
constexpr size_t kSettingsDisplayLanguageIndex = 11;
constexpr size_t kSettingsDisplayFocusColorIndex = 12;
constexpr size_t kSettingsDisplayHelpHintsIndex = 13;
constexpr size_t kSettingsDisplayNavModeIndex = 14;
// Appended at the end, not next to kSettingsDisplaySavePointBtnIndex, to
// avoid reshuffling every other index in this list.
constexpr size_t kSettingsDisplaySavePointNameModeIndex = 15;
constexpr size_t kSettingsPacingReadingModeIndex = 1;
constexpr size_t kSettingsPacingPauseModeIndex = 2;
constexpr size_t kSettingsPacingWpmIndex = 3;
constexpr size_t kSettingsPacingLongWordsIndex = 4;
constexpr size_t kSettingsPacingComplexityIndex = 5;
constexpr size_t kSettingsPacingPunctuationIndex = 6;
constexpr size_t kSettingsPacingResetIndex = 7;
// Scroll-specific indices (when readerMode_ == Scroll, these replace RSVP items)
constexpr size_t kSettingsPacingScrollFontSizeIndex = 2;
constexpr size_t kSettingsPacingScrollLineSpacingIndex = 3;
constexpr size_t kSettingsPacingScrollMarginIndex = 4;
constexpr size_t kSettingsPacingScrollPreviewIndex = 5;
constexpr size_t kWifiSettingsNetworkIndex = 1;
constexpr size_t kWifiSettingsChooseIndex = 2;
constexpr size_t kWifiSettingsForgetIndex = 3;
// Dev-only items start after ForgetNetwork
constexpr size_t kWifiSettingsAutoUpdateIndex = 4;
constexpr size_t kWifiSettingsOtaOwnerIndex = 5;
constexpr size_t kWifiSettingsOtaChannelIndex = 6;

// ScreensaverSettings submenu indices
constexpr size_t kScreensaverSettingsBackIndex = 0;
constexpr size_t kScreensaverSettingsStyleIndex = 1;
constexpr size_t kScreensaverSettingsTimeoutIndex = 2;
constexpr size_t kScreensaverSettingsAutoOffIndex = 3;
constexpr size_t kScreensaverSettingsSleepGuardIndex = 4;
constexpr size_t kScreensaverSettingsPreviewIndex = 5;

// Screensaver timeout values in minutes: 1, 2, 3, 5, 10, 15, 20, 30
constexpr uint8_t kScreensaverTimeoutCount = 8;
constexpr uint16_t kScreensaverTimeoutMinutes[] = {1, 2, 3, 5, 10, 15, 20, 30};

// Screensaver auto-off values in minutes: 0=Never, 5, 10, 15, 20, 30, 45, 60
constexpr uint8_t kScreensaverAutoOffCount = 8;
constexpr uint16_t kScreensaverAutoOffMinutes[] = {0, 5, 10, 15, 20, 30, 45, 60};

// Sleep guard (reading protection) values in minutes: 0=Off, 5, 10, 15, 20, 30, 45, 60
constexpr uint8_t kScreensaverSleepGuardCount = 8;
constexpr uint16_t kScreensaverSleepGuardMinutes[] = {0, 5, 10, 15, 20, 30, 45, 60};

constexpr size_t kBookPickerBackIndex = 0;
constexpr size_t kChapterPickerBackIndex = 0;
constexpr size_t kChapterPickerFallbackIndex = 1;
constexpr size_t kWifiNetworksBackIndex = 0;
constexpr size_t kWifiNetworksFirstItemIndex = 1;
constexpr const char *kPrefsNamespace = "rsvp";
constexpr const char *kPrefBookPath = "book";
constexpr const char *kPrefLegacyWordIndex = "word";
constexpr const char *kPrefWpm = "wpm";
constexpr const char *kPrefBrightness = "bright";
constexpr const char *kPrefDarkMode = "dark";
constexpr const char *kPrefNightMode = "night";
constexpr const char *kPrefUiLanguage = "ui_lang";
constexpr const char *kPrefReaderMode = "read_mode";
// Jednorazowa migracja: urządzenia flashowane przed zmianą domyślnego trybu
// czytania na RSVP mogły zapisać "read_mode"=Scroll w NVS z poprzednich
// testów — sam nowy default w kodzie ich nie dotyczy, bo preferences_
// przetrwają reflash. Ta flaga pozwala wymusić RSVP dokładnie raz, nie
// nadpisując decyzji, którą użytkownik podejmie świadomie już po migracji.
constexpr const char *kPrefReaderModeMigrated = "read_mode_mig";
constexpr const char *kPrefHandedness = "handed";
constexpr const char *kPrefPhantomWords = "phantom_on";
constexpr const char *kPrefFooterMetricMode = "prog_md";
constexpr const char *kPrefBatteryLabelMode = "bat_md";
constexpr const char *kPrefScreensaverMode = "scrn_sv";
constexpr const char *kPrefReaderBatteryVisible = "read_bat";
constexpr const char *kPrefReaderChapterVisible = "read_ch";
constexpr const char *kPrefReaderProgressVisible = "read_pct";
constexpr const char *kPrefSavePointButtonVisible = "sp_btn";
constexpr const char *kPrefSavePointCustomName = "sp_name_cust";
constexpr const char *kPrefReaderFontSize = "font_size";
constexpr const char *kPrefReaderTypeface = "typeface";
constexpr const char *kPrefTypographyFocusHighlight = "type_hlt";
constexpr const char *kPrefFocusColorIndex = "foc_clr";
constexpr const char *kPrefLegacyPacingLong = "pace_len";
constexpr const char *kPrefLegacyPacingComplex = "pace_cpx";
constexpr const char *kPrefLegacyPacingPunctuation = "pace_pnc";
constexpr const char *kPrefPacingLongMs = "pace_lms";
constexpr const char *kPrefPacingComplexMs = "pace_cms";
constexpr const char *kPrefPacingPunctuationMs = "pace_pms";
constexpr const char *kPrefPauseMode = "pause_md";
constexpr const char *kPrefAccurateTime = "time_est_a";
constexpr const char *kPrefTypographyTracking = "type_trk";
constexpr const char *kPrefTypographyAnchor = "type_anc";
constexpr const char *kPrefTypographyGuideWidth = "type_wid";
constexpr const char *kPrefTypographyGuideGap = "type_gap";
constexpr const char *kPrefScrollFontSize = "sc_font";
constexpr const char *kPrefScrollLineSpacing = "sc_line_sp";
constexpr const char *kPrefScrollMargin = "sc_margin";
constexpr const char *kPrefScreensaverTimeout = "scrn_tmo";
constexpr const char *kPrefScreensaverAutoOff = "scrn_aof";
constexpr const char *kPrefScreensaverSleepGuard = "scrn_slp";
constexpr const char *kPrefRecentSeq = "seq";
constexpr const char *kPrefWifiSsid = "wifi_ssid";
constexpr const char *kPrefWifiPass = "wifi_pass";
// Small remembered-networks ring: kPrefWifiSsid/kPrefWifiPass above only
// ever hold the *currently active* network, so switching networks and
// coming back used to mean retyping the password every time. This keeps
// up to kMaxSavedWifiNetworks past SSID/password pairs in NVS so
// reconnecting to a known network skips the password prompt entirely.
constexpr size_t kMaxSavedWifiNetworks = 5;
constexpr const char *kPrefSavedWifiSsidPrefix = "wnet_s";
constexpr const char *kPrefSavedWifiPassPrefix = "wnet_p";
constexpr const char *kPrefOtaAuto = "ota_auto";
constexpr const char *kPrefOtaOwner = "ota_owner";
// Staging repo mirrors the public one for pre-release testing (see
// GRKarol/czytnik01-staging) — switching this flag repoints OTA checks there
// without touching the owner field, which stays reserved for forks.
constexpr const char *kPrefOtaChannel = "ota_channel";
constexpr const char *kOtaStagingRepo = "czytnik01-staging";
constexpr const char *kPrefDevMode = "dev_mode";
constexpr const char *kPrefBleEnabled = "ble_on";
constexpr const char *kPrefShowHelpHints = "help_hints";
constexpr const char *kPrefNavMode = "nav_mode";
constexpr size_t kReaderFontSizeCount = 3;
constexpr size_t kPhantomBeforeCharTargets[] = {64, 96, 144};
constexpr size_t kPhantomAfterCharTargets[] = {96, 144, 208};
constexpr uint32_t kNoSavedWordIndex = 0xFFFFFFFFUL;
constexpr uint16_t kPacingDelayMinMs = 0;
constexpr uint16_t kPacingDelayMaxMs = 600;
constexpr uint16_t kPacingDelayStepMs = 50;
constexpr uint16_t kDefaultPacingDelayMs = 200;
// Geometry of the PacingDelayEditor slider button — shared by
// App::renderPacingDelayEditor() (draws it) and
// App::applyPacingDelayEditorTouchX() (hit-tests drags against it), both of
// which pass these same numbers into DisplayManager::sliderTrackRectFor()
// so the two can't drift apart.
constexpr uint16_t kPacingSliderX = 4;
constexpr uint16_t kPacingSliderY = 20;
constexpr uint16_t kPacingSliderW = BoardConfig::DISPLAY_WIDTH - 8;
constexpr uint16_t kPacingSliderH = BoardConfig::DISPLAY_HEIGHT - 24;
// Generous hit box around the small corner Back icon (drawn at 3,3,26,20 by
// applyBackButtonCornerLayout()) for the editor's hand-rolled touch handler.
// Matches App::backCornerHitZone()'s non-deep-screen size so Back is exactly
// as easy to hit here as everywhere else in the app.
constexpr int kPacingSliderBackHitX1 = 80;
constexpr int kPacingSliderBackHitY1 = 35;
// Vertical/horizontal slack around the visible slider track (see
// DisplayManager::sliderTrackRectFor()) within which a touch still counts as
// "on the slider". Outside this band — e.g. a tap on the label text near the
// top of the screen — the touch is dead: it must not silently drag the
// value just because it shares an x-coordinate with some position on the
// track.
constexpr int kTypographySliderHitToleranceX = 20;
constexpr int kTypographySliderHitToleranceY = 40;
constexpr uint16_t kSettingsWpmMin = 10;
constexpr uint16_t kSettingsWpmMax = 1000;
constexpr uint16_t kWpmSliderStepWpm = 10;
constexpr int8_t kTypographyTrackingMin = -2;
constexpr int8_t kTypographyTrackingMax = 3;
constexpr uint8_t kTypographyAnchorMin = 30;
constexpr uint8_t kTypographyAnchorMax = 40;
constexpr uint8_t kLeftHandAnchorOffset = 20;
constexpr uint8_t kLeftHandAnchorMin = kTypographyAnchorMin + kLeftHandAnchorOffset;
constexpr uint8_t kLeftHandAnchorMax = kTypographyAnchorMax + kLeftHandAnchorOffset;
constexpr uint8_t kTypographyGuideWidthMin = 12;
constexpr uint8_t kTypographyGuideWidthMax = 30;
constexpr uint8_t kTypographyGuideWidthStep = 2;
constexpr uint8_t kTypographyGuideGapMin = 0;
constexpr uint8_t kTypographyGuideGapMax = 8;
constexpr const char *kTypographyPreviewWords[] = {
    "minimum",
    "encyclopaedia",
    "state-of-the-art",
    "HTTP/2",
    "well-known",
    "rhythms",
    "illumination",
    "WAVEFORM",
    "I",
};
constexpr size_t kTypographyPreviewWordCount =
    sizeof(kTypographyPreviewWords) / sizeof(kTypographyPreviewWords[0]);
constexpr size_t kWifiPasswordMaxLength = 63;
constexpr uint16_t kKeyboardMarginX = 8;
constexpr uint16_t kKeyboardTopY = 48;
constexpr uint16_t kKeyboardRowGap = 4;
constexpr uint16_t kKeyboardRowHeight = 27;

void logApp(const char *message) {
  ESP_LOGI(kAppTag, "%s", message);
  Serial.printf("[app] %s\n", message);
}

String displayNameForPath(const String &path) {
  const int separator = path.lastIndexOf('/');
  String name = separator >= 0 ? path.substring(separator + 1) : path;
  String lowered = name;
  lowered.toLowerCase();
  if (lowered.endsWith(".txt")) {
    name.remove(name.length() - 4);
  }
  if (lowered.endsWith(".rsvp")) {
    name.remove(name.length() - 5);
  }
  return name;
}

uint32_t hashBookPath(const String &path) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < path.length(); ++i) {
    hash ^= static_cast<uint8_t>(path[i]);
    hash *= 16777619UL;
  }
  return hash;
}

int clampIntSetting(int value, int minValue, int maxValue) {
  return std::max(minValue, std::min(maxValue, value));
}

DisplayManager::TypographyConfig defaultTypographyConfig() {
  return DisplayManager::TypographyConfig();
}

bool wifiNetworkRequiresPassword(uint8_t authMode) {
  return static_cast<wifi_auth_mode_t>(authMode) != WIFI_AUTH_OPEN;
}

String wifiSecurityLabel(uint8_t authMode) {
  return wifiNetworkRequiresPassword(authMode) ? "Secure" : "Open";
}

String maskedValue(const String &value) {
  String masked;
  masked.reserve(value.length());
  for (size_t i = 0; i < value.length(); ++i) {
    masked += '*';
  }
  return masked;
}

const char *keyboardRowText(uint8_t modeValue, size_t rowIndex) {
  static constexpr const char *kLowerRows[] = {
      "qwertyuiop",
      "asdfghjkl",
      "zxcvbnm",
  };
  static constexpr const char *kUpperRows[] = {
      "QWERTYUIOP",
      "ASDFGHJKL",
      "ZXCVBNM",
  };
  static constexpr const char *kSymbolRows[] = {
      "1234567890",
      "!@#$%^&*?",
      "-_=+/:;.,",
  };

  if (rowIndex >= 3) {
    return "";
  }

  switch (modeValue) {
    case 1:
      return kUpperRows[rowIndex];
    case 2:
      return kSymbolRows[rowIndex];
    default:
      return kLowerRows[rowIndex];
  }
}

String storedOrFallbackLabel(const String &value, const String &fallback) {
  return value.isEmpty() ? fallback : value;
}

size_t packedLifeWordCount(size_t cellCount) { return (cellCount + 31U) / 32U; }

bool packedLifeCellAlive(const std::vector<uint32_t> &cells, size_t index) {
  const size_t word = index / 32U;
  if (word >= cells.size()) {
    return false;
  }
  return (cells[word] & (1UL << (index % 32U))) != 0;
}

void setPackedLifeCell(std::vector<uint32_t> &cells, size_t index, bool alive) {
  const size_t word = index / 32U;
  if (word >= cells.size()) {
    return;
  }
  const uint32_t mask = 1UL << (index % 32U);
  if (alive) {
    cells[word] |= mask;
  } else {
    cells[word] &= ~mask;
  }
}

uint32_t advanceStandbyRng(uint32_t &rng) {
  rng = (rng * 1664525UL) + 1013904223UL;
  return rng;
}

struct LifePoint {
  int8_t x;
  int8_t y;
};

void setPackedLifeCellAt(std::vector<uint32_t> &cells, uint16_t columns, uint16_t rows, int x,
                         int y, bool alive) {
  if (x < 0 || y < 0 || x >= static_cast<int>(columns) || y >= static_cast<int>(rows)) {
    return;
  }
  setPackedLifeCell(cells, static_cast<size_t>(y) * columns + static_cast<size_t>(x), alive);
}

void clearPackedLifeRect(std::vector<uint32_t> &cells, uint16_t columns, uint16_t rows, int x,
                         int y, int width, int height) {
  const int xEnd = std::min(static_cast<int>(columns), x + width);
  const int yEnd = std::min(static_cast<int>(rows), y + height);
  for (int cy = std::max(0, y); cy < yEnd; ++cy) {
    for (int cx = std::max(0, x); cx < xEnd; ++cx) {
      setPackedLifeCellAt(cells, columns, rows, cx, cy, false);
    }
  }
}

void stampPackedLifePattern(std::vector<uint32_t> &cells, uint16_t columns, uint16_t rows,
                            const LifePoint *points, size_t pointCount, int originX,
                            int originY) {
  for (size_t i = 0; i < pointCount; ++i) {
    setPackedLifeCellAt(cells, columns, rows, originX + points[i].x, originY + points[i].y, true);
  }
}

void clearAndStampPackedLifePattern(std::vector<uint32_t> &cells, uint16_t columns, uint16_t rows,
                                    const LifePoint *points, size_t pointCount, int originX,
                                    int originY, int width, int height) {
  if (originX < 0 || originY < 0 || originX + width > static_cast<int>(columns) ||
      originY + height > static_cast<int>(rows)) {
    return;
  }
  constexpr int kPatternMargin = 5;
  clearPackedLifeRect(cells, columns, rows, originX - kPatternMargin, originY - kPatternMargin,
                      width + kPatternMargin * 2, height + kPatternMargin * 2);
  stampPackedLifePattern(cells, columns, rows, points, pointCount, originX, originY);
}

constexpr LifePoint kLifeGlider[] = {
    {1, 0},
    {2, 1},
    {0, 2},
    {1, 2},
    {2, 2},
};

constexpr LifePoint kLifeLightweightSpaceship[] = {
    {1, 0}, {4, 0}, {0, 1}, {0, 2}, {4, 2}, {0, 3}, {1, 3}, {2, 3}, {3, 3},
};

constexpr LifePoint kLifePentadecathlon[] = {
    {2, 0}, {2, 1}, {1, 2}, {3, 2}, {2, 3}, {2, 4},
    {2, 5}, {2, 6}, {1, 7}, {3, 7}, {2, 8}, {2, 9},
};

constexpr LifePoint kLifePulsar[] = {
    {2, 0},  {3, 0},  {4, 0},  {8, 0},  {9, 0},  {10, 0}, {0, 2},  {5, 2},
    {7, 2},  {12, 2}, {0, 3},  {5, 3},  {7, 3},  {12, 3}, {0, 4},  {5, 4},
    {7, 4},  {12, 4}, {2, 5},  {3, 5},  {4, 5},  {8, 5},  {9, 5},  {10, 5},
    {2, 7},  {3, 7},  {4, 7},  {8, 7},  {9, 7},  {10, 7}, {0, 8},  {5, 8},
    {7, 8},  {12, 8}, {0, 9},  {5, 9},  {7, 9},  {12, 9}, {0, 10}, {5, 10},
    {7, 10}, {12, 10}, {2, 12}, {3, 12}, {4, 12}, {8, 12}, {9, 12}, {10, 12},
};

constexpr LifePoint kLifeGosperGliderGun[] = {
    {24, 0}, {22, 1}, {24, 1}, {12, 2}, {13, 2}, {20, 2}, {21, 2}, {34, 2}, {35, 2},
    {11, 3}, {15, 3}, {20, 3}, {21, 3}, {34, 3}, {35, 3}, {0, 4},  {1, 4},
    {10, 4}, {16, 4}, {20, 4}, {21, 4}, {0, 5},  {1, 5},  {10, 5}, {14, 5},
    {16, 5}, {17, 5}, {22, 5}, {24, 5}, {10, 6}, {16, 6}, {24, 6}, {11, 7},
    {15, 7}, {12, 8}, {13, 8},
};

void copyOtaLabel(char *destination, size_t destinationSize, const String &source) {
  if (destination == nullptr || destinationSize == 0) {
    return;
  }

  const size_t copyLength = std::min(destinationSize - 1, source.length());
  for (size_t i = 0; i < copyLength; ++i) {
    destination[i] = source[i];
  }
  destination[copyLength] = '\0';
}

bool sdCardFolderRepairNeeded(const StorageManager::DiagnosticResult &result) {
  return result.mounted &&
         (!result.booksDirectory || !result.bookFilesDirectory ||
          !result.articleFilesDirectory || !result.configDirectory ||
          !result.pluginsDirectory);
}

DisplayManager::ReaderTypeface readerTypefaceFromSetting(uint8_t value) {
  if (value < static_cast<uint8_t>(DisplayManager::ReaderTypeface::Count)) {
    return static_cast<DisplayManager::ReaderTypeface>(value);
  }
  return DisplayManager::ReaderTypeface::Standard;
}

App::ReaderMode readerModeFromSetting(uint8_t value) {
  switch (value) {
    case static_cast<uint8_t>(App::ReaderMode::Scroll):
    case 2:  // Migrate the removed word-scroll mode to page scroll.
      return App::ReaderMode::Scroll;
    case static_cast<uint8_t>(App::ReaderMode::Rsvp):
    default:
      return App::ReaderMode::Rsvp;
  }
}

App::HandednessMode handednessModeFromSetting(uint8_t value) {
  switch (value) {
    case static_cast<uint8_t>(App::HandednessMode::Left):
      return App::HandednessMode::Left;
    case static_cast<uint8_t>(App::HandednessMode::Right):
    default:
      return App::HandednessMode::Right;
  }
}

App::NavMode navModeFromSetting(uint8_t value) {
  switch (value) {
    case static_cast<uint8_t>(App::NavMode::DPad):
      return App::NavMode::DPad;
    case static_cast<uint8_t>(App::NavMode::Swipe):
      return App::NavMode::Swipe;
    case static_cast<uint8_t>(App::NavMode::Buttons):
    default:
      return App::NavMode::Buttons;
  }
}

App::NavMode nextNavMode(App::NavMode current) {
  switch (navModeFromSetting(static_cast<uint8_t>(current))) {
    case App::NavMode::Buttons:
      return App::NavMode::Swipe;
    case App::NavMode::Swipe:
      return App::NavMode::DPad;
    case App::NavMode::DPad:
    default:
      return App::NavMode::Buttons;
  }
}

App::HandednessMode nextHandednessMode(App::HandednessMode current) {
  switch (handednessModeFromSetting(static_cast<uint8_t>(current))) {
    case App::HandednessMode::Left:
      return App::HandednessMode::Right;
    case App::HandednessMode::Right:
    default:
      return App::HandednessMode::Left;
  }
}

App::ReaderMode nextReaderMode(App::ReaderMode current) {
  switch (readerModeFromSetting(static_cast<uint8_t>(current))) {
    case App::ReaderMode::Rsvp:
      return App::ReaderMode::Scroll;
    case App::ReaderMode::Scroll:
    default:
      return App::ReaderMode::Rsvp;
  }
}

uint16_t pacingDelayMsForLegacyLevel(uint8_t levelIndex) {
  constexpr uint16_t kLegacyPacingDelayMs[] = {100, 150, 200, 250, 300};
  constexpr size_t kLegacyPacingLevelCount =
      sizeof(kLegacyPacingDelayMs) / sizeof(kLegacyPacingDelayMs[0]);

  if (levelIndex >= kLegacyPacingLevelCount) {
    levelIndex = 2;
  }
  return kLegacyPacingDelayMs[levelIndex];
}

uint16_t loadPacingDelayMs(Preferences &preferences, const char *key, const char *legacyKey) {
  if (preferences.isKey(key)) {
    return static_cast<uint16_t>(
        clampIntSetting(preferences.getUShort(key, kDefaultPacingDelayMs), kPacingDelayMinMs,
                        kPacingDelayMaxMs));
  }

  if (preferences.isKey(legacyKey)) {
    const uint16_t migratedDelayMs =
        pacingDelayMsForLegacyLevel(preferences.getUChar(legacyKey, 2));
    preferences.putUShort(key, migratedDelayMs);
    return migratedDelayMs;
  }

  return kDefaultPacingDelayMs;
}

}  // namespace

App::App() : button_(BoardConfig::PIN_BOOT_BUTTON), powerButton_(BoardConfig::PIN_PWR_BUTTON) {}

void App::begin() {
  BoardConfig::begin();
  button_.begin();
  powerButton_.begin();
  bootButtonReleasedSinceBoot_ = !button_.isHeld();
  bootButtonLongPressHandled_ = false;
  powerButtonReleasedSinceBoot_ = !powerButton_.isHeld();
  powerButtonLongPressHandled_ = false;
  powerTapPending_ = false;
  storage_.setStatusCallback(&App::handleStorageStatus, this);
  preferences_.begin(kPrefsNamespace, false);
  if (!preferences_.getBool(kPrefReaderModeMigrated, false)) {
    preferences_.putUChar(kPrefReaderMode, static_cast<uint8_t>(ReaderMode::Rsvp));
    preferences_.putBool(kPrefReaderModeMigrated, true);
  }
  pluginLibrary_.begin();
  pluginLoader_.begin();
  recorder_.begin();
  pluginLoader_.setManagers(&display_, &audio_, &recorder_);
  brightnessLevelIndex_ = preferences_.getUChar(kPrefBrightness, brightnessLevelIndex_);
  if (brightnessLevelIndex_ >= kBrightnessLevelCount) {
    brightnessLevelIndex_ = kBrightnessLevelCount - 1;
  }
  phantomWordsEnabled_ = preferences_.getBool(kPrefPhantomWords, phantomWordsEnabled_);
  readerBatteryVisibleWhilePlaying_ =
      preferences_.getBool(kPrefReaderBatteryVisible, readerBatteryVisibleWhilePlaying_);
  readerChapterVisibleWhilePlaying_ =
      preferences_.getBool(kPrefReaderChapterVisible, readerChapterVisibleWhilePlaying_);
  readerProgressVisibleWhilePlaying_ =
      preferences_.getBool(kPrefReaderProgressVisible, readerProgressVisibleWhilePlaying_);
  savePointButtonVisible_ =
      preferences_.getBool(kPrefSavePointButtonVisible, savePointButtonVisible_);
  savePointUseCustomName_ =
      preferences_.getBool(kPrefSavePointCustomName, savePointUseCustomName_);
  showHelpHints_ = preferences_.getBool(kPrefShowHelpHints, showHelpHints_);
  {
    navMode_ = navModeFromSetting(
        preferences_.getUChar(kPrefNavMode, static_cast<uint8_t>(navMode_)));
  }
  uiLanguage_ =
      Localization::sanitizeLanguage(preferences_.getUChar(
          kPrefUiLanguage, static_cast<uint8_t>(uiLanguage_)));
  DeviceServicesBridge::setLanguageIndex(static_cast<int>(uiLanguage_));
  readerMode_ = readerModeFromSetting(
      preferences_.getUChar(kPrefReaderMode, static_cast<uint8_t>(readerMode_)));
  handednessMode_ = handednessModeFromSetting(
      preferences_.getUChar(kPrefHandedness, static_cast<uint8_t>(handednessMode_)));
  readerFontSizeIndex_ = preferences_.getUChar(kPrefReaderFontSize, readerFontSizeIndex_);
  if (readerFontSizeIndex_ >= kReaderFontSizeCount) {
    readerFontSizeIndex_ = 0;
  }
  scrollFontSize_ = preferences_.getUChar(kPrefScrollFontSize, scrollFontSize_);
  if (scrollFontSize_ > 8) scrollFontSize_ = 4;
  scrollLineSpacing_ = preferences_.getUChar(kPrefScrollLineSpacing, scrollLineSpacing_);
  if (scrollLineSpacing_ > 2) scrollLineSpacing_ = 1;
  scrollMargin_ = preferences_.getUChar(kPrefScrollMargin, scrollMargin_);
  if (scrollMargin_ > 2) scrollMargin_ = 1;
  switch (preferences_.getUChar(kPrefFooterMetricMode,
                                static_cast<uint8_t>(footerMetricMode_))) {
    case static_cast<uint8_t>(FooterMetricMode::ChapterTime):
      footerMetricMode_ = FooterMetricMode::ChapterTime;
      break;
    case static_cast<uint8_t>(FooterMetricMode::BookTime):
      footerMetricMode_ = FooterMetricMode::BookTime;
      break;
    case static_cast<uint8_t>(FooterMetricMode::Percentage):
    default:
      footerMetricMode_ = FooterMetricMode::Percentage;
      break;
  }
  switch (preferences_.getUChar(kPrefBatteryLabelMode,
                                static_cast<uint8_t>(batteryLabelMode_))) {
    case static_cast<uint8_t>(BatteryLabelMode::TimeRemaining):
      batteryLabelMode_ = BatteryLabelMode::TimeRemaining;
      break;
    case static_cast<uint8_t>(BatteryLabelMode::Voltage):
      batteryLabelMode_ = BatteryLabelMode::Voltage;
      break;
    case static_cast<uint8_t>(BatteryLabelMode::Percent):
    default:
      batteryLabelMode_ = BatteryLabelMode::Percent;
      break;
  }
  switch (preferences_.getUChar(kPrefScreensaverMode, static_cast<uint8_t>(screensaverMode_))) {
    case static_cast<uint8_t>(ScreensaverMode::Maze):
      screensaverMode_ = ScreensaverMode::Maze;
      break;
    case static_cast<uint8_t>(ScreensaverMode::Voronoi):
      screensaverMode_ = ScreensaverMode::Voronoi;
      break;
    case static_cast<uint8_t>(ScreensaverMode::Stars):
      screensaverMode_ = ScreensaverMode::Stars;
      break;
    case static_cast<uint8_t>(ScreensaverMode::Matrix):
      screensaverMode_ = ScreensaverMode::Matrix;
      break;
    case static_cast<uint8_t>(ScreensaverMode::ScreenOff):
      screensaverMode_ = ScreensaverMode::ScreenOff;
      break;
    case static_cast<uint8_t>(ScreensaverMode::Life):
    default:
      screensaverMode_ = ScreensaverMode::Life;
      break;
  }
  screensaverTimeoutIndex_ = preferences_.getUChar(kPrefScreensaverTimeout, 2);
  if (screensaverTimeoutIndex_ >= kScreensaverTimeoutCount) {
    screensaverTimeoutIndex_ = 2;
  }
  screensaverAutoOffIndex_ = preferences_.getUChar(kPrefScreensaverAutoOff, 0);
  if (screensaverAutoOffIndex_ >= kScreensaverAutoOffCount) {
    screensaverAutoOffIndex_ = 0;
  }
  screensaverSleepGuardIndex_ = preferences_.getUChar(kPrefScreensaverSleepGuard, 0);
  if (screensaverSleepGuardIndex_ >= kScreensaverSleepGuardCount) {
    screensaverSleepGuardIndex_ = 0;
  }
  switch (preferences_.getUChar(kPrefPauseMode, static_cast<uint8_t>(pauseMode_))) {
    case static_cast<uint8_t>(PauseMode::Instant):
      pauseMode_ = PauseMode::Instant;
      break;
    case static_cast<uint8_t>(PauseMode::SentenceEnd):
    default:
      pauseMode_ = PauseMode::SentenceEnd;
      break;
  }
  pacingLongWordDelayMs_ =
      loadPacingDelayMs(preferences_, kPrefPacingLongMs, kPrefLegacyPacingLong);
  pacingComplexWordDelayMs_ =
      loadPacingDelayMs(preferences_, kPrefPacingComplexMs, kPrefLegacyPacingComplex);
  pacingPunctuationDelayMs_ =
      loadPacingDelayMs(preferences_, kPrefPacingPunctuationMs, kPrefLegacyPacingPunctuation);
  accurateTimeEstimateEnabled_ = true;
  loadTypographyConfigFromPreferences();
  darkMode_ = preferences_.getBool(kPrefDarkMode, darkMode_);
  nightMode_ = preferences_.getBool(kPrefNightMode, nightMode_);
  display_.setFocusColorIndex(preferences_.getUChar(kPrefFocusColorIndex, 1));
  applyHandednessSettings(0, false);
  applyDisplayPreferences(0, false);
  applyTypographySettings(0, false);
  applyPacingSettings();
  bootStartedMs_ = millis();
  lastStateLogMs_ = bootStartedMs_;
  lastScrollAnimationRenderMs_ = 0;
  Serial.printf("[app] version=%s\n", otaUpdater_.currentVersion().c_str());

  logApp("Initializing hardware modules");
  const bool displayReady = display_.begin();

  // Boot splash: a beat of black, then the artwork appears at full
  // brightness (see DisplayManager::renderBootSplash() for why this isn't a
  // fade — the backlight PWM driver glitches on every ramp step). kBootSplashMs
  // (the total time the Booting state holds before handing off to the
  // wizard/reader) is sized to comfortably cover this sequence.
  if (displayReady) {
    display_.renderBootSplash(kBootSplashBlackMs);
    logApp("Display init ok");
  } else {
    ESP_LOGE(kAppTag, "Display init failed");
    Serial.println("[app] Display init failed");
  }

  // Initialize remaining hardware while the splash is still visible on screen.
  updateBatteryStatus(bootStartedMs_, true);
  touchInitialized_ = touch_.begin();
  audio_.begin();

#if RSVP_USB_TRANSFER_ENABLED && RSVP_USB_TRANSFER_AUTO_START
  state_ = AppState::Booting;
  Serial.println("[app] USB transfer auto-start active");
  enterUsbTransfer(millis());
  return;
#endif

  // Keep SD status text ("Mounting card", "Scanning books", ...) off the
  // boot splash for the whole boot sequence, not just the deferred book
  // load — storage_.begin() below fires the same status callback. Cleared
  // at every exit out of AppState::Booting in updateState().
  suppressBootStorageStatusRender_ = true;
  storageReady_ = storage_.begin();
  if (storageReady_) {
    // The typeface saved in preferences may be an SD-backed font (see
    // DisplayManager::ReaderTypeface). The earlier applyTypographySettings()
    // call above ran before storage_.begin(), so if the saved typeface lives
    // on SD, that first load attempt always failed (card not mounted yet)
    // and silently fell back to Atkinson for the rest of the session. Retry
    // now that the card is actually mounted.
    applyTypographySettings(bootStartedMs_, false);
  }
  if (!display_.isActiveTypefaceLoaded()) {
    // Still not loaded — either storage_.begin() itself failed on this cold
    // power-on (card needs longer than its internal retry loop allows), or
    // the .fnt pair just hasn't finished the Wi-Fi auto-download yet. Keep
    // retrying at a low rate from the main loop (see
    // maybeRetryTypographyFontLoad) instead of settling on Atkinson for the
    // whole session.
    typographyFontRetryPending_ = true;
    typographyFontRetryLastAttemptMs_ = bootStartedMs_;
    typographyFontRetryDeadlineMs_ = bootStartedMs_ + 20000;
  }
  const uint16_t savedWpm = preferences_.getUShort(kPrefWpm, reader_.wpm());
  reader_.setWpm(savedWpm);

  pendingBootBookLoad_ = prepareBootBookLoad();
  if (!pendingBootBookLoad_) {
    usingStorageBook_ = false;
    chapterMarkers_.clear();
    paragraphStarts_.clear();
    currentBookPath_ = "";
    currentBookTitle_ = "Demo";
    reader_.begin(bootStartedMs_);
    invalidateContextPreviewWindow();
    rebuildTimeEstimateCache();
    Serial.println("[app] using built-in demo text");
  } else {
    currentBookTitle_ = storage_.bookDisplayName(pendingBootBookIndex_);
    if (currentBookTitle_.isEmpty()) {
      currentBookTitle_ = tr(TrKey::LoadingBook);
    }
  }

  maybeAutoCheckForUpdates(bootStartedMs_);
  refreshFontPackComplete();
  maybeAutoDownloadFonts(bootStartedMs_);
  // Plugin sync runs in background after first update loop iteration
  // (moved out of boot path to prevent blocking)

  // Auto-start BLE peripheral tylko jeśli użytkownik wcześniej włączył je w
  // Ustawienia > Łączność > Bluetooth (domyślnie wyłączone — patrz
  // kPrefBleEnabled, ustawiane przez selectSettingsConnectivityItem()).
  extern BleApi *g_bleApiPtr;
  g_bleApiPtr = &ble_;
  if (preferences_.getBool(kPrefBleEnabled, false)) {
    ble_.begin(this);
    Serial.println("========================================");
    Serial.printf("FLOWER BLE NAME: %s\n", ble_.deviceName().c_str());
    Serial.printf("FLOWER TOKEN: %s\n", ble_.currentToken().c_str());
    Serial.printf("FLOWER QR: %s\n", ble_.qrPayload().c_str());
    Serial.println("========================================");
  } else {
    Serial.println("[app] BLE off by default (not enabled in settings)");
  }

  // Auto-start companion sync AP for 30 seconds on boot.
  // If no client connects within the timeout, AP shuts down to save power.
  {
    CompanionSyncManager::Config syncConfig;
    syncConfig.wifiSsid = "";
    syncConfig.wifiPassword = "";
    if (companionSync_.begin(syncConfig)) {
      autoSyncActive_ = true;
      autoSyncStartedMs_ = millis();
      autoSyncClientConnected_ = false;
      Serial.println("[app] auto-sync AP started (30s timeout)");
    } else {
      Serial.println("[app] auto-sync AP failed to start");
    }
  }

  Serial.printf("[app] WPM=%u interval=%lu ms\n", reader_.wpm(),
                static_cast<unsigned long>(reader_.wordIntervalMs()));

  state_ = AppState::Booting;
  lastActivityMs_ = millis();
  Serial.println("[app] READY splash active");

  // Boot reached a confirmed-healthy point (display drawn, storage/reader
  // init ran) — cancel the OTA rollback timer for this image. If a bad OTA
  // update crashes before ever reaching here, the image stays in
  // PENDING_VERIFY state and the bootloader reverts to the previous working
  // partition on the next reset instead of leaving the reader bricked.
  //
  // No platformio.ini flag needed for this: CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
  // is already 1 in the precompiled Arduino core for our board (qio_opi memory
  // type), and boards/esp32-s3-r8-opi.json's default_16MB.csv already carries
  // otadata + ota_0/ota_1 slots, which rollback requires. Verified directly in
  // framework-arduinoespressif32/tools/sdk/esp32s3/qio_opi/include/sdkconfig.h.
  esp_ota_mark_app_valid_cancel_rollback();
}

void App::update(uint32_t nowMs) {
  button_.update(nowMs);
  powerButton_.update(nowMs);

  // ── Plugin running: plugin owns the screen, App just monitors ──────────
  if (pluginLoader_.isRunning()) {
    pluginLoader_.watchdogCheck(nowMs);

    // Plugin crashed or watchdog timed out → show error, return to plugins menu
    if (pluginLoader_.state() == PluginLoader::State::Error) {
      const char* errMsg = pluginLoader_.lastErrorMessage();
      Serial.printf("[plugin] error detected: %s\n", errMsg ? errMsg : "unknown");
      display_.renderStatus("Plugin", errMsg ? errMsg : "Plugin error", "");
      delay(2000);
      pluginLoader_.unload();
      openPluginsActive();
      return;
    }

    // Long-press power button → power off (same as normal behavior)
    if (powerButton_.isHeld() && nowMs - powerButton_.lastEdgeMs() >= kPowerOffHoldMs) {
      pluginLoader_.unload();
      powerButtonLongPressHandled_ = true;
      enterPowerOff(nowMs);
      return;
    }

    // Short-press power button → exit plugin, return to plugins menu
    if (powerButton_.wasReleasedEvent()) {
      Serial.println("[plugin] power button pressed — unloading plugin");
      pluginLoader_.unload();
      openPluginsActive();
      return;
    }

    // Forward boot button events to the plugin
    if (button_.wasReleasedEvent()) {
      PluginButtonEvent btnEvent = {};
      btnEvent.buttonId = 0;  // boot button
      btnEvent.pressed = true;
      btnEvent.timestampMs = nowMs;
      pluginLoader_.forwardButton(btnEvent);
    }

    // Forward touch events to the plugin
    TouchEvent touchEv;
    if (touch_.poll(touchEv)) {
      PluginTouchEvent pluginTouch = {};
      pluginTouch.x = touchEv.x;
      pluginTouch.y = touchEv.y;
      pluginTouch.phase = static_cast<uint8_t>(touchEv.phase);
      pluginTouch.timestampMs = nowMs;
      pluginLoader_.forwardTouch(pluginTouch);
    }

    // Battery monitoring continues while plugin is running
    updateBatteryStatus(nowMs);
    if (powerOffStarted_) {
      pluginLoader_.unload();
      return;
    }
    return;
  }
  // ── End plugin running block ───────────────────────────────────────────

  const bool standbyComboConsumed = handleStandbyCombo(nowMs);
  if (!standbyComboConsumed) {
    handleBootButton(nowMs);
    handlePowerButton(nowMs);
  }
  if (powerOffStarted_) {
    return;
  }

  const bool batteryChanged = updateBatteryStatus(nowMs);
  if (powerOffStarted_) {
    return;
  }

  if (batteryWarningOverlayVisible_) {
    updateBatteryWarningOverlay(nowMs);
    if (batteryWarningOverlayVisible_) {
      if (nowMs - lastStateLogMs_ > 1500) {
        lastStateLogMs_ = nowMs;
        ESP_LOGI(kAppTag, "state=%s", stateName(state_));
        Serial.printf("[app] state=%s ms=%lu\n", stateName(state_),
                      static_cast<unsigned long>(nowMs));
      }
      return;
    }
  }

  if (state_ == AppState::Standby) {
    handleTouch(nowMs);
    updateStandbyScreensaver(nowMs);
    if (nowMs - lastStateLogMs_ > 1500) {
      lastStateLogMs_ = nowMs;
      ESP_LOGI(kAppTag, "state=%s", stateName(state_));
      Serial.printf("[app] state=%s ms=%lu\n", stateName(state_),
                    static_cast<unsigned long>(nowMs));
    }
    return;
  }

  pollOtaCheckResult(nowMs);
  pollFontDownloadResult(nowMs);
  pollBookDownloadResult(nowMs);
  maybeAutoDownloadFonts(nowMs);
  maybeRetryTypographyFontLoad(nowMs);
  updateWelcomeTimedScreens(nowMs);
  updateWelcomeReadingModePreview(nowMs);
  updateState(nowMs);
  loadPendingBootBook(nowMs);
  // Deliberately not auto-opening UpdateConfirm here: an update found mid-read
  // used to yank the user out of Playing/Paused into a blocking Update/Skip
  // screen. pollOtaCheckResult() already surfaces it as a ">> Update vX.Y.Z"
  // row at the top of the main menu (see selectMenuItem()'s otaUpdatePromptPending_
  // handling) — informing without interrupting whatever the user is doing.

  updateReader(nowMs);
  handleTouch(nowMs);
  updateGridToastOverlay(nowMs);

  // Screensaver idle timeout: activate screensaver if user hasn't interacted
  if (lastActivityMs_ > 0 && state_ != AppState::Booting &&
      state_ != AppState::UsbTransfer && state_ != AppState::CompanionSync &&
      state_ != AppState::Sleeping && !powerOffStarted_) {
    const uint32_t timeoutMs =
        static_cast<uint32_t>(kScreensaverTimeoutMinutes[screensaverTimeoutIndex_]) * 60000UL;
    const uint32_t elapsed = nowMs - lastActivityMs_;

    if (state_ == AppState::Playing) {
      // Sleep guard: if reading and no touch for X minutes, pause and enter standby
      if (screensaverSleepGuardIndex_ > 0) {
        const uint32_t sleepGuardMs =
            static_cast<uint32_t>(kScreensaverSleepGuardMinutes[screensaverSleepGuardIndex_]) * 60000UL;
        if (elapsed >= sleepGuardMs) {
          Serial.println("[app] sleep guard: no touch while reading, entering standby");
          saveReadingPosition(true);
          enterStandby(nowMs);
          return;
        }
      }
    } else if (state_ == AppState::Paused || state_ == AppState::Menu) {
      // Normal idle timeout: enter screensaver when paused/menu idle
      if (elapsed >= timeoutMs) {
        Serial.println("[app] idle timeout: entering standby screensaver");
        enterStandby(nowMs);
        return;
      }
    }
  }

  updateWpmFeedback(nowMs);
  maybeSaveReadingPosition(nowMs);
  updateTimeEstimateBuild(nowMs);

  // BLE: drenuj komendy zakolejkowane przez host task (set-dev-mode itp.)
  // i sprawdź czy zmiana stanu (connect/disconnect) wymaga odświeżenia
  // menu Polaczenia. Bez tego label „Bluetooth: WLACZONY" zostaje stale
  // gdy telefon się podłączy w trakcie wyświetlania menu.
  ble_.update();

  // Spontaneous BLE events (Protocol v2)
  if (ble_.isAuthenticated()) {
    // Battery event: every 60s or when change > 2%
    static uint32_t lastBleEventBatteryMs = 0;
    static uint8_t lastBleEventBatteryPercent = 0;
    if (nowMs - lastBleEventBatteryMs > 60000 ||
        (batteryPresent_ && abs((int)batteryDisplayedPercent_ - (int)lastBleEventBatteryPercent) >= 2)) {
      if (batteryPresent_ && batteryDisplayedPercent_ != lastBleEventBatteryPercent) {
        ble_.emitEvent("{\"ev\":\"battery\",\"percent\":" + String(batteryDisplayedPercent_) + "}");
        lastBleEventBatteryPercent = batteryDisplayedPercent_;
      }
      lastBleEventBatteryMs = nowMs;
    }
  }

  if (ble_.consumeMenuDirty() && state_ == AppState::Menu &&
      menuScreen_ == MenuScreen::SettingsConnectivity) {
    rebuildSettingsMenuItems();
    renderSettings();
  }

  // Auto-sync AP timeout: shut down if no client connects within 30 seconds.
  // Once a client connects, keep AP alive until user manually exits sync
  // or enters CompanionSync state from menu (which takes over).
  if (autoSyncActive_ && state_ != AppState::CompanionSync) {
    companionSync_.update();
    const uint8_t clients = WiFi.softAPgetStationNum();
    if (clients > 0 && !autoSyncClientConnected_) {
      autoSyncClientConnected_ = true;
      Serial.println("[app] auto-sync: client connected, keeping AP alive");
      // Bez tego ekran WelcomeConnect zostawal na "Oczekiwanie na
      // polaczenie..." az do nastepnego niepowiazanego przerysowania —
      // "Polaczono!" pojawialo sie dopiero przypadkiem, czasem naklejone
      // na uklad liczony jeszcze dla poprzedniego stanu ekranu.
      if (menuScreen_ == MenuScreen::WelcomeAppPairing) {
        renderWelcomeAppPairing();
      }
    }
    if (!autoSyncClientConnected_ && (nowMs - autoSyncStartedMs_ >= 30000)) {
      Serial.println("[app] auto-sync: 30s timeout, no client — shutting down AP");
      companionSync_.end();
      autoSyncActive_ = false;
    }
  }

  if (batteryChanged && (state_ == AppState::Paused || state_ == AppState::Playing)) {
    renderActiveReader(nowMs);
  } else if (batteryChanged && state_ == AppState::Menu) {
    renderMenu();
  }

  if (nowMs - lastStateLogMs_ > 1500) {
    lastStateLogMs_ = nowMs;
    ESP_LOGI(kAppTag, "state=%s", stateName(state_));
    Serial.printf("[app] state=%s ms=%lu\n", stateName(state_),
                  static_cast<unsigned long>(nowMs));
  }
}

const char *App::stateName(AppState state) const {
  switch (state) {
    case AppState::Booting:
      return "Booting";
    case AppState::Paused:
      return "Paused";
    case AppState::Playing:
      return "Playing";
    case AppState::Menu:
      return "Menu";
    case AppState::CompanionSync:
      return "CompanionSync";
    case AppState::UsbTransfer:
      return "UsbTransfer";
    case AppState::Standby:
      return "Standby";
    case AppState::Sleeping:
      return "Sleeping";
  }
  return "Unknown";
}

const char *App::touchPhaseName(TouchPhase phase) const {
  switch (phase) {
    case TouchPhase::Start:
      return "Start";
    case TouchPhase::Move:
      return "Move";
    case TouchPhase::End:
      return "End";
  }
  return "Unknown";
}

void App::setState(AppState nextState, uint32_t nowMs) {
  if (nextState == state_) {
    return;
  }

  // Reset activity timer on any state change (user interaction drove us here)
  lastActivityMs_ = nowMs;

  const AppState previousState = state_;
  if (previousState == AppState::Menu && nextState != AppState::Menu) {
    flushPendingTimeEstimateRebuild();
  }

  if (nextState != AppState::Paused) {
    pausedTouch_.active = false;
    pausedTouchIntent_ = TouchIntent::None;
    contextViewVisible_ = false;
    invalidateContextPreviewWindow();
    wpmFeedbackVisible_ = false;
  }
  if (nextState != AppState::Playing) {
    touchPlayHeld_ = false;
    touchPlayPendingRelease_ = false;
    playLocked_ = false;
    pauseAtSentenceEndRequested_ = false;
    chapterTransitionVisible_ = false;
  }
  if (nextState != AppState::Paused && nextState != AppState::Playing) {
    resetReaderTapTracking();
  }

  state_ = nextState;

  switch (state_) {
    case AppState::Paused:
      renderActiveReader(nowMs);
      break;
    case AppState::Playing:
      reader_.start(nowMs);
      renderActiveReader(nowMs);
      break;
    case AppState::Menu:
      renderMenu();
      break;
    case AppState::CompanionSync:
      if (companionSync_.hasQrCode()) {
        display_.renderStatusWithQr("Wi-Fi", companionSync_.statusLine1(),
                                    companionSync_.qrCodeData(), companionSync_.qrCodeSize());
      } else {
        display_.renderStatus("Sync", companionSync_.statusLine1(), companionSync_.statusLine2());
      }
      break;
    case AppState::UsbTransfer:
      display_.renderStatus("USB", tr(TrKey::PreparingSD),
                            tr(TrKey::EjectWhenDone));
      break;
    case AppState::Standby:
      seedStandbyScreensaver(nowMs);
      updateStandbyScreensaver(nowMs, true);
      break;
    case AppState::Sleeping:
      display_.renderCenteredWord("SLEEP");
      break;
    case AppState::Booting:
      display_.renderCenteredWord("READY");
      break;
  }

  if (state_ == AppState::Paused && previousState == AppState::Playing) {
    saveReadingPosition(true);
  }

  if (previousState == AppState::Booting) {
    // The boot splash faded the backlight out before this switch drew the
    // wizard/reader screen underneath it (see updateState()'s
    // bootSplashFadedOut_ handling) — fade back in now that new content is
    // on screen, instead of an abrupt cut from black to full brightness.
    display_.fadeInBacklight(kBootSplashFadeMs);
  }

  ESP_LOGI(kAppTag, "state -> %s", stateName(state_));
  Serial.printf("[app] state -> %s at %lu ms\n", stateName(state_),
                static_cast<unsigned long>(nowMs));
}

void App::updateState(uint32_t nowMs) {
  if (state_ == AppState::Booting) {
    if (nowMs - bootStartedMs_ < kBootSplashMs) {
      return;
    }

    // Fade the splash artwork out to black exactly once, right as we're
    // about to draw the next real screen underneath it — not up front. It
    // used to run here, before the deferred SD/index book load below, which
    // meant the backlight sat off (screen genuinely black, not just static)
    // for however long that load took — worst case several seconds on a
    // fresh index build or a failed/retried load, stacking silent dead time
    // onto boot. Calling it right before each setState() instead keeps the
    // splash fully lit (frozen, since render is suppressed) while that work
    // happens, so the only guaranteed-black window is the fade transition
    // itself. bootSplashFadedOut_ still guards it to exactly one call.
    // setState() fades back in once the wizard/reader screen underneath has
    // actually been drawn.
    auto fadeOutSplashOnce = [&]() {
      if (!bootSplashFadedOut_) {
        bootSplashFadedOut_ = true;
        display_.fadeOutBacklight(kBootSplashFadeMs);
      }
    };

    // Pierwsze uruchomienie po flashowaniu — pokaż welcome wizard zamiast
    // od razu otwierać czytnik. Ważne: ustaw menuScreen_ ZANIM zawołamy
    // setState(Menu), bo setState→renderMenu() patrzy na menuScreen_ i bez
    // tego renderuje Main menu zamiast naszej listy języków.
    if (!preferences_.getBool(kPrefSetupDone, false)) {
      // Krok 1 kreatora: wybór języka. rebuildSettingsMenuItems() MUSI polecieć
      // przed setState(Menu) — renderMenu()->renderSettings() rysuje
      // settingsMenuItems_ tak jak stoi, bez własnego rebuildu.
      menuScreen_ = MenuScreen::WelcomeLanguage;
      settingsSelectedIndex_ = 0;
      rebuildSettingsMenuItems();
      // Leaving Booting here without ever calling loadPendingBootBook() —
      // clear the suppress flag set in setup() so SD status text works
      // normally again from here on.
      suppressBootStorageStatusRender_ = false;
      fadeOutSplashOnce();
      setState(AppState::Menu, nowMs);
      return;
    }

    // Setup done but tutorial not finished — resume tutorial
    tutorialCompleted_ = preferences_.getBool(kPrefTutorialDone, false);
    if (!tutorialCompleted_) {
      menuScreen_ = MenuScreen::TutorialStep1;
      suppressBootStorageStatusRender_ = false;
      fadeOutSplashOnce();
      setState(AppState::Menu, nowMs);
      return;
    }

    // If the saved typeface lives on SD and hasn't loaded yet, hold here —
    // still on the frozen splash frame, no new screen shown — instead of
    // handing off to the reader in Atkinson and popping to the real font
    // mid-read once maybeRetryTypographyFontLoad() (called every tick before
    // updateState()) succeeds. Bounded so a genuinely absent/broken card
    // can't freeze boot forever; the retry keeps going in the background
    // afterward regardless of this timeout.
    if (typographyFontRetryPending_ && nowMs - bootStartedMs_ < kBootFontWaitBudgetMs) {
      return;
    }

    // Do the deferred SD/index book load now, still under the boot splash
    // (fully lit — see fadeOutSplashOnce() above), instead of switching to
    // Paused first and showing a separate "Ładowanie książki" screen while
    // it runs — see loadPendingBootBook().
    loadPendingBootBook(nowMs);
    // loadPendingBootBook() already clears this when it actually ran a
    // load; also clear it here so the no-deferred-load path (built-in demo
    // text, pendingBootBookLoad_ was already false) doesn't leave SD status
    // text suppressed for the rest of the session.
    suppressBootStorageStatusRender_ = false;

    fadeOutSplashOnce();
    setState((touchPlayHeld_ || playLocked_ || pauseAtSentenceEndRequested_) ? AppState::Playing
                                                                              : AppState::Paused,
             nowMs);
    return;
  }

  if (state_ == AppState::UsbTransfer) {
    updateUsbTransfer(nowMs);
    return;
  }

  if (state_ == AppState::CompanionSync) {
    updateCompanionSync(nowMs);
    return;
  }

  if (state_ == AppState::Menu || state_ == AppState::Standby || state_ == AppState::Sleeping) {
    // Menu, standby, and sleeping state changes are driven by direct input and power events.
    return;
  }

  if (touchPlayHeld_ || playLocked_ || pauseAtSentenceEndRequested_) {
    setState(AppState::Playing, nowMs);
    return;
  }

  setState(AppState::Paused, nowMs);
}

void App::updateReader(uint32_t nowMs) {
  if (state_ != AppState::Playing) {
    return;
  }

  if (updateChapterTransition(nowMs)) {
    return;
  }

  if (!ensureCurrentBookWordAvailable(nowMs)) {
    return;
  }

  if (shouldFinalizeReaderPause(nowMs)) {
    finalizeReaderPause(nowMs);
    return;
  }

  const size_t previousIndex = reader_.currentIndex();
  const bool changed = reader_.update(nowMs, !pauseAtSentenceEndRequested_);
  if (!ensureCurrentBookWordAvailable(nowMs)) {
    return;
  }
  if (changed && maybeStartChapterTransition(previousIndex, reader_.currentIndex(), nowMs)) {
    return;
  }
  if (scrollModeEnabled()) {
    if (changed || nowMs - lastScrollAnimationRenderMs_ >= kScrollAnimationFrameMs) {
      renderScrollReader(nowMs);
      lastScrollAnimationRenderMs_ = nowMs;
    }
    return;
  }

  if (changed) {
    renderReaderWord();
  }
}

void App::maybeSaveReadingPosition(uint32_t nowMs) {
  if (!usingStorageBook_ || currentBookPath_.isEmpty() || state_ != AppState::Playing) {
    return;
  }

  if (nowMs - lastProgressSaveMs_ < kProgressSaveIntervalMs) {
    return;
  }

  lastProgressSaveMs_ = nowMs;
  saveReadingPosition(false);
}

bool App::handleStandbyCombo(uint32_t nowMs) {
  if (state_ == AppState::Booting || state_ == AppState::UsbTransfer ||
      state_ == AppState::CompanionSync ||
      state_ == AppState::Sleeping || powerOffStarted_ || !bootButtonReleasedSinceBoot_ ||
      !powerButtonReleasedSinceBoot_) {
    return false;
  }

  const bool bothHeld = button_.isHeld() && powerButton_.isHeld();
  if (state_ == AppState::Standby) {
    const bool pastGrace = nowMs - standbyEnteredMs_ >= kStandbyWakeGraceMs;
    if (!bothHeld && !button_.isHeld() && !powerButton_.isHeld() && pastGrace) {
      standbyButtonsReleased_ = true;
    }

    if (bothHeld) {
      if (standbyButtonsReleased_) {
        bootButtonLongPressHandled_ = true;
        powerButtonLongPressHandled_ = true;
        powerTapPending_ = false;
        exitStandby(nowMs);
      }
      return true;
    }

    if (standbyComboActive_) {
      standbyComboActive_ = false;
      standbyComboHandled_ = false;
      bootButtonLongPressHandled_ = false;
      powerButtonLongPressHandled_ = false;
      powerTapPending_ = false;
      return true;
    }

    return false;
  }

  if (bothHeld) {
    if (!standbyComboActive_) {
      standbyComboActive_ = true;
      standbyComboHandled_ = true;
      standbyComboStartedMs_ = nowMs;
      bootButtonLongPressHandled_ = true;
      powerButtonLongPressHandled_ = true;
      powerTapPending_ = false;
      enterStandby(nowMs);
    }
    return true;
  }

  if (standbyComboActive_) {
    standbyComboActive_ = false;
    standbyComboHandled_ = false;
    bootButtonLongPressHandled_ = false;
    powerButtonLongPressHandled_ = false;
    powerTapPending_ = false;
    return true;
  }

  return false;
}

void App::handleBootButton(uint32_t nowMs) {
  if (state_ == AppState::Standby) {
    if (!standbyButtonsReleased_ && !button_.isHeld() && !powerButton_.isHeld() &&
        nowMs - standbyEnteredMs_ >= kStandbyWakeGraceMs) {
      standbyButtonsReleased_ = true;
    }
    if (standbyButtonsReleased_ && button_.wasPressedEvent()) {
      bootButtonLongPressHandled_ = true;
      exitStandby(nowMs);
    }
    return;
  }

  if (state_ == AppState::Booting || state_ == AppState::UsbTransfer ||
      state_ == AppState::CompanionSync ||
      state_ == AppState::Sleeping || powerOffStarted_) {
    return;
  }

  if (showingHelpPopup_) {
    // Only dismiss on actual button press, not every frame
    if (button_.wasPressedEvent() || button_.wasReleasedEvent()) {
      dismissHelpPopup(nowMs);
      bootButtonLongPressHandled_ = true;
    }
    return;
  }

  if (!bootButtonReleasedSinceBoot_) {
    if (!button_.isHeld()) {
      bootButtonReleasedSinceBoot_ = true;
    }
    return;
  }

  if (button_.isHeld() || button_.wasPressedEvent() || button_.wasReleasedEvent()) {
    lastActivityMs_ = nowMs;
  }

  if (button_.isHeld() && !bootButtonLongPressHandled_ &&
      button_.heldDurationMs(nowMs) >= kThemeToggleHoldMs) {
    bootButtonLongPressHandled_ = true;
    cycleThemeMode(nowMs);
    return;
  }

  if (!button_.wasReleasedEvent()) {
    return;
  }

  if (bootButtonLongPressHandled_) {
    bootButtonLongPressHandled_ = false;
    return;
  }

  if (button_.lastHoldDurationMs() < kThemeToggleHoldMs) {
    // In settings menu with help enabled → show help instead of cycling brightness
    if (state_ == AppState::Menu && showHelpHints_ && !showingHelpPopup_ &&
        (menuScreen_ == MenuScreen::SettingsDisplay || menuScreen_ == MenuScreen::SettingsPacing)) {
      showHelpForCurrentItem();
      if (showingHelpPopup_) return;  // help was shown, don't cycle brightness
    }
    cycleBrightness();
  }
}

void App::handlePowerButton(uint32_t nowMs) {
  if (!powerButtonReleasedSinceBoot_) {
    if (!powerButton_.isHeld()) {
      powerButtonReleasedSinceBoot_ = true;
    }
    return;
  }

  if (state_ == AppState::Standby) {
    if (!standbyButtonsReleased_ && !button_.isHeld() && !powerButton_.isHeld() &&
        nowMs - standbyEnteredMs_ >= kStandbyWakeGraceMs) {
      standbyButtonsReleased_ = true;
    }
    if (standbyButtonsReleased_ && powerButton_.wasPressedEvent()) {
      powerButtonLongPressHandled_ = true;
      exitStandby(nowMs);
    }
    return;
  }

  if (state_ == AppState::UsbTransfer || state_ == AppState::CompanionSync || powerOffStarted_) {
    return;
  }

  if (powerButtonLongPressHandled_ && powerButton_.isHeld()) {
    return;
  }

  if (powerButton_.isHeld() || powerButton_.wasPressedEvent() || powerButton_.wasReleasedEvent()) {
    lastActivityMs_ = nowMs;
  }

  if (powerButton_.isHeld() && nowMs - powerButton_.lastEdgeMs() >= kPowerOffHoldMs) {
    powerButtonLongPressHandled_ = true;
    powerTapPending_ = false;
    enterPowerOff(nowMs);
    return;
  }

  if (!powerButton_.wasReleasedEvent()) {
    return;
  }

  if (powerButtonLongPressHandled_) {
    powerButtonLongPressHandled_ = false;
    powerTapPending_ = false;
    return;
  }

  // Single tap always acts immediately — no waiting to see if a second tap
  // follows. Double-tap-to-restart is instead detected retroactively: if a
  // tap opened the menu and a second PWR tap lands within the window while
  // still on that freshly-opened Main screen (i.e. nothing else navigated
  // in the meantime), treat it as "restart" instead of the normal in-menu
  // tap action (back / triple-tap-for-savepoint tracking).
  if (state_ == AppState::Menu && menuScreen_ == MenuScreen::Main && powerTapPending_ &&
      nowMs - powerTapPendingMs_ <= kPowerDoubleTapWindowMs) {
    powerTapPending_ = false;
    pwrTapCount_ = 0;
    restartFromPowerButtonDoubleTap();
    return;
  }

  const bool openingMenuFromReadingScreen = (state_ != AppState::Menu);
  powerTapPending_ = false;
  toggleMenuFromPowerButton(nowMs);
  if (openingMenuFromReadingScreen) {
    powerTapPending_ = true;
    powerTapPendingMs_ = nowMs;
  }
}

void App::restartFromPowerButtonDoubleTap() {
  saveReadingPosition(true);
  display_.renderStatus("", tr(TrKey::Restarting), "");
  delay(300);
  ESP.restart();
}

void App::toggleMenuFromPowerButton(uint32_t nowMs) {
  if (state_ == AppState::Booting || state_ == AppState::UsbTransfer ||
      state_ == AppState::CompanionSync || state_ == AppState::Standby ||
      state_ == AppState::Sleeping) {
    return;
  }

  if (state_ == AppState::Menu) {
    if (menuScreen_ == MenuScreen::Main) {
      // Check for triple-tap save point: if we just opened the menu via PWR
      // and get another PWR tap quickly, count towards triple-tap.
      constexpr uint32_t kTripleTapWindowMs = 900;
      if (pwrTapCount_ > 0 && nowMs - pwrFirstTapMs_ <= kTripleTapWindowMs) {
        pwrTapCount_++;
        if (pwrTapCount_ >= 3) {
          pwrTapCount_ = 0;
          createSavePoint(nowMs);
          display_.renderStatus(uiText(UiText::SavePoints),
                                tr3(TrKey3::SavePointAdded), "");
          delay(1500);
          setState(AppState::Paused, nowMs);
          return;
        }
        // 2nd tap — go back to reading, wait for 3rd
        setState(AppState::Paused, nowMs);
        return;
      }
      // Normal single tap on Main menu = go back to reading
      pwrTapCount_ = 0;
      setState(AppState::Paused, nowMs);
    } else {
      if (menuScreen_ == MenuScreen::WelcomeLanguage) {
        // Zupełnie pierwszy ekran kreatora po flashu/resecie — przycisk
        // power/select ma tu nic nie robić (poza realnym wyłączeniem, które
        // jest obsłużone wcześniej przez przytrzymanie), żeby jednym
        // przypadkowym kliknięciem nie dało się wyskoczyć z konfiguracji
        // początkowej prosto do czytania.
        return;
      }
      if (menuScreen_ == MenuScreen::WelcomeConnect ||
          menuScreen_ == MenuScreen::WelcomeAppPairing ||
          menuScreen_ == MenuScreen::WelcomeConfigureInApp ||
          menuScreen_ == MenuScreen::WelcomeTheme ||
          menuScreen_ == MenuScreen::WelcomeHighlightColor ||
          menuScreen_ == MenuScreen::WelcomeLoading ||
          menuScreen_ == MenuScreen::WelcomeSuper ||
          menuScreen_ == MenuScreen::WelcomeConfigureIntro ||
          menuScreen_ == MenuScreen::WelcomeReadingMode ||
          menuScreen_ == MenuScreen::WelcomeReadingModePreview ||
          (menuScreen_ == MenuScreen::TypographyFontPicker && wizardFontPickerActive_) ||
          (menuScreen_ == MenuScreen::BookPicker && wizardBookPickerActive_)) {
        // Kreatora pierwszego uruchomienia nie da się już pominąć jednym
        // kliknięciem PWR — krótkie kliknięcie cofa o krok, tak jak Back
        // gdziekolwiek indziej w aplikacji.
        wizardStepBack(nowMs);
        return;
      }
      if (menuScreen_ == MenuScreen::TutorialStep1 ||
          menuScreen_ == MenuScreen::TutorialStep2 ||
          menuScreen_ == MenuScreen::TutorialStep3 ||
          menuScreen_ == MenuScreen::TutorialStep4 ||
          menuScreen_ == MenuScreen::TutorialStep5) {
        finishTutorial(nowMs);
        return;
      }
      menuScreen_ = MenuScreen::Main;
      renderMainMenu();
    }
    return;
  }

  // Opening menu from Paused/Playing — start triple-tap tracking
  pwrTapCount_ = 1;
  pwrFirstTapMs_ = nowMs;
  openMainMenu(nowMs);
}

void App::openMainMenu(uint32_t nowMs) {
  pausedTouch_.active = false;
  pausedTouchIntent_ = TouchIntent::None;
  touchPlayHeld_ = false;
  menuScreen_ = MenuScreen::Main;
  menuSelectedIndex_ = MenuRead;
  wpmFeedbackVisible_ = false;
  contextViewVisible_ = false;
  if (state_ == AppState::Playing) {
    saveReadingPosition(true);
  }
  setState(AppState::Menu, nowMs);
}

uint8_t App::currentBrightnessPercent() const {
  return nightMode_ ? kNightBrightnessLevels[brightnessLevelIndex_]
                    : kBrightnessLevels[brightnessLevelIndex_];
}

void App::applyDisplayPreferences(uint32_t nowMs, bool rerender) {
  display_.setDarkMode(darkMode_);
  display_.setNightMode(nightMode_);
  display_.setBrightnessPercent(currentBrightnessPercent());
  display_.setScrollFontSize(scrollFontSize_);
  display_.setScrollLineSpacing(scrollLineSpacing_);
  display_.setScrollMargin(scrollMargin_);

  if (!rerender) {
    return;
  }

  if (state_ == AppState::Menu) {
    if (isSettingsListScreen()) {
      rebuildSettingsMenuItems();
      if (settingsSelectedIndex_ < settingsMenuItems_.size()) {
        showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
      }
      renderSettings();
      return;
    }
    renderMenu();
    return;
  }

  if (state_ == AppState::Paused || state_ == AppState::Playing) {
    renderActiveReader(nowMs);
    return;
  }

  if (state_ == AppState::Booting) {
    display_.renderCenteredWord("READY");
  }
}

void App::applyHandednessSettings(uint32_t nowMs, bool rerender) {
  (void)nowMs;
  applyReaderUiOrientation();

  if (!rerender) {
    return;
  }

  if (state_ == AppState::Menu && isSettingsListScreen()) {
    rebuildSettingsMenuItems();
    if (settingsSelectedIndex_ < settingsMenuItems_.size()) {
      showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
    }
  }

  applyTypographySettings(nowMs);
}

void App::reloadRuntimePreferences(uint32_t nowMs, bool rerender) {
  brightnessLevelIndex_ = preferences_.getUChar(kPrefBrightness, brightnessLevelIndex_);
  if (brightnessLevelIndex_ >= kBrightnessLevelCount) {
    brightnessLevelIndex_ = kBrightnessLevelCount - 1;
  }
  phantomWordsEnabled_ = preferences_.getBool(kPrefPhantomWords, phantomWordsEnabled_);
  readerBatteryVisibleWhilePlaying_ =
      preferences_.getBool(kPrefReaderBatteryVisible, readerBatteryVisibleWhilePlaying_);
  readerChapterVisibleWhilePlaying_ =
      preferences_.getBool(kPrefReaderChapterVisible, readerChapterVisibleWhilePlaying_);
  readerProgressVisibleWhilePlaying_ =
      preferences_.getBool(kPrefReaderProgressVisible, readerProgressVisibleWhilePlaying_);
  savePointButtonVisible_ =
      preferences_.getBool(kPrefSavePointButtonVisible, savePointButtonVisible_);
  savePointUseCustomName_ =
      preferences_.getBool(kPrefSavePointCustomName, savePointUseCustomName_);
  showHelpHints_ = preferences_.getBool(kPrefShowHelpHints, showHelpHints_);
  {
    navMode_ = navModeFromSetting(
        preferences_.getUChar(kPrefNavMode, static_cast<uint8_t>(navMode_)));
  }
  uiLanguage_ =
      Localization::sanitizeLanguage(preferences_.getUChar(
          kPrefUiLanguage, static_cast<uint8_t>(uiLanguage_)));
  DeviceServicesBridge::setLanguageIndex(static_cast<int>(uiLanguage_));
  readerMode_ = readerModeFromSetting(
      preferences_.getUChar(kPrefReaderMode, static_cast<uint8_t>(readerMode_)));
  handednessMode_ = handednessModeFromSetting(
      preferences_.getUChar(kPrefHandedness, static_cast<uint8_t>(handednessMode_)));
  readerFontSizeIndex_ = preferences_.getUChar(kPrefReaderFontSize, readerFontSizeIndex_);
  if (readerFontSizeIndex_ >= kReaderFontSizeCount) {
    readerFontSizeIndex_ = 0;
  }
  scrollFontSize_ = preferences_.getUChar(kPrefScrollFontSize, scrollFontSize_);
  if (scrollFontSize_ > 8) scrollFontSize_ = 4;
  scrollLineSpacing_ = preferences_.getUChar(kPrefScrollLineSpacing, scrollLineSpacing_);
  if (scrollLineSpacing_ > 2) scrollLineSpacing_ = 1;
  scrollMargin_ = preferences_.getUChar(kPrefScrollMargin, scrollMargin_);
  if (scrollMargin_ > 2) scrollMargin_ = 1;

  switch (preferences_.getUChar(kPrefFooterMetricMode,
                                static_cast<uint8_t>(footerMetricMode_))) {
    case static_cast<uint8_t>(FooterMetricMode::ChapterTime):
      footerMetricMode_ = FooterMetricMode::ChapterTime;
      break;
    case static_cast<uint8_t>(FooterMetricMode::BookTime):
      footerMetricMode_ = FooterMetricMode::BookTime;
      break;
    case static_cast<uint8_t>(FooterMetricMode::Percentage):
    default:
      footerMetricMode_ = FooterMetricMode::Percentage;
      break;
  }

  switch (preferences_.getUChar(kPrefBatteryLabelMode,
                                static_cast<uint8_t>(batteryLabelMode_))) {
    case static_cast<uint8_t>(BatteryLabelMode::TimeRemaining):
      batteryLabelMode_ = BatteryLabelMode::TimeRemaining;
      break;
    case static_cast<uint8_t>(BatteryLabelMode::Voltage):
      batteryLabelMode_ = BatteryLabelMode::Voltage;
      break;
    case static_cast<uint8_t>(BatteryLabelMode::Percent):
    default:
      batteryLabelMode_ = BatteryLabelMode::Percent;
      break;
  }

  switch (preferences_.getUChar(kPrefScreensaverMode, static_cast<uint8_t>(screensaverMode_))) {
    case static_cast<uint8_t>(ScreensaverMode::Maze):
      screensaverMode_ = ScreensaverMode::Maze;
      break;
    case static_cast<uint8_t>(ScreensaverMode::Voronoi):
      screensaverMode_ = ScreensaverMode::Voronoi;
      break;
    case static_cast<uint8_t>(ScreensaverMode::Stars):
      screensaverMode_ = ScreensaverMode::Stars;
      break;
    case static_cast<uint8_t>(ScreensaverMode::Matrix):
      screensaverMode_ = ScreensaverMode::Matrix;
      break;
    case static_cast<uint8_t>(ScreensaverMode::ScreenOff):
      screensaverMode_ = ScreensaverMode::ScreenOff;
      break;
    case static_cast<uint8_t>(ScreensaverMode::Life):
    default:
      screensaverMode_ = ScreensaverMode::Life;
      break;
  }
  screensaverTimeoutIndex_ = preferences_.getUChar(kPrefScreensaverTimeout, screensaverTimeoutIndex_);
  if (screensaverTimeoutIndex_ >= kScreensaverTimeoutCount) {
    screensaverTimeoutIndex_ = 2;
  }
  screensaverAutoOffIndex_ = preferences_.getUChar(kPrefScreensaverAutoOff, screensaverAutoOffIndex_);
  if (screensaverAutoOffIndex_ >= kScreensaverAutoOffCount) {
    screensaverAutoOffIndex_ = 0;
  }
  screensaverSleepGuardIndex_ = preferences_.getUChar(kPrefScreensaverSleepGuard, screensaverSleepGuardIndex_);
  if (screensaverSleepGuardIndex_ >= kScreensaverSleepGuardCount) {
    screensaverSleepGuardIndex_ = 0;
  }

  switch (preferences_.getUChar(kPrefPauseMode, static_cast<uint8_t>(pauseMode_))) {
    case static_cast<uint8_t>(PauseMode::Instant):
      pauseMode_ = PauseMode::Instant;
      break;
    case static_cast<uint8_t>(PauseMode::SentenceEnd):
    default:
      pauseMode_ = PauseMode::SentenceEnd;
      break;
  }

  pacingLongWordDelayMs_ =
      loadPacingDelayMs(preferences_, kPrefPacingLongMs, kPrefLegacyPacingLong);
  pacingComplexWordDelayMs_ =
      loadPacingDelayMs(preferences_, kPrefPacingComplexMs, kPrefLegacyPacingComplex);
  pacingPunctuationDelayMs_ =
      loadPacingDelayMs(preferences_, kPrefPacingPunctuationMs, kPrefLegacyPacingPunctuation);
  accurateTimeEstimateEnabled_ = true;

  loadTypographyConfigFromPreferences();
  darkMode_ = preferences_.getBool(kPrefDarkMode, darkMode_);
  nightMode_ = preferences_.getBool(kPrefNightMode, nightMode_);
  display_.setFocusColorIndex(preferences_.getUChar(kPrefFocusColorIndex, 1));

  reader_.setWpm(preferences_.getUShort(kPrefWpm, reader_.wpm()));
  applyReaderUiOrientation();
  applyDisplayPreferences(nowMs, false);
  applyTypographySettings(nowMs, false);
  applyPacingSettings();
  if (rerender) {
    renderActiveReader(nowMs);
  }
}

void App::loadTypographyConfigFromPreferences() {
  typographyConfig_ = defaultTypographyConfig();
  typographyConfig_.typeface = readerTypefaceFromSetting(
      preferences_.getUChar(kPrefReaderTypeface, static_cast<uint8_t>(typographyConfig_.typeface)));
  typographyConfig_.focusHighlight =
      preferences_.getBool(kPrefTypographyFocusHighlight, typographyConfig_.focusHighlight);
  typographyConfig_.trackingPx = static_cast<int8_t>(clampIntSetting(
      preferences_.getChar(kPrefTypographyTracking, typographyConfig_.trackingPx),
      kTypographyTrackingMin, kTypographyTrackingMax));
  typographyConfig_.anchorPercent = static_cast<uint8_t>(clampIntSetting(
      preferences_.getUChar(kPrefTypographyAnchor, typographyConfig_.anchorPercent),
      kTypographyAnchorMin, kTypographyAnchorMax));
  typographyConfig_.guideHalfWidth = static_cast<uint8_t>(clampIntSetting(
      preferences_.getUChar(kPrefTypographyGuideWidth, typographyConfig_.guideHalfWidth),
      kTypographyGuideWidthMin, kTypographyGuideWidthMax));
  typographyConfig_.guideGap = static_cast<uint8_t>(clampIntSetting(
      preferences_.getUChar(kPrefTypographyGuideGap, typographyConfig_.guideGap),
      kTypographyGuideGapMin, kTypographyGuideGapMax));
}

void App::applyTypographySettings(uint32_t nowMs, bool rerender) {
  display_.setTypographyConfig(effectiveTypographyConfig());

  Serial.printf("[typography] face=%s highlight=%s track=%d anchor=%u guideWidth=%u guideGap=%u\n",
                readerTypefaceLabel().c_str(),
                focusHighlightLabel().c_str(),
                static_cast<int>(typographyConfig_.trackingPx),
                static_cast<unsigned int>(effectiveAnchorPercent()),
                static_cast<unsigned int>(typographyConfig_.guideHalfWidth),
                static_cast<unsigned int>(typographyConfig_.guideGap));

  if (!rerender) {
    return;
  }

  if (state_ == AppState::Menu) {
    renderMenu();
    return;
  }

  if (state_ == AppState::Paused || state_ == AppState::Playing) {
    renderActiveReader(nowMs);
  }
}

void App::maybeRetryTypographyFontLoad(uint32_t nowMs) {
  if (!typographyFontRetryPending_) {
    return;
  }
  if (display_.isActiveTypefaceLoaded()) {
    typographyFontRetryPending_ = false;
    return;
  }
  if (nowMs >= typographyFontRetryDeadlineMs_) {
    // Give up after ~20s — either the card is genuinely absent/unreadable or
    // the font pack hasn't downloaded yet (maybeAutoDownloadFonts() keeps
    // trying that independently). Stay on the Atkinson fallback rather than
    // retrying an SD read every tick for the rest of the session.
    typographyFontRetryPending_ = false;
    return;
  }
  if (nowMs - typographyFontRetryLastAttemptMs_ < 750) {
    return;
  }
  typographyFontRetryLastAttemptMs_ = nowMs;
  if (!storageReady_) {
    storageReady_ = storage_.begin();
  }
  if (storageReady_) {
    applyTypographySettings(nowMs, true);
  }
}

void App::cycleBrightness() {
  brightnessLevelIndex_ = static_cast<uint8_t>((brightnessLevelIndex_ + 1) % kBrightnessLevelCount);
  preferences_.putUChar(kPrefBrightness, brightnessLevelIndex_);
  const uint8_t percent = currentBrightnessPercent();
  Serial.printf("[display] brightness level %u/%u (%u%%)\n",
                static_cast<unsigned int>(brightnessLevelIndex_ + 1),
                static_cast<unsigned int>(kBrightnessLevelCount),
                static_cast<unsigned int>(percent));
  applyDisplayPreferences(millis());
}

void App::cycleThemeMode(uint32_t nowMs) {
  if (darkMode_ && nightMode_) {
    // night -> light
    darkMode_ = false;
    nightMode_ = false;
  } else if (darkMode_) {
    // dark -> night
    nightMode_ = true;
  } else {
    // light -> dark
    darkMode_ = true;
  }

  preferences_.putBool(kPrefDarkMode, darkMode_);
  preferences_.putBool(kPrefNightMode, nightMode_);
  Serial.printf("[display] theme=%s\n", themeModeLabel().c_str());

  // Ekran WelcomeTheme w kreatorze wybiera motyw z listy (Light/Dark/Night)
  // po zaznaczonym wierszu, nie po darkMode_/nightMode_ — bez tej
  // synchronizacji długi przytrzymanie fizycznego przycisku zmieniało kolory
  // na ekranie, ale kolejny tap (selectWelcomeThemeItem) i tak zapisywał
  // stary, wcześniej podświetlony wiersz, cofając zmianę bez ostrzeżenia.
  if (menuScreen_ == MenuScreen::WelcomeTheme) {
    settingsSelectedIndex_ = nightMode_ ? 2 : (darkMode_ ? 1 : 0);
  }

  applyDisplayPreferences(nowMs);
}

void App::cycleUiLanguage(uint32_t nowMs) {
  uiLanguage_ = Localization::nextLanguage(uiLanguage_);
  preferences_.putUChar(kPrefUiLanguage, static_cast<uint8_t>(uiLanguage_));
  DeviceServicesBridge::setLanguageIndex(static_cast<int>(uiLanguage_));
  Serial.printf("[display] language=%s\n", uiLanguageLabel().c_str());

  if (state_ == AppState::Menu) {
    if (isSettingsListScreen()) {
      rebuildSettingsMenuItems();
      if (settingsSelectedIndex_ < settingsMenuItems_.size()) {
        showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
      }
      renderSettings();
      return;
    }
    renderMenu();
    return;
  }

  if (state_ == AppState::Paused || state_ == AppState::Playing) {
    renderActiveReader(nowMs);
  }
}

void App::cycleReaderMode(uint32_t nowMs) {
  readerMode_ = nextReaderMode(readerMode_);
  preferences_.putUChar(kPrefReaderMode, static_cast<uint8_t>(readerMode_));
  Serial.printf("[display] reader mode=%s\n", readerModeLabel().c_str());
  invalidateContextPreviewWindow();

  if (state_ == AppState::Menu) {
    rebuildSettingsMenuItems();
    showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
    renderSettings();
    return;
  }

  renderActiveReader(nowMs);
}

void App::cycleHandednessMode(uint32_t nowMs) {
  handednessMode_ = nextHandednessMode(handednessMode_);
  preferences_.putUChar(kPrefHandedness, static_cast<uint8_t>(handednessMode_));
  Serial.printf("[display] handedness=%s rotation180=%u\n", handednessLabel().c_str(),
                uiRotated180() ? 1U : 0U);
  applyHandednessSettings(nowMs);
}

void App::togglePhantomWords(uint32_t nowMs) {
  phantomWordsEnabled_ = !phantomWordsEnabled_;
  preferences_.putBool(kPrefPhantomWords, phantomWordsEnabled_);
  Serial.printf("[display] phantom words=%s\n", phantomWordsLabel().c_str());
  applyDisplayPreferences(nowMs);
}

bool App::updateBatteryStatus(uint32_t nowMs, bool force) {
  if (!force) {
    const bool lowBatteryKnown =
        batteryPresent_ && batterySampleInitialized_ &&
        (batteryFilteredVoltage_ <= kBatteryLowWarningVoltage ||
         batteryDisplayedPercent_ <= kBatteryLowWarningPercent);
    uint32_t sampleIntervalMs =
        lowBatteryKnown ? kBatteryLowSampleIntervalMs : kBatterySampleIntervalMs;
    if (state_ == AppState::Playing && !lowBatteryKnown) {
      sampleIntervalMs = kBatteryPlayingSampleIntervalMs;
    }
    if (nowMs - lastBatterySampleMs_ < sampleIntervalMs) {
      return false;
    }
  }

  lastBatterySampleMs_ = nowMs;

  BoardConfig::BatteryStatus status;
  if (BoardConfig::readBatteryStatus(status)) {
    batteryPresent_ = true;
    if (!batterySampleInitialized_) {
      batteryFilteredVoltage_ = status.voltage;
      batteryFilteredPercent_ = status.percent;
      batteryDisplayedPercent_ = status.percent;
      batteryRuntimeAnchorPercent_ = status.percent;
      batteryRuntimeAnchorMs_ = nowMs;
      batterySampleInitialized_ = true;
    } else {
      batteryFilteredVoltage_ = (batteryFilteredVoltage_ * 0.72f) + (status.voltage * 0.28f);
      batteryFilteredPercent_ = (batteryFilteredPercent_ * 0.72f) + (status.percent * 0.28f);

      const int filteredPercent =
          std::max(0, std::min(100, static_cast<int>(batteryFilteredPercent_ + 0.5f)));
      const int delta = filteredPercent - static_cast<int>(batteryDisplayedPercent_);
      if (force || abs(delta) >= kBatteryDisplayHysteresisPercent ||
          filteredPercent <= 10 || filteredPercent >= 99) {
        batteryDisplayedPercent_ = static_cast<uint8_t>(filteredPercent);
      }

      if (batteryDisplayedPercent_ > batteryRuntimeAnchorPercent_) {
        batteryRuntimeAnchorPercent_ = batteryDisplayedPercent_;
        batteryRuntimeAnchorMs_ = nowMs;
        batteryRuntimeEstimateReady_ = false;
      } else {
        const uint8_t percentDrop = batteryRuntimeAnchorPercent_ - batteryDisplayedPercent_;
        const uint32_t elapsedMs = nowMs - batteryRuntimeAnchorMs_;
        if (percentDrop >= kBatteryRuntimeMinDropPercent &&
            elapsedMs >= kBatteryRuntimeMinElapsedMs) {
          const float minutesPerPercent =
              (static_cast<float>(elapsedMs) / 60000.0f) / static_cast<float>(percentDrop);
          batteryRuntimeMinutesRemaining_ =
              static_cast<uint32_t>(batteryDisplayedPercent_ * minutesPerPercent + 0.5f);
          batteryRuntimeEstimateReady_ = true;
        }
      }
    }
  } else {
    batteryPresent_ = false;
    batteryCriticalSampleCount_ = 0;
  }

  handleBatteryProtection(nowMs);
  if (powerOffStarted_) {
    return false;
  }

  const String nextLabel = currentBatteryLabel();
  if (nextLabel == batteryLabel_) {
    return false;
  }

  batteryLabel_ = nextLabel;
  display_.setBatteryLabel(batteryLabel_);
  if (!batteryLabel_.isEmpty()) {
    Serial.printf("[power] battery %.2f V raw=%u%% shown=%u%% label=%s\n", status.voltage,
                  static_cast<unsigned int>(status.percent),
                  static_cast<unsigned int>(batteryDisplayedPercent_), batteryLabel_.c_str());
  } else {
    Serial.println("[power] battery not detected");
  }
  return true;
}

void App::handleBatteryProtection(uint32_t nowMs) {
  if (!batteryPresent_ || !batterySampleInitialized_) {
    batteryCriticalSampleCount_ = 0;
    return;
  }

  const bool critical = batteryFilteredVoltage_ <= kBatteryCriticalVoltage ||
                        batteryDisplayedPercent_ <= kBatteryCriticalPercent;
  if (critical) {
    if (batteryCriticalSampleCount_ < 255) {
      ++batteryCriticalSampleCount_;
    }
  } else {
    batteryCriticalSampleCount_ = 0;
  }

  if (batteryCriticalSampleCount_ >= kBatteryCriticalConsecutiveSamples) {
    const String line2 =
        batteryVoltageLabel() + " " + String(static_cast<unsigned int>(batteryDisplayedPercent_)) +
        "%";
    Serial.printf("[power] critical battery %.2f V %u%%; powering off\n",
                  static_cast<double>(batteryFilteredVoltage_),
                  static_cast<unsigned int>(batteryDisplayedPercent_));
    display_.renderStatus(tr(TrKey::LowBattery),
                          tr(TrKey::PoweringOff), line2);
    delay(kBatteryShutdownNoticeMs);
    enterPowerOff(millis());
    return;
  }

  const bool low = batteryFilteredVoltage_ <= kBatteryLowWarningVoltage ||
                   batteryDisplayedPercent_ <= kBatteryLowWarningPercent;
  if (!low) {
    return;
  }

  if (lastLowBatteryWarningMs_ == 0 ||
      nowMs - lastLowBatteryWarningMs_ >= kBatteryLowWarningRepeatMs) {
    showLowBatteryWarning(nowMs);
  }
}

void App::showLowBatteryWarning(uint32_t nowMs) {
  lastLowBatteryWarningMs_ = nowMs;
  batteryWarningOverlayVisible_ = true;
  batteryWarningRestoreAtMs_ = nowMs + kBatteryWarningVisibleMs;
  touchPlayHeld_ = false;
  playLocked_ = false;
  pauseAtSentenceEndRequested_ = false;
  wpmFeedbackVisible_ = false;
  pausedTouch_.active = false;
  pausedTouchIntent_ = TouchIntent::None;

  if (state_ == AppState::Playing) {
    setState(AppState::Paused, nowMs);
  }

  const String line1 =
      String(static_cast<unsigned int>(batteryDisplayedPercent_)) +
      tr(TrKey::Remaining);
  display_.renderStatus(tr(TrKey::LowBattery), line1,
                        batteryVoltageLabel() +
                            tr(TrKey::ChargeSoon));
  Serial.printf("[power] low battery warning %.2f V %u%%\n",
                static_cast<double>(batteryFilteredVoltage_),
                static_cast<unsigned int>(batteryDisplayedPercent_));
}

void App::updateBatteryWarningOverlay(uint32_t nowMs) {
  if (!batteryWarningOverlayVisible_ || nowMs < batteryWarningRestoreAtMs_) {
    return;
  }

  batteryWarningOverlayVisible_ = false;
  if (state_ == AppState::Paused || state_ == AppState::Playing) {
    renderActiveReader(nowMs);
  } else if (state_ == AppState::Menu) {
    renderMenu();
  } else if (state_ == AppState::Standby) {
    updateStandbyScreensaver(nowMs, true);
  }
}

void App::showGridToast(const String &text, uint32_t nowMs) {
  pendingToastText_ = text;
  toastVisibleUntilMs_ = nowMs + kGridToastVisibleMs;
}

void App::updateGridToastOverlay(uint32_t nowMs) {
  if (pendingToastText_.isEmpty()) {
    return;
  }
  if (nowMs < toastVisibleUntilMs_) {
    return;
  }
  pendingToastText_ = "";
  if (state_ == AppState::Menu) {
    renderMenu();
  }
}

String App::activeGridToastText(uint32_t nowMs) const {
  if (pendingToastText_.isEmpty() || nowMs >= toastVisibleUntilMs_) {
    return "";
  }
  return pendingToastText_;
}

void App::updateWpmFeedback(uint32_t nowMs) {
  if (!wpmFeedbackVisible_ || state_ != AppState::Paused) {
    return;
  }

  if (nowMs < wpmFeedbackUntilMs_) {
    return;
  }

  wpmFeedbackVisible_ = false;
  renderActiveReader(nowMs);
}

void App::resetReaderTapTracking() { lastReaderTapValid_ = false; }

// A finger resting through a blocking delay() (renderStatus + delay after a
// destructive action) can produce a TouchPhase::End with coordinates from
// before the screen was rebuilt, which then gets hit-tested against the new,
// differently-laid-out screen (e.g. deleting the last save point shifts
// "+ Dodaj punkt zapisu" onto the old delete button's pixels, silently
// opening the name-entry keyboard). Call this right after such a delay(),
// before rendering the next screen, so any stale touch is dropped and the
// user must start a fresh touch-down before a tap can register.
void App::flushStaleTouch() {
  touch_.cancel();
  pausedTouch_.active = false;
  pausedTouchIntent_ = TouchIntent::None;
}

bool App::isFooterMetricTap(uint16_t x, uint16_t y) const {
  return x >= BoardConfig::DISPLAY_WIDTH - kFooterMetricTapWidthPx &&
         y >= BoardConfig::DISPLAY_HEIGHT - kFooterMetricTapHeightPx;
}

bool App::isBatteryBadgeTap(uint16_t x, uint16_t y) const {
  return x >= BoardConfig::DISPLAY_WIDTH - kBatteryBadgeTapWidthPx &&
         y <= kBatteryBadgeTapHeightPx;
}

bool App::isPreviousSentenceTap(uint16_t x, uint16_t y) const {
  // Left portion of top area only (not overlapping with SP button)
  return x < 38 && y < 30;
}

bool App::isSavePointButtonTap(uint16_t x, uint16_t y) const {
  // Right of "<<", top area: matches the save-point floppy icon at (40, 4).
  return savePointButtonVisible_ && x >= 38 && x < 120 && y < 30;
}

bool App::isActivelyReading() const { return state_ == AppState::Playing; }

DisplayManager::ReaderChrome App::readerChrome() const {
  DisplayManager::ReaderChrome chrome;
  const bool reading = isActivelyReading();
  // Unlike chapter/progress (only ever hidden during active flashing, always
  // shown again once paused), battery visibility follows the toggle in both
  // Playing and Paused — otherwise the setting only ever takes effect during
  // the brief moments words are actively flashing, and looks broken/no-op
  // the rest of the time since the reader is paused far more often than not.
  chrome.showBattery = readerBatteryVisibleWhilePlaying_;
  chrome.showChapter = !reading || readerChapterVisibleWhilePlaying_;
  chrome.showProgress = !reading || readerProgressVisibleWhilePlaying_;
  chrome.showPreviousSentenceHint = !contextViewVisible_ || scrollModeEnabled();
  chrome.showSavePointButton = savePointButtonVisible_;
  chrome.savePointAtCurrentPosition = isCurrentPositionSaved();
  return chrome;
}

bool App::isCurrentPositionSaved() const {
  if (currentBookPath_.isEmpty()) {
    return false;
  }
  const size_t currentWord = reader_.currentIndex();
  for (const auto &sp : savePoints_) {
    if (sp.bookPath == currentBookPath_ && sp.wordIndex == currentWord) {
      return true;
    }
  }
  return false;
}

bool App::readerFooterVisible() const {
  const DisplayManager::ReaderChrome chrome = readerChrome();
  return chrome.showChapter || chrome.showProgress;
}

String App::readerFooterStatusLabel() const {
  // Used to hardcode "<percent>%" whenever actively playing, ignoring
  // footerMetricMode_ entirely — cycling the footer to chapter/book time
  // in Settings had no effect the moment you pressed play, only while
  // paused. currentFooterMetricLabel() already computes a live value for
  // every mode (including Percentage), so it works for both states.
  return currentFooterMetricLabel();
}

String App::onOffLabel(bool enabled) const { return enabled ? uiText(UiText::On) : uiText(UiText::Off); }

bool App::handlePreviousSentenceTap(uint16_t x, uint16_t y, uint32_t nowMs) {
  if (!isPreviousSentenceTap(x, y)) {
    return false;
  }

  resetReaderTapTracking();
  pausedTouch_.active = false;
  pausedTouchIntent_ = TouchIntent::None;
  wpmFeedbackVisible_ = false;
  contextViewVisible_ = false;
  reader_.rewindSentence();

  if (state_ == AppState::Playing) {
    setState(AppState::Paused, nowMs);
  } else {
    renderActiveReader(nowMs);
    saveReadingPosition(true);
  }

  Serial.printf("[app] sentence rewind index=%u word=%s\n",
                static_cast<unsigned int>(reader_.currentIndex()), reader_.currentWord().c_str());
  return true;
}

bool App::handleFooterMetricTap(uint16_t x, uint16_t y, uint32_t nowMs) {
  if (isActivelyReading() || !readerFooterVisible() || !isFooterMetricTap(x, y)) {
    return false;
  }

  switch (footerMetricMode_) {
    case FooterMetricMode::Percentage:
      footerMetricMode_ = FooterMetricMode::ChapterTime;
      break;
    case FooterMetricMode::ChapterTime:
      footerMetricMode_ = FooterMetricMode::BookTime;
      break;
    case FooterMetricMode::BookTime:
    default:
      footerMetricMode_ = FooterMetricMode::Percentage;
      break;
  }

  preferences_.putUChar(kPrefFooterMetricMode, static_cast<uint8_t>(footerMetricMode_));
  resetReaderTapTracking();
  renderActiveReader(nowMs);
  const char *modeName = "percent";
  switch (footerMetricMode_) {
    case FooterMetricMode::ChapterTime:
      modeName = "chapter";
      break;
    case FooterMetricMode::BookTime:
      modeName = "book";
      break;
    case FooterMetricMode::Percentage:
    default:
      modeName = "percent";
      break;
  }
  Serial.printf("[reader] footer metric=%s\n", modeName);
  return true;
}

bool App::handleBatteryBadgeTap(uint16_t x, uint16_t y, uint32_t nowMs) {
  if (batteryLabel_.isEmpty() || !readerChrome().showBattery || !isBatteryBadgeTap(x, y)) {
    return false;
  }

  switch (batteryLabelMode_) {
    case BatteryLabelMode::Percent:
      batteryLabelMode_ = BatteryLabelMode::TimeRemaining;
      break;
    case BatteryLabelMode::TimeRemaining:
      batteryLabelMode_ = BatteryLabelMode::Voltage;
      break;
    case BatteryLabelMode::Voltage:
    default:
      batteryLabelMode_ = BatteryLabelMode::Percent;
      break;
  }
  preferences_.putUChar(kPrefBatteryLabelMode, static_cast<uint8_t>(batteryLabelMode_));
  batteryLabel_ = currentBatteryLabel();
  display_.setBatteryLabel(batteryLabel_);
  resetReaderTapTracking();
  renderActiveReader(nowMs);
  const char *modeName = "percent";
  if (batteryLabelMode_ == BatteryLabelMode::TimeRemaining) {
    modeName = "time";
  } else if (batteryLabelMode_ == BatteryLabelMode::Voltage) {
    modeName = "voltage";
  }
  Serial.printf("[power] battery label mode=%s label=%s\n", modeName, batteryLabel_.c_str());
  return true;
}

void App::handleReaderTap(uint16_t x, uint16_t y, uint32_t nowMs) {
  const bool recentTap =
      lastReaderTapValid_ && nowMs - lastReaderTapMs_ <= kReaderDoubleTapWindowMs;
  const bool sameRegion =
      recentTap &&
      abs(static_cast<int>(x) - static_cast<int>(lastReaderTapX_)) <=
          static_cast<int>(kReaderDoubleTapSlopPx) &&
      abs(static_cast<int>(y) - static_cast<int>(lastReaderTapY_)) <=
          static_cast<int>(kReaderDoubleTapSlopPx);

  if (sameRegion) {
    resetReaderTapTracking();

    if (state_ == AppState::Playing) {
      requestReaderPauseAtSentenceEnd(nowMs);
    } else if (state_ == AppState::Paused) {
      playLocked_ = true;
      pauseAtSentenceEndRequested_ = false;
      wpmFeedbackVisible_ = false;
      setState(AppState::Playing, nowMs);
    }
    Serial.printf("[touch] reader double tap state=%s\n", stateName(state_));
    return;
  }

  if (recentTap) {
    Serial.printf("[touch] double tap miss dx=%d dy=%d dt=%lu\n",
                  static_cast<int>(x) - static_cast<int>(lastReaderTapX_),
                  static_cast<int>(y) - static_cast<int>(lastReaderTapY_),
                  static_cast<unsigned long>(nowMs - lastReaderTapMs_));
  }

  lastReaderTapValid_ = true;
  lastReaderTapMs_ = nowMs;
  lastReaderTapX_ = x;
  lastReaderTapY_ = y;
}

void App::requestReaderPauseAtSentenceEnd(uint32_t nowMs) {
  if (state_ != AppState::Playing) {
    return;
  }

  playLocked_ = false;
  touchPlayHeld_ = false;
  touchPlayPendingRelease_ = false;
  if (pauseMode_ == PauseMode::Instant) {
    pauseAtSentenceEndRequested_ = false;
    setState(AppState::Paused, nowMs);
    return;
  }

  if (!pauseAtSentenceEndRequested_) {
    pauseAtSentenceEndRequested_ = true;
    Serial.println("[app] pause requested at sentence end");
  }

  if (shouldFinalizeReaderPause(nowMs)) {
    finalizeReaderPause(nowMs);
  }
}

bool App::shouldFinalizeReaderPause(uint32_t nowMs) const {
  if (state_ != AppState::Playing || !pauseAtSentenceEndRequested_) {
    return false;
  }

  const uint32_t durationMs = reader_.currentWordDurationMs();
  if (durationMs == 0 || reader_.elapsedInCurrentWordMs(nowMs) < durationMs) {
    return false;
  }

  return reader_.currentWordEndsSentence() || reader_.atEnd();
}

void App::finalizeReaderPause(uint32_t nowMs) {
  pauseAtSentenceEndRequested_ = false;
  playLocked_ = false;
  touchPlayHeld_ = false;
  setState(AppState::Paused, nowMs);
}

void App::maybeFinalizeTouchPlayRelease(uint32_t nowMs) {
  if (!touchPlayPendingRelease_) {
    return;
  }

  if (nowMs - touchPlayPendingReleaseAtMs_ < kTouchPlayReleaseGraceMs) {
    return;
  }

  touchPlayPendingRelease_ = false;
  touchPlayHeld_ = false;
  saveReadingPosition(true);
  requestReaderPauseAtSentenceEnd(nowMs);
}

void App::handleTouch(uint32_t nowMs) {
  if (!touchInitialized_) {
    return;
  }

  maybeFinalizeTouchPlayRelease(nowMs);

  // Grid arm-then-confirm: once the confirm window lapses without a second
  // tap, clear the highlight even without any new touch input, so the UI
  // doesn't keep showing a button as "armed" when a tap would actually
  // re-arm it instead of confirming.
  if (state_ == AppState::Menu && armedGridItemIndex_ != -1 &&
      (nowMs - armedGridArmedAtMs_) > kArmedConfirmWindowMs) {
    armedGridItemIndex_ = -1;
    renderMenu();
  }

  if (state_ == AppState::Menu && pendingFlashItemIndex_ != -1) {
    firePendingGridFlash(nowMs);
  }

  if (menuScreen_ == MenuScreen::TextEntry && pendingTextEntryFlashIndex_ != -1) {
    firePendingTextEntryFlash(nowMs);
  }

  if (state_ == AppState::Booting || state_ == AppState::UsbTransfer ||
      state_ == AppState::Sleeping) {
    touch_.cancel();
    pausedTouch_.active = false;
    pausedTouchIntent_ = TouchIntent::None;
    touchPlayHeld_ = false;
    touchPlayPendingRelease_ = false;
    resetReaderTapTracking();
    return;
  }

  if (state_ == AppState::Standby) {
    TouchEvent ev;
    if (touch_.poll(ev)) {
      // Ignore touch during standby — only physical buttons wake the device
      // to prevent accidental wake in pocket
    }
    return;
  }

  TouchEvent ev;
  if (!touch_.poll(ev)) {
    return;
  }

  lastActivityMs_ = nowMs;

  Serial.printf("[touch] phase=%s touched=%u x=%u y=%u gesture=%u state=%s\n",
                touchPhaseName(ev.phase), ev.touched ? 1 : 0, ev.x, ev.y, ev.gesture,
                stateName(state_));
  if (state_ == AppState::Menu) {
    applyMenuTouchGesture(ev, nowMs);
  } else {
    applyPausedTouchGesture(ev, nowMs);
  }
}

void App::applyPausedTouchGesture(const TouchEvent &event, uint32_t nowMs) {
  if (event.phase == TouchPhase::End && touchPlayHeld_) {
    resetReaderTapTracking();
    pausedTouch_.active = false;
    pausedTouchIntent_ = TouchIntent::None;
    // Don't pause immediately — the touch controller can drop a sample or
    // two under a steady hold (see kTouchPlayReleaseGraceMs) and report it
    // as a real release. Keep touchPlayHeld_/Playing alive for a short
    // grace window; maybeFinalizeTouchPlayRelease() commits the pause once
    // it actually expires, and a Start event arriving in the meantime
    // cancels it below so a genuine ongoing hold never stutters.
    touchPlayPendingRelease_ = true;
    touchPlayPendingReleaseAtMs_ = nowMs;
    return;
  }

  if (event.phase == TouchPhase::Start) {
    touchPlayPendingRelease_ = false;
    pausedTouch_.active = true;
    pausedTouchIntent_ = TouchIntent::None;
    if (state_ != AppState::Playing) {
      invalidateContextPreviewWindow();
    }
    pausedTouch_.startX = event.x;
    pausedTouch_.startY = event.y;
    pausedTouch_.lastX = event.x;
    pausedTouch_.lastY = event.y;
    pausedTouch_.startMs = nowMs;
    pausedTouch_.lastMs = nowMs;
    pausedTouch_.startWordIndex = reader_.currentIndex();
    pausedTouch_.gestureStepsApplied = 0;
    pausedTouch_.browseOffsetPermille = 0;
    return;
  }

  if (!pausedTouch_.active) {
    return;
  }

  const uint32_t elapsedSinceLastEventMs = nowMs - pausedTouch_.lastMs;
  pausedTouch_.lastX = event.x;
  pausedTouch_.lastY = event.y;
  pausedTouch_.lastMs = nowMs;

  const int deltaX = static_cast<int>(pausedTouch_.lastX) - static_cast<int>(pausedTouch_.startX);
  const int deltaY = static_cast<int>(pausedTouch_.lastY) - static_cast<int>(pausedTouch_.startY);
  const int absDeltaX = abs(deltaX);
  const int absDeltaY = abs(deltaY);
  const uint32_t pressDurationMs = nowMs - pausedTouch_.startMs;
  const bool ended = event.phase == TouchPhase::End;
  const bool tapLike = absDeltaX <= static_cast<int>(kTapSlopPx) &&
                       absDeltaY <= static_cast<int>(kTapSlopPx);
  const bool previewBrowseMode = contextViewVisible_ && !scrollModeEnabled();

  if (state_ == AppState::Playing) {
    if (ended) {
      pausedTouch_.active = false;
      pausedTouchIntent_ = TouchIntent::None;
      if (tapLike) {
        if (handleBatteryBadgeTap(event.x, event.y, nowMs)) {
          return;
        }
        if (handleFooterMetricTap(event.x, event.y, nowMs)) {
          return;
        }
        if (handlePreviousSentenceTap(event.x, event.y, nowMs)) {
          return;
        }
        if (isSavePointButtonTap(event.x, event.y)) {
          resetReaderTapTracking();
          // Pause reading and open name entry for save point
          if (state_ == AppState::Playing) {
            setState(AppState::Paused, nowMs);
          }
          saveReadingPosition(true);
          const String defaultName = savePointDefaultName();
          savePointQuickSaveFromReader_ = true;
          if (savePointUseCustomName_) {
            menuScreen_ = MenuScreen::Main;
            setState(AppState::Menu, nowMs);
            openTextEntry(TextEntryPurpose::SavePointName,
                          tr3(TrKey3::NameBookmark),
                          tr3(TrKey3::EnterNamePrompt),
                          "", defaultName, "", false, 30,
                          MenuScreen::SavePointsList);
          } else {
            finishSavePointCreation(defaultName, nowMs);
          }
          return;
        }
        if (playLocked_ || pauseAtSentenceEndRequested_) {
          resetReaderTapTracking();
          requestReaderPauseAtSentenceEnd(nowMs);
        } else {
          handleReaderTap(event.x, event.y, nowMs);
        }
      } else {
        resetReaderTapTracking();
      }
    }
    return;
  }

  if (!previewBrowseMode && !ended && pausedTouchIntent_ == TouchIntent::None &&
      pressDurationMs >= kTouchPlayHoldMs && tapLike) {
    resetReaderTapTracking();
    touchPlayHeld_ = true;
    pausedTouchIntent_ = TouchIntent::PlayHold;
    wpmFeedbackVisible_ = false;
    setState(AppState::Playing, nowMs);
    return;
  }

  if (pausedTouchIntent_ == TouchIntent::None) {
    if (absDeltaX >= static_cast<int>(kSwipeThresholdPx) &&
        absDeltaX > absDeltaY + static_cast<int>(kAxisBiasPx)) {
      resetReaderTapTracking();
      pausedTouchIntent_ = TouchIntent::Scrub;
    } else if (previewBrowseMode && !ended && pressDurationMs >= kPreviewBrowseHoldMs &&
               absDeltaY > absDeltaX + static_cast<int>(kAxisBiasPx)) {
      resetReaderTapTracking();
      pausedTouchIntent_ = TouchIntent::BrowseScroll;
    } else if (!previewBrowseMode && absDeltaY >= static_cast<int>(kSwipeThresholdPx) &&
               absDeltaY > absDeltaX + static_cast<int>(kAxisBiasPx)) {
      resetReaderTapTracking();
      pausedTouchIntent_ = TouchIntent::Wpm;
    }
  }

  if (pausedTouchIntent_ == TouchIntent::Scrub) {
    applyScrubTarget(scrubStepsForDrag(deltaX), nowMs);
    if (ended) {
      pausedTouch_.active = false;
      pausedTouchIntent_ = TouchIntent::None;
      saveReadingPosition(true);
    }
    return;
  }

  if (pausedTouchIntent_ == TouchIntent::BrowseScroll) {
    applyBrowseHoldScroll(event.y, elapsedSinceLastEventMs, nowMs);
    if (ended) {
      pausedTouch_.active = false;
      pausedTouchIntent_ = TouchIntent::None;
      saveReadingPosition(true);
    }
    return;
  }

  if (pausedTouchIntent_ == TouchIntent::Wpm) {
    if (!ended) {
      return;
    }

    const int wpmDelta = (deltaY < 0) ? 1 : -1;
    reader_.adjustWpm(wpmDelta);
    preferences_.putUShort(kPrefWpm, reader_.wpm());
    renderWpmFeedback(nowMs);
    Serial.printf("[app] WPM=%u interval=%lu ms\n", reader_.wpm(),
                  static_cast<unsigned long>(reader_.wordIntervalMs()));
    pausedTouch_.active = false;
    pausedTouchIntent_ = TouchIntent::None;
    return;
  }

  if (ended) {
    pausedTouch_.active = false;
    pausedTouchIntent_ = TouchIntent::None;
    if (tapLike && handleBatteryBadgeTap(event.x, event.y, nowMs)) {
      return;
    }
    if (tapLike && handleFooterMetricTap(event.x, event.y, nowMs)) {
      return;
    }
    if (tapLike && handlePreviousSentenceTap(event.x, event.y, nowMs)) {
      return;
    }
    if (tapLike && isSavePointButtonTap(event.x, event.y)) {
      resetReaderTapTracking();
      saveReadingPosition(true);
      const String defaultName = savePointDefaultName();
      savePointQuickSaveFromReader_ = true;
      if (savePointUseCustomName_) {
        menuScreen_ = MenuScreen::Main;
        setState(AppState::Menu, nowMs);
        openTextEntry(TextEntryPurpose::SavePointName,
                      tr3(TrKey3::NameBookmark),
                      tr3(TrKey3::EnterNamePrompt),
                      "", defaultName, "", false, 30,
                      MenuScreen::SavePointsList);
      } else {
        finishSavePointCreation(defaultName, nowMs);
      }
      return;
    }
    if (tapLike && previewBrowseMode) {
      resetReaderTapTracking();
      contextViewVisible_ = false;
      renderActiveReader(nowMs);
    } else if (tapLike) {
      handleReaderTap(event.x, event.y, nowMs);
    } else {
      resetReaderTapTracking();
    }
  }
}

int App::scrubStepsForDrag(int deltaX) const {
  const int absDeltaX = abs(deltaX);
  if (absDeltaX < static_cast<int>(kSwipeThresholdPx)) {
    return 0;
  }

  int steps = 1 + ((absDeltaX - static_cast<int>(kSwipeThresholdPx)) /
                   static_cast<int>(kScrubStepPx));
  steps = std::min(steps, kMaxScrubStepsPerGesture);

  return (deltaX > 0) ? steps : -steps;
}

void App::applyScrubTarget(int targetSteps, uint32_t nowMs) {
  if (targetSteps == pausedTouch_.gestureStepsApplied) {
    return;
  }

  reader_.seekRelative(pausedTouch_.startWordIndex, targetSteps);
  pausedTouch_.gestureStepsApplied = targetSteps;
  if (!scrollModeEnabled()) {
    contextViewVisible_ = true;
  }
  wpmFeedbackVisible_ = false;
  renderActiveReader(nowMs);
  Serial.printf("[app] scrub target=%d word=%s\n", targetSteps, reader_.currentWord().c_str());
}

int App::browseScrollRatePermille(uint16_t y) const {
  const int centerY = BoardConfig::DISPLAY_HEIGHT / 2;
  const int signedDistance = static_cast<int>(y) - centerY;
  const int absDistance = abs(signedDistance);
  if (absDistance <= static_cast<int>(kBrowseNeutralZonePx)) {
    return 0;
  }

  const int activeRange = std::max(1, centerY - static_cast<int>(kBrowseNeutralZonePx));
  const int activeDistance =
      std::min(activeRange, absDistance - static_cast<int>(kBrowseNeutralZonePx));
  const uint32_t speedPermille =
      kBrowseMinWordsPerSecondPermille +
      ((kBrowseMaxWordsPerSecondPermille - kBrowseMinWordsPerSecondPermille) *
       static_cast<uint32_t>(activeDistance)) /
          static_cast<uint32_t>(activeRange);

  return signedDistance < 0 ? -static_cast<int>(speedPermille) : static_cast<int>(speedPermille);
}

void App::renderContextBrowsePreview(size_t currentIndex, uint16_t scrollProgressPermille) {
  const size_t wordCount = reader_.wordCount();
  if (wordCount == 0) {
    renderReaderWord();
    return;
  }

  if (currentIndex >= wordCount) {
    currentIndex = wordCount - 1;
    scrollProgressPermille = 0;
  }

  updateContextPreviewWindow(currentIndex);
  contextViewVisible_ = true;
  const DisplayManager::ReaderChrome chrome = readerChrome();
  display_.renderScrollView(contextPreviewWords_, currentReaderContentToken(),
                            contextPreviewStartIndex_, currentIndex, scrollProgressPermille,
                            currentChapterLabel(), readingProgressPercent(), "",
                            readerFooterStatusLabel(), chrome);
}

void App::applyBrowseHoldScroll(uint16_t y, uint32_t elapsedMs, uint32_t nowMs) {
  if (elapsedMs == 0) {
    return;
  }

  const int ratePermille = browseScrollRatePermille(y);
  pausedTouch_.browseOffsetPermille +=
      (static_cast<int32_t>(ratePermille) * static_cast<int32_t>(elapsedMs)) / 1000;

  int targetWords = pausedTouch_.browseOffsetPermille / 1000;
  int32_t remainderPermille = pausedTouch_.browseOffsetPermille % 1000;
  if (remainderPermille < 0) {
    remainderPermille += 1000;
    --targetWords;
  }

  reader_.seekRelative(pausedTouch_.startWordIndex, targetWords);
  if (!ensureCurrentBookWordAvailable(nowMs)) {
    return;
  }
  pausedTouch_.gestureStepsApplied = targetWords;
  contextViewVisible_ = true;
  wpmFeedbackVisible_ = false;
  renderContextBrowsePreview(reader_.currentIndex(),
                             static_cast<uint16_t>(remainderPermille));
  Serial.printf("[app] browse hold target=%d progress=%ld word=%s\n", targetWords,
                static_cast<long>(remainderPermille), reader_.currentWord().c_str());
}

void App::applyMenuTouchGesture(const TouchEvent &event, uint32_t nowMs) {
  // Direct-drag slider screen: bypasses the tap/swipe state machine below
  // entirely — it needs live TouchPhase::Move updates, which that machine
  // only forwards on TouchPhase::End.
  if (menuScreen_ == MenuScreen::PacingDelayEditor) {
    handlePacingDelayEditorTouch(event, nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::WpmEditor) {
    handleWpmEditorTouch(event, nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::TypographyValueEditor) {
    handleTypographyValueEditorTouch(event, nowMs);
    return;
  }

  if (event.phase == TouchPhase::Start) {
    pausedTouch_.active = true;
    pausedTouchIntent_ = TouchIntent::None;
    pausedTouch_.startX = event.x;
    pausedTouch_.startY = event.y;
    pausedTouch_.lastX = event.x;
    pausedTouch_.lastY = event.y;
    pausedTouch_.startMs = nowMs;
    pausedTouch_.lastMs = nowMs;
    return;
  }

  if (!pausedTouch_.active) {
    return;
  }

  pausedTouch_.lastX = event.x;
  pausedTouch_.lastY = event.y;
  pausedTouch_.lastMs = nowMs;

  // Help popup dismiss: any touch while popup is showing dismisses it
  if (showingHelpPopup_ && event.phase == TouchPhase::End) {
    pausedTouch_.active = false;
    dismissHelpPopup(nowMs);
    return;
  }



  if (event.phase != TouchPhase::End) {
    return;
  }

  pausedTouch_.active = false;

  const int deltaX = static_cast<int>(pausedTouch_.lastX) - static_cast<int>(pausedTouch_.startX);
  const int deltaY = static_cast<int>(pausedTouch_.lastY) - static_cast<int>(pausedTouch_.startY);
  const int absDeltaX = abs(deltaX);
  const int absDeltaY = abs(deltaY);

  if (menuScreen_ == MenuScreen::TextEntry) {
    if (absDeltaX <= static_cast<int>(kTapSlopPx) && absDeltaY <= static_cast<int>(kTapSlopPx)) {
      handleTextEntryTap(event.x, event.y, nowMs);
    }
    return;
  }

  // Tutorial (5 kroków) nie jest ekranem siatki — renderTutorialStep()
  // rysuje przez renderStatus(), nie renderItemGrid(), więc currentGridButtons_
  // poniżej to zawsze zastane dane z ekranu sprzed wejścia w tutorial (np.
  // Ustawienia > O aplikacji). Bez tej gałęzi tap trafiał w te martwe
  // przyciski (albo w nic, gdy nawigacja nie była w trybie Swipe — jedynym,
  // dla którego istniała logika "róg = Wróć") i przycisk Wróć w rogu nie
  // robił nic. Obsługujemy tap bezpośrednio, niezależnie od navMode_.
  if (menuScreen_ == MenuScreen::TutorialStep1 || menuScreen_ == MenuScreen::TutorialStep2 ||
      menuScreen_ == MenuScreen::TutorialStep3 || menuScreen_ == MenuScreen::TutorialStep4 ||
      menuScreen_ == MenuScreen::TutorialStep5) {
    if (absDeltaX <= static_cast<int>(kTapSlopPx) && absDeltaY <= static_cast<int>(kTapSlopPx)) {
      if (event.x < 40 && event.y < 40) {
        previousTutorialStep(nowMs);
      } else {
        handleTutorialTap(nowMs);
      }
    }
    return;
  }

  // Kroki "Pobierz appkę"/"Połączenie z aplikacją"/"Skonfiguruj w aplikacji"
  // i ekrany "Super!"/"Skonfigurujmy" nie mają listy ani siatki przycisków —
  // cały ekran jest jednym przyciskiem "Dalej" (Super/ConfigureIntro/
  // ConfigureInApp dodatkowo auto-advance po 3s). Górny-lewy róg (ten sam
  // 40x40 obszar co ikona Back w renderStatus/renderStatusWithQr i co
  // przycisk Wróć w tutorialu wyżej) cofa o krok zamiast iść dalej —
  // wcześniej jedynym sposobem cofnięcia stąd był fizyczny przycisk PWR.
  if (menuScreen_ == MenuScreen::WelcomeConnect || menuScreen_ == MenuScreen::WelcomeAppPairing ||
      menuScreen_ == MenuScreen::WelcomeConfigureInApp || menuScreen_ == MenuScreen::WelcomeSuper ||
      menuScreen_ == MenuScreen::WelcomeConfigureIntro) {
    if (absDeltaX <= static_cast<int>(kTapSlopPx) && absDeltaY <= static_cast<int>(kTapSlopPx)) {
      if (event.x < 40 && event.y < 40) {
        wizardStepBack(nowMs);
      } else if (menuScreen_ == MenuScreen::WelcomeConnect &&
                 (!welcomeConnectQrAvailable() || isWizardNextCornerTap(event.x, event.y))) {
        // With a QR on screen, only the corner "Next" (after the 5s look-
        // at-the-code delay) advances — see selectWelcomeConnectTap(). If
        // QR generation failed there's no corner button to gate on, so the
        // whole screen stays tap-through like every other step here.
        selectWelcomeConnectTap(nowMs);
      } else if (menuScreen_ == MenuScreen::WelcomeAppPairing) {
        selectWelcomeAppPairingTap(nowMs);
      } else if (menuScreen_ == MenuScreen::WelcomeConfigureInApp) {
        openWelcomeBookPicker(nowMs);
      }
    }
    return;
  }
  if (menuScreen_ == MenuScreen::WelcomeReadingModePreview) {
    if (absDeltaX <= static_cast<int>(kTapSlopPx) && absDeltaY <= static_cast<int>(kTapSlopPx)) {
      openWelcomeReadingMode();
    }
    return;
  }

  // Te 3 ekrany kreatora (wybór języka/motywu/koloru) reużywają zwykłą
  // siatkę Ustawień, ale rebuildSettingsMenuItems() celowo NIE dodaje im
  // pozycji "Wstecz" (to czysta lista wyboru) — bez tego bloku tap w
  // lewym-górnym rogu trafiał w kafelek pierwszej pozycji (np.
  // "English"/"Light") zamiast cofać, tak jak wszędzie indziej w kreatorze.
  // WelcomeReadingMode NIE jest tu wymieniony — ten róg zajmuje teraz
  // prawdziwy przycisk (ikonka oka, applyReadingModePreviewButtonLayout()),
  // więc tap musi przejść do handleGridTap() niżej zamiast być połykany.
  if ((menuScreen_ == MenuScreen::WelcomeLanguage || menuScreen_ == MenuScreen::WelcomeTheme ||
       menuScreen_ == MenuScreen::WelcomeHighlightColor) &&
      absDeltaX <= static_cast<int>(kTapSlopPx) && absDeltaY <= static_cast<int>(kTapSlopPx) &&
      event.x < 40 && event.y < 40) {
    // WelcomeLanguage to pierwszy krok — nie ma dokąd cofnąć, więc tap w
    // rogu jest tu po prostu połykany zamiast przypadkowo wybierać "English".
    if (menuScreen_ != MenuScreen::WelcomeLanguage) {
      wizardStepBack(nowMs);
    }
    return;
  }

  // Typography is a single-item-at-a-time screen, not a grid — it never
  // calls renderItemGrid(), so currentGridButtons_/handleGridTap() below
  // would either do nothing or (worse) hit stale buttons left over from
  // whatever grid screen was open before. Handle its taps/swipes fully
  // here, independent of navMode, so it works the same in every nav mode
  // instead of only in legacy Swipe mode. Horizontal swipe now moves
  // between the 10 settings (matches the "swipe = change page" rule used
  // everywhere else); vertical swipe still cycles the preview sample word.
  if (menuScreen_ == MenuScreen::TypographyTuning) {
    if (absDeltaX <= static_cast<int>(kTapSlopPx) && absDeltaY <= static_cast<int>(kTapSlopPx)) {
      uint16_t backZoneW = 0;
      uint16_t backZoneH = 0;
      backCornerHitZone(backZoneW, backZoneH);
      if (event.x < backZoneW && event.y < backZoneH) {
        typographyTuningSelectedIndex_ = TypographyTuningBack;
      }
      selectMenuItem(nowMs);
      return;
    }
    if (absDeltaX >= static_cast<int>(kSwipeThresholdPx) &&
        absDeltaX > absDeltaY + static_cast<int>(kAxisBiasPx)) {
      moveMenuSelection(deltaX < 0 ? 1 : -1);
      return;
    }
    if (absDeltaY >= static_cast<int>(kSwipeThresholdPx) &&
        absDeltaY > absDeltaX + static_cast<int>(kAxisBiasPx)) {
      cycleTypographyPreviewSample(deltaY < 0 ? 1 : -1);
      return;
    }
    return;
  }

  // Immediate-mode button grid: tap directly hits whichever button is
  // visible at that spot (same Rects the last render built), swipe
  // left/right pages through screens with more items than fit at once.
  // D-Pad mode keeps the legacy scrolling list on the left side of the
  // screen, so it's excluded here.
  if (navMode_ != NavMode::DPad) {
    if (absDeltaX <= static_cast<int>(kTapSlopPx) && absDeltaY <= static_cast<int>(kTapSlopPx)) {
      if (handleGridTap(event.x, event.y, nowMs)) {
        return;
      }
    } else if (handleGridPageSwipe(deltaX, deltaY, nowMs)) {
      return;
    }
  }

  // D-Pad mode: handle taps on the D-Pad panel (right side of screen)
  if (navMode_ == NavMode::DPad &&
      absDeltaX <= static_cast<int>(kTapSlopPx) && absDeltaY <= static_cast<int>(kTapSlopPx)) {
    constexpr uint16_t kDPadPanelStartX = 520;  // 640 - 120
    if (event.x >= kDPadPanelStartX) {
      if (lastDPadTapAtMs_ != 0 && nowMs - lastDPadTapAtMs_ < kDPadTapDebounceMs) {
        // Swallow a second End event from the same physical tap (contact
        // bounce) — without this, one press on the up/down zone could move
        // the selection twice. See kDPadTapDebounceMs.
        return;
      }
      lastDPadTapAtMs_ = nowMs;

      // Determine which D-Pad button was tapped based on position
      // Buttons are large zones dividing the panel into quadrants + center
      const int padCenterX = kDPadPanelStartX + 60;  // center of 120px panel
      const int padCenterY = 86;  // center of 172px screen
      const int relX = static_cast<int>(event.x) - padCenterX;
      const int relY = static_cast<int>(event.y) - padCenterY;
      const int absRelX = abs(relX);
      const int absRelY = abs(relY);

      if (absRelX < 25 && absRelY < 25) {
        // Center: OK/confirm
        selectMenuItem(nowMs);
      } else if (absRelY > absRelX) {
        // Vertical: up or down
        moveMenuSelection(relY < 0 ? -1 : 1);
      } else {
        // Horizontal: left (back) or right (enter/select)
        if (relX < 0) {
          // Left = go back (simulate selecting index 0)
          settingsSelectedIndex_ = 0;
          selectMenuItem(nowMs);
        } else {
          // Right = enter/select current item
          selectMenuItem(nowMs);
        }
      }
      return;
    }

    // D-Pad mode: tap directly on a menu item text row (left side)
    if (event.x >= 28 && event.x < kDPadPanelStartX) {
      // Determine item count and selected index for current screen
      size_t *selectedIndex = &menuSelectedIndex_;
      size_t itemCount = mainMenuItemCount();
      if (menuScreen_ == MenuScreen::Presets || menuScreen_ == MenuScreen::PresetsDeleteConfirm) {
        selectedIndex = &presetsSelectedIndex_;
        itemCount = settingsMenuItems_.size();
      } else if (isSettingsListScreen()) {
        selectedIndex = &settingsSelectedIndex_;
        itemCount = settingsMenuItems_.size();
      } else if (menuScreen_ == MenuScreen::BookPicker) {
        selectedIndex = &bookPickerSelectedIndex_;
        itemCount = bookMenuItems_.size();
      } else if (menuScreen_ == MenuScreen::BookDetails) {
        selectedIndex = &bookDetailsSelectedIndex_;
        itemCount = bookDetailsMenuItems_.size();
      } else if (menuScreen_ == MenuScreen::BookDeleteConfirm) {
        selectedIndex = &bookDeleteConfirmSelectedIndex_;
        itemCount = bookDeleteConfirmMenuItems_.size();
      } else if (menuScreen_ == MenuScreen::ChapterPicker) {
        selectedIndex = &chapterPickerSelectedIndex_;
        itemCount = chapterMenuItems_.size();
      } else if (menuScreen_ == MenuScreen::SavePointsList) {
        selectedIndex = &savePointSelectedIndex_;
        itemCount = savePointMenuItems_.size();
      } else if (menuScreen_ == MenuScreen::SavePointDeleteConfirm) {
        selectedIndex = &savePointDeleteConfirmSelectedIndex_;
        itemCount = savePointDeleteConfirmMenuItems_.size();
      } else if (menuScreen_ == MenuScreen::PluginsHome) {
        selectedIndex = &pluginsHomeSelectedIndex_;
        itemCount = pluginsHomeMenuItems_.size();
      } else if (menuScreen_ == MenuScreen::PluginsActive) {
        selectedIndex = &pluginsActiveSelectedIndex_;
        itemCount = pluginsActiveMenuItems_.size();
      } else if (menuScreen_ == MenuScreen::PluginLibraryScreen) {
        selectedIndex = &pluginLibrarySelectedIndex_;
        itemCount = pluginLibraryMenuItems_.size();
      } else if (menuScreen_ == MenuScreen::PluginDetail) {
        selectedIndex = &pluginDetailSelectedIndex_;
        itemCount = pluginDetailMenuItems_.size();
      }

      if (itemCount > 0) {
        size_t tappedIndex = 0;
        if (hitTestMenuListRow(event.x, event.y, itemCount, *selectedIndex, tappedIndex)) {
          *selectedIndex = tappedIndex;
          selectMenuItem(nowMs);
          return;
        }
      }
    }
    // In DPad mode, ignore taps that didn't hit anything
    return;
  }

  // D-Pad mode: disable swipe navigation (only DPad buttons work)
  if (navMode_ == NavMode::DPad) {
    return;
  }

  // Everything below this point is the legacy scrolling-list gesture set
  // (vertical swipe moves a cursor, any tap confirms whatever's currently
  // selected) — it belongs to Swipe mode only. In Buttons mode the grid-tap
  // and grid-page-swipe handlers above already own taps and horizontal page
  // swipes; without this guard, an imprecise/diagonal swipe that neither
  // one recognized used to fall through into moveMenuSelection() here,
  // silently cycling focus between buttons (including non-interactive ones
  // like separators) instead of doing nothing, which read as "swiping to
  // change page moves buttons around instead."
  if (navMode_ != NavMode::Swipe) {
    return;
  }

  if (absDeltaY >= static_cast<int>(kSwipeThresholdPx) &&
      absDeltaY > absDeltaX + static_cast<int>(kAxisBiasPx)) {
    moveMenuSelection(deltaY < 0 ? -1 : 1);
    return;
  }

  // A tap that confirms the currently selected item fires on *any* contact
  // that releases close to where it started, regardless of how long the
  // finger sat there — unlike Buttons mode, which looks up the button under
  // the finger, Swipe mode confirms whatever's already highlighted. Cheap
  // capacitive touch drifts a few px over a long, motionless hold, so a
  // held finger could still read back as "release within tap slop" well
  // after it stopped being a deliberate tap, silently changing/committing
  // whatever setting happened to be selected. Require the whole gesture to
  // finish within a normal tap's duration; a long hold that ends up
  // drifting into tap-slop range is ignored instead of confirming.
  if (nowMs - pausedTouch_.startMs > kSwipeTapMaxHoldMs) {
    return;
  }

  if (absDeltaX <= static_cast<int>(kTapSlopPx) && absDeltaY <= static_cast<int>(kTapSlopPx)) {
    // Top-left corner tap = Back button on any menu screen — zone is wider
    // on deep screens, see backCornerHitZone().
    uint16_t backZoneW = 0;
    uint16_t backZoneH = 0;
    backCornerHitZone(backZoneW, backZoneH);
    if (event.x < backZoneW && event.y < backZoneH && menuScreen_ != MenuScreen::Main) {
      // Simulate selecting "Back" (index 0) for list-based screens
      if (isSettingsListScreen() || menuScreen_ == MenuScreen::BookPicker ||
          menuScreen_ == MenuScreen::BookDetails || menuScreen_ == MenuScreen::BookDeleteConfirm ||
          menuScreen_ == MenuScreen::ChapterPicker ||
          menuScreen_ == MenuScreen::SavePointsList ||
          menuScreen_ == MenuScreen::SavePointDeleteConfirm || menuScreen_ == MenuScreen::PluginsHome ||
          menuScreen_ == MenuScreen::PluginsActive ||
          menuScreen_ == MenuScreen::PluginLibraryScreen ||
          menuScreen_ == MenuScreen::PluginDetail ||
          menuScreen_ == MenuScreen::TypographyTuning ||
          menuScreen_ == MenuScreen::TypographyFontPicker) {
        // Set selection to Back and select it
        if (menuScreen_ == MenuScreen::Presets || menuScreen_ == MenuScreen::PresetsDeleteConfirm) {
          presetsSelectedIndex_ = 0;
        } else if (isSettingsListScreen()) {
          settingsSelectedIndex_ = 0;
        } else if (menuScreen_ == MenuScreen::BookPicker) {
          bookPickerSelectedIndex_ = 0;
        } else if (menuScreen_ == MenuScreen::BookDetails) {
          bookDetailsSelectedIndex_ = 0;
        } else if (menuScreen_ == MenuScreen::BookDeleteConfirm) {
          bookDeleteConfirmSelectedIndex_ = 0;
        } else if (menuScreen_ == MenuScreen::ChapterPicker) {
          chapterPickerSelectedIndex_ = 0;
        } else if (menuScreen_ == MenuScreen::SavePointsList) {
          savePointSelectedIndex_ = 0;
        } else if (menuScreen_ == MenuScreen::SavePointDeleteConfirm) {
          savePointDeleteConfirmSelectedIndex_ = 0;
        } else if (menuScreen_ == MenuScreen::PluginsHome) {
          pluginsHomeSelectedIndex_ = 0;
        } else if (menuScreen_ == MenuScreen::PluginsActive) {
          pluginsActiveSelectedIndex_ = 0;
        } else if (menuScreen_ == MenuScreen::PluginLibraryScreen) {
          pluginLibrarySelectedIndex_ = 0;
        } else if (menuScreen_ == MenuScreen::PluginDetail) {
          pluginDetailSelectedIndex_ = 0;
        } else if (menuScreen_ == MenuScreen::TypographyTuning) {
          typographyTuningSelectedIndex_ = TypographyTuningBack;
        } else if (menuScreen_ == MenuScreen::TypographyFontPicker) {
          typographyFontPickerSelectedIndex_ = 0;
        }
        selectMenuItem(nowMs);
        return;
      }
      // For confirm screens, just go back to main
      menuScreen_ = MenuScreen::Main;
      renderMainMenu();
      return;
    }
    // Hit-test the tap against the row actually drawn under the finger
    // (see handleSwipeListGesture()) instead of blindly confirming
    // whatever moveMenuSelection() last scrolled the cursor to — a tap on
    // the right side of the screen now selects what's there, matching
    // Buttons mode's direct-hit behavior.
    handleSwipeListGesture(event, deltaX, deltaY, nowMs);
  }
}

bool App::isDeepMenuScreen() const {
  switch (menuScreen_) {
    case MenuScreen::SettingsDisplay:
    case MenuScreen::SettingsPacing:
    case MenuScreen::SettingsConnectivity:
    case MenuScreen::SettingsAbout:
    case MenuScreen::ScreensaverSettings:
    case MenuScreen::WifiSettings:
    case MenuScreen::WifiNetworks:
    case MenuScreen::TextEntry:
    case MenuScreen::TypographyTuning:
    case MenuScreen::BookDetails:
    case MenuScreen::BookDeleteConfirm:
    case MenuScreen::ChapterPicker:
    case MenuScreen::SavePointDeleteConfirm:
    case MenuScreen::SavePointNameEntry:
    case MenuScreen::PluginsActive:
    case MenuScreen::PluginLibraryScreen:
    case MenuScreen::PluginDetail:
    case MenuScreen::Presets:
    case MenuScreen::PresetsDeleteConfirm:
    case MenuScreen::PacingDelayEditor:
    case MenuScreen::WpmEditor:
    case MenuScreen::TypographyFontPicker:
    case MenuScreen::TypographyValueEditor:
      return true;
    default:
      // Main, and everything one hop off it (SettingsHome, BookPicker,
      // SavePointsList, PluginsHome, the Welcome/Tutorial onboarding flow,
      // RestartConfirm/SdCardRepairConfirm/UpdateConfirm) — back is either
      // unavailable or a less likely next tap than on a nested screen.
      return false;
  }
}

void App::backCornerHitZone(uint16_t &outW, uint16_t &outH) const {
  if (isDeepMenuScreen()) {
    outW = 130;
    outH = 60;
  } else {
    outW = 80;
    outH = 35;
  }
}

bool App::hitTestMenuListRow(uint16_t tapX, uint16_t tapY, size_t itemCount, size_t selectedIndex,
                              size_t &outIndex) const {
  (void)tapX;  // Whole row width is tappable — no side column reserved.
  if (itemCount == 0) {
    return false;
  }

  // Mirrors DisplayManager::renderMenuScroll()/renderMenuWithDPad()'s exact
  // layout (same row height, same vertical centering/scroll-window formula)
  // so a tap lands on the row the user is actually looking at.
  constexpr int kRowHeight = 22;  // DisplayManager::kCompactMenuRowHeight
  const int virtualHeight = BoardConfig::DISPLAY_HEIGHT;
  const size_t visibleCount =
      std::min(itemCount, static_cast<size_t>(std::max(1, virtualHeight / kRowHeight)));
  size_t firstVisible = 0;
  if (selectedIndex >= visibleCount / 2) {
    firstVisible = selectedIndex - visibleCount / 2;
  }
  if (firstVisible + visibleCount > itemCount) {
    firstVisible = itemCount - visibleCount;
  }
  const int totalHeight = kRowHeight * static_cast<int>(visibleCount);
  const int startY = std::max(0, (virtualHeight - totalHeight) / 2);
  const int y = static_cast<int>(tapY);

  if (y < startY || y >= startY + totalHeight) {
    return false;
  }

  const size_t tappedRow = static_cast<size_t>((y - startY) / kRowHeight);
  const size_t tappedIndex = firstVisible + tappedRow;
  if (tappedIndex >= itemCount) {
    return false;
  }

  outIndex = tappedIndex;
  return true;
}

bool App::handleSwipeListGesture(const TouchEvent &event, int deltaX, int deltaY,
                                  uint32_t nowMs) {
  (void)deltaX;
  (void)deltaY;
  size_t itemCount = 0;
  size_t *selectedIndex = currentMenuSelectedIndexPtr(itemCount);

  size_t tappedIndex = 0;
  if (!hitTestMenuListRow(event.x, event.y, itemCount, *selectedIndex, tappedIndex)) {
    return false;
  }

  *selectedIndex = tappedIndex;
  selectMenuItem(nowMs);
  return true;
}

size_t *App::currentMenuSelectedIndexPtr(size_t &itemCountOut) {
  size_t *selectedIndex = &menuSelectedIndex_;
  size_t itemCount = mainMenuItemCount();
  if (menuScreen_ == MenuScreen::Presets || menuScreen_ == MenuScreen::PresetsDeleteConfirm) {
    selectedIndex = &presetsSelectedIndex_;
    itemCount = settingsMenuItems_.size();
  } else if (menuScreen_ == MenuScreen::SettingsHome || menuScreen_ == MenuScreen::SettingsDisplay ||
      menuScreen_ == MenuScreen::SettingsPacing || menuScreen_ == MenuScreen::WifiSettings ||
      menuScreen_ == MenuScreen::SettingsConnectivity ||
      menuScreen_ == MenuScreen::SettingsAbout || menuScreen_ == MenuScreen::ScreensaverSettings ||
      menuScreen_ == MenuScreen::WelcomeLanguage || menuScreen_ == MenuScreen::WelcomeTheme ||
      menuScreen_ == MenuScreen::WelcomeHighlightColor || menuScreen_ == MenuScreen::WelcomeReadingMode) {
    selectedIndex = &settingsSelectedIndex_;
    itemCount = settingsMenuItems_.size();
  } else if (menuScreen_ == MenuScreen::WifiNetworks) {
    selectedIndex = &wifiNetworkSelectedIndex_;
    itemCount = wifiNetworkMenuItems_.size();
  } else if (menuScreen_ == MenuScreen::TypographyTuning) {
    selectedIndex = &typographyTuningSelectedIndex_;
    itemCount = TypographyTuningItemCount;
  } else if (menuScreen_ == MenuScreen::TypographyFontPicker) {
    selectedIndex = &typographyFontPickerSelectedIndex_;
    itemCount = typographyFontPickerMenuItems_.size();
  } else if (menuScreen_ == MenuScreen::BookPicker) {
    selectedIndex = &bookPickerSelectedIndex_;
    itemCount = bookMenuItems_.size();
  } else if (menuScreen_ == MenuScreen::BookDetails) {
    selectedIndex = &bookDetailsSelectedIndex_;
    itemCount = bookDetailsMenuItems_.size();
  } else if (menuScreen_ == MenuScreen::BookDeleteConfirm) {
    selectedIndex = &bookDeleteConfirmSelectedIndex_;
    itemCount = bookDeleteConfirmMenuItems_.size();
  } else if (menuScreen_ == MenuScreen::ChapterPicker) {
    selectedIndex = &chapterPickerSelectedIndex_;
    itemCount = chapterMenuItems_.size();
  } else if (menuScreen_ == MenuScreen::SavePointsList) {
    selectedIndex = &savePointSelectedIndex_;
    itemCount = savePointMenuItems_.size();
  } else if (menuScreen_ == MenuScreen::SavePointDeleteConfirm) {
    selectedIndex = &savePointDeleteConfirmSelectedIndex_;
    itemCount = savePointDeleteConfirmMenuItems_.size();
  } else if (menuScreen_ == MenuScreen::PluginsHome) {
    selectedIndex = &pluginsHomeSelectedIndex_;
    itemCount = pluginsHomeMenuItems_.size();
  } else if (menuScreen_ == MenuScreen::PluginsActive) {
    selectedIndex = &pluginsActiveSelectedIndex_;
    itemCount = pluginsActiveMenuItems_.size();
  } else if (menuScreen_ == MenuScreen::PluginLibraryScreen) {
    selectedIndex = &pluginLibrarySelectedIndex_;
    itemCount = pluginLibraryMenuItems_.size();
  } else if (menuScreen_ == MenuScreen::PluginDetail) {
    selectedIndex = &pluginDetailSelectedIndex_;
    itemCount = pluginDetailMenuItems_.size();
  } else if (menuScreen_ == MenuScreen::RestartConfirm) {
    selectedIndex = &restartConfirmSelectedIndex_;
    itemCount = RestartConfirmItemCount;
  } else if (menuScreen_ == MenuScreen::TypographyResetConfirm) {
    selectedIndex = &typographyResetConfirmSelectedIndex_;
    itemCount = TypographyResetConfirmItemCount;
  } else if (menuScreen_ == MenuScreen::SdCardRepairConfirm) {
    selectedIndex = &sdCardRepairConfirmSelectedIndex_;
    itemCount = SdCardRepairConfirmItemCount;
  } else if (menuScreen_ == MenuScreen::UpdateConfirm) {
    selectedIndex = &updateConfirmSelectedIndex_;
    itemCount = UpdateConfirmItemCount;
  }

  itemCountOut = itemCount;
  return selectedIndex;
}

void App::moveMenuSelection(int direction) {
  if (direction == 0 || menuScreen_ == MenuScreen::TextEntry) {
    return;
  }

  size_t itemCount = 0;
  size_t *selectedIndex = currentMenuSelectedIndexPtr(itemCount);

  if (itemCount == 0) {
    return;
  }

  const int next = static_cast<int>(*selectedIndex) + direction;
  if (next < 0) {
    *selectedIndex = itemCount - 1;
  } else if (next >= static_cast<int>(itemCount)) {
    *selectedIndex = 0;
  } else {
    *selectedIndex = static_cast<size_t>(next);
  }

  renderMenu();
  if (menuScreen_ == MenuScreen::Presets || menuScreen_ == MenuScreen::PresetsDeleteConfirm) {
    Serial.printf("[presets] selected=%s\n", settingsMenuItems_[presetsSelectedIndex_].c_str());
  } else if (menuScreen_ == MenuScreen::SettingsHome || menuScreen_ == MenuScreen::SettingsDisplay ||
      menuScreen_ == MenuScreen::SettingsPacing || menuScreen_ == MenuScreen::WifiSettings ||
      menuScreen_ == MenuScreen::SettingsConnectivity ||
      menuScreen_ == MenuScreen::SettingsAbout || menuScreen_ == MenuScreen::ScreensaverSettings ||
      menuScreen_ == MenuScreen::WelcomeLanguage || menuScreen_ == MenuScreen::WelcomeTheme ||
      menuScreen_ == MenuScreen::WelcomeHighlightColor || menuScreen_ == MenuScreen::WelcomeReadingMode) {
    Serial.printf("[settings] selected=%s\n", settingsMenuItems_[settingsSelectedIndex_].c_str());
  } else if (menuScreen_ == MenuScreen::WifiNetworks) {
    Serial.printf("[wifi] selected=%s\n", wifiNetworkMenuItems_[wifiNetworkSelectedIndex_].title.c_str());
  } else if (menuScreen_ == MenuScreen::TypographyTuning) {
    Serial.printf("[typography] selected=%s\n", typographyTuningLabel().c_str());
  } else if (menuScreen_ == MenuScreen::TypographyFontPicker) {
    Serial.printf("[typography-font-picker] selected=%s\n",
                  typographyFontPickerMenuItems_[typographyFontPickerSelectedIndex_].c_str());
  } else if (menuScreen_ == MenuScreen::BookPicker) {
    Serial.printf("[book-picker] selected=%s\n",
                  bookMenuItems_[bookPickerSelectedIndex_].title.c_str());
  } else if (menuScreen_ == MenuScreen::ChapterPicker) {
    Serial.printf("[chapter-picker] selected=%s\n",
                  chapterMenuItems_[chapterPickerSelectedIndex_].c_str());
  } else if (menuScreen_ == MenuScreen::RestartConfirm) {
    String selectedLabel = uiText(UiText::AreYouSure);
    switch (restartConfirmSelectedIndex_) {
      case RestartConfirmNo:
        selectedLabel = uiText(UiText::NoKeepPlace);
        break;
      case RestartConfirmYes:
        selectedLabel = uiText(UiText::YesRestart);
        break;
      default:
        break;
    }
    Serial.printf("[restart] selected=%s\n", selectedLabel.c_str());
  } else if (menuScreen_ == MenuScreen::SdCardRepairConfirm) {
    const String selectedLabel =
        sdCardRepairConfirmSelectedIndex_ == SdCardRepairConfirmYes ? "Create folders" : "Not now";
    Serial.printf("[sd-check] selected=%s\n", selectedLabel.c_str());
  } else if (menuScreen_ == MenuScreen::UpdateConfirm) {
    const String selectedLabel =
        updateConfirmSelectedIndex_ == UpdateConfirmUpdate ? "Update" : "Skip for now";
    Serial.printf("[ota] selected=%s\n", selectedLabel.c_str());
  } else {
    String selectedLabel = uiText(UiText::Read);
    const size_t menuItem = (menuSelectedIndex_ < mainMenuOrder_.size())
                                 ? mainMenuOrder_[menuSelectedIndex_]
                                 : static_cast<size_t>(MenuItemCount);
    switch (menuItem) {
      case MenuRead:
        selectedLabel = uiText(UiText::Read);
        break;
      case MenuLibrary:
        selectedLabel = uiText(UiText::Library);
        break;
      case MenuSavePoints:
        selectedLabel = uiText(UiText::SavePoints);
        break;
      case MenuSettings:
        selectedLabel = uiText(UiText::Settings);
        break;
      case MenuPlugins:
        selectedLabel = uiText(UiText::Plugins);
        break;
      case MenuPowerOff:
        selectedLabel = uiText(UiText::PowerOff);
        break;
      default:
        break;
    }
    Serial.printf("[menu] selected=%s\n", selectedLabel.c_str());
  }
}

namespace {
// SavePointsList's vertical paging (see gridPagesVertically_): tile 0 is
// "+ Add save point" and gets a page to itself (full-width, not squeezed
// next to a bookmark); every tile after that is a (name, delete) pair for
// one bookmark, two tiles per page. Both renderItemGrid() (draws it) and
// handleGridPageSwipe() (pages it) must agree on this exact mapping or a
// swipe would land on a different bookmark than what's drawn.
size_t savePointsPageCount(size_t tileCount) {
  if (tileCount <= 1) {
    return 1;
  }
  return 1 + ((tileCount - 1) + 1) / 2;
}
size_t savePointsPageForTile(size_t tileIndex) {
  return tileIndex == 0 ? 0 : 1 + (tileIndex - 1) / 2;
}
size_t savePointsTileStartForPage(size_t page) {
  return page == 0 ? 0 : 1 + (page - 1) * 2;
}
}  // namespace

void App::renderItemGrid(const String &title, const std::vector<String> &items,
                         size_t selectedIndex, size_t headerRows, bool showBatteryBadge) {
  applyReaderUiOrientation();

  currentGridButtons_.clear();
  currentGridItemIndices_.clear();

  if (items.size() <= headerRows) {
    display_.renderButtonGrid(title, currentGridButtons_, 0, 1);
    return;
  }

  const size_t actionableCount = items.size() - headerRows;
  size_t effectiveSelected = selectedIndex >= headerRows ? selectedIndex - headerRows : 0;
  if (effectiveSelected >= actionableCount) {
    effectiveSelected = actionableCount - 1;
  }

  // Back (canonicalIndex 0, when present) is never a real grid tile — it's
  // a fixed icon-only affordance pinned to the top-left corner (see
  // applyBackButtonCornerLayout()). It used to still be counted as one of
  // the tiles the grid laid out, then get yanked into the corner
  // afterward, leaving the tile it vacated permanently blank. Exclude it
  // from the grid math entirely and lay out only the real, tappable tiles;
  // the corner Back icon is added separately below, on every page.
  const bool hasBack = actionableCount > 0 && items[headerRows] == uiText(UiText::Back);
  const size_t tileCount = hasBack ? actionableCount - 1 : actionableCount;
  const size_t tileSelected =
      hasBack ? (effectiveSelected == 0 ? 0 : effectiveSelected - 1) : effectiveSelected;

  ui::GridSpec spec = ui::computeGridSpec(tileCount);
  gridPagesVertically_ = menuScreen_ == MenuScreen::SavePointsList ||
                        menuScreen_ == MenuScreen::PluginsActive;
  if (menuScreen_ == MenuScreen::SavePointsList) {
    // One full-width row per savepoint's name, one below it for its Delete
    // — long "42.3% Book Title" names get real room instead of being
    // truncated in a 4-up tile, and every page is unambiguously "this one
    // bookmark", not two rows of eight that all look alike.
    spec.columns = 1;
    spec.rows = 2;
    spec.itemsPerPage = 2;
  } else if (menuScreen_ == MenuScreen::PluginsActive) {
    // Full-width single-column tiles, 3 per page — same vertical
    // swipe/paging feel as SavePointsList, just without its "+Add gets its
    // own page" pairing (every tile here is equally a plugin to launch).
    spec.columns = 1;
    spec.rows = 3;
    spec.itemsPerPage = 3;
  } else if (menuScreen_ == MenuScreen::PluginDetail) {
    // Full-width description (Label tile, non-interactive) stacked over a
    // full-width Enable/Disable button — exactly two tiles, always one
    // page, so the description gets real width instead of being squeezed
    // side-by-side with the toggle.
    spec.columns = 1;
    spec.rows = 2;
    spec.itemsPerPage = 2;
  }
  const size_t itemsPerPage = std::max<size_t>(1, spec.itemsPerPage);
  size_t pageCount;
  size_t page;
  size_t pageStart;
  size_t itemsOnPage;
  if (menuScreen_ == MenuScreen::SavePointsList) {
    pageCount = savePointsPageCount(tileCount);
    page = tileCount == 0 ? 0 : savePointsPageForTile(tileSelected);
    pageStart = savePointsTileStartForPage(page);
    const size_t pageCapacity = page == 0 ? 1 : 2;
    itemsOnPage = std::min(pageCapacity, tileCount - pageStart);
  } else {
    pageCount = tileCount == 0 ? 1 : (tileCount + itemsPerPage - 1) / itemsPerPage;
    page = tileCount == 0 ? 0 : tileSelected / itemsPerPage;
    pageStart = page * itemsPerPage;
    itemsOnPage = std::min(itemsPerPage, tileCount - pageStart);
  }

  gridHeaderRows_ = headerRows;
  gridHasBack_ = hasBack;
  gridItemsPerPage_ = itemsPerPage;
  gridPageCount_ = pageCount;
  gridPage_ = page;

  // Left-edge dot column needs room carved out of the left margin; every
  // other screen keeps the dots bottom-centered and doesn't need it.
  const uint16_t kAreaX = (gridPagesVertically_ && pageCount > 1) ? 16 : 6;
  // 32, not the tile height's natural 24: leaves a real gap below the
  // enlarged corner Back button (applyBackButtonCornerLayout(), bottom edge
  // at y=28) so a few px of touch-coordinate jitter can't land a Back tap on
  // the first grid tile — see the ghost-touch bug report this fixed.
  constexpr uint16_t kAreaY = 32;
  constexpr uint16_t kGap = 4;
  const uint16_t areaW = static_cast<uint16_t>(BoardConfig::DISPLAY_WIDTH - 2 * kAreaX);
  const uint16_t bottomReserve = (pageCount > 1 && !gridPagesVertically_) ? 10 : 2;
  const uint16_t areaH =
      static_cast<uint16_t>(BoardConfig::DISPLAY_HEIGHT - kAreaY - bottomReserve);

  // Page 0 ("+ Add save point") is a single tile — give it the full-height
  // 1-row spec instead of the 2-row spec so it isn't squeezed into the top
  // half with an empty gap below it.
  ui::GridSpec pageSpec = spec;
  if (menuScreen_ == MenuScreen::SavePointsList && page == 0) {
    pageSpec.rows = 1;
  }
  const std::vector<ui::Rect> rects =
      ui::layoutGrid(pageSpec, itemsOnPage, kAreaX, kAreaY, areaW, areaH, kGap);

  currentGridButtons_.reserve(itemsOnPage + (hasBack ? 1 : 0));

  if (hasBack) {
    DisplayManager::Button backButton;
    backButton.icon = ui::IconId::Back;
    backButton.active = effectiveSelected == 0;
    backButton.armed = isGridItemArmed(0, millis()) || isGridItemFlashing(0, millis());
    currentGridButtons_.push_back(backButton);
    currentGridItemIndices_.push_back(0);
  }

  for (size_t i = 0; i < itemsOnPage && i < rects.size(); ++i) {
    const size_t canonicalIndex = (hasBack ? 1 : 0) + pageStart + i;
    const size_t labelIndex = headerRows + canonicalIndex;
    DisplayManager::Button button;
    button.label = items[labelIndex];
    button.x = rects[i].x;
    button.y = rects[i].y;
    button.width = rects[i].w;
    button.height = rects[i].h;
    if (button.label == "---") {
      button.kind = DisplayManager::Button::ButtonKind::Separator;
      currentGridButtons_.push_back(button);
      currentGridItemIndices_.push_back(canonicalIndex);
      continue;
    }
    button.active = canonicalIndex == effectiveSelected;
    button.armed = isGridItemArmed(canonicalIndex, millis()) || isGridItemFlashing(canonicalIndex, millis());
    if (!button.armed && canonicalIndex == effectiveSelected &&
        (menuScreen_ == MenuScreen::SettingsDisplay || menuScreen_ == MenuScreen::SettingsPacing) &&
        isConfirmGatedRow(menuScreen_, canonicalIndex)) {
      // Confirm-gated Settings rows reuse the same solid-focus-color
      // "armed" fill as the destructive two-tap flow — it's the same idea
      // (one more tap elsewhere before this actually applies), so the tile
      // needs to look unmistakably "picked, not yet applied" the same way,
      // not a new visual language. Toggle/Cycle rows can't use the plain
      // active-tint for this (see the tileActiveTint comment in
      // DisplayManager.cpp — active there means on/off or cycle position,
      // not selection), so armed is the only signal that works for every
      // row kind on these screens.
      button.armed = true;
    }
    if (menuScreen_ == MenuScreen::SettingsDisplay) {
      annotateSettingsDisplayButton(button, canonicalIndex);
    } else if (menuScreen_ == MenuScreen::SavePointsList) {
      annotateSavePointsButton(button, canonicalIndex);
    } else if (menuScreen_ == MenuScreen::Main) {
      annotateMainMenuButton(button);
    } else if (menuScreen_ == MenuScreen::PluginDetail) {
      annotatePluginDetailButton(button, canonicalIndex);
    } else if (menuScreen_ == MenuScreen::TypographyFontPicker) {
      annotateTypographyFontPickerButton(button, canonicalIndex);
    }
    currentGridButtons_.push_back(button);
    currentGridItemIndices_.push_back(canonicalIndex);
  }

  applyBackButtonCornerLayout();
  applyConfirmButtonCornerLayout();
  applyReadingModePreviewButtonLayout();
  // First-run wizard screens (language/theme/highlight/reading-mode picks)
  // get the same filled-bar, scale-2 title treatment as the toast below —
  // legible for low-vision users on their very first boot, instead of the
  // scale-1 footnote every other grid screen uses.
  const bool prominentTitle = menuScreen_ == MenuScreen::WelcomeLanguage ||
                              menuScreen_ == MenuScreen::WelcomeTheme ||
                              menuScreen_ == MenuScreen::WelcomeHighlightColor ||
                              menuScreen_ == MenuScreen::WelcomeReadingMode;
  display_.renderButtonGrid(title, currentGridButtons_, page, pageCount, activeGridToastText(millis()),
                            showBatteryBadge, gridPagesVertically_, prominentTitle);
}

void App::renderItemGridLibrary(const std::vector<DisplayManager::LibraryItem> &items,
                                size_t selectedIndex, const String &title) {
  applyReaderUiOrientation();

  currentGridButtons_.clear();
  currentGridItemIndices_.clear();

  if (items.empty()) {
    display_.renderButtonGrid("", currentGridButtons_, 0, 1);
    return;
  }

  size_t effectiveSelected = selectedIndex;
  if (effectiveSelected >= items.size()) {
    effectiveSelected = items.size() - 1;
  }

  // Same corner-Back exclusion as renderItemGrid() — see the comment there.
  // Also pulls the wizard-only "Pomiń"/"Skip" row (WifiNetworks/BookPicker
  // reused inside the onboarding wizard) into the same small corner slot —
  // it used to render as a full-size tile, competing visually with the real
  // choices even though skipping Wi-Fi/the starter book is the exception,
  // not the recommended path.
  const bool isRealBack = items[0].title == uiText(UiText::Back);
  const bool isSkip = items[0].title == tr2(TrKey2::SkipForNow);
  const bool hasBack = isRealBack || isSkip;
  const size_t tileCount = hasBack ? items.size() - 1 : items.size();
  const size_t tileSelected =
      hasBack ? (effectiveSelected == 0 ? 0 : effectiveSelected - 1) : effectiveSelected;

  const ui::GridSpec spec = ui::computeGridSpec(tileCount);
  const size_t itemsPerPage = std::max<size_t>(1, spec.itemsPerPage);
  const size_t pageCount = tileCount == 0 ? 1 : (tileCount + itemsPerPage - 1) / itemsPerPage;
  const size_t page = tileCount == 0 ? 0 : tileSelected / itemsPerPage;
  const size_t pageStart = page * itemsPerPage;
  const size_t itemsOnPage = std::min(itemsPerPage, tileCount - pageStart);

  gridHeaderRows_ = 0;
  gridHasBack_ = hasBack;
  gridItemsPerPage_ = itemsPerPage;
  gridPageCount_ = pageCount;
  gridPage_ = page;
  gridPagesVertically_ = false;

  constexpr uint16_t kAreaX = 6;
  // 32, not the tile height's natural 24: leaves a real gap below the
  // enlarged corner Back button (applyBackButtonCornerLayout(), bottom edge
  // at y=28) so a few px of touch-coordinate jitter can't land a Back tap on
  // the first grid tile — see the ghost-touch bug report this fixed.
  constexpr uint16_t kAreaY = 32;
  constexpr uint16_t kGap = 4;
  const uint16_t areaW = static_cast<uint16_t>(BoardConfig::DISPLAY_WIDTH - 2 * kAreaX);
  const uint16_t bottomReserve = pageCount > 1 ? 10 : 2;
  const uint16_t areaH =
      static_cast<uint16_t>(BoardConfig::DISPLAY_HEIGHT - kAreaY - bottomReserve);

  const std::vector<ui::Rect> rects =
      ui::layoutGrid(spec, itemsOnPage, kAreaX, kAreaY, areaW, areaH, kGap);

  currentGridButtons_.reserve(itemsOnPage + (hasBack ? 1 : 0));

  if (hasBack) {
    DisplayManager::Button cornerButton;
    if (isRealBack) {
      cornerButton.icon = ui::IconId::Back;
      // Position finalized below by applyBackButtonCornerLayout().
    } else {
      // "Pomiń"/"Skip" — small text corner button, not positioned by
      // applyBackButtonCornerLayout() (that only matches icon==Back), so set
      // its rect directly here. Deliberately smaller than a real grid tile.
      cornerButton.label = items[0].title;
      cornerButton.x = 0;
      cornerButton.y = 2;
      cornerButton.width = 60;
      cornerButton.height = 22;
    }
    cornerButton.active = effectiveSelected == 0;
    cornerButton.armed = isGridItemArmed(0, millis()) || isGridItemFlashing(0, millis());
    currentGridButtons_.push_back(cornerButton);
    currentGridItemIndices_.push_back(0);
  }

  for (size_t i = 0; i < itemsOnPage && i < rects.size(); ++i) {
    const size_t absoluteItemIndex = (hasBack ? 1 : 0) + pageStart + i;
    DisplayManager::Button button;
    button.label = items[absoluteItemIndex].title;
    button.sublabel = items[absoluteItemIndex].subtitle;
    button.x = rects[i].x;
    button.y = rects[i].y;
    button.width = rects[i].w;
    button.height = rects[i].h;
    button.active = absoluteItemIndex == effectiveSelected;
    button.armed = isGridItemArmed(absoluteItemIndex, millis()) || isGridItemFlashing(absoluteItemIndex, millis());
    currentGridButtons_.push_back(button);
    currentGridItemIndices_.push_back(absoluteItemIndex);
  }

  applyBackButtonCornerLayout();
  display_.renderButtonGrid(title, currentGridButtons_, page, pageCount, activeGridToastText(millis()));
}

void App::renderMenuAnyMode(const String &title, const std::vector<String> &items,
                            size_t selectedIndex, size_t headerRows) {
  if (navMode_ == NavMode::DPad) {
    applyReaderUiOrientation();
    // renderItemGrid() is the only place that clears these — without this,
    // stale Button rects from whatever grid screen was open last stick
    // around and handleGridTap() (which runs before the DPad-specific tap
    // handling below it) can hit-test a tap on this list screen against
    // them, firing an action that belongs to a completely different,
    // previously-visited screen.
    currentGridButtons_.clear();
    currentGridItemIndices_.clear();
    display_.renderMenuWithDPad(items, selectedIndex);
  } else if (navMode_ == NavMode::Swipe) {
    applyReaderUiOrientation();
    // Same stale-rect hazard as the DPad branch above, but for Swipe mode's
    // handleGridTap() pass.
    currentGridButtons_.clear();
    currentGridItemIndices_.clear();
    display_.renderMenuScroll(items, selectedIndex);
  } else {
    renderItemGrid(title, items, selectedIndex, headerRows);
  }
}

void App::renderMenuAnyModeLibrary(const std::vector<DisplayManager::LibraryItem> &items,
                                   size_t selectedIndex, const String &title) {
  if (navMode_ == NavMode::Buttons) {
    renderItemGridLibrary(items, selectedIndex, title);
    return;
  }

  // DPad/Swipe are single-line lists — no room for the subtitle, so fall
  // back to titles only.
  std::vector<String> titles;
  titles.reserve(items.size());
  for (const DisplayManager::LibraryItem &item : items) {
    titles.push_back(item.title);
  }

  applyReaderUiOrientation();
  currentGridButtons_.clear();
  currentGridItemIndices_.clear();
  if (navMode_ == NavMode::DPad) {
    display_.renderMenuWithDPad(titles, selectedIndex);
  } else {
    display_.renderMenuScroll(titles, selectedIndex);
  }
}

bool App::isGridItemArmed(size_t canonicalIndex, uint32_t nowMs) const {
  return armedGridItemIndex_ >= 0 && armedGridScreen_ == menuScreen_ &&
         static_cast<size_t>(armedGridItemIndex_) == canonicalIndex &&
         (nowMs - armedGridArmedAtMs_) <= kArmedConfirmWindowMs;
}

bool App::isGridItemFlashing(size_t canonicalIndex, uint32_t nowMs) const {
  return pendingFlashItemIndex_ >= 0 && pendingFlashScreen_ == menuScreen_ &&
         static_cast<size_t>(pendingFlashItemIndex_) == canonicalIndex &&
         nowMs < pendingFlashFireAtMs_;
}

void App::firePendingGridFlash(uint32_t nowMs) {
  if (pendingFlashItemIndex_ == -1 || pendingFlashScreen_ != menuScreen_ ||
      nowMs < pendingFlashFireAtMs_) {
    return;
  }
  pendingFlashItemIndex_ = -1;
  selectMenuItem(nowMs);
}

void App::applyBackButtonCornerLayout() {
  for (DisplayManager::Button &button : currentGridButtons_) {
    if (button.icon == ui::IconId::Back) {
      button.label = "";
      button.sublabel = "";
      button.x = 0;
      button.y = 2;
      // Wider than the old 32px so a slightly off-center thumb tap in the
      // corner still lands on Back. Height is left at 26 (bottom edge at
      // y=28) on purpose — see the kAreaY=32 comment in renderItemGrid():
      // growing it further would eat into the jitter gap that keeps a Back
      // tap from landing on the first grid tile below it.
      button.width = 44;
      button.height = 26;
      break;
    }
  }
}

bool App::isWizardConfirmPickerScreen() const {
  return menuScreen_ == MenuScreen::WelcomeLanguage || menuScreen_ == MenuScreen::WelcomeTheme ||
         menuScreen_ == MenuScreen::WelcomeHighlightColor ||
         menuScreen_ == MenuScreen::WelcomeReadingMode ||
         // Font picker reused from Ustawienia > Typografia — only gated in
         // wizard mode. Outside the wizard it stays instant tap-to-apply
         // (unchanged, matches every other Settings row); inside the wizard
         // a tap used to commit AND immediately advance past this screen in
         // one motion, so there was no way to look at more than one font
         // before the wizard moved on.
         (menuScreen_ == MenuScreen::TypographyFontPicker && wizardFontPickerActive_);
}

bool App::isConfirmGatedRow(MenuScreen screen, size_t canonicalIndex) const {
  // Confirm-then-apply only earns its keep where a tap actually previews a
  // distinct, visible option before it's committed — the wizard picker
  // screens (WelcomeLanguage/Theme/HighlightColor/ReadingMode), where each
  // tile is one of several choices laid out side by side. Settings rows
  // (SettingsDisplay/SettingsPacing) are single cycle/toggle controls: a tap
  // there never shows a pending value to compare, it just advances the same
  // control Confirm would — so gating them behind Confirm added a second tap
  // with nothing to preview. Reverted back to instant tap-to-apply for those
  // (2026-09-23, per Karol: "nie wszystko powinno w ustawieniach wymagać
  // potwierdzenia").
  (void)canonicalIndex;
  return screen == MenuScreen::WelcomeLanguage || screen == MenuScreen::WelcomeTheme ||
         screen == MenuScreen::WelcomeHighlightColor || screen == MenuScreen::WelcomeReadingMode ||
         (screen == MenuScreen::TypographyFontPicker && wizardFontPickerActive_);
}

void App::applyConfirmButtonCornerLayout() {
  bool showConfirm = isWizardConfirmPickerScreen();
  if (!showConfirm) {
    return;
  }
  DisplayManager::Button confirmButton;
  confirmButton.icon = ui::IconId::Check;
  confirmButton.label = "";
  confirmButton.sublabel = "";
  // Bigger than Back's 44x26 corner rect on purpose — this is the one button
  // that commits the whole wizard step, and Karol reported it was too small
  // to hit reliably with a thumb. Hit-testing widens it further still, see
  // confirmCornerHitZone() in handleGridTap().
  confirmButton.x = static_cast<uint16_t>(BoardConfig::DISPLAY_WIDTH - kWizardConfirmButtonWidth);
  confirmButton.y = static_cast<uint16_t>(BoardConfig::DISPLAY_HEIGHT - kWizardConfirmButtonHeight - 2);
  confirmButton.width = kWizardConfirmButtonWidth;
  confirmButton.height = kWizardConfirmButtonHeight;
  confirmButton.armed = isGridItemFlashing(kWizardConfirmCanonicalIndex, millis());
  currentGridButtons_.push_back(confirmButton);
  currentGridItemIndices_.push_back(kWizardConfirmCanonicalIndex);
}

void App::applyReadingModePreviewButtonLayout() {
  // WelcomeReadingMode only: a small eye-icon button pinned to the top-left
  // corner (free real estate — this screen, like the other wizard pickers,
  // has no Back tile) that opens a live preview of whichever tile
  // (RSVP/Przewijanie) is currently highlighted. Deliberately NOT gated
  // behind Potwierdź — see handleGridTap()'s isReadingModePreviewButton
  // bypass — because looking at a preview isn't a decision, only actually
  // picking RSVP vs Przewijanie is.
  if (menuScreen_ != MenuScreen::WelcomeReadingMode) {
    return;
  }
  DisplayManager::Button previewButton;
  previewButton.icon = ui::IconId::Eye;
  previewButton.label = "";
  previewButton.sublabel = "";
  previewButton.x = 0;
  previewButton.y = 2;
  previewButton.width = 44;
  previewButton.height = 26;
  previewButton.armed = isGridItemFlashing(kReadingModePreviewCanonicalIndex, millis());
  currentGridButtons_.push_back(previewButton);
  currentGridItemIndices_.push_back(kReadingModePreviewCanonicalIndex);
}

namespace {
// SettingsDisplay rows are built as "<Name>: <current value>" — for
// Toggle/Cycle buttons the widget itself shows the value (switch position,
// lit dot), so keep only the name to avoid saying the same thing twice.
void stripTrailingValueLabel(String &label) {
  const int sep = label.lastIndexOf(": ");
  if (sep >= 0) {
    label = label.substring(0, sep);
  }
}
}  // namespace

void App::annotateSettingsDisplayButton(DisplayManager::Button &button,
                                        size_t canonicalIndex) const {
  switch (canonicalIndex) {
    case kSettingsDisplayFooterIndex:
      button.kind = DisplayManager::Button::ButtonKind::Cycle;
      button.cycleCount = 3;
      button.cycleState = static_cast<uint8_t>(footerMetricMode_);
      break;
    case kSettingsDisplayBatteryIndex:
      button.kind = DisplayManager::Button::ButtonKind::Cycle;
      button.cycleCount = 3;
      button.cycleState = static_cast<uint8_t>(batteryLabelMode_);
      break;
    case kSettingsDisplayNavModeIndex:
      button.kind = DisplayManager::Button::ButtonKind::Cycle;
      button.cycleCount = 3;
      button.cycleState = static_cast<uint8_t>(navMode_);
      break;
    case kSettingsDisplayReaderBatteryIndex:
      button.kind = DisplayManager::Button::ButtonKind::Toggle;
      button.active = readerBatteryVisibleWhilePlaying_;
      break;
    case kSettingsDisplayReaderChapterIndex:
      button.kind = DisplayManager::Button::ButtonKind::Toggle;
      button.active = readerChapterVisibleWhilePlaying_;
      break;
    case kSettingsDisplayReaderProgressIndex:
      button.kind = DisplayManager::Button::ButtonKind::Toggle;
      button.active = readerProgressVisibleWhilePlaying_;
      break;
    case kSettingsDisplaySavePointBtnIndex:
      button.kind = DisplayManager::Button::ButtonKind::Toggle;
      button.active = savePointButtonVisible_;
      break;
    case kSettingsDisplaySavePointNameModeIndex:
      button.kind = DisplayManager::Button::ButtonKind::Toggle;
      button.active = savePointUseCustomName_;
      break;
    case kSettingsDisplayHelpHintsIndex:
      button.kind = DisplayManager::Button::ButtonKind::Toggle;
      button.active = showHelpHints_;
      break;
    default:
      return;
  }
  stripTrailingValueLabel(button.label);
}

void App::annotateSavePointsButton(DisplayManager::Button &button, size_t canonicalIndex) const {
  // Index 0 is "Back" — already handled generically (icon-only corner
  // button) by the label match in renderItemGrid(). Index 1 is
  // "+ Dodaj punkt zapisu"/"+ Add save point". From index 2 on, entries
  // alternate: the save point's own name (even offset), then its paired
  // "Usun: <nazwa>"/"Delete: <name>" button (odd offset) — see
  // openSavePointsList(). Give the "+ Add" row and every save-point-name
  // row the floppy-disk icon so a save point reads as one at a glance;
  // leave the Delete rows alone.
  if (canonicalIndex == 0) {
    return;
  }
  if (canonicalIndex == 1) {
    button.icon = ui::IconId::SavePoint;
    return;
  }
  if ((canonicalIndex - 2) % 2 == 0) {
    button.icon = ui::IconId::SavePoint;
  }
}

void App::annotateMainMenuButton(DisplayManager::Button &button) const {
  if (button.label == uiText(UiText::Read)) {
    button.icon = ui::IconId::Play;
  } else if (button.label == uiText(UiText::Library)) {
    button.icon = ui::IconId::Book;
  } else if (button.label == uiText(UiText::SavePoints)) {
    button.icon = ui::IconId::SavePoint;
  } else if (button.label == uiText(UiText::Settings)) {
    button.icon = ui::IconId::Settings;
  } else if (button.label == uiText(UiText::Plugins)) {
    button.icon = ui::IconId::Plugin;
  } else if (button.label == uiText(UiText::PowerOff)) {
    button.icon = ui::IconId::Power;
  }
}

void App::annotatePluginDetailButton(DisplayManager::Button &button, size_t canonicalIndex) const {
  // canonicalIndex 0 is Back (pinned to the corner, never reaches here).
  // 1 is always the description row — see openPluginDetail().
  if (canonicalIndex != 1) {
    return;
  }
  button.kind = DisplayManager::Button::ButtonKind::Label;
  button.sublabel = pluginDetailDescLine2_;
}

namespace {
// Arm-then-confirm exists to protect against fat-finger data loss, not to
// gate every tap in the UI. It only makes sense for buttons that commit an
// irreversible action immediately, with no dedicated Yes/No screen behind
// them — the "Usuń: <nazwa>"/"Delete: <name>" rows built inline into
// SavePointsList/PresetsDeleteConfirm/BookDeleteConfirm. Everything else
// (navigation, settings toggles, opening a book/chapter/preset/plugin, and
// the Yes/No buttons on screens that are themselves a confirmation step
// like RestartConfirm/UpdateConfirm/SdCardRepairConfirm) should act on the
// first tap like a normal app button — doubling that up with a second
// required tap only makes the UI feel unresponsive and broken.
bool isDestructiveGridLabel(const String &label) {
  const bool startsWithDelete = label.startsWith("Usun") || label.startsWith("Usuń") ||
                                 label.startsWith("Delete");
  return startsWithDelete && label.indexOf(": ") >= 0;
}
}  // namespace

bool App::handleGridTap(uint16_t x, uint16_t y, uint32_t nowMs) {
  if (lastGridPageChangeAtMs_ != 0 &&
      nowMs - lastGridPageChangeAtMs_ < kGridPageChangeInputBlackoutMs) {
    // See kGridPageChangeInputBlackoutMs — swallow taps that land right
    // after a page-turn swipe instead of hitting whatever button the
    // finger settled on.
    return true;
  }
  for (size_t i = 0; i < currentGridButtons_.size(); ++i) {
    const DisplayManager::Button &button = currentGridButtons_[i];
    if (button.width <= 2 || button.height <= 2) {
      continue;
    }
    if (button.kind == DisplayManager::Button::ButtonKind::Separator) {
      continue;
    }

    // Back keeps its small visible rect (see applyBackButtonCornerLayout())
    // but hit-tests against a wider zone on deep screens — see
    // isDeepMenuScreen(). On deep screens the zone is allowed to bleed a
    // little past kAreaY=32 (the first grid tile's top edge) on purpose:
    // Back is pushed into currentGridButtons_ before every other tile (see
    // applyBackButtonCornerLayout()), so this loop always reaches it first —
    // a tap landing in that small overlap resolves to Back no matter which
    // tile is actually drawn underneath it. Shallow screens keep the old
    // behaviour (capped below kAreaY) since Back is a much less likely tap
    // there.
    uint16_t hitX = button.x;
    uint16_t hitY = button.y;
    uint16_t hitW = button.width;
    uint16_t hitH = button.height;
    if (button.icon == ui::IconId::Back) {
      uint16_t zoneW = 0;
      uint16_t zoneH = 0;
      backCornerHitZone(zoneW, zoneH);
      constexpr uint16_t kDeepBackZoneBleedPx = 15;
      const uint16_t hitHCap = isDeepMenuScreen()
                                   ? static_cast<uint16_t>(32 + kDeepBackZoneBleedPx)
                                   : static_cast<uint16_t>(30);
      hitW = std::max(hitW, zoneW);
      hitH = std::max(hitH, std::min(zoneH, hitHCap));
    } else if (button.icon == ui::IconId::Check) {
      // Wizard Confirm corner button — same "widen the hit zone past the
      // drawn rect" trick as Back above. Grown from the visible 70x40 box
      // because Karol reported it was still too easy to miss with a thumb;
      // no risk of stealing taps meant for other tiles since Confirm only
      // ever appears alone in the bottom-right corner.
      constexpr uint16_t kConfirmHitZoneW = 110;
      constexpr uint16_t kConfirmHitZoneH = 60;
      const uint16_t growW = static_cast<uint16_t>(kConfirmHitZoneW - button.width);
      const uint16_t growH = static_cast<uint16_t>(kConfirmHitZoneH - button.height);
      hitX = static_cast<uint16_t>(hitX - growW);
      hitY = static_cast<uint16_t>(hitY - growH);
      hitW = kConfirmHitZoneW;
      hitH = kConfirmHitZoneH;
    }

    if (x < hitX || x >= hitX + hitW || y < hitY || y >= hitY + hitH) {
      continue;
    }

    size_t itemCount = 0;
    size_t *selectedIndex = currentMenuSelectedIndexPtr(itemCount);
    const size_t canonicalIndex =
        (i < currentGridItemIndices_.size()) ? currentGridItemIndices_[i] : i;

    // Back/Wróć is exempt from arm-then-confirm: it's pure navigation, not
    // a committing action, so a second tap would only add friction. Same
    // goes for every non-destructive button (see isDestructiveGridLabel).
    // Checked by icon, not label text: the corner Back button's label is
    // blanked out by applyBackButtonCornerLayout() (icon-only rendering).
    const bool isBack = button.icon == ui::IconId::Back;
    const bool isWizardConfirmButton =
        button.icon == ui::IconId::Check && canonicalIndex == kWizardConfirmCanonicalIndex;

    // Reading-mode preview eye button: a pure "show me", not a decision, so
    // it bypasses arm/confirm AND the generic flash-then-selectMenuItem()
    // pipeline entirely (that pipeline would otherwise dispatch straight
    // into selectWelcomeReadingModeItem() and wrongly commit+advance on a
    // mere preview tap). Opens the preview for whichever tile is currently
    // highlighted and returns immediately.
    if (button.icon == ui::IconId::Eye && canonicalIndex == kReadingModePreviewCanonicalIndex) {
      openWelcomeReadingModePreview(settingsSelectedIndex_ == 0 ? 0 : 1);
      return true;
    }

    // Confirm-gated rows (see isConfirmGatedRow()): only the wizard picker
    // screens (WelcomeLanguage/Theme/HighlightColor/ReadingMode), where a
    // tile tap moves the highlight to one of several visible options AND
    // (via previewWizardPickerSelection()) applies it live so the effect is
    // actually visible — but advancing to the next wizard step still only
    // happens on the dedicated Potwierdź corner button below. Settings rows
    // (SettingsDisplay/SettingsPacing) are plain cycle/toggle controls —
    // nothing to preview between tap and apply — so they keep the old
    // instant-tap behaviour below.
    if (isConfirmGatedRow(menuScreen_, canonicalIndex) && !isBack && !isWizardConfirmButton) {
      if (lastFiredGridItemIndex_ == static_cast<int>(canonicalIndex) &&
          lastFiredGridScreen_ == menuScreen_ &&
          (nowMs - lastFiredGridAtMs_) < kGridTapDebounceMs) {
        return true;
      }
      if (canonicalIndex < itemCount) {
        *selectedIndex = canonicalIndex;
        previewWizardPickerSelection(nowMs);
      }
      lastFiredGridItemIndex_ = static_cast<int>(canonicalIndex);
      lastFiredGridScreen_ = menuScreen_;
      lastFiredGridAtMs_ = nowMs;
      renderMenu();
      return true;
    }

    const bool needsConfirm = !isBack && !isWizardConfirmButton && isDestructiveGridLabel(button.label);

    if (!needsConfirm || isGridItemArmed(canonicalIndex, nowMs)) {
      // Contact-bounce guard: a physical tap that briefly loses and regains
      // contact can reach here twice for the same button a few ms apart —
      // most visible on Cycle buttons like screensaver style, where it
      // read as one tap skipping two steps ahead. Swallow the repeat
      // instead of scheduling another fire.
      if (lastFiredGridItemIndex_ == static_cast<int>(canonicalIndex) &&
          lastFiredGridScreen_ == menuScreen_ &&
          (nowMs - lastFiredGridAtMs_) < kGridTapDebounceMs) {
        return true;
      }
      if (canonicalIndex < itemCount) {
        *selectedIndex = canonicalIndex;
      }
      armedGridItemIndex_ = -1;
      lastFiredGridItemIndex_ = static_cast<int>(canonicalIndex);
      lastFiredGridScreen_ = menuScreen_;
      lastFiredGridAtMs_ = nowMs;
      // Don't fire the action immediately — flash the tapped button in
      // focusColor for kPressFlashMs first (see firePendingGridFlash()) so
      // every tap gets a visible "yes, that landed" before the screen
      // changes underneath it.
      pendingFlashScreen_ = menuScreen_;
      pendingFlashItemIndex_ = static_cast<int>(canonicalIndex);
      pendingFlashFireAtMs_ = nowMs + kPressFlashMs;
      renderMenu();
      return true;
    }

    // First tap on this button (or a different button than what was armed,
    // or the previous arm expired): arm it and redraw so it highlights,
    // without performing the action yet.
    armedGridScreen_ = menuScreen_;
    armedGridItemIndex_ = static_cast<int>(canonicalIndex);
    armedGridArmedAtMs_ = nowMs;
    renderMenu();
    return true;
  }

  // Tap missed every button on screen: disarm whatever was armed.
  if (armedGridItemIndex_ != -1) {
    armedGridItemIndex_ = -1;
    renderMenu();
  }
  return false;
}

bool App::handleGridPageSwipe(int deltaX, int deltaY, uint32_t nowMs) {
  // Uses gridPageCount_/gridItemsPerPage_/gridHasBack_/gridHeaderRows_,
  // cached by the renderItemGrid*() call that last drew the screen — NOT
  // recomputed from the raw item count here, which used to include the
  // Back tile the grid itself excludes and drifted the page math by one
  // (a screen with a Back button could think it had 2 pages, swipe to a
  // "page 2" that didn't really exist, and land back on page 1 with the
  // wrong item selected). See the App.h comment on those fields.
  if (gridPageCount_ <= 1) {
    return false;
  }

  const int absDeltaX = abs(deltaX);
  const int absDeltaY = abs(deltaY);
  // SavePointsList pages vertically (one bookmark per page, dots on the
  // left) instead of the usual horizontal paging — see gridPagesVertically_.
  const int pagingDelta = gridPagesVertically_ ? deltaY : deltaX;
  const int primaryAbs = gridPagesVertically_ ? absDeltaY : absDeltaX;
  const int crossAbs = gridPagesVertically_ ? absDeltaX : absDeltaY;
  if (primaryAbs < static_cast<int>(kSwipeThresholdPx) ||
      primaryAbs <= crossAbs + static_cast<int>(kAxisBiasPx)) {
    return false;
  }

  size_t itemCount = 0;
  size_t *selectedIndex = currentMenuSelectedIndexPtr(itemCount);
  if (itemCount == 0) {
    return false;
  }

  size_t page = gridPage_;
  if (pagingDelta < 0) {
    page = (page + 1 < gridPageCount_) ? page + 1 : page;
  } else {
    page = (page > 0) ? page - 1 : 0;
  }
  if (page != gridPage_) {
    lastGridPageChangeAtMs_ = nowMs;
  }
  const size_t newTileSelected = menuScreen_ == MenuScreen::SavePointsList
                                      ? savePointsTileStartForPage(page)
                                      : page * gridItemsPerPage_;
  *selectedIndex = gridHeaderRows_ + (gridHasBack_ ? 1 : 0) + newTileSelected;
  renderMenu();
  return true;
}

void App::selectMenuItem(uint32_t nowMs) {
  if (lastMenuActionAtMs_ != 0 && nowMs - lastMenuActionAtMs_ < kMenuActionDebounceMs) {
    // Swallow a commit that lands right after the previous one — see
    // kMenuActionDebounceMs.
    return;
  }
  lastMenuActionAtMs_ = nowMs;

  if (menuScreen_ == MenuScreen::WelcomeConnect) {
    // Cały ekran to jeden przycisk "Dalej" — bez tego wejścia potwierdzenie
    // z D-Pada/przycisku wpadłoby w switch menu głównego i wyrzuciło
    // użytkownika z kreatora. D-Pad confirm bypasses the 5s look-first
    // delay on purpose — it's a deliberate button press, not an accidental
    // screen tap.
    openWelcomeAppPairing(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::WelcomeAppPairing) {
    selectWelcomeAppPairingTap(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::WelcomeConfigureInApp) {
    openWelcomeBookPicker(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::WelcomeReadingModePreview) {
    openWelcomeReadingMode();
    return;
  }
  if (menuScreen_ == MenuScreen::TutorialStep1 ||
      menuScreen_ == MenuScreen::TutorialStep2 ||
      menuScreen_ == MenuScreen::TutorialStep3 ||
      menuScreen_ == MenuScreen::TutorialStep4 ||
      menuScreen_ == MenuScreen::TutorialStep5) {
    handleTutorialTap(nowMs);
    return;
  }
  if (isSettingsListScreen()) {
    selectSettingsItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::WifiNetworks) {
    selectWifiNetworkItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::TypographyTuning) {
    selectTypographyTuningItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::TypographyFontPicker) {
    selectTypographyFontPickerItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::BookPicker) {
    selectBookPickerItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::BookDetails) {
    selectBookDetailsItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::BookDeleteConfirm) {
    selectBookDeleteConfirmItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::ChapterPicker) {
    selectChapterPickerItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::SavePointsList) {
    selectSavePointItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::SavePointDeleteConfirm) {
    selectSavePointDeleteConfirmItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::PluginsHome) {
    selectPluginsHomeItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::PluginsActive) {
    selectPluginsActiveItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::PluginLibraryScreen) {
    selectPluginLibraryItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::PluginDetail) {
    selectPluginDetailItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::RestartConfirm) {
    selectRestartConfirmItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::TypographyResetConfirm) {
    selectTypographyResetConfirmItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::SdCardRepairConfirm) {
    selectSdCardRepairConfirmItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::UpdateConfirm) {
    selectUpdateConfirmItem(nowMs);
    return;
  }

  // When update button is shown, all menu indices shift by 1
  size_t effectiveIndex = menuSelectedIndex_;
  if (otaUpdatePromptPending_) {
    if (menuSelectedIndex_ == 0) {
      // User tapped "Update" button — run firmware update
      otaUpdatePromptPending_ = false;
      OtaUpdater::Config config = preferredOtaConfig();
      runFirmwareUpdate(config, false, nowMs);
      return;
    }
    effectiveIndex = menuSelectedIndex_ - 1;
  }

  if (effectiveIndex >= mainMenuOrder_.size()) {
    return;
  }
  switch (mainMenuOrder_[effectiveIndex]) {
    case MenuRead:
      setState(AppState::Paused, nowMs);
      return;
    case MenuLibrary:
      openBookPicker(false);
      return;
    case MenuSavePoints:
      openSavePointsList();
      return;
    case MenuSettings:
      openSettings();
      return;
    case MenuPowerOff:
      enterPowerOff(nowMs);
      return;
    case MenuPlugins:
      // Rysowany tylko w trybie zaawansowanym — mainMenuOrder_ w ogóle nie
      // zawiera tego wpisu w trybie podstawowym.
      openPluginsHome();
      return;
    default:
      return;
  }
}

void App::openSettings() {
  settingsSelectedIndex_ = kSettingsHomeReadingIndex;
  menuScreen_ = MenuScreen::SettingsHome;
  rebuildSettingsMenuItems();
  renderSettings();
}

void App::selectSettingsItem(uint32_t nowMs) {
  if (settingsMenuItems_.empty()) {
    if (menuScreen_ == MenuScreen::WifiSettings) {
      openWifiSettings();
    } else {
      openSettings();
    }
    return;
  }

  if (menuScreen_ == MenuScreen::SettingsHome) {
    switch (settingsSelectedIndex_) {
      case kSettingsBackIndex:
        menuScreen_ = MenuScreen::Main;
        renderMainMenu();
        return;
      case kSettingsHomeAdvancedIndex:
        // Widoczny przełącznik prosty/zaawansowany. Po przełączeniu MUSIMY
        // przebudować listę — pozycje dev-only (Presety, Wi-Fi, Aktualizacja)
        // pojawiają się/znikają natychmiast, tak samo jak Pluginy w menu
        // głównym i RSS w Łączności.
        setDevModeEnabled(!devModeEnabled());
        rebuildSettingsMenuItems();
        showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
        renderSettings();
        return;
      case kSettingsHomeReadingIndex:
        settingsSelectedIndex_ = kSettingsPacingReadingModeIndex;
        menuScreen_ = MenuScreen::SettingsPacing;
        rebuildSettingsMenuItems();
        renderSettings();
        return;
      case kSettingsHomeDisplayIndex:
        settingsSelectedIndex_ = kSettingsDisplayThemeIndex;
        menuScreen_ = MenuScreen::SettingsDisplay;
        rebuildSettingsMenuItems();
        renderSettings();
        return;
      case kSettingsHomeConnectivityIndex:
        openSettingsConnectivity();
        return;
      case kSettingsHomePresetsIndex:
        if (devModeEnabled()) openPresets();
        return;
      case kSettingsHomeAboutIndex:
        openSettingsAbout();
        return;
      // Typography is now always visible (not dev-only)
      case kSettingsHomeTypographyIndex:
        openTypographyTuning();
        return;
      // Pozycje dev-only — branchują tylko gdy menu je faktycznie pokazało.
      case kSettingsHomeWifiIndex:
        if (devModeEnabled()) openWifiSettings();
        return;
      case kSettingsHomeUpdateIndex:
        if (devModeEnabled()) runFirmwareUpdate(preferredOtaConfig(), false, nowMs);
        return;
      default:
        return;
    }
  }

  if (menuScreen_ == MenuScreen::Presets) {
    selectPresetsItem(nowMs);
    return;
  }

  if (menuScreen_ == MenuScreen::PresetsDeleteConfirm) {
    if (presetsSelectedIndex_ == 0) {
      // Back → return to presets list
      openPresets();
    } else if (presetsSelectedIndex_ == 1) {
      // Apply → restore the preset
      executeRestorePreset(presetsDeleteTargetIndex_, nowMs);
    } else if (presetsSelectedIndex_ == 2) {
      // Delete → delete the preset
      executeDeletePreset(nowMs);
    }
    return;
  }

  if (menuScreen_ == MenuScreen::SettingsConnectivity) {
    selectSettingsConnectivityItem(nowMs);
    return;
  }

  if (menuScreen_ == MenuScreen::SettingsAbout) {
    selectSettingsAboutItem(nowMs);
    return;
  }

  if (menuScreen_ == MenuScreen::WelcomeLanguage) {
    selectWelcomeLanguageItem(nowMs);
    return;
  }

  if (menuScreen_ == MenuScreen::WelcomeTheme) {
    selectWelcomeThemeItem(nowMs);
    return;
  }

  if (menuScreen_ == MenuScreen::WelcomeHighlightColor) {
    selectWelcomeHighlightColorItem(nowMs);
    return;
  }

  if (menuScreen_ == MenuScreen::WelcomeReadingMode) {
    selectWelcomeReadingModeItem(nowMs);
    return;
  }

  if (menuScreen_ == MenuScreen::WifiSettings) {
    selectWifiSettingsItem(nowMs);
    return;
  }

  if (menuScreen_ == MenuScreen::SettingsDisplay) {
    switch (settingsSelectedIndex_) {
      case kSettingsBackIndex:
        settingsSelectedIndex_ = kSettingsHomeDisplayIndex;
        menuScreen_ = MenuScreen::SettingsHome;
        rebuildSettingsMenuItems();
        renderSettings();
        return;
      case kSettingsDisplayThemeIndex:
        cycleThemeMode(nowMs);
        return;
      case kSettingsDisplayBrightnessIndex:
        cycleBrightness();
        return;
      case kSettingsDisplayHandednessIndex:
        cycleHandednessMode(nowMs);
        return;
      case kSettingsDisplayFooterIndex:
        switch (footerMetricMode_) {
          case FooterMetricMode::Percentage:
            footerMetricMode_ = FooterMetricMode::ChapterTime;
            break;
          case FooterMetricMode::ChapterTime:
            footerMetricMode_ = FooterMetricMode::BookTime;
            break;
          case FooterMetricMode::BookTime:
            footerMetricMode_ = FooterMetricMode::Percentage;
            break;
        }
        preferences_.putUChar(kPrefFooterMetricMode, static_cast<uint8_t>(footerMetricMode_));
        rebuildSettingsMenuItems();
        showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
        renderSettings();
        return;
      case kSettingsDisplayBatteryIndex:
        switch (batteryLabelMode_) {
          case BatteryLabelMode::Percent:
            batteryLabelMode_ = BatteryLabelMode::TimeRemaining;
            break;
          case BatteryLabelMode::TimeRemaining:
            batteryLabelMode_ = BatteryLabelMode::Voltage;
            break;
          case BatteryLabelMode::Voltage:
          default:
            batteryLabelMode_ = BatteryLabelMode::Percent;
            break;
        }
        preferences_.putUChar(kPrefBatteryLabelMode, static_cast<uint8_t>(batteryLabelMode_));
        batteryLabel_ = currentBatteryLabel();
        display_.setBatteryLabel(batteryLabel_);
        rebuildSettingsMenuItems();
        showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
        renderSettings();
        return;
      case kSettingsDisplayScreensaverIndex:
        openScreensaverSettings();
        return;
      case kSettingsDisplayReaderBatteryIndex:
        readerBatteryVisibleWhilePlaying_ = !readerBatteryVisibleWhilePlaying_;
        preferences_.putBool(kPrefReaderBatteryVisible, readerBatteryVisibleWhilePlaying_);
        rebuildSettingsMenuItems();
        showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
        renderSettings();
        return;
      case kSettingsDisplayReaderChapterIndex:
        readerChapterVisibleWhilePlaying_ = !readerChapterVisibleWhilePlaying_;
        preferences_.putBool(kPrefReaderChapterVisible, readerChapterVisibleWhilePlaying_);
        rebuildSettingsMenuItems();
        showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
        renderSettings();
        return;
      case kSettingsDisplayReaderProgressIndex:
        readerProgressVisibleWhilePlaying_ = !readerProgressVisibleWhilePlaying_;
        preferences_.putBool(kPrefReaderProgressVisible, readerProgressVisibleWhilePlaying_);
        rebuildSettingsMenuItems();
        showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
        renderSettings();
        return;
      case kSettingsDisplayLanguageIndex:
        cycleUiLanguage(nowMs);
        return;
      case kSettingsDisplayFocusColorIndex:
        // cycleFocusColor() -> applyDisplayPreferences() already rebuilds
        // the list, shows the toast and re-renders (see kPrefFocusColorIndex
        // path) — nothing left to do here.
        cycleFocusColor(nowMs);
        return;
      case kSettingsDisplaySavePointBtnIndex:
        savePointButtonVisible_ = !savePointButtonVisible_;
        preferences_.putBool(kPrefSavePointButtonVisible, savePointButtonVisible_);
        rebuildSettingsMenuItems();
        showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
        renderSettings();
        return;
      case kSettingsDisplaySavePointNameModeIndex:
        savePointUseCustomName_ = !savePointUseCustomName_;
        preferences_.putBool(kPrefSavePointCustomName, savePointUseCustomName_);
        rebuildSettingsMenuItems();
        showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
        renderSettings();
        return;
      case kSettingsDisplayHelpHintsIndex:
        showHelpHints_ = !showHelpHints_;
        preferences_.putBool(kPrefShowHelpHints, showHelpHints_);
        rebuildSettingsMenuItems();
        showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
        renderSettings();
        return;
      case kSettingsDisplayNavModeIndex:
        navMode_ = nextNavMode(navMode_);
        preferences_.putUChar(kPrefNavMode, static_cast<uint8_t>(navMode_));
        rebuildSettingsMenuItems();
        showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
        renderSettings();
        return;
      default:
        return;
    }
  }

  if (menuScreen_ == MenuScreen::ScreensaverSettings) {
    selectScreensaverSettingsItem(nowMs);
    return;
  }

  if (menuScreen_ != MenuScreen::SettingsPacing) {
    return;
  }

  bool pacingConfigChanged = false;
  switch (settingsSelectedIndex_) {
    case kSettingsBackIndex:
      flushPendingTimeEstimateRebuild();
      settingsSelectedIndex_ = kSettingsHomePacingIndex;
      menuScreen_ = MenuScreen::SettingsHome;
      rebuildSettingsMenuItems();
      renderSettings();
      return;
    case kSettingsPacingReadingModeIndex:
      cycleReaderMode(nowMs);
      return;
  }

  // Scroll mode settings (indices 2, 3, 4, 5 when in Scroll mode)
  if (readerMode_ == ReaderMode::Scroll) {
    switch (settingsSelectedIndex_) {
      case kSettingsPacingScrollFontSizeIndex:
        scrollFontSize_ = static_cast<uint8_t>((scrollFontSize_ + 1) % 9);
        preferences_.putUChar(kPrefScrollFontSize, scrollFontSize_);
        display_.setScrollFontSize(scrollFontSize_);
        Serial.printf("[settings] scroll font size=%u\n", scrollFontSize_);
        rebuildSettingsMenuItems();
        showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
        renderSettings();
        return;
      case kSettingsPacingScrollLineSpacingIndex:
        scrollLineSpacing_ = static_cast<uint8_t>((scrollLineSpacing_ + 1) % 3);
        preferences_.putUChar(kPrefScrollLineSpacing, scrollLineSpacing_);
        display_.setScrollLineSpacing(scrollLineSpacing_);
        Serial.printf("[settings] scroll line spacing=%s\n", scrollLineSpacingLabel().c_str());
        rebuildSettingsMenuItems();
        showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
        renderSettings();
        return;
      case kSettingsPacingScrollMarginIndex:
        scrollMargin_ = static_cast<uint8_t>((scrollMargin_ + 1) % 3);
        preferences_.putUChar(kPrefScrollMargin, scrollMargin_);
        display_.setScrollMargin(scrollMargin_);
        Serial.printf("[settings] scroll margin=%s\n", scrollMarginLabel().c_str());
        rebuildSettingsMenuItems();
        showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
        renderSettings();
        return;
      case kSettingsPacingScrollPreviewIndex:
        showScrollSettingsPreview();
        return;
      default:
        return;
    }
  }

  switch (settingsSelectedIndex_) {
    case kSettingsPacingPauseModeIndex:
      pauseMode_ =
          pauseMode_ == PauseMode::SentenceEnd ? PauseMode::Instant : PauseMode::SentenceEnd;
      preferences_.putUChar(kPrefPauseMode, static_cast<uint8_t>(pauseMode_));
      Serial.printf("[settings] pause mode=%s\n", pauseModeLabel().c_str());
      rebuildSettingsMenuItems();
      showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
      renderSettings();
      return;
    case kSettingsPacingWpmIndex:
      openWpmEditor(nowMs);
      return;
    case kSettingsPacingLongWordsIndex:
      openPacingDelayEditor(PacingDelayTarget::LongWords, nowMs);
      return;
    case kSettingsPacingComplexityIndex:
      openPacingDelayEditor(PacingDelayTarget::Complexity, nowMs);
      return;
    case kSettingsPacingPunctuationIndex:
      openPacingDelayEditor(PacingDelayTarget::Punctuation, nowMs);
      return;
    case kSettingsPacingResetIndex:
      pacingLongWordDelayMs_ = kDefaultPacingDelayMs;
      pacingComplexWordDelayMs_ = kDefaultPacingDelayMs;
      pacingPunctuationDelayMs_ = kDefaultPacingDelayMs;
      preferences_.putUShort(kPrefPacingLongMs, pacingLongWordDelayMs_);
      preferences_.putUShort(kPrefPacingComplexMs, pacingComplexWordDelayMs_);
      preferences_.putUShort(kPrefPacingPunctuationMs, pacingPunctuationDelayMs_);
      pacingConfigChanged = true;
      break;
    default:
      return;
  }

  if (pacingConfigChanged) {
    applyPacingSettings();
  }
  rebuildSettingsMenuItems();
  showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
  renderSettings();
}

void App::openWifiSettings() {
  wifiFlowFromWizard_ = false;
  settingsSelectedIndex_ = kWifiSettingsChooseIndex;
  menuScreen_ = MenuScreen::WifiSettings;
  rebuildSettingsMenuItems();
  renderSettings();
}

void App::selectWifiSettingsItem(uint32_t nowMs) {
  (void)nowMs;

  switch (settingsSelectedIndex_) {
    case kSettingsBackIndex:
      // W trybie podstawowym pozycji "Wi-Fi zaawansowane" nie ma na liście
      // (Wi-Fi otwiera się wtedy z Łączności), więc wracamy na Łączność —
      // inaczej kursor wskazywałby indeks poza listą.
      settingsSelectedIndex_ =
          devModeEnabled() ? kSettingsHomeWifiIndex : kSettingsHomeConnectivityIndex;
      menuScreen_ = MenuScreen::SettingsHome;
      rebuildSettingsMenuItems();
      renderSettings();
      return;
    case kWifiSettingsNetworkIndex:
    case kWifiSettingsChooseIndex:
      scanWifiNetworks();
      return;
    case kWifiSettingsAutoUpdateIndex:
      preferences_.putBool(kPrefOtaAuto, !otaAutoCheckEnabled());
      rebuildSettingsMenuItems();
      showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
      renderSettings();
      return;
    case kWifiSettingsForgetIndex:
      forgetSavedWifiNetwork(configuredWifiSsid());
      preferences_.remove(kPrefWifiSsid);
      preferences_.remove(kPrefWifiPass);
      WiFi.disconnect(true, true);  // disconnect + erase stored credentials
      display_.renderStatus("Wi-Fi", tr2(TrKey2::CredentialsCleared), "");
      delay(900);
      rebuildSettingsMenuItems();
      renderSettings();
      return;
    case kWifiSettingsOtaOwnerIndex:
      openTextEntry(TextEntryPurpose::OtaOwner, "OTA Source", "GitHub owner", "",
                    preferences_.getString(kPrefOtaOwner, ""), "", false, 39,
                    MenuScreen::WifiSettings);
      return;
    case kWifiSettingsOtaChannelIndex:
      preferences_.putBool(kPrefOtaChannel, !otaChannelEnabled());
      rebuildSettingsMenuItems();
      showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
      renderSettings();
      return;
    default:
      return;
  }
}

void App::scanWifiNetworks() {
  if (blockNetworkActionForOtaCheck("Wi-Fi", millis())) {
    return;
  }

  display_.renderProgress("Wi-Fi", "Scanning networks", "", 5);

  WiFi.persistent(false);
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_STA);
  WiFi.scanDelete();

  const int networkCount = WiFi.scanNetworks(false, true);
  wifiNetworks_.clear();
  wifiNetworkMenuItems_.clear();
  wifiNetworkMenuItems_.push_back(
      {wifiFlowFromWizard_ ? tr2(TrKey2::SkipForNow) : uiText(UiText::Back),
       wifiFlowFromWizard_ ? tr3(TrKey3::WifiSkipHint) : ""});

  if (networkCount > 0) {
    for (int i = 0; i < networkCount; ++i) {
      const String ssid = WiFi.SSID(i);
      if (ssid.isEmpty()) {
        continue;
      }

      WifiNetworkInfo network;
      network.ssid = ssid;
      network.rssi = WiFi.RSSI(i);
      network.authMode = static_cast<uint8_t>(WiFi.encryptionType(i));
      wifiNetworks_.push_back(network);
    }
  }

  WiFi.scanDelete();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);

  if (wifiNetworks_.empty()) {
    display_.renderStatus("Wi-Fi", tr2(TrKey2::NoNetworksFound), "");
    delay(1200);
    returnFromWifiFlow(millis());
    return;
  }

  const String savedSsid = configuredWifiSsid();
  std::stable_sort(wifiNetworks_.begin(), wifiNetworks_.end(),
                   [&savedSsid](const WifiNetworkInfo &left, const WifiNetworkInfo &right) {
                     const bool leftSaved = !savedSsid.isEmpty() && left.ssid == savedSsid;
                     const bool rightSaved = !savedSsid.isEmpty() && right.ssid == savedSsid;
                     if (leftSaved != rightSaved) {
                       return leftSaved;
                     }
                     if (left.rssi != right.rssi) {
                       return left.rssi > right.rssi;
                     }
                     return left.ssid < right.ssid;
                   });

  wifiNetworkMenuItems_.reserve(wifiNetworks_.size() + 1);
  for (const WifiNetworkInfo &network : wifiNetworks_) {
    wifiNetworkMenuItems_.push_back(
        {network.ssid, wifiSecurityLabel(network.authMode) + "  " + String(network.rssi) + " dBm"});
  }

  wifiNetworkSelectedIndex_ =
      wifiNetworkMenuItems_.size() > 1 ? kWifiNetworksFirstItemIndex : kWifiNetworksBackIndex;
  menuScreen_ = MenuScreen::WifiNetworks;
  renderWifiNetworks();
}

void App::renderWifiNetworks() {
  if (wifiNetworkMenuItems_.empty()) {
    display_.renderStatus("Wi-Fi", tr2(TrKey2::NoNetworksFound), "");
    return;
  }

  renderMenuAnyModeLibrary(wifiNetworkMenuItems_, wifiNetworkSelectedIndex_);
}

void App::selectWifiNetworkItem(uint32_t nowMs) {
  (void)nowMs;

  if (wifiNetworkSelectedIndex_ == kWifiNetworksBackIndex || wifiNetworkMenuItems_.size() <= 1) {
    returnFromWifiFlow(nowMs);
    return;
  }

  const size_t networkIndex = wifiNetworkSelectedIndex_ - kWifiNetworksFirstItemIndex;
  if (networkIndex >= wifiNetworks_.size()) {
    returnFromWifiFlow(nowMs);
    return;
  }

  const WifiNetworkInfo &network = wifiNetworks_[networkIndex];
  if (wifiNetworkRequiresPassword(network.authMode)) {
    const String savedPassword = findSavedWifiPassword(network.ssid);
    if (!savedPassword.isEmpty()) {
      // Already connected to this network before — reuse the remembered
      // password instead of asking for it again. Persist kPrefWifiSsid/Pass
      // only once the connection actually succeeds — see the matching
      // comment in commitTextEntry()'s WifiPassword case for why (a bad
      // remembered password used to get silently re-tried forever with no
      // way to fix it from the UI).
      const bool connected = attemptWifiConnection(network.ssid, savedPassword, nowMs);
      if (connected) {
        preferences_.putString(kPrefWifiSsid, network.ssid);
        preferences_.putString(kPrefWifiPass, savedPassword);
        returnFromWifiFlow(nowMs);
        return;
      }
      // Remembered password no longer works (network re-keyed, wrong
      // password saved earlier, etc.) — forget it so this branch isn't
      // taken again, then fall through to asking for a fresh password.
      forgetSavedWifiNetwork(network.ssid);
    }
    String initialValue;
    if (configuredWifiSsid() == network.ssid) {
      initialValue = preferredOtaConfig().wifiPassword;
    }
    openTextEntry(TextEntryPurpose::WifiPassword, network.ssid, "Password", "",
                  initialValue, network.ssid, true, kWifiPasswordMaxLength,
                  MenuScreen::WifiNetworks);
    return;
  }

  preferences_.putString(kPrefWifiSsid, network.ssid);
  preferences_.putString(kPrefWifiPass, "");
  attemptWifiConnection(network.ssid, "", nowMs);
  returnFromWifiFlow(nowMs);
}

// Łączy się realnie z zapisaną siecią i pokazuje wynik na ekranie — bez tego
// ekran po prostu chwalił się "Siec zapisana" nawet gdy haslo bylo zle albo
// siec byla poza zasiegiem, a kreator/Ustawienia jechaly dalej w ciemno,
// zostawiajac krok "auto-pobierz z GitHub" bez realnego Wi-Fi.
bool App::attemptWifiConnection(const String &ssid, const String &password, uint32_t nowMs) {
  (void)nowMs;
  display_.renderStatus("Wi-Fi", tr2(TrKey2::ConnectingToNetwork), ssid);

  OtaUpdater::Config config;
  config.wifiSsid = ssid;
  config.wifiPassword = password;
  const bool connected = otaUpdater_.connectWiFi(config, &App::handleStorageStatus, this);
  if (connected) {
    // Na błędzie connectWiFi() już posprzątała i zwolniła sesję sama (patrz
    // komentarz w OtaUpdater::connectWiFi) — drugie wołanie tutaj otwierało
    // okno na wyścig z inną sesją WiFi (np. w tle działającym OTA-checkiem).
    otaUpdater_.disconnectWiFi();
  }

  if (connected) {
    display_.renderStatus("Wi-Fi", tr(TrKey::Connected), ssid);
  } else {
    display_.renderStatus("Wi-Fi", tr2(TrKey2::ConnectFailedCheckPassword), ssid);
  }
  delay(1200);
  return connected;
}

void App::openTextEntry(TextEntryPurpose purpose, const String &title, const String &prompt,
                        const String &helperText, const String &initialValue,
                        const String &contextValue, bool masked, size_t maxLength,
                        MenuScreen returnScreen) {
  textEntrySession_ = TextEntrySession();
  textEntrySession_.active = true;
  textEntrySession_.purpose = purpose;
  textEntrySession_.mode = KeyboardMode::Lower;
  textEntrySession_.returnScreen = returnScreen;
  textEntrySession_.title = title;
  textEntrySession_.prompt = prompt;
  textEntrySession_.helperText = helperText;
  textEntrySession_.value = initialValue;
  textEntrySession_.contextValue = contextValue;
  textEntrySession_.maxLength = maxLength;
  textEntrySession_.masked = masked;
  textEntrySession_.revealValue = false;
  menuScreen_ = MenuScreen::TextEntry;
  rebuildTextEntryButtons();
  renderTextEntry();
}

void App::rebuildTextEntryButtons() {
  textEntryButtons_.clear();
  if (!textEntrySession_.active) {
    return;
  }

  const uint16_t rowPitch = kKeyboardRowHeight + kKeyboardRowGap;
  for (size_t rowIndex = 0; rowIndex < 3; ++rowIndex) {
    const String rowChars = keyboardRowText(static_cast<uint8_t>(textEntrySession_.mode), rowIndex);
    const size_t keyCount = rowChars.length();
    if (keyCount == 0) {
      continue;
    }

    const int availableWidth =
        BoardConfig::DISPLAY_WIDTH - (2 * kKeyboardMarginX) -
        static_cast<int>((keyCount - 1) * kKeyboardRowGap);
    const int keyWidth = std::max(28, availableWidth / static_cast<int>(keyCount));
    const int totalWidth =
        keyWidth * static_cast<int>(keyCount) + static_cast<int>((keyCount - 1) * kKeyboardRowGap);
    int x = std::max(0, (BoardConfig::DISPLAY_WIDTH - totalWidth) / 2);
    const int y = kKeyboardTopY + static_cast<int>(rowIndex * rowPitch);

    for (size_t charIndex = 0; charIndex < keyCount; ++charIndex) {
      TextEntryButton button;
      button.view.label = String(rowChars[charIndex]);
      button.view.x = static_cast<uint16_t>(x);
      button.view.y = static_cast<uint16_t>(y);
      button.view.width = static_cast<uint16_t>(keyWidth);
      button.view.height = kKeyboardRowHeight;
      button.action = TextEntryAction::Insert;
      button.payload = String(rowChars[charIndex]);
      textEntryButtons_.push_back(button);
      x += keyWidth + kKeyboardRowGap;
    }
  }

  struct ControlButtonDef {
    String label;
    TextEntryAction action;
    uint16_t units;
    bool accent;
    bool active;
  };

  const bool revealActive = textEntrySession_.masked && textEntrySession_.revealValue;
  const ControlButtonDef controls[] = {
      {"abc", TextEntryAction::SetLower, 11, false,
       textEntrySession_.mode == KeyboardMode::Lower},
      {"ABC", TextEntryAction::SetUpper, 11, false,
       textEntrySession_.mode == KeyboardMode::Upper},
      {"123", TextEntryAction::SetSymbols, 11, false,
       textEntrySession_.mode == KeyboardMode::Symbols},
      {"space", TextEntryAction::Space, 24, false, false},
      {"back", TextEntryAction::Backspace, 13, false, false},
      {textEntrySession_.masked ? (revealActive ? "hide" : "show") : "clear",
       textEntrySession_.masked ? TextEntryAction::ToggleMask : TextEntryAction::Clear, 13, false,
       revealActive},
      {"save", TextEntryAction::Save, 12, true, false},
      {"cancel", TextEntryAction::Cancel, 14, false, false},
  };

  uint16_t totalUnits = 0;
  for (const ControlButtonDef &control : controls) {
    totalUnits += control.units;
  }

  const size_t controlCount = sizeof(controls) / sizeof(controls[0]);
  const int totalGapWidth = static_cast<int>((controlCount - 1) * kKeyboardRowGap);
  const int availableWidth = BoardConfig::DISPLAY_WIDTH - (2 * kKeyboardMarginX) - totalGapWidth;
  int remainingWidth = availableWidth;
  uint16_t x = kKeyboardMarginX;
  const uint16_t y = kKeyboardTopY + static_cast<uint16_t>(3 * rowPitch);

  for (size_t i = 0; i < controlCount; ++i) {
    const ControlButtonDef &control = controls[i];
    int width = remainingWidth;
    if (i + 1 < controlCount) {
      width = (availableWidth * control.units) / totalUnits;
      remainingWidth -= width;
    }

    TextEntryButton button;
    button.view.label = control.label;
    button.view.x = x;
    button.view.y = y;
    button.view.width = static_cast<uint16_t>(std::max(28, width));
    button.view.height = kKeyboardRowHeight;
    button.view.accent = control.accent;
    button.view.active = control.active;
    button.action = control.action;
    textEntryButtons_.push_back(button);

    x = static_cast<uint16_t>(x + button.view.width + kKeyboardRowGap);
  }
}

void App::renderTextEntry() {
  if (!textEntrySession_.active) {
    return;
  }

  const String visibleValue =
      (textEntrySession_.masked && !textEntrySession_.revealValue)
          ? maskedValue(textEntrySession_.value)
          : textEntrySession_.value;

  std::vector<DisplayManager::Button> buttons;
  buttons.reserve(textEntryButtons_.size());
  for (const TextEntryButton &button : textEntryButtons_) {
    buttons.push_back(button.view);
  }

  display_.renderTextEntry(textEntrySession_.title, textEntrySession_.prompt, visibleValue,
                           textEntrySession_.helperText, buttons);
}

bool App::handleTextEntryTap(uint16_t x, uint16_t y, uint32_t nowMs) {
  if (!textEntrySession_.active) {
    return false;
  }

  for (size_t i = 0; i < textEntryButtons_.size(); ++i) {
    const DisplayManager::Button &button = textEntryButtons_[i].view;
    const uint16_t maxX = button.x + button.width;
    const uint16_t maxY = button.y + button.height;
    if (x < button.x || x > maxX || y < button.y || y > maxY) {
      continue;
    }

    // Contact-bounce guard (see lastFiredTextEntryButtonIndex_): a physical
    // tap that briefly loses and regains contact can reach here twice for
    // the same key a few ms apart — this used to insert the same character
    // (or fire Backspace/mode-switch) twice for one tap. Swallow the repeat.
    if (lastFiredTextEntryButtonIndex_ == static_cast<int>(i) &&
        (nowMs - lastFiredTextEntryAtMs_) < kTextEntryTapDebounceMs) {
      return true;
    }
    lastFiredTextEntryButtonIndex_ = static_cast<int>(i);
    lastFiredTextEntryAtMs_ = nowMs;

    // Letter/space/backspace keys get typed immediately — never delay the
    // character landing, or typing at any real speed drops keystrokes
    // whenever the next tap lands inside the still-pending flash window.
    // The visible "press" flash still happens, just *after* the character
    // is already in, as a pure highlight-then-clear with no action queued
    // (see pendingTextEntryFlashIsPostActionOnly_).
    const TextEntryAction action = textEntryButtons_[i].action;
    const bool isTypingKey = action == TextEntryAction::Insert ||
                             action == TextEntryAction::Space ||
                             action == TextEntryAction::Backspace;
    if (isTypingKey) {
      activateTextEntryButton(i, nowMs);
      if (i < textEntryButtons_.size()) {
        textEntryButtons_[i].view.armed = true;
        pendingTextEntryFlashIndex_ = static_cast<int>(i);
        pendingTextEntryFlashFireAtMs_ = nowMs + kPressFlashMs;
        pendingTextEntryFlashIsPostActionOnly_ = true;
        renderTextEntry();
      }
      return true;
    }

    // Everything else (mode switches, clear, save, cancel...) is tapped
    // rarely, not spammed — flash first, then fire, same as the menu grid.
    textEntryButtons_[i].view.armed = true;
    pendingTextEntryFlashIndex_ = static_cast<int>(i);
    pendingTextEntryFlashFireAtMs_ = nowMs + kPressFlashMs;
    pendingTextEntryFlashIsPostActionOnly_ = false;
    renderTextEntry();
    return true;
  }

  return false;
}

void App::firePendingTextEntryFlash(uint32_t nowMs) {
  if (pendingTextEntryFlashIndex_ == -1 || nowMs < pendingTextEntryFlashFireAtMs_) {
    return;
  }
  const size_t index = static_cast<size_t>(pendingTextEntryFlashIndex_);
  const bool postActionOnly = pendingTextEntryFlashIsPostActionOnly_;
  pendingTextEntryFlashIndex_ = -1;
  if (postActionOnly) {
    if (index < textEntryButtons_.size()) {
      textEntryButtons_[index].view.armed = false;
      renderTextEntry();
    }
    return;
  }
  activateTextEntryButton(index, nowMs);
}

void App::activateTextEntryButton(size_t buttonIndex, uint32_t nowMs) {
  if (buttonIndex >= textEntryButtons_.size()) {
    return;
  }

  TextEntryButton &button = textEntryButtons_[buttonIndex];
  switch (button.action) {
    case TextEntryAction::Insert:
      if (textEntrySession_.value.length() < textEntrySession_.maxLength) {
        textEntrySession_.value += button.payload;
      }
      break;
    case TextEntryAction::SetLower:
      textEntrySession_.mode = KeyboardMode::Lower;
      break;
    case TextEntryAction::SetUpper:
      textEntrySession_.mode = KeyboardMode::Upper;
      break;
    case TextEntryAction::SetSymbols:
      textEntrySession_.mode = KeyboardMode::Symbols;
      break;
    case TextEntryAction::Space:
      if (textEntrySession_.value.length() < textEntrySession_.maxLength) {
        textEntrySession_.value += ' ';
      }
      break;
    case TextEntryAction::Backspace:
      if (!textEntrySession_.value.isEmpty()) {
        textEntrySession_.value.remove(textEntrySession_.value.length() - 1);
      }
      break;
    case TextEntryAction::Clear:
      textEntrySession_.value = "";
      break;
    case TextEntryAction::ToggleMask:
      if (textEntrySession_.masked) {
        textEntrySession_.revealValue = !textEntrySession_.revealValue;
      }
      break;
    case TextEntryAction::Save:
      commitTextEntry(nowMs);
      return;
    case TextEntryAction::Cancel:
      if (textEntrySession_.purpose == TextEntryPurpose::SavePointName &&
          savePointQuickSaveFromReader_) {
        savePointQuickSaveFromReader_ = false;
        textEntrySession_ = TextEntrySession();
        textEntryButtons_.clear();
        menuScreen_ = MenuScreen::Main;
        setState(AppState::Paused, nowMs);
        return;
      }
      menuScreen_ = textEntrySession_.returnScreen;
      textEntrySession_ = TextEntrySession();
      textEntryButtons_.clear();
      renderMenu();
      return;
  }

  rebuildTextEntryButtons();
  renderTextEntry();
}

void App::commitTextEntry(uint32_t nowMs) {
  (void)nowMs;

  switch (textEntrySession_.purpose) {
    case TextEntryPurpose::WifiPassword: {
      if (textEntrySession_.value.isEmpty()) {
        display_.renderStatus("Wi-Fi", tr2(TrKey2::PasswordRequired), textEntrySession_.contextValue);
        delay(1000);
        renderTextEntry();
        return;
      }

      const String ssid = textEntrySession_.contextValue;
      const String password = textEntrySession_.value;
      // Persist/remember the password only once the connection actually
      // succeeds — persisting it unconditionally (the old behaviour) meant a
      // wrong password got saved as both the active config AND a
      // "remembered network" before the result was known. The next tap on
      // this SSID then took the findSavedWifiPassword() fast path in
      // selectWifiNetworkItem() and silently retried the same wrong
      // password forever, with no path back to the password screen.
      const bool connected = attemptWifiConnection(ssid, password, nowMs);
      if (connected) {
        preferences_.putString(kPrefWifiSsid, ssid);
        preferences_.putString(kPrefWifiPass, password);
        rememberWifiNetwork(ssid, password);
        textEntrySession_ = TextEntrySession();
        textEntryButtons_.clear();
        returnFromWifiFlow(nowMs);
        return;
      }
      // Wrong password (or network briefly out of reach) — stay on this
      // same screen with the field cleared so the user can just retype,
      // instead of being bounced back to the network list.
      textEntrySession_.value = "";
      rebuildTextEntryButtons();
      renderTextEntry();
      return;
    }
    case TextEntryPurpose::OtaOwner: {
      const String owner = textEntrySession_.value;
      if (owner.isEmpty()) {
        preferences_.remove(kPrefOtaOwner);
        display_.renderStatus("OTA", tr2(TrKey2::ResetToDefault), "");
      } else {
        preferences_.putString(kPrefOtaOwner, owner);
        display_.renderStatus("OTA", tr2(TrKey2::OwnerSaved), owner);
      }
      delay(900);
      textEntrySession_ = TextEntrySession();
      textEntryButtons_.clear();
      openWifiSettings();
      return;
    }
    case TextEntryPurpose::SavePointName: {
      // Create save point with the entered name
      String name = textEntrySession_.value;
      if (name.isEmpty()) {
        // Use context value as default name
        name = textEntrySession_.contextValue;
      }
      textEntrySession_ = TextEntrySession();
      textEntryButtons_.clear();
      finishSavePointCreation(name, nowMs);
      return;
    }
    case TextEntryPurpose::PresetName: {
      executeSavePreset(nowMs);
      return;
    }
    case TextEntryPurpose::None:
    default:
      menuScreen_ = textEntrySession_.returnScreen;
      textEntrySession_ = TextEntrySession();
      textEntryButtons_.clear();
      renderMenu();
      return;
  }
}

void App::openTypographyTuning() {
  if (typographyTuningSelectedIndex_ >= TypographyTuningItemCount) {
    typographyTuningSelectedIndex_ = TypographyTuningFontSize;
  }
  if (typographyTuningSelectedIndex_ == TypographyTuningBack) {
    typographyTuningSelectedIndex_ = TypographyTuningFontSize;
  }
  menuScreen_ = MenuScreen::TypographyTuning;
  renderTypographyTuning();
}

void App::selectTypographyTuningItem(uint32_t nowMs) {
  switch (typographyTuningSelectedIndex_) {
    case TypographyTuningBack:
      settingsSelectedIndex_ = kSettingsHomeTypographyIndex;
      menuScreen_ = MenuScreen::SettingsHome;
      rebuildSettingsMenuItems();
      renderSettings();
      return;
    case TypographyTuningFontSize:
      // Was tap-to-cycle; now opens the same drag-slider widget as WPM, so
      // it matches the gesture the user actually asked for.
      openTypographyValueEditor(TypographyValueEditorTarget::FontSize, nowMs);
      return;
    case TypographyTuningTypeface:
      // 10 fonts is too many to cycle one at a time (see font picker below)
      // — tapping this row now opens a grid where each button previews its
      // own name in its own font, tap one to select it directly.
      openTypographyFontPicker();
      return;
    case TypographyTuningPhantomWords:
      togglePhantomWords(nowMs);
      return;
    case TypographyTuningFocusHighlight:
      typographyConfig_.focusHighlight = !typographyConfig_.focusHighlight;
      preferences_.putBool(kPrefTypographyFocusHighlight, typographyConfig_.focusHighlight);
      break;
    case TypographyTuningTracking:
      openTypographyValueEditor(TypographyValueEditorTarget::Tracking, nowMs);
      return;
    case TypographyTuningAnchor:
      openTypographyValueEditor(TypographyValueEditorTarget::Anchor, nowMs);
      return;
    case TypographyTuningGuideWidth:
      openTypographyValueEditor(TypographyValueEditorTarget::GuideWidth, nowMs);
      return;
    case TypographyTuningGuideGap:
      openTypographyValueEditor(TypographyValueEditorTarget::GuideGap, nowMs);
      return;
    case TypographyTuningReset:
      openTypographyResetConfirm();
      return;
    default:
      return;
  }

  applyTypographySettings(nowMs);
}

void App::openTypographyResetConfirm() {
  typographyResetConfirmSelectedIndex_ = TypographyResetConfirmNo;
  menuScreen_ = MenuScreen::TypographyResetConfirm;
  renderTypographyResetConfirm();
}

void App::selectTypographyResetConfirmItem(uint32_t nowMs) {
  if (typographyResetConfirmSelectedIndex_ != TypographyResetConfirmYes) {
    menuScreen_ = MenuScreen::TypographyTuning;
    renderTypographyTuning();
    return;
  }

  typographyConfig_ = defaultTypographyConfig();
  preferences_.putUChar(kPrefReaderTypeface, static_cast<uint8_t>(typographyConfig_.typeface));
  preferences_.putBool(kPrefTypographyFocusHighlight, typographyConfig_.focusHighlight);
  preferences_.putChar(kPrefTypographyTracking, typographyConfig_.trackingPx);
  preferences_.putUChar(kPrefTypographyAnchor, typographyConfig_.anchorPercent);
  preferences_.putUChar(kPrefTypographyGuideWidth, typographyConfig_.guideHalfWidth);
  preferences_.putUChar(kPrefTypographyGuideGap, typographyConfig_.guideGap);
  applyTypographySettings(nowMs);

  menuScreen_ = MenuScreen::TypographyTuning;
  renderTypographyTuning();
}

void App::cycleTypographyPreviewSample(int direction) {
  if (kTypographyPreviewWordCount == 0 || direction == 0) {
    return;
  }

  const int current = static_cast<int>(typographyPreviewSampleIndex_);
  int next = current + direction;
  if (next < 0) {
    next = static_cast<int>(kTypographyPreviewWordCount) - 1;
  } else if (next >= static_cast<int>(kTypographyPreviewWordCount)) {
    next = 0;
  }
  typographyPreviewSampleIndex_ = static_cast<size_t>(next);
  renderTypographyTuning();
}

// Replaces the old tap-to-cycle Krój behavior (annoying once there were 10
// typefaces instead of 3): a grid of buttons, one per font, each showing its
// own name live in that actual font (annotateTypographyFontPickerButton())
// so you can see what a krój looks like before picking it.
void App::openTypographyFontPicker() {
  typographyFontPickerMenuItems_.clear();
  typographyFontPickerTypefaceForIndex_.clear();
  typographyFontPickerMenuItems_.reserve(
      static_cast<size_t>(DisplayManager::ReaderTypeface::Count) + 1);
  typographyFontPickerTypefaceForIndex_.reserve(
      static_cast<size_t>(DisplayManager::ReaderTypeface::Count) + 1);
  typographyFontPickerMenuItems_.push_back(uiText(UiText::Back));
  typographyFontPickerTypefaceForIndex_.push_back(DisplayManager::ReaderTypeface::Count);

  // Rows are filtered to fonts actually present: the 3 built-in (flash)
  // faces are always available; the 17 SD-backed ones only show up once
  // downloadAsset() has put both their .fnt files on the card (see
  // maybeAutoDownloadFonts()) — no point offering a font that will just
  // fall back to Atkinson.
  size_t currentSelection = 0;
  for (uint8_t i = 0; i < static_cast<uint8_t>(DisplayManager::ReaderTypeface::Count); ++i) {
    const auto typeface = static_cast<DisplayManager::ReaderTypeface>(i);
    if (!DisplayManager::isTypefaceAvailableOnSd(typeface)) {
      continue;
    }
    typographyFontPickerMenuItems_.push_back(typefaceDisplayName(typeface));
    typographyFontPickerTypefaceForIndex_.push_back(typeface);
    if (typeface == typographyConfig_.typeface) {
      currentSelection = typographyFontPickerMenuItems_.size() - 1;
    }
  }

  typographyFontPickerSelectedIndex_ = currentSelection;
  menuScreen_ = MenuScreen::TypographyFontPicker;
  renderTypographyFontPicker();
}

void App::selectTypographyFontPickerItem(uint32_t nowMs) {
  if (typographyFontPickerSelectedIndex_ == 0) {
    if (wizardFontPickerActive_) {
      wizardFontPickerActive_ = false;
      openWelcomeReadingMode();
      return;
    }
    menuScreen_ = MenuScreen::TypographyTuning;
    renderTypographyTuning();
    return;
  }

  if (typographyFontPickerSelectedIndex_ >= typographyFontPickerTypefaceForIndex_.size()) {
    return;
  }

  typographyConfig_.typeface = typographyFontPickerTypefaceForIndex_[typographyFontPickerSelectedIndex_];
  preferences_.putUChar(kPrefReaderTypeface, static_cast<uint8_t>(typographyConfig_.typeface));
  applyTypographySettings(nowMs);

  if (display_.consumeFontLoadFailure()) {
    display_.renderStatus("Font", "Not found on SD", "Using Atkinson");
    delay(1400);
  }

  if (wizardFontPickerActive_) {
    wizardFontPickerActive_ = false;
    openWelcomeReadingMode();
    return;
  }

  menuScreen_ = MenuScreen::TypographyTuning;
  renderTypographyTuning();
}

void App::renderTypographyFontPicker() {
  const String title = wizardFontPickerActive_ ? tr3(TrKey3::WelcomeChooseFontTitle) : uiText(UiText::Typeface);
  renderItemGrid(title, typographyFontPickerMenuItems_, typographyFontPickerSelectedIndex_);
}

void App::annotateTypographyFontPickerButton(DisplayManager::Button &button,
                                              size_t canonicalIndex) const {
  // canonicalIndex is 1-based here (0 is Back, pulled into the corner
  // button before this ever runs) and mirrors typographyFontPickerTypefaceForIndex_,
  // built in the same filtered order by openTypographyFontPicker() above.
  if (canonicalIndex == 0 || canonicalIndex >= typographyFontPickerTypefaceForIndex_.size()) {
    return;
  }
  button.previewTypeface = typographyFontPickerTypefaceForIndex_[canonicalIndex];
}

void App::rebuildSettingsMenuItems() {
  settingsMenuItems_.clear();
  settingsMenuItems_.reserve(SettingsItemCount);
  if (menuScreen_ == MenuScreen::SettingsHome) {
    // Nowy układ: 6 pozycji zawsze widocznych, dev-only na końcu.
    settingsMenuItems_.push_back(uiText(UiText::Back));
    settingsMenuItems_.push_back(tr3(TrKey3::ReadingSettings));   // 1 = Reading settings
    settingsMenuItems_.push_back(uiText(UiText::Display));         // 2 = Display
    settingsMenuItems_.push_back(uiText(UiText::TypographyTune));  // 3 = Typography (always)
    settingsMenuItems_.push_back(tr(TrKey::Connectivity));         // 4
    settingsMenuItems_.push_back(String(tr3(TrKey3::AdvancedModeColon)) +  // 5 = przełącznik
                                 onOffLabel(devModeEnabled()));
    settingsMenuItems_.push_back(tr(TrKey::AboutHelp));            // 6
    if (devModeEnabled()) {
      settingsMenuItems_.push_back(tr3(TrKey3::PresetsLabel));  // 7 dev
      settingsMenuItems_.push_back(tr(TrKey::WifiAdvanced));       // 8 dev
      settingsMenuItems_.push_back(firmwareUpdateMenuLabel());     // 9 dev
    }
  } else if (menuScreen_ == MenuScreen::SettingsConnectivity) {
    settingsMenuItems_.push_back(uiText(UiText::Back));
    // 1. Wi-Fi
    settingsMenuItems_.push_back(String(tr(TrKey::HomeWifi)) +
                                 storedOrFallbackLabel(configuredWifiSsid(),
                                                       tr(TrKey::NotSet)));
#if FLOWER_BLE_ENABLED
    // 2. Bluetooth
    settingsMenuItems_.push_back(
        String("Bluetooth: ") +
        (ble_.isActive()
             ? (ble_.isConnected() ? tr(TrKey::Connected)
                                   : tr(TrKey::Yes))
             : tr(TrKey::No)));
#endif
    // 3. Synchronizacja z telefonem
    const bool syncActive = state_ == AppState::CompanionSync;
    settingsMenuItems_.push_back(
        String(tr(TrKey::PhoneSync)) +
        (syncActive ? tr(TrKey::Yes) : tr(TrKey::No)));
#if RSVP_USB_TRANSFER_ENABLED
    // 4. Kopiuj przez USB
    settingsMenuItems_.push_back(uiText(UiText::UsbTransfer));
#endif
  } else if (menuScreen_ == MenuScreen::SettingsAbout) {
    settingsMenuItems_.push_back(uiText(UiText::Back));
    settingsMenuItems_.push_back(String(tr(TrKey::Version)) +
                                 otaUpdater_.currentVersion());
    settingsMenuItems_.push_back(tr(TrKey::BrandLabel));
    settingsMenuItems_.push_back(tr2(TrKey2::SdCardCheck));
    settingsMenuItems_.push_back(tr3(TrKey3::TutorialLabel));
    if (devModeEnabled()) {
      settingsMenuItems_.push_back(tr(TrKey::DevModeOn));
    }
  } else if (menuScreen_ == MenuScreen::WelcomeLanguage) {
    // First-run wizard — krok 1/2. Lista języków po ich własnych nazwach
    // (te się nie tłumaczą — Polski to zawsze Polski, niezależnie od języka UI).
    settingsMenuItems_.push_back("English");
    settingsMenuItems_.push_back("Polski");
    settingsMenuItems_.push_back("Deutsch");
    settingsMenuItems_.push_back("Espanol");
    settingsMenuItems_.push_back("Francais");
    settingsMenuItems_.push_back("Romana");
  } else if (menuScreen_ == MenuScreen::WelcomeTheme) {
    // First-run wizard — krok 2/5. Po wyborze języka w step 1 (już zapisanym
    // w uiLanguage_), te labele lecą przez UiText więc są przetłumaczone.
    settingsMenuItems_.push_back(uiText(UiText::Light));
    settingsMenuItems_.push_back(uiText(UiText::Dark));
    settingsMenuItems_.push_back(uiText(UiText::Night));
  } else if (menuScreen_ == MenuScreen::WelcomeHighlightColor) {
    // First-run wizard — krok 3/5. Kolor podświetlenia litery.
    settingsMenuItems_.push_back(tr3(TrKey3::ColorRed));
    settingsMenuItems_.push_back(tr3(TrKey3::ColorBlue));
    settingsMenuItems_.push_back(tr3(TrKey3::ColorGreen));
    settingsMenuItems_.push_back(tr3(TrKey3::ColorYellow));
    settingsMenuItems_.push_back(tr3(TrKey3::ColorOrange));
    settingsMenuItems_.push_back(tr3(TrKey3::ColorPurple));
  } else if (menuScreen_ == MenuScreen::WelcomeReadingMode) {
    // First-run wizard — krok 2.2. Sposób czytania. Podgląd każdego trybu
    // to teraz osobna ikonka oka w rogu (applyReadingModePreviewButtonLayout()),
    // nie osobny kafelek na liście.
    settingsMenuItems_.push_back("RSVP");
    settingsMenuItems_.push_back(tr3(TrKey3::WelcomeReadingModeScrollRow));
  } else if (menuScreen_ == MenuScreen::SettingsDisplay) {
    settingsMenuItems_.push_back(uiText(UiText::Back));
    settingsMenuItems_.push_back(String(uiText(UiText::Theme)) + ": " + themeModeLabel());
    settingsMenuItems_.push_back(uiText(UiText::Brightness) + ": " +
                                 String(currentBrightnessPercent()) + "%");
    settingsMenuItems_.push_back(String(tr(TrKey::ReaderHand)) + handednessLabel());
    settingsMenuItems_.push_back(String(tr3(TrKey3::SaveBtnColon)) +
                                 onOffLabel(savePointButtonVisible_));
    settingsMenuItems_.push_back(String(tr(TrKey::FooterLabel)) +
                                 footerMetricModeLabel());
    settingsMenuItems_.push_back(String(tr(TrKey::BatteryLabel)) +
                                 batteryLabelModeLabel());
    settingsMenuItems_.push_back(String(tr(TrKey::Screensaver)) +
                                 screensaverModeLabel() + " >");
    settingsMenuItems_.push_back(String(tr(TrKey::ReadingBattery)) +
                                 onOffLabel(readerBatteryVisibleWhilePlaying_));
    settingsMenuItems_.push_back(String(tr(TrKey::ReadingChapter)) +
                                 onOffLabel(readerChapterVisibleWhilePlaying_));
    settingsMenuItems_.push_back(String(tr(TrKey::ReadingPercent)) +
                                 onOffLabel(readerProgressVisibleWhilePlaying_));
    settingsMenuItems_.push_back(uiText(UiText::Language) + ": " + uiLanguageLabel());
    settingsMenuItems_.push_back(String(tr3(TrKey3::FocusColorColon)) + focusColorLabel());
    settingsMenuItems_.push_back(String(tr3(TrKey3::HelpQColon)) +
                                 onOffLabel(showHelpHints_));
    settingsMenuItems_.push_back(String(tr3(TrKey3::NavigationColon)) + navModeLabel());
    settingsMenuItems_.push_back(String(tr3(TrKey3::SavePointNameModeColon)) +
                                 savePointNameModeLabel());
  } else if (menuScreen_ == MenuScreen::ScreensaverSettings) {
    settingsMenuItems_.push_back(uiText(UiText::Back));
    settingsMenuItems_.push_back(String(tr(TrKey::ScreensaverStyle)) +
                                 screensaverModeLabel());
    settingsMenuItems_.push_back(String(tr(TrKey::ScreensaverTimeout)) +
                                 screensaverTimeoutLabel());
    settingsMenuItems_.push_back(String(tr(TrKey::ScreensaverAutoOff)) +
                                 screensaverAutoOffLabel());
    settingsMenuItems_.push_back(String(tr(TrKey::ScreensaverSleepGuard)) +
                                 screensaverSleepGuardLabel());
    settingsMenuItems_.push_back(tr(TrKey::ScreensaverPreview));
  } else if (menuScreen_ == MenuScreen::SettingsPacing) {
    settingsMenuItems_.push_back(uiText(UiText::Back));
    settingsMenuItems_.push_back(uiText(UiText::ReadingMode) + ": " + readerModeLabel());
    if (readerMode_ == ReaderMode::Scroll) {
      settingsMenuItems_.push_back(uiText(UiText::FontSize) + ": " + scrollFontSizeLabel());
      settingsMenuItems_.push_back(uiText(UiText::ScrollLineSpacing) + ": " + scrollLineSpacingLabel());
      settingsMenuItems_.push_back(uiText(UiText::ScrollMargins) + ": " + scrollMarginLabel());
      settingsMenuItems_.push_back(String("Preview"));
    } else {
      settingsMenuItems_.push_back(String(tr(TrKey::PauseBehaviour)) +
                                   pauseModeLabel());
      settingsMenuItems_.push_back(String(tr(TrKey::BaseSpeed)) +
                                   String(reader_.wpm()) + " WPM");
      settingsMenuItems_.push_back(uiText(UiText::LongWords) + ": " +
                                   pacingDelayLabel(pacingLongWordDelayMs_));
      settingsMenuItems_.push_back(uiText(UiText::Complexity) + ": " +
                                   pacingDelayLabel(pacingComplexWordDelayMs_));
      settingsMenuItems_.push_back(uiText(UiText::Punctuation) + ": " +
                                   pacingDelayLabel(pacingPunctuationDelayMs_));
      settingsMenuItems_.push_back(uiText(UiText::ResetPacing));
    }
  } else if (menuScreen_ == MenuScreen::WifiSettings) {
    settingsMenuItems_.push_back(uiText(UiText::Back));
    settingsMenuItems_.push_back(String(tr(TrKey::Network)) +
                                 storedOrFallbackLabel(configuredWifiSsid(),
                                                       tr(TrKey::NotSet)));
    settingsMenuItems_.push_back(tr(TrKey::ChooseNetwork));
    settingsMenuItems_.push_back(tr(TrKey::ForgetNetwork));
    // Advanced — pokazywane tylko w trybie developera. Klient nie potrzebuje
    // grzebać w „OTA Owner" ani „Auto OTA"; te rzeczy steruje się z aplikacji.
    if (devModeEnabled()) {
      settingsMenuItems_.push_back(String("Auto OTA: ") +
                                   (otaAutoCheckEnabled() ? tr(TrKey::Yes) : tr(TrKey::No)));
      settingsMenuItems_.push_back(String("OTA Owner: ") + otaOwnerLabel());
      settingsMenuItems_.push_back(String("OTA Channel: ") + otaChannelLabel());
    }
  }

  if (settingsSelectedIndex_ >= settingsMenuItems_.size()) {
    settingsSelectedIndex_ = kSettingsBackIndex;
  }
}

void App::applyPacingSettings() {
  ReadingLoop::PacingConfig pacingConfig;
  pacingConfig.longWordDelayMs = pacingLongWordDelayMs_;
  pacingConfig.complexWordDelayMs = pacingComplexWordDelayMs_;
  pacingConfig.punctuationDelayMs = pacingPunctuationDelayMs_;
  reader_.setPacingConfig(pacingConfig);

  Serial.printf("[settings] pacing long=%u ms complexity=%u ms punctuation=%u ms\n",
                static_cast<unsigned int>(pacingLongWordDelayMs_),
                static_cast<unsigned int>(pacingComplexWordDelayMs_),
                static_cast<unsigned int>(pacingPunctuationDelayMs_));
  // Defer the (potentially slow, screen-clobbering) rebuild while still on
  // a pacing-related screen — it draws its own "Reading time..." status
  // screen over whatever's currently showing, which on PacingDelayEditor
  // meant the slider screen visibly flickered to that status screen and
  // back on every single finger-release. flushPendingTimeEstimateRebuild()
  // (called when leaving either screen) applies it once, off-screen.
  if (state_ == AppState::Menu &&
      (menuScreen_ == MenuScreen::SettingsPacing || menuScreen_ == MenuScreen::PacingDelayEditor)) {
    pacingCacheDirty_ = true;
  } else {
    rebuildTimeEstimateCache();
  }
}

void App::flushPendingTimeEstimateRebuild() {
  if (!pacingCacheDirty_) {
    return;
  }
  rebuildTimeEstimateCache();
}

String App::otaOwnerLabel() {
  if (preferences_.isKey(kPrefOtaOwner)) {
    return preferences_.getString(kPrefOtaOwner, "");
  }
  OtaUpdater::Config cfg;
  otaUpdater_.loadConfig(cfg);
  return cfg.githubOwner;
}

bool App::devModeEnabled() {
  return preferences_.getBool(kPrefDevMode, false);
}

String App::firmwareVersionLabel() const { return otaUpdater_.currentVersion(); }

void App::setDevModeEnabled(bool enabled) {
  preferences_.putBool(kPrefDevMode, enabled);
  if (state_ == AppState::Menu &&
      (menuScreen_ == MenuScreen::WifiSettings || menuScreen_ == MenuScreen::SettingsHome ||
       menuScreen_ == MenuScreen::SettingsAbout)) {
    rebuildSettingsMenuItems();
  }
}

bool App::isSettingsListScreen() const {
  return menuScreen_ == MenuScreen::SettingsHome ||
         menuScreen_ == MenuScreen::SettingsDisplay ||
         menuScreen_ == MenuScreen::SettingsPacing ||
         menuScreen_ == MenuScreen::SettingsConnectivity ||
         menuScreen_ == MenuScreen::SettingsAbout ||
         menuScreen_ == MenuScreen::ScreensaverSettings ||
         menuScreen_ == MenuScreen::WifiSettings ||
         menuScreen_ == MenuScreen::Presets ||
         menuScreen_ == MenuScreen::PresetsDeleteConfirm ||
         menuScreen_ == MenuScreen::WelcomeLanguage ||
         menuScreen_ == MenuScreen::WelcomeTheme ||
         menuScreen_ == MenuScreen::WelcomeHighlightColor ||
         menuScreen_ == MenuScreen::WelcomeReadingMode;
}

void App::showHelpForCurrentItem() {
  if (!showHelpHints_) return;
  if (settingsSelectedIndex_ == kSettingsBackIndex) return;

  const HelpEntry* entry = nullptr;

  if (menuScreen_ == MenuScreen::SettingsDisplay) {
    entry = HelpTexts::getDisplayHelp(settingsSelectedIndex_ - 1);
  } else if (menuScreen_ == MenuScreen::SettingsPacing) {
    if (readerMode_ == ReaderMode::Scroll) {
      entry = HelpTexts::getPacingScrollHelp(settingsSelectedIndex_ - 1);
    } else {
      entry = HelpTexts::getPacingHelp(settingsSelectedIndex_ - 1);
    }
  }

  if (entry == nullptr) return;

  const bool isPl = (uiLanguage_ == UiLanguage::Polish);
  helpPopupTitle_ = isPl ? entry->titlePl : entry->titleEn;
  helpPopupDesc_ = isPl ? entry->line1Pl : entry->line1En;
  showingHelpPopup_ = true;

  const char* line2 = isPl ? entry->line2Pl : entry->line2En;
  display_.renderStatus(String(helpPopupTitle_), String(helpPopupDesc_),
                        line2 ? String(line2) : "");
}

void App::dismissHelpPopup(uint32_t nowMs) {
  (void)nowMs;
  showingHelpPopup_ = false;
  helpPopupTitle_ = nullptr;
  helpPopupDesc_ = nullptr;
  rebuildSettingsMenuItems();
  renderSettings();
}

const char *App::tr(TrKey key) const {
  return Translations::tr(uiLanguage_, key);
}

const char *App::tr2(TrKey2 key) const {
  return Translations2::tr2(uiLanguage_, key);
}

const char *App::tr3(TrKey3 key) const {
  return Translations3::tr3(uiLanguage_, key);
}

// ─── SettingsConnectivity ────────────────────────────────────────────────────

void App::openSettingsConnectivity() {
  menuScreen_ = MenuScreen::SettingsConnectivity;
  settingsSelectedIndex_ = kSettingsConnWifiIndex;
  rebuildSettingsMenuItems();
  renderSettings();
}

void App::selectSettingsConnectivityItem(uint32_t nowMs) {
  switch (settingsSelectedIndex_) {
    case kSettingsBackIndex:
      settingsSelectedIndex_ = kSettingsHomeConnectivityIndex;
      menuScreen_ = MenuScreen::SettingsHome;
      rebuildSettingsMenuItems();
      renderSettings();
      return;
    case kSettingsConnWifiIndex:
      openWifiSettings();
      return;
#if FLOWER_BLE_ENABLED
    case kSettingsConnBluetoothIndex: {
      const bool wasOn = ble_.isActive();
      if (wasOn) {
        ble_.stop();
        preferences_.putBool(kPrefBleEnabled, false);
        Serial.println("[app] BLE turned OFF by user");
      } else {
        ble_.begin(this);
        preferences_.putBool(kPrefBleEnabled, true);
        Serial.printf("[app] BLE turned ON by user (name=%s)\n", ble_.deviceName().c_str());
      }
      rebuildSettingsMenuItems();
      showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
      renderSettings();
      return;
    }
#endif
    case kSettingsConnSyncToggleIndex:
      if (state_ == AppState::CompanionSync) {
        exitCompanionSync(nowMs);
      } else {
        enterCompanionSync(nowMs);
      }
      return;
#if RSVP_USB_TRANSFER_ENABLED
    case kSettingsConnUsbIndex:
      enterUsbTransfer(nowMs);
      return;
#endif
    default:
      return;
  }
}

// ─── SettingsAbout ───────────────────────────────────────────────────────────

void App::openSettingsAbout() {
  menuScreen_ = MenuScreen::SettingsAbout;
  settingsSelectedIndex_ = kSettingsAboutVersionIndex;
  aboutTapCount_ = 0;
  aboutLastTapMs_ = 0;
  rebuildSettingsMenuItems();
  renderSettings();
}

void App::selectSettingsAboutItem(uint32_t nowMs) {
  switch (settingsSelectedIndex_) {
    case kSettingsBackIndex:
      settingsSelectedIndex_ = kSettingsHomeAboutIndex;
      menuScreen_ = MenuScreen::SettingsHome;
      aboutTapCount_ = 0;
      rebuildSettingsMenuItems();
      renderSettings();
      return;
    case kSettingsAboutVersionIndex: {
      // Łańcuch tapów odblokowuje dev mode (jak Android Build number).
      constexpr uint32_t kTapWindowMs = 1500;
      constexpr uint8_t kTapsToUnlock = 10;
      if (aboutLastTapMs_ != 0 && nowMs - aboutLastTapMs_ > kTapWindowMs) {
        aboutTapCount_ = 0;
      }
      aboutLastTapMs_ = nowMs;
      aboutTapCount_++;
      bool justUnlocked = false;
      if (!devModeEnabled() && aboutTapCount_ >= kTapsToUnlock) {
        setDevModeEnabled(true);
        aboutTapCount_ = 0;
        justUnlocked = true;
      }
      rebuildSettingsMenuItems();
      if (justUnlocked) {
        showGridToast(String(tr3(TrKey3::AdvancedModeColon)) + onOffLabel(true), nowMs);
      }
      renderSettings();
      return;
    }
    case kSettingsAboutSdCardIndex:
      runSdCardCheck(nowMs);
      return;
    case kSettingsAboutTutorialIndex:
      openTutorialStep1();
      return;
    case kSettingsAboutDevModeIndex:
      // Pokazane tylko gdy dev mode jest już włączone — pozwala wyłączyć.
      if (devModeEnabled()) {
        setDevModeEnabled(false);
        rebuildSettingsMenuItems();
        showGridToast(String(tr3(TrKey3::AdvancedModeColon)) + onOffLabel(false), nowMs);
        renderSettings();
        return;
      }
      rebuildSettingsMenuItems();
      renderSettings();
      return;
    case kSettingsAboutBrandIndex:
    default:
      return;
  }
}

// ─── First-run welcome wizard ────────────────────────────────────────────────

namespace {
// Krok "Połącz czytnik z telefonem" — adres strony pobrania apki, zaszyty na
// sztywno w QR. Statyczny tekst, więc generujemy go raz, przy pierwszym
// rysowaniu ekranu.
constexpr const char *kInstallAppUrl = "https://flower.theworkpc.com/appdownload";
// Wersja 4 = 33x33 modułów, ECC_LOW mieści 78 bajtów — URL ma ~40 znaków,
// czyli zapas jest spory, a moduł nadal wychodzi 4 px na 172 px wysokości.
constexpr uint8_t kInstallAppQrVersion = 4;
constexpr uint8_t kInstallAppQrMaxSize = 33;  // 4 * 4 + 17
bool g_installAppQrData[kInstallAppQrMaxSize * kInstallAppQrMaxSize] = {};
uint8_t g_installAppQrSize = 0;
bool g_installAppQrTried = false;

void ensureInstallAppQr() {
  if (g_installAppQrTried) {
    return;
  }
  g_installAppQrTried = true;

  QRCode qrcode;
  uint8_t qrcodeData[qrcode_getBufferSize(kInstallAppQrVersion)];
  const int8_t result =
      qrcode_initText(&qrcode, qrcodeData, kInstallAppQrVersion, ECC_LOW, kInstallAppUrl);
  if (result != 0 || qrcode.size > kInstallAppQrMaxSize) {
    g_installAppQrSize = 0;
    Serial.printf("[welcome] install QR failed (result=%d size=%u)\n", result,
                  static_cast<unsigned>(result == 0 ? qrcode.size : 0));
    return;
  }
  g_installAppQrSize = qrcode.size;
  for (uint8_t y = 0; y < g_installAppQrSize; ++y) {
    for (uint8_t x = 0; x < g_installAppQrSize; ++x) {
      g_installAppQrData[y * g_installAppQrSize + x] = qrcode_getModule(&qrcode, x, y);
    }
  }
  Serial.printf("[welcome] install QR ready %ux%u\n", static_cast<unsigned>(g_installAppQrSize),
                static_cast<unsigned>(g_installAppQrSize));
}

// Krok "Ładowanie" (~15s) — trzy frazy rotują w pętli, każda widoczna ~2s z
// krótką przerwą między nimi. To jest cięcie czas-na-czas, nie prawdziwy
// pixel-alpha fade (ten wymagałby nowej prymitywy w DisplayManager) — wygląda
// jak "pojawia się / znika", tylko bez płynnego blendu.
constexpr uint32_t kWelcomeLoadingPhraseCycleMs = 2500;
constexpr uint32_t kWelcomeLoadingPhraseVisibleMs = 2000;
constexpr uint32_t kWelcomeLoadingMinMs = 15000;
// Siatka bezpieczeństwa — gdyby pobieranie fontów utknęło (np. słabe Wi-Fi),
// ekran i tak rusza dalej zamiast wisieć w nieskończoność.
constexpr uint32_t kWelcomeLoadingMaxMs = 25000;
constexpr uint32_t kWelcomeTimedMessageMs = 3000;
constexpr uint32_t kWelcomeScreenFrameMs = 150;
// Forces a look at the download QR before the corner "Next" appears —
// see App::renderWelcomeConnect()/isWizardNextCornerTap().
constexpr uint32_t kWelcomeConnectNextDelayMs = 5000;
// Must match the box DisplayManager::renderStatusWithQr() draws for
// cornerHint (70x20 at a 4px margin) — widened a little for an easier tap,
// same idea as the wizard Confirm button's oversized hit zone.
constexpr int kWizardNextCornerX = BoardConfig::DISPLAY_WIDTH - 90;
constexpr int kWizardNextCornerY = BoardConfig::DISPLAY_HEIGHT - 30;
}  // namespace

bool App::welcomeConnectQrAvailable() const { return g_installAppQrSize > 0; }

void App::openWelcomeLanguage() {
  menuScreen_ = MenuScreen::WelcomeLanguage;
  // Bez „Back" — w wizardzie zaczynamy od pierwszego elementu listy.
  settingsSelectedIndex_ = 0;
  rebuildSettingsMenuItems();
  renderSettings();
  // Jedyne miejsce, gdzie tłumaczymy co robi PWR w kreatorze — na kolejnych
  // krokach ten sam toast byłby już tylko szumem.
  showGridToast(tr3(TrKey3::WelcomePowerBackHint), millis());
}

namespace {
// Zachowaj kolejność z rebuildSettingsMenuItems() dla WelcomeLanguage:
// 0 English, 1 Polski, 2 Deutsch, 3 Español, 4 Français, 5 Română.
// Mapuje na UiLanguage enum (0=English, 1=Spanish, 2=French, 3=German,
// 4=Romanian, 5=Polish — patrz Localization.h).
const uint8_t kWelcomeLangByIndex[] = {0, 5, 3, 1, 2, 4};
}  // namespace

void App::previewWizardPickerSelection(uint32_t nowMs) {
  // Wywoływane na KAŻDE dotknięcie kafelka na ekranach kreatora Język/Motyw/
  // Kolor podświetlenia — stosuje wybór od razu (widać efekt na żywo), zanim
  // Potwierdź przejdzie do kolejnego kroku. select*Item() (wywoływane przez
  // Potwierdź) robi dokładnie to samo jeszcze raz — idempotentne z rozmysłem,
  // żeby nie duplikować logiki apply w osobnym "pending" stanie.
  switch (menuScreen_) {
    case MenuScreen::WelcomeLanguage:
      if (settingsSelectedIndex_ < sizeof(kWelcomeLangByIndex)) {
        uiLanguage_ = Localization::sanitizeLanguage(kWelcomeLangByIndex[settingsSelectedIndex_]);
        preferences_.putUChar(kPrefUiLanguage, static_cast<uint8_t>(uiLanguage_));
        DeviceServicesBridge::setLanguageIndex(static_cast<int>(uiLanguage_));
        // Tytuł ekranu i podpowiedzi PWR są tłumaczone przez tr3()/tr() —
        // trzeba przebudować listę, żeby zobaczyć zmianę natychmiast.
        rebuildSettingsMenuItems();
      }
      break;
    case MenuScreen::WelcomeTheme: {
      const bool dark = settingsSelectedIndex_ >= 1;
      const bool night = settingsSelectedIndex_ == 2;
      darkMode_ = dark;
      nightMode_ = night;
      preferences_.putBool(kPrefDarkMode, darkMode_);
      preferences_.putBool(kPrefNightMode, nightMode_);
      applyDisplayPreferences(nowMs);
      break;
    }
    case MenuScreen::WelcomeHighlightColor: {
      const uint8_t colorIndex = static_cast<uint8_t>(settingsSelectedIndex_);
      display_.setFocusColorIndex(colorIndex);
      preferences_.putUChar(kPrefFocusColorIndex, colorIndex);
      break;
    }
    default:
      break;
  }
}

void App::selectWelcomeLanguageItem(uint32_t nowMs) {
  previewWizardPickerSelection(nowMs);
  Serial.printf("[welcome] language=%s (idx=%u → enum=%u)\n",
                uiLanguageLabel().c_str(),
                static_cast<unsigned>(settingsSelectedIndex_),
                static_cast<unsigned>(uiLanguage_));
  openWelcomeTheme();
}

void App::openWelcomeTheme() {
  menuScreen_ = MenuScreen::WelcomeTheme;
  settingsSelectedIndex_ = 0;
  rebuildSettingsMenuItems();
  renderSettings();
}

void App::selectWelcomeThemeItem(uint32_t nowMs) {
  previewWizardPickerSelection(nowMs);
  Serial.printf("[welcome] theme dark=%d night=%d\n",
                static_cast<int>(darkMode_), static_cast<int>(nightMode_));
  openWelcomeHighlightColor();
}

void App::openWelcomeHighlightColor() {
  menuScreen_ = MenuScreen::WelcomeHighlightColor;
  settingsSelectedIndex_ = display_.focusColorIndex();
  rebuildSettingsMenuItems();
  renderSettings();
}

void App::selectWelcomeHighlightColorItem(uint32_t nowMs) {
  previewWizardPickerSelection(nowMs);
  Serial.printf("[welcome] highlight color=%u\n",
                static_cast<unsigned>(settingsSelectedIndex_));
  openWelcomeWifi();
}

// ─── Krok 4: Wi-Fi domowe ────────────────────────────────────────────────────
// Nie ma osobnego ekranu "chcesz się połączyć?" — wchodzimy prosto do skanu
// sieci, tak samo jak z Ustawień > Wi-Fi > Wybierz sieć. wifiFlowFromWizard_
// mówi scanWifiNetworks()/selectWifiNetworkItem()/commitTextEntry() żeby po
// zapisaniu (albo pominięciu) sieci wrócić do kolejnego kroku kreatora
// zamiast do ekranu Ustawień Wi-Fi.

void App::openWelcomeWifi() {
  wifiFlowFromWizard_ = true;
  scanWifiNetworks();
}

void App::returnFromWifiFlow(uint32_t nowMs) {
  if (wifiFlowFromWizard_) {
    wifiFlowFromWizard_ = false;
    openWelcomeLoading(nowMs);
    return;
  }
  openWifiSettings();
}

// ─── Krok 5+6: skan aktualizacji + pobranie zasobów, ekran "Ładowanie" ──────
// Jedna wizualizacja dla obu — w tle rusza pobieranie brakujących fontów z SD
// (ten sam mechanizm co cichy auto-download po skonfigurowaniu Wi-Fi, patrz
// maybeAutoDownloadFonts()), a na ekranie kręcą się trzy zachęcające frazy
// przez minimum ~15s. Świadomie NIE odpalamy tu prawdziwej instalacji
// aktualizacji firmware (checkAndInstall) — restart w środku kreatora
// pierwszego uruchomienia byłby złym zaskoczeniem.
void App::openWelcomeLoading(uint32_t nowMs) {
  menuScreen_ = MenuScreen::WelcomeLoading;
  welcomeScreenEnteredMs_ = nowMs;
  welcomeLoadingLastRenderMs_ = 0;
  welcomeLoadingWorkStarted_ = false;
  renderWelcomeLoading(nowMs);
}

void App::updateWelcomeLoading(uint32_t nowMs) {
  if (!welcomeLoadingWorkStarted_) {
    welcomeLoadingWorkStarted_ = true;
    OtaUpdater::Config config = preferredOtaConfig();
    if (!fontPackComplete_ && otaUpdater_.isConfigured(config)) {
      startBackgroundFontDownload(config);
    }
    // Karta pusta na tym etapie (typowe dla świeżo sformatowanej/nowej) —
    // spróbuj po cichu ściągnąć starter library dla języka wybranego w kroku
    // 1, żeby krok 2.4 miał co pokazać zamiast pustej biblioteki.
    if (storage_.bookCount() == 0 && otaUpdater_.isConfigured(config)) {
      startBackgroundBookDownload(config);
    }
  }

  const uint32_t elapsed = nowMs - welcomeScreenEnteredMs_;
  const bool workDone = !fontDownloadInProgress_ && !bookDownloadInProgress_;
  if ((elapsed >= kWelcomeLoadingMinMs && workDone) || elapsed >= kWelcomeLoadingMaxMs) {
    openWelcomeSuper(nowMs);
    return;
  }

  if (nowMs - welcomeLoadingLastRenderMs_ >= kWelcomeScreenFrameMs) {
    renderWelcomeLoading(nowMs);
  }
}

void App::renderWelcomeLoading(uint32_t nowMs) {
  welcomeLoadingLastRenderMs_ = nowMs;
  const uint32_t elapsed = nowMs - welcomeScreenEnteredMs_;

  const uint32_t phraseCycle = elapsed % (kWelcomeLoadingPhraseCycleMs * 3);
  const size_t phraseIndex = static_cast<size_t>(phraseCycle / kWelcomeLoadingPhraseCycleMs);
  const uint32_t phraseLocal = phraseCycle % kWelcomeLoadingPhraseCycleMs;
  String phrase;
  if (phraseLocal < kWelcomeLoadingPhraseVisibleMs) {
    switch (phraseIndex) {
      case 0: phrase = tr3(TrKey3::WelcomeLoadingPhrase1); break;
      case 1: phrase = tr3(TrKey3::WelcomeLoadingPhrase2); break;
      default: phrase = tr3(TrKey3::WelcomeLoadingPhrase3); break;
    }
  }

  const bool downloading = fontDownloadInProgress_;
  const String bottomLabel = downloading ? tr3(TrKey3::WelcomeLoadingBottomDownloading)
                                          : tr3(TrKey3::WelcomeLoadingBottomLoading);
  // Pasek postępu jako zastępnik "kręcącego się kółka" — prawdziwa animacja
  // spinnera wymagałaby nowej prymitywy rysującej w DisplayManager, a pasek
  // 0->100 w pętli daje ten sam efekt "coś się dzieje" bez nowego kodu w
  // warstwie wyświetlacza.
  const int sawtoothPercent = static_cast<int>((elapsed % 2000UL) / 20UL);
  // Skala 64/44 — 42/38 wciąż było za małe na tym ekranie ("Odzyskaj stan
  // Flow" itp. czytane jako "malutki druczek" mimo dużo wolnego miejsca na
  // 640x172 ekranie); fitSerifTextScaled i tak bezpiecznie skróci z "..." dla
  // najdłuższych tłumaczeń (np. niemiecki dolny label), więc nie ma ryzyka
  // wyjścia poza ekran.
  display_.renderProgress("", phrase, bottomLabel, sawtoothPercent, 64, 44);
}

// ─── Ekrany "Super!" / "Skonfigurujmy Twoje urządzenie!" ────────────────────
// Auto-advance po 3s, bez potrzeby dotyku — patrz updateWelcomeTimedScreens().

void App::openWelcomeSuper(uint32_t nowMs) {
  menuScreen_ = MenuScreen::WelcomeSuper;
  welcomeScreenEnteredMs_ = nowMs;
  renderWelcomeTimedMessage(tr3(TrKey3::WelcomeSuperTitle));
}

void App::openWelcomeConfigureIntro(uint32_t nowMs) {
  menuScreen_ = MenuScreen::WelcomeConfigureIntro;
  welcomeScreenEnteredMs_ = nowMs;
  renderWelcomeTimedMessage(tr3(TrKey3::WelcomeConfigureTitle));
}

void App::renderWelcomeTimedMessage(const String &line1, const String &line2) {
  // Skala 48% zamiast domyślnej 36% — to jedyny tekst na tych ekranach
  // ("Super!" / "Skonfigurujmy Twoje urządzenie!"), więc może być duży.
  display_.renderStatus("", line1, line2, 48, 28);
}

void App::updateWelcomeTimedScreens(uint32_t nowMs) {
  if (menuScreen_ == MenuScreen::WelcomeLoading) {
    updateWelcomeLoading(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::WelcomeConnect) {
    // Re-render every tick so the corner "Next" button appears exactly
    // once the 5s look-first delay passes, with no extra input needed —
    // see renderWelcomeConnect(). Cheap: DisplayManager's render-key cache
    // skips the actual redraw except right at that 5s edge.
    renderWelcomeConnect();
    return;
  }
  if (menuScreen_ != MenuScreen::WelcomeSuper && menuScreen_ != MenuScreen::WelcomeConfigureIntro &&
      menuScreen_ != MenuScreen::WelcomeConfigureInApp) {
    return;
  }
  if (nowMs - welcomeScreenEnteredMs_ < kWelcomeTimedMessageMs) {
    return;
  }
  if (menuScreen_ == MenuScreen::WelcomeSuper) {
    openWelcomeConfigureIntro(nowMs);
  } else if (menuScreen_ == MenuScreen::WelcomeConfigureIntro) {
    // Krok 2.1 — reużywamy cały ekran/handler TypographyFontPicker; flaga
    // mówi selectTypographyFontPickerItem() żeby po wyborze wrócić do
    // następnego kroku kreatora zamiast do TypographyTuning.
    wizardFontPickerActive_ = true;
    openTypographyFontPicker();
  } else {
    // WelcomeConfigureInApp — "Skonfiguruj w aplikacji" auto-advances into
    // the starter-library picker just like Super/ConfigureIntro do.
    openWelcomeBookPicker(nowMs);
  }
}

// ─── Krok 2.2: sposób czytania (RSVP / przewijanie) + podgląd ───────────────

void App::openWelcomeReadingMode() {
  menuScreen_ = MenuScreen::WelcomeReadingMode;
  settingsSelectedIndex_ = 0;
  rebuildSettingsMenuItems();
  renderSettings();
}

void App::selectWelcomeReadingModeItem(uint32_t nowMs) {
  // Tylko 2 kafelki (RSVP / Przewijanie) — Potwierdź zatwierdza który z nich
  // wybrano. Podgląd każdego trybu wisi teraz jako osobna ikonka oka w rogu
  // (applyReadingModePreviewButtonLayout()) i NIE wymaga Potwierdź, bo to
  // tylko podgląd, nie decyzja — patrz handleGridTap().
  switch (settingsSelectedIndex_) {
    case 0:  // RSVP
      readerMode_ = ReaderMode::Rsvp;
      preferences_.putUChar(kPrefReaderMode, static_cast<uint8_t>(readerMode_));
      Serial.println("[welcome] reading mode=RSVP");
      openWelcomeConnect(nowMs);
      return;
    case 1:  // Przewijanie strony
      readerMode_ = ReaderMode::Scroll;
      preferences_.putUChar(kPrefReaderMode, static_cast<uint8_t>(readerMode_));
      Serial.println("[welcome] reading mode=Scroll");
      openWelcomeConnect(nowMs);
      return;
    default:
      return;
  }
}

namespace {
// Kilka kopii tego samego zdania w kolejnych "akapitach" — wystarczy słów,
// żeby okno przewijania miało co scrollować w pętli przez cały czas
// pokazywania podglądu.
constexpr int kWelcomeScrollPreviewParagraphs = 4;
constexpr uint32_t kWelcomeRsvpPreviewWordMs = 500;
constexpr uint32_t kWelcomeScrollPreviewWordMs = 260;
}  // namespace

void App::openWelcomeReadingModePreview(uint8_t mode) {
  welcomeReadingModePreviewMode_ = mode;
  welcomeReadingModePreviewWordIndex_ = 0;
  welcomeReadingModePreviewLastTickMs_ = millis();
  menuScreen_ = MenuScreen::WelcomeReadingModePreview;

  if (mode == 1) {
    // Zbuduj listę słów raz przy otwarciu — to samo zdanie powtórzone kilka
    // razy jako kolejne akapity, żeby renderScrollView() miało realny,
    // wielowierszowy tekst do przewijania zamiast jednego zdania na sztywno.
    welcomeScrollPreviewWords_.clear();
    const String sentence = tr3(TrKey3::WelcomePreviewScrollBody);
    for (int paragraph = 0; paragraph < kWelcomeScrollPreviewParagraphs; ++paragraph) {
      int start = 0;
      bool firstWordInParagraph = true;
      while (start < static_cast<int>(sentence.length())) {
        int spaceIndex = sentence.indexOf(' ', start);
        if (spaceIndex < 0) {
          spaceIndex = sentence.length();
        }
        if (spaceIndex > start) {
          DisplayManager::ContextWord word;
          word.text = sentence.substring(start, spaceIndex);
          word.paragraphStart = firstWordInParagraph;
          firstWordInParagraph = false;
          welcomeScrollPreviewWords_.push_back(word);
        }
        start = spaceIndex + 1;
      }
    }
  }

  renderWelcomeReadingModePreview();
}

void App::updateWelcomeReadingModePreview(uint32_t nowMs) {
  if (menuScreen_ != MenuScreen::WelcomeReadingModePreview) {
    return;
  }

  const uint32_t tickMs =
      welcomeReadingModePreviewMode_ == 0 ? kWelcomeRsvpPreviewWordMs : kWelcomeScrollPreviewWordMs;
  if (nowMs - welcomeReadingModePreviewLastTickMs_ < tickMs) {
    return;
  }
  welcomeReadingModePreviewLastTickMs_ = nowMs;

  if (welcomeReadingModePreviewMode_ == 0) {
    if (kTypographyPreviewWordCount > 0) {
      welcomeReadingModePreviewWordIndex_ =
          (welcomeReadingModePreviewWordIndex_ + 1) % kTypographyPreviewWordCount;
    }
  } else if (!welcomeScrollPreviewWords_.empty()) {
    welcomeReadingModePreviewWordIndex_ =
        (welcomeReadingModePreviewWordIndex_ + 1) % welcomeScrollPreviewWords_.size();
  }

  renderWelcomeReadingModePreview();
}

void App::renderWelcomeReadingModePreview() {
  if (welcomeReadingModePreviewMode_ == 0) {
    // Ta sama prymitywa co podgląd w Typography Tuning (jedno "słowo-duch"
    // z sąsiadami przygaszonymi po bokach) — realny wygląd RSVP na tym
    // urządzeniu. Słowo faktycznie zmienia się co kWelcomeRsvpPreviewWordMs
    // (patrz updateWelcomeReadingModePreview) — bez tego ekran pokazywał
    // jedno zamrożone słowo i niczego nie demonstrował.
    if (kTypographyPreviewWordCount == 0) {
      return;
    }
    const size_t current = welcomeReadingModePreviewWordIndex_ % kTypographyPreviewWordCount;
    const bool hasNeighbours = kTypographyPreviewWordCount > 1;
    const size_t beforeIndex =
        current == 0 ? kTypographyPreviewWordCount - 1 : current - 1;
    const size_t afterIndex = (current + 1) % kTypographyPreviewWordCount;
    const String before = hasNeighbours ? kTypographyPreviewWords[beforeIndex] : "";
    const String after = hasNeighbours ? kTypographyPreviewWords[afterIndex] : "";
    display_.renderTypographyPreview(before, kTypographyPreviewWords[current], after,
                                     readerFontSizeIndex_, "RSVP",
                                     tr3(TrKey3::WelcomePreviewRsvpLine),
                                     tr3(TrKey3::WelcomeTapToGoBack));
  } else {
    // Przewijanie faktycznie płynie — currentWordIndex rośnie co
    // kWelcomeScrollPreviewWordMs (patrz updateWelcomeReadingModePreview),
    // renderScrollView() sam dosuwa tekst tak, żeby ta pozycja była
    // widoczna. Statyczny renderStatus() wcześniej nie ruszał się wcale.
    if (welcomeScrollPreviewWords_.empty()) {
      display_.renderStatus(tr3(TrKey3::WelcomeReadingModeScrollLabel),
                            tr3(TrKey3::WelcomePreviewScrollBody),
                            tr3(TrKey3::WelcomeTapToGoBack));
      return;
    }
    DisplayManager::ReaderChrome chrome;
    chrome.showBattery = false;
    chrome.showChapter = false;
    chrome.showProgress = false;
    chrome.showPreviousSentenceHint = false;
    chrome.showSavePointButton = false;
    const size_t current =
        welcomeReadingModePreviewWordIndex_ % welcomeScrollPreviewWords_.size();
    display_.renderScrollView(welcomeScrollPreviewWords_, 0, 0, current, 0, "", 0,
                              tr3(TrKey3::WelcomeTapToGoBack), "", chrome);
  }
}

// ─── Krok 2.3: połącz z telefonem (QR + parowanie AP) ───────────────────────

void App::openWelcomeConnect(uint32_t nowMs) {
  menuScreen_ = MenuScreen::WelcomeConnect;
  welcomeScreenEnteredMs_ = nowMs;
  renderWelcomeConnect();
}

void App::renderWelcomeConnect() {
  ensureInstallAppQr();
  // First 5s: just the QR, no tap-through — give the user a chance to
  // actually point their camera at it before offering a way past it. See
  // isWizardNextCornerTap() for the matching touch hit-test.
  const bool showNext = millis() - welcomeScreenEnteredMs_ >= kWelcomeConnectNextDelayMs;
  const String cornerHint = showNext ? tr3(TrKey3::NextLabel) : "";
  if (g_installAppQrSize > 0) {
    // No waiting/connected hint here anymore — that's the pairing screen's
    // job now (renderWelcomeAppPairing()), not this download-only step.
    display_.renderStatusWithQr(tr3(TrKey3::WelcomeConnectTitle), tr3(TrKey3::WelcomeConnectLine1),
                                g_installAppQrData, g_installAppQrSize, "", cornerHint);
  } else {
    // QR generation failed (rare) — no corner button to gate on, so the
    // whole screen stays a single tap-through like it always was.
    display_.renderStatus(tr3(TrKey3::WelcomeConnectTitle), kInstallAppUrl, "");
  }
}

bool App::isWizardNextCornerTap(uint16_t x, uint16_t y) const {
  return x >= kWizardNextCornerX && y >= kWizardNextCornerY;
}

void App::selectWelcomeConnectTap(uint32_t nowMs) {
  if (g_installAppQrSize > 0 && millis() - welcomeScreenEnteredMs_ < kWelcomeConnectNextDelayMs) {
    // Corner button isn't showing yet — ignore, this screen only advances
    // via that corner or (fallback, no QR at all) a tap anywhere.
    return;
  }
  openWelcomeAppPairing(nowMs);
}

void App::openWelcomeAppPairing(uint32_t nowMs) {
  menuScreen_ = MenuScreen::WelcomeAppPairing;
  welcomeScreenEnteredMs_ = nowMs;

  // Start AP + BLE pairing if not already running from auto-sync — moved
  // here (was in openWelcomeConnect()) so the radios only come on once the
  // user is actually looking at the pairing QR, not while they're still
  // reading the app-download screen. Must happen BEFORE the render call
  // below: renderWelcomeAppPairing() only draws the real QR once
  // companionSync_.hasQrCode() is true, which begin() is what sets up —
  // rendering first left the very first paint of this screen QR-less
  // (fallback text branch) with nothing left to repaint it afterwards,
  // since the client-connected redraw in App::update() never fires without
  // a QR to scan in the first place.
  if (!autoSyncActive_ && !companionSync_.active()) {
    CompanionSyncManager::Config syncConfig;
    syncConfig.wifiSsid = "";
    syncConfig.wifiPassword = "";
    if (companionSync_.begin(syncConfig)) {
      autoSyncActive_ = true;
      autoSyncStartedMs_ = millis();
      autoSyncClientConnected_ = false;
      Serial.println("[welcome] started AP for phone pairing");
    }
  }
  if (!ble_.isActive()) {
    ble_.begin(this);
    Serial.printf("[welcome] BLE turned on for phone pairing (name=%s)\n",
                  ble_.deviceName().c_str());
  }
  renderWelcomeAppPairing();
}

void App::renderWelcomeAppPairing() {
  const String hint = autoSyncClientConnected_ ? tr3(TrKey3::WelcomeConnectHintConnected)
                                                : tr3(TrKey3::WelcomeConnectHintWaiting);
  if (companionSync_.hasQrCode()) {
    // statusLine1() is the AP's SSID once begin() finishes (see its own
    // startAccessPoint()-then-overwrite sequence) — the same value the
    // Ustawienia > Sync screen shows next to this exact QR.
    display_.renderStatusWithQr(tr3(TrKey3::WelcomeAppPairingTitle), companionSync_.statusLine1(),
                                companionSync_.qrCodeData(), companionSync_.qrCodeSize(), hint);
  } else {
    display_.renderStatus(tr3(TrKey3::WelcomeAppPairingTitle), companionSync_.statusLine1(), hint);
  }
}

void App::selectWelcomeAppPairingTap(uint32_t nowMs) {
  openWelcomeConfigureInApp(nowMs);
}

void App::openWelcomeConfigureInApp(uint32_t nowMs) {
  menuScreen_ = MenuScreen::WelcomeConfigureInApp;
  welcomeScreenEnteredMs_ = nowMs;
  renderWelcomeTimedMessage(tr3(TrKey3::WelcomeConfigureInAppLine1),
                            tr3(TrKey3::WelcomeConfigureInAppLine2));
}

// ─── Krok 2.4: "Prawie gotowe! Co dziś czytamy?" ────────────────────────────
// Reużywa cały ekran Biblioteki (BookPicker). Starter library (patrz
// startBackgroundBookDownload(), uruchamiane w kroku 5+6/WelcomeLoading) już
// próbowała ściągnąć do 5 tytułów dla wybranego języka, zanim doszliśmy tu —
// jeśli się udało, storage_.bookCount() > 0 i widać realną listę. Jeśli
// Karol jeszcze nie wgrał żadnego "starter-<kod>-N.rsvp" dla tego języka na
// GitHuba (albo nie było Wi-Fi), karta zostaje pusta i ekran po prostu
// przechodzi dalej bez zawieszania kreatora na czymś, czego nie może
// pokazać.
void App::openWelcomeBookPicker(uint32_t nowMs) {
  wizardBookPickerActive_ = true;
  openBookPicker(false);
  if (storage_.bookCount() == 0) {
    finishWelcomeWizard(nowMs);
    return;
  }
  showGridToast(tr3(TrKey3::WelcomeBookPickerTitle), nowMs);
}

void App::finishWelcomeWizard(uint32_t nowMs) {
  wifiFlowFromWizard_ = false;
  wizardFontPickerActive_ = false;
  wizardBookPickerActive_ = false;
  preferences_.putBool(kPrefSetupDone, true);
  // Stary 5-krokowy tutorial na urządzeniu (RSVP/tempo/pauza/menu/pomoc)
  // zostaje dostępny ręcznie z Ustawienia > O aplikacji > Tutorial, ale nie
  // jest już wymuszany po kreatorze — krok "Co dziś czytamy" prowadzi prosto
  // do czytania, tak jak poprosił Karol. Bez oznaczenia tut_done=true
  // updateState() wymuszałby tutorial przy KAŻDYM kolejnym boocie (patrz
  // komentarz "Setup done but tutorial not finished" wyżej w tym pliku).
  preferences_.putBool(kPrefTutorialDone, true);
  tutorialCompleted_ = true;
  menuScreen_ = MenuScreen::Main;
  menuSelectedIndex_ = 0;
  renderMainMenu();
  setState(AppState::Menu, nowMs);
}

// PWR krótkie kliknięcie na ekranach kreatora cofa o jeden krok zamiast
// wychodzić z konfiguracji (patrz toggleMenuFromPowerButton) — kreator
// pierwszego uruchomienia musi się dać tylko przejść do końca, nie ominąć.
void App::wizardStepBack(uint32_t nowMs) {
  switch (menuScreen_) {
    case MenuScreen::WelcomeTheme:
      openWelcomeLanguage();
      return;
    case MenuScreen::WelcomeHighlightColor:
      openWelcomeTheme();
      return;
    case MenuScreen::WelcomeLoading:
      openWelcomeHighlightColor();
      return;
    case MenuScreen::WelcomeSuper:
      openWelcomeLoading(nowMs);
      return;
    case MenuScreen::WelcomeConfigureIntro:
      openWelcomeSuper(nowMs);
      return;
    case MenuScreen::WelcomeReadingMode:
      wizardFontPickerActive_ = true;
      openTypographyFontPicker();
      return;
    case MenuScreen::WelcomeReadingModePreview:
      openWelcomeReadingMode();
      return;
    case MenuScreen::WelcomeConnect:
      openWelcomeReadingMode();
      return;
    case MenuScreen::WelcomeAppPairing:
      openWelcomeConnect(nowMs);
      return;
    case MenuScreen::WelcomeConfigureInApp:
      openWelcomeAppPairing(nowMs);
      return;
    default:
      break;
  }
  if (menuScreen_ == MenuScreen::TypographyFontPicker && wizardFontPickerActive_) {
    wizardFontPickerActive_ = false;
    openWelcomeConfigureIntro(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::BookPicker && wizardBookPickerActive_) {
    wizardBookPickerActive_ = false;
    openWelcomeConfigureInApp(nowMs);
    return;
  }
}

// ─── Post-wizard tutorial ────────────────────────────────────────────────────

void App::openTutorialStep1() {
  menuScreen_ = MenuScreen::TutorialStep1;
  renderTutorialStep();
}

void App::openTutorialStep2() {
  menuScreen_ = MenuScreen::TutorialStep2;
  renderTutorialStep();
}

void App::openTutorialStep3() {
  menuScreen_ = MenuScreen::TutorialStep3;
  renderTutorialStep();
}

void App::openTutorialStep4() {
  menuScreen_ = MenuScreen::TutorialStep4;
  renderTutorialStep();
}

void App::openTutorialStep5() {
  menuScreen_ = MenuScreen::TutorialStep5;
  renderTutorialStep();
}

void App::renderTutorialStep() {
  const char *title = "";
  const char *desc = "";
  int step = 0;

  switch (menuScreen_) {
    case MenuScreen::TutorialStep1:
      title = "RSVP";
      desc = tr3(TrKey3::TutorialRsvpDesc);
      step = 1;
      break;
    case MenuScreen::TutorialStep2:
      title = tr3(TrKey3::SpeedLabel);
      desc = tr3(TrKey3::TutorialSpeedDesc);
      step = 2;
      break;
    case MenuScreen::TutorialStep3:
      title = tr3(TrKey3::PauseLabel);
      desc = tr3(TrKey3::TutorialPauseDesc);
      step = 3;
      break;
    case MenuScreen::TutorialStep4:
      title = "Menu";
      desc = tr3(TrKey3::TutorialMenuDesc);
      step = 4;
      break;
    case MenuScreen::TutorialStep5:
      title = tr3(TrKey3::HelpQLabel);
      desc = tr3(TrKey3::TutorialHelpDesc);
      step = 5;
      break;
    default:
      return;
  }

  String progress = String(step) + "/5";
  display_.renderStatus(title, desc, progress);
}

void App::handleTutorialTap(uint32_t nowMs) {
  switch (menuScreen_) {
    case MenuScreen::TutorialStep1: openTutorialStep2(); break;
    case MenuScreen::TutorialStep2: openTutorialStep3(); break;
    case MenuScreen::TutorialStep3: openTutorialStep4(); break;
    case MenuScreen::TutorialStep4: openTutorialStep5(); break;
    case MenuScreen::TutorialStep5: finishTutorial(nowMs); break;
    default: break;
  }
}

void App::previousTutorialStep(uint32_t nowMs) {
  (void)nowMs;
  switch (menuScreen_) {
    // Krok 1 to początek — nie ma dokąd wrócić (tutorial bywa uruchamiany
    // zarówno z kreatora pierwszego uruchomienia, jak i ręcznie z Ustawienia
    // > O aplikacji, więc nie ma jednego stałego ekranu "przed" nim).
    case MenuScreen::TutorialStep1: renderTutorialStep(); break;
    case MenuScreen::TutorialStep2: openTutorialStep1(); break;
    case MenuScreen::TutorialStep3: openTutorialStep2(); break;
    case MenuScreen::TutorialStep4: openTutorialStep3(); break;
    case MenuScreen::TutorialStep5: openTutorialStep4(); break;
    default: break;
  }
}

void App::finishTutorial(uint32_t nowMs) {
  tutorialCompleted_ = true;
  preferences_.putBool(kPrefTutorialDone, true);
  menuScreen_ = MenuScreen::Main;
  menuSelectedIndex_ = 0;
  settingsSelectedIndex_ = kSettingsBackIndex;
  setState((touchPlayHeld_ || playLocked_ || pauseAtSentenceEndRequested_)
               ? AppState::Playing
               : AppState::Paused,
           nowMs);
}

OtaUpdater::Config App::preferredOtaConfig() {
  OtaUpdater::Config otaConfig;
  otaUpdater_.loadConfig(otaConfig);

  // Default to auto-check enabled
  otaConfig.autoCheck = true;

  if (preferences_.isKey(kPrefWifiSsid)) {
    otaConfig.wifiSsid = preferences_.getString(kPrefWifiSsid, "");
  }
  if (preferences_.isKey(kPrefWifiPass)) {
    otaConfig.wifiPassword = preferences_.getString(kPrefWifiPass, "");
  }
  if (preferences_.isKey(kPrefOtaAuto)) {
    otaConfig.autoCheck = preferences_.getBool(kPrefOtaAuto, true);
  }
  if (preferences_.isKey(kPrefOtaOwner)) {
    otaConfig.githubOwner = preferences_.getString(kPrefOtaOwner, "");
  }
  if (otaChannelEnabled()) {
    otaConfig.githubRepo = kOtaStagingRepo;
  }

  return otaConfig;
}

String App::configuredWifiSsid() {
  String ssid = preferences_.getString(kPrefWifiSsid, "");
  if (ssid.isEmpty()) {
    OtaUpdater::Config otaConfig;
    otaUpdater_.loadConfig(otaConfig);
    ssid = otaConfig.wifiSsid;
  }
  ssid.trim();
  return ssid;
}

String App::findSavedWifiPassword(const String &ssid) {
  if (ssid.isEmpty()) {
    return "";
  }
  for (size_t i = 0; i < kMaxSavedWifiNetworks; ++i) {
    const String key = String(kPrefSavedWifiSsidPrefix) + String(i);
    if (preferences_.getString(key.c_str(), "") == ssid) {
      const String passKey = String(kPrefSavedWifiPassPrefix) + String(i);
      return preferences_.getString(passKey.c_str(), "");
    }
  }
  return "";
}

void App::rememberWifiNetwork(const String &ssid, const String &pass) {
  if (ssid.isEmpty()) {
    return;
  }
  // Overwrite the slot if this SSID is already remembered; otherwise use
  // the first empty slot; otherwise evict slot 0 (oldest-ish — good enough
  // for a handful of home/work networks, no need for real LRU tracking).
  int targetSlot = -1;
  for (size_t i = 0; i < kMaxSavedWifiNetworks; ++i) {
    const String key = String(kPrefSavedWifiSsidPrefix) + String(i);
    const String storedSsid = preferences_.getString(key.c_str(), "");
    if (storedSsid == ssid) {
      targetSlot = static_cast<int>(i);
      break;
    }
    if (targetSlot < 0 && storedSsid.isEmpty()) {
      targetSlot = static_cast<int>(i);
    }
  }
  if (targetSlot < 0) {
    targetSlot = 0;
  }
  preferences_.putString((String(kPrefSavedWifiSsidPrefix) + String(targetSlot)).c_str(), ssid);
  preferences_.putString((String(kPrefSavedWifiPassPrefix) + String(targetSlot)).c_str(), pass);
}

void App::forgetSavedWifiNetwork(const String &ssid) {
  if (ssid.isEmpty()) {
    return;
  }
  for (size_t i = 0; i < kMaxSavedWifiNetworks; ++i) {
    const String key = String(kPrefSavedWifiSsidPrefix) + String(i);
    if (preferences_.getString(key.c_str(), "") == ssid) {
      preferences_.remove(key.c_str());
      preferences_.remove((String(kPrefSavedWifiPassPrefix) + String(i)).c_str());
      return;
    }
  }
}

bool App::otaAutoCheckEnabled() {
  if (preferences_.isKey(kPrefOtaAuto)) {
    return preferences_.getBool(kPrefOtaAuto, true);
  }

  // Default to true — silent auto-update is always on unless explicitly disabled
  return true;
}

bool App::otaChannelEnabled() {
  // Default to false — production repo unless the tester flips this on.
  return preferences_.getBool(kPrefOtaChannel, false);
}

String App::otaChannelLabel() {
  return otaChannelEnabled() ? tr3(TrKey3::ChannelStaging) : tr3(TrKey3::ChannelProduction);
}

void App::maybeAutoCheckForUpdates(uint32_t nowMs) {
  (void)nowMs;
  OtaUpdater::Config otaConfig = preferredOtaConfig();
  if (!otaConfig.autoCheck || !otaUpdater_.isConfigured(otaConfig)) {
    return;
  }

  Serial.println("[ota] auto-check enabled");
  startBackgroundOtaCheck(otaConfig);
}

bool App::startBackgroundOtaCheck(const OtaUpdater::Config &config) {
  if (otaCheckInProgress_) {
    Serial.println("[ota] background check already running");
    return false;
  }

  if (otaCheckQueue_ == nullptr) {
    otaCheckQueue_ = xQueueCreate(1, sizeof(OtaCheckResult));
    if (otaCheckQueue_ == nullptr) {
      Serial.println("[ota] could not create result queue");
      return false;
    }
  }
  xQueueReset(otaCheckQueue_);

  OtaCheckTaskParams *params = new OtaCheckTaskParams();
  if (params == nullptr) {
    Serial.println("[ota] could not allocate task params");
    return false;
  }
  params->config = config;
  params->resultQueue = otaCheckQueue_;

  otaCheckInProgress_ = true;
  otaCheckStartedMs_ = millis();
  BaseType_t created = xTaskCreatePinnedToCore(otaCheckTask, "ota_check",
                                               kOtaCheckTaskStackBytes, params, 1, nullptr, 0);
  if (created != pdPASS) {
    Serial.printf("[ota] background task create failed: %ld\n", static_cast<long>(created));
    otaCheckInProgress_ = false;
    delete params;
    return false;
  }

  Serial.println("[ota] background check started");
  return true;
}

void App::otaCheckTask(void *params) {
  OtaCheckTaskParams *taskParams = static_cast<OtaCheckTaskParams *>(params);
  if (taskParams == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  OtaCheckResult queuedResult;

  const OtaUpdater::Result result =
      OtaUpdater().checkOnly(taskParams->config, nullptr, nullptr);
  queuedResult.code = result.code;
  copyOtaLabel(queuedResult.currentVersion, sizeof(queuedResult.currentVersion),
               result.currentVersion);
  copyOtaLabel(queuedResult.latestVersion, sizeof(queuedResult.latestVersion),
               result.latestVersion);
  copyOtaLabel(queuedResult.summary, sizeof(queuedResult.summary), result.summary);
  copyOtaLabel(queuedResult.detail, sizeof(queuedResult.detail), result.detail);

  if (taskParams->resultQueue != nullptr) {
    xQueueOverwrite(taskParams->resultQueue, &queuedResult);
  }

  delete taskParams;
  vTaskDelete(nullptr);
}

void App::pollOtaCheckResult(uint32_t nowMs) {
  if (otaCheckQueue_ == nullptr) {
    return;
  }

  if (otaCheckInProgress_ && otaCheckStartedMs_ != 0 &&
      nowMs - otaCheckStartedMs_ > kOtaCheckWatchdogTimeoutMs) {
    Serial.println("[ota] background check watchdog timeout — task never reported back, "
                   "releasing lock");
    otaCheckInProgress_ = false;
    otaCheckStartedMs_ = 0;
  }

  OtaCheckResult result;
  while (xQueueReceive(otaCheckQueue_, &result, 0) == pdTRUE) {
    otaCheckInProgress_ = false;
    otaCheckStartedMs_ = 0;
    Serial.printf("[ota] background result code=%u current=%s latest=%s\n",
                  static_cast<unsigned int>(result.code), result.currentVersion,
                  result.latestVersion);

    if (result.code == OtaUpdater::ResultCode::UpdateAvailable) {
      String current = otaUpdater_.currentVersion();
      String latest = String(result.latestVersion);
      if (latest.isEmpty()) {
        Serial.println("[ota] latest version empty — skipping");
        return;
      }
      // Strip git describe suffix (e.g. "v0.3.1-2-gabcdef" → "v0.3.1")
      int dashPos = current.indexOf('-');
      String currentBase = (dashPos > 0) ? current.substring(0, dashPos) : current;
      if (currentBase == latest) {
        Serial.printf("[ota] versions match (%s == %s) — no update needed\n",
                      currentBase.c_str(), latest.c_str());
        return;
      }
      // Flag it — the reading screen will be interrupted with a blocking
      // Update/Later tile (see maybeOpenUpdateConfirm), and it also shows
      // as a row in the main menu for later.
      pendingUpdateCurrentVersion_ = currentBase;
      pendingUpdateNewVersion_ = latest;
      otaUpdatePromptPending_ = true;
      otaUpdatePromptDismissed_ = false;
      Serial.printf("[ota] update available: %s -> %s (user will be notified)\n",
                    currentBase.c_str(), latest.c_str());
    }
  }
}

// ─── Font pack auto-download (Etap 4/5+, docs/PLAN_FONTY_NA_SD.md) ──────────
//
// No menu entry, no prompt: the 17 SD-backed typefaces stay hidden from the
// font picker (see openTypographyFontPicker()) until they're actually on the
// card, and download themselves silently the moment saved Wi-Fi credentials
// exist — at boot, right after a successful SD repair, or on the periodic
// retry below if Wi-Fi gets configured later (e.g. via the Flower app).

bool App::refreshFontPackComplete() {
  bool complete = true;
  for (uint8_t i = static_cast<uint8_t>(DisplayManager::ReaderTypeface::Literata);
       i < static_cast<uint8_t>(DisplayManager::ReaderTypeface::Count); ++i) {
    const auto typeface = static_cast<DisplayManager::ReaderTypeface>(i);
    if (!DisplayManager::isTypefaceAvailableOnSd(typeface)) {
      complete = false;
      break;
    }
  }
  fontPackComplete_ = complete;
  return complete;
}

void App::maybeAutoDownloadFonts(uint32_t nowMs) {
  if (fontPackComplete_ || fontDownloadInProgress_) {
    return;
  }
  if (lastFontDownloadAttemptMs_ != 0 &&
      nowMs - lastFontDownloadAttemptMs_ < kFontDownloadRetryIntervalMs) {
    return;
  }
  lastFontDownloadAttemptMs_ = nowMs;

  OtaUpdater::Config config = preferredOtaConfig();
  if (!otaUpdater_.isConfigured(config)) {
    // No saved Wi-Fi yet — stay quiet and try again on the next interval.
    return;
  }

  Serial.println("[fonts] Wi-Fi configured — starting background font pack download");
  startBackgroundFontDownload(config);
}

bool App::startBackgroundFontDownload(const OtaUpdater::Config &config) {
  if (fontDownloadInProgress_) {
    Serial.println("[fonts] background download already running");
    return false;
  }

  if (fontDownloadQueue_ == nullptr) {
    fontDownloadQueue_ = xQueueCreate(1, sizeof(FontDownloadResult));
    if (fontDownloadQueue_ == nullptr) {
      Serial.println("[fonts] could not create result queue");
      return false;
    }
  }
  xQueueReset(fontDownloadQueue_);

  FontDownloadTaskParams *params = new FontDownloadTaskParams();
  if (params == nullptr) {
    Serial.println("[fonts] could not allocate task params");
    return false;
  }
  params->config = config;
  params->resultQueue = fontDownloadQueue_;

  fontDownloadInProgress_ = true;
  BaseType_t created = xTaskCreatePinnedToCore(
      fontDownloadTask, "font_dl", kFontDownloadTaskStackBytes, params, 1, nullptr, 0);
  if (created != pdPASS) {
    Serial.printf("[fonts] background task create failed: %ld\n", static_cast<long>(created));
    fontDownloadInProgress_ = false;
    delete params;
    return false;
  }

  Serial.println("[fonts] background download started");
  return true;
}

void App::fontDownloadTask(void *params) {
  FontDownloadTaskParams *taskParams = static_cast<FontDownloadTaskParams *>(params);
  if (taskParams == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  std::vector<DisplayManager::ReaderTypeface> missing;
  for (uint8_t i = static_cast<uint8_t>(DisplayManager::ReaderTypeface::Literata);
       i < static_cast<uint8_t>(DisplayManager::ReaderTypeface::Count); ++i) {
    const auto typeface = static_cast<DisplayManager::ReaderTypeface>(i);
    if (!DisplayManager::isTypefaceAvailableOnSd(typeface)) {
      missing.push_back(typeface);
    }
  }

  FontDownloadResult queuedResult;
  queuedResult.totalMissing = static_cast<uint8_t>(missing.size());

  if (!missing.empty()) {
    OtaUpdater updater;
    if (!updater.connectWiFi(taskParams->config, nullptr, nullptr)) {
      queuedResult.wifiFailed = true;
      Serial.println("[fonts] Wi-Fi connect failed, will retry later");
    } else {
      for (DisplayManager::ReaderTypeface typeface : missing) {
        const String base = DisplayManager::sdFontFileBaseName(typeface);
        if (base.isEmpty()) {
          continue;
        }

        String errorDetail;
        const bool baseOk =
            updater.downloadAsset(taskParams->config, base + ".fnt", "",
                                  "/fonts/" + base + ".fnt", errorDetail);
        if (!baseOk) {
          Serial.printf("[fonts] %s.fnt failed: %s\n", base.c_str(), errorDetail.c_str());
          continue;
        }

        errorDetail = "";
        const bool mediumOk =
            updater.downloadAsset(taskParams->config, base + "_70.fnt", "",
                                  "/fonts/" + base + "_70.fnt", errorDetail);
        if (!mediumOk) {
          Serial.printf("[fonts] %s_70.fnt failed: %s\n", base.c_str(), errorDetail.c_str());
          continue;
        }

        queuedResult.downloaded++;
      }
      updater.disconnectWiFi();
    }
  }

  Serial.printf("[fonts] background download finished: %u/%u fetched\n",
               static_cast<unsigned int>(queuedResult.downloaded),
               static_cast<unsigned int>(queuedResult.totalMissing));

  if (taskParams->resultQueue != nullptr) {
    xQueueOverwrite(taskParams->resultQueue, &queuedResult);
  }

  delete taskParams;
  vTaskDelete(nullptr);
}

void App::pollFontDownloadResult(uint32_t nowMs) {
  (void)nowMs;
  if (fontDownloadQueue_ == nullptr) {
    return;
  }

  FontDownloadResult result;
  while (xQueueReceive(fontDownloadQueue_, &result, 0) == pdTRUE) {
    fontDownloadInProgress_ = false;
    if (result.totalMissing > 0 && result.downloaded == result.totalMissing) {
      refreshFontPackComplete();
      Serial.println("[fonts] font pack complete");
      // Rebuild the picker in place if it's open right now, so the newly
      // downloaded fonts show up without requiring a menu round-trip.
      if (menuScreen_ == MenuScreen::TypographyFontPicker) {
        openTypographyFontPicker();
      }
    }
    // Partial/failed batches leave fontPackComplete_ false; the missing-only
    // re-scan in fontDownloadTask() means the next retry only fetches what's
    // still absent, so a flaky connection just costs time, not redundant work.
  }
}

// ─── Starter library (krok 2.4/5-6 kreatora) ────────────────────────────────
// Ten sam wzorzec co pobieranie fontów: osobny task FreeRTOS, wynik przez
// jednoelementową kolejkę, brak asseta = po prostu pomiń, nie błąd. Assety na
// GitHubie: "starter-<kod języka>-<1..5>.rsvp", np. "starter-pl-1.rsvp".
// Rozszerzenie MUSI być .rsvp, nie .txt — StorageManager czyta dyrektywy
// "@title"/"@author"/"@chapter" tylko z plików .rsvp (hasRsvpExtension()),
// pliki .txt są parsowane jako czysty tekst i dyrektywy wyświetliłyby się
// jako zwykłe słowa. Zobacz firmware/tools/generate_starter_library.py,
// który generuje te assety w tym samym formacie co
// firmware/tools/sd_card_converter/convert_books.py.
namespace {
String starterBookLanguageCode(UiLanguage lang) {
  switch (lang) {
    case UiLanguage::Polish: return "pl";
    case UiLanguage::German: return "de";
    case UiLanguage::Spanish: return "es";
    case UiLanguage::French: return "fr";
    case UiLanguage::Romanian: return "ro";
    case UiLanguage::English:
    default: return "en";
  }
}
}  // namespace

bool App::startBackgroundBookDownload(const OtaUpdater::Config &config) {
  if (bookDownloadInProgress_) {
    Serial.println("[books] background download already running");
    return false;
  }

  if (bookDownloadQueue_ == nullptr) {
    bookDownloadQueue_ = xQueueCreate(1, sizeof(BookDownloadResult));
    if (bookDownloadQueue_ == nullptr) {
      Serial.println("[books] could not create result queue");
      return false;
    }
  }
  xQueueReset(bookDownloadQueue_);

  BookDownloadTaskParams *params = new BookDownloadTaskParams();
  if (params == nullptr) {
    Serial.println("[books] could not allocate task params");
    return false;
  }
  params->config = config;
  params->resultQueue = bookDownloadQueue_;
  params->languageIndex = static_cast<uint8_t>(uiLanguage_);

  bookDownloadInProgress_ = true;
  BaseType_t created = xTaskCreatePinnedToCore(
      bookDownloadTask, "book_dl", kFontDownloadTaskStackBytes, params, 1, nullptr, 0);
  if (created != pdPASS) {
    Serial.printf("[books] background task create failed: %ld\n", static_cast<long>(created));
    bookDownloadInProgress_ = false;
    delete params;
    return false;
  }

  Serial.println("[books] background download started");
  return true;
}

void App::bookDownloadTask(void *params) {
  BookDownloadTaskParams *taskParams = static_cast<BookDownloadTaskParams *>(params);
  if (taskParams == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  const String code =
      starterBookLanguageCode(static_cast<UiLanguage>(taskParams->languageIndex));

  BookDownloadResult queuedResult;
  queuedResult.totalAttempted = kStarterBookCountPerLanguage;

  OtaUpdater updater;
  if (!updater.connectWiFi(taskParams->config, nullptr, nullptr)) {
    queuedResult.wifiFailed = true;
    Serial.println("[books] Wi-Fi connect failed, skipping starter books this attempt");
  } else {
    for (uint8_t i = 1; i <= kStarterBookCountPerLanguage; ++i) {
      const String assetName = "starter-" + code + "-" + String(i) + ".rsvp";
      const String destPath = "/books/books/" + assetName;
      String errorDetail;
      if (updater.downloadAsset(taskParams->config, assetName, "", destPath, errorDetail)) {
        queuedResult.downloaded++;
      } else {
        Serial.printf("[books] %s not available yet: %s\n", assetName.c_str(),
                      errorDetail.c_str());
      }
    }
    updater.disconnectWiFi();
  }

  Serial.printf("[books] starter download finished: %u/%u fetched (lang=%s)\n",
               static_cast<unsigned int>(queuedResult.downloaded),
               static_cast<unsigned int>(queuedResult.totalAttempted), code.c_str());

  if (taskParams->resultQueue != nullptr) {
    xQueueOverwrite(taskParams->resultQueue, &queuedResult);
  }

  delete taskParams;
  vTaskDelete(nullptr);
}

void App::pollBookDownloadResult(uint32_t nowMs) {
  (void)nowMs;
  if (bookDownloadQueue_ == nullptr) {
    return;
  }

  BookDownloadResult result;
  while (xQueueReceive(bookDownloadQueue_, &result, 0) == pdTRUE) {
    bookDownloadInProgress_ = false;
    if (result.downloaded > 0) {
      storage_.refreshBooks();
      Serial.printf("[books] library refreshed, %u starter title(s) added\n",
                    static_cast<unsigned int>(result.downloaded));
    }
  }
}

bool App::updateConfirmCanOpen() const {
  return otaUpdatePromptPending_ && !otaUpdatePromptDismissed_ && !pendingBootBookLoad_ &&
         (state_ == AppState::Paused || state_ == AppState::Playing);
}

void App::maybeOpenUpdateConfirm(uint32_t nowMs) {
  if (menuScreen_ == MenuScreen::UpdateConfirm || !updateConfirmCanOpen()) {
    return;
  }

  // Route through the same Menu state transition every other confirm
  // screen (Restart, SD repair) relies on for its tap-to-confirm grid
  // input handling — opening straight from Playing/Paused would leave
  // taps on this screen unresponsive, and Menu also halts reading
  // (updateReader() only advances while state_ == Playing).
  if (state_ != AppState::Menu) {
    setState(AppState::Menu, nowMs);
  }
  openUpdateConfirm();
}

bool App::blockNetworkActionForOtaCheck(const String &title, uint32_t nowMs) {
  pollOtaCheckResult(nowMs);
  if (!otaCheckInProgress_) {
    return false;
  }

  display_.renderStatus(title, "OTA check running", "Try again soon");
  delay(1200);
  renderMenu();
  return true;
}

void App::runFirmwareUpdate(const OtaUpdater::Config &config, bool automatic, uint32_t nowMs) {
  if (blockNetworkActionForOtaCheck("OTA", nowMs)) {
    return;
  }

  if (!automatic) {
    otaUpdatePromptPending_ = false;
  }

  if (!otaUpdater_.isConfigured(config)) {
    if (!automatic) {
      display_.renderStatus("OTA", tr(TrKey::WifiNotSet),
                            tr(TrKey::SettingsWifi));
      delay(1600);
      if (state_ == AppState::Menu && isSettingsListScreen()) {
        rebuildSettingsMenuItems();
        renderSettings();
      } else {
        menuScreen_ = MenuScreen::Main;
        setState(AppState::Paused, nowMs);
      }
    }
    return;
  }

  saveReadingPosition(true);
  const OtaUpdater::Result result =
      otaUpdater_.checkAndInstall(config, &App::handleStorageStatus, this);

  Serial.printf("[ota] code=%u current=%s latest=%s summary=%s detail=%s\n",
                static_cast<unsigned int>(result.code), result.currentVersion.c_str(),
                result.latestVersion.c_str(), result.summary.c_str(), result.detail.c_str());

  if (result.rebootRequired) {
    display_.renderStatus("OTA", tr(TrKey::Restarting), result.latestVersion);
    delay(300);
    ESP.restart();
    return;
  }

  if (automatic) {
    return;
  }

  const String line2 = result.detail.isEmpty() ? result.currentVersion : result.detail;
  display_.renderStatus("OTA", result.summary, line2);
  delay(1600);
  if (state_ == AppState::Menu && isSettingsListScreen()) {
    rebuildSettingsMenuItems();
    renderSettings();
  } else {
    menuScreen_ = MenuScreen::Main;
    setState(AppState::Paused, nowMs);
  }
}


String App::pacingDelayLabel(uint16_t delayMs) const { return String(delayMs) + " ms"; }

uint16_t *App::pacingDelayEditorValuePtr() {
  switch (pacingDelayEditorTarget_) {
    case PacingDelayTarget::Complexity:
      return &pacingComplexWordDelayMs_;
    case PacingDelayTarget::Punctuation:
      return &pacingPunctuationDelayMs_;
    case PacingDelayTarget::LongWords:
    default:
      return &pacingLongWordDelayMs_;
  }
}

const char *App::pacingDelayEditorPrefKey() const {
  switch (pacingDelayEditorTarget_) {
    case PacingDelayTarget::Complexity:
      return kPrefPacingComplexMs;
    case PacingDelayTarget::Punctuation:
      return kPrefPacingPunctuationMs;
    case PacingDelayTarget::LongWords:
    default:
      return kPrefPacingLongMs;
  }
}

String App::pacingDelayEditorLabel() const {
  switch (pacingDelayEditorTarget_) {
    case PacingDelayTarget::Complexity:
      return uiText(UiText::Complexity);
    case PacingDelayTarget::Punctuation:
      return uiText(UiText::Punctuation);
    case PacingDelayTarget::LongWords:
    default:
      return uiText(UiText::LongWords);
  }
}

void App::openPacingDelayEditor(PacingDelayTarget target, uint32_t nowMs) {
  pacingDelayEditorTarget_ = target;
  pacingDelayEditorTouchOnBack_ = false;
  menuScreen_ = MenuScreen::PacingDelayEditor;
  renderPacingDelayEditor();
}

void App::renderPacingDelayEditor() {
  applyReaderUiOrientation();
  currentGridButtons_.clear();
  currentGridItemIndices_.clear();

  DisplayManager::Button backButton;
  backButton.icon = ui::IconId::Back;
  currentGridButtons_.push_back(backButton);
  currentGridItemIndices_.push_back(0);

  DisplayManager::Button slider;
  slider.kind = DisplayManager::Button::ButtonKind::Slider;
  slider.label = pacingDelayEditorLabel();
  slider.x = kPacingSliderX;
  slider.y = kPacingSliderY;
  slider.width = kPacingSliderW;
  slider.height = kPacingSliderH;
  slider.sliderMin = kPacingDelayMinMs;
  slider.sliderMax = kPacingDelayMaxMs;
  slider.sliderValue = *pacingDelayEditorValuePtr();
  currentGridButtons_.push_back(slider);
  currentGridItemIndices_.push_back(1);

  applyBackButtonCornerLayout();
  display_.renderButtonGrid("", currentGridButtons_, 0, 1, activeGridToastText(millis()));
}

void App::applyPacingDelayEditorTouchX(uint16_t x) {
  DisplayManager::Button geom;
  geom.x = kPacingSliderX;
  geom.y = kPacingSliderY;
  geom.width = kPacingSliderW;
  geom.height = kPacingSliderH;
  const ui::Rect track = DisplayManager::sliderTrackRectFor(geom);

  const int clampedX = std::max(static_cast<int>(track.x),
                                std::min(static_cast<int>(x), static_cast<int>(track.x + track.w)));
  const float ratio = track.w > 0 ? static_cast<float>(clampedX - track.x) /
                                        static_cast<float>(track.w)
                                  : 0.0f;
  const int rawValue =
      kPacingDelayMinMs +
      static_cast<int>(ratio * static_cast<float>(kPacingDelayMaxMs - kPacingDelayMinMs) + 0.5f);
  const int step = std::max<int>(1, kPacingDelayStepMs);
  int snapped = ((rawValue + step / 2) / step) * step;
  snapped = clampIntSetting(snapped, kPacingDelayMinMs, kPacingDelayMaxMs);

  uint16_t *value = pacingDelayEditorValuePtr();
  if (*value != static_cast<uint16_t>(snapped)) {
    *value = static_cast<uint16_t>(snapped);
    renderPacingDelayEditor();
  }
}

void App::handlePacingDelayEditorTouch(const TouchEvent &event, uint32_t nowMs) {
  if (event.phase == TouchPhase::Start) {
    pacingDelayEditorTouchOnBack_ =
        event.x <= kPacingSliderBackHitX1 && event.y <= kPacingSliderBackHitY1;
    if (!pacingDelayEditorTouchOnBack_) {
      applyPacingDelayEditorTouchX(event.x);
    }
    return;
  }

  if (pacingDelayEditorTouchOnBack_) {
    if (event.phase == TouchPhase::End) {
      pacingDelayEditorTouchOnBack_ = false;
      if (event.x <= kPacingSliderBackHitX1 && event.y <= kPacingSliderBackHitY1) {
        flushPendingTimeEstimateRebuild();
        switch (pacingDelayEditorTarget_) {
          case PacingDelayTarget::Complexity:
            settingsSelectedIndex_ = kSettingsPacingComplexityIndex;
            break;
          case PacingDelayTarget::Punctuation:
            settingsSelectedIndex_ = kSettingsPacingPunctuationIndex;
            break;
          case PacingDelayTarget::LongWords:
          default:
            settingsSelectedIndex_ = kSettingsPacingLongWordsIndex;
            break;
        }
        menuScreen_ = MenuScreen::SettingsPacing;
        rebuildSettingsMenuItems();
        renderSettings();
      }
    }
    return;
  }

  applyPacingDelayEditorTouchX(event.x);

  if (event.phase == TouchPhase::End) {
    const uint16_t value = *pacingDelayEditorValuePtr();
    preferences_.putUShort(pacingDelayEditorPrefKey(), value);
    applyPacingSettings();
    showGridToast(pacingDelayEditorLabel() + ": " + pacingDelayLabel(value), nowMs);
    renderPacingDelayEditor();
  }
}

String App::wpmEditorLabel() const {
  String label = tr(TrKey::BaseSpeed);
  while (label.endsWith(" ") || label.endsWith(":")) {
    label.remove(label.length() - 1);
  }
  return label;
}

void App::openWpmEditor(uint32_t nowMs) {
  (void)nowMs;
  wpmEditorTouchOnBack_ = false;
  menuScreen_ = MenuScreen::WpmEditor;
  renderWpmEditor();
}

void App::renderWpmEditor() {
  applyReaderUiOrientation();
  currentGridButtons_.clear();
  currentGridItemIndices_.clear();

  DisplayManager::Button backButton;
  backButton.icon = ui::IconId::Back;
  currentGridButtons_.push_back(backButton);
  currentGridItemIndices_.push_back(0);

  DisplayManager::Button slider;
  slider.kind = DisplayManager::Button::ButtonKind::Slider;
  slider.label = wpmEditorLabel();
  slider.sliderUnit = " WPM";
  slider.x = kPacingSliderX;
  slider.y = kPacingSliderY;
  slider.width = kPacingSliderW;
  slider.height = kPacingSliderH;
  slider.sliderMin = kSettingsWpmMin;
  slider.sliderMax = kSettingsWpmMax;
  slider.sliderValue = reader_.wpm();
  currentGridButtons_.push_back(slider);
  currentGridItemIndices_.push_back(1);

  applyBackButtonCornerLayout();
  display_.renderButtonGrid("", currentGridButtons_, 0, 1, activeGridToastText(millis()));
}

void App::applyWpmEditorTouchX(uint16_t x) {
  DisplayManager::Button geom;
  geom.x = kPacingSliderX;
  geom.y = kPacingSliderY;
  geom.width = kPacingSliderW;
  geom.height = kPacingSliderH;
  const ui::Rect track = DisplayManager::sliderTrackRectFor(geom);

  const int clampedX = std::max(static_cast<int>(track.x),
                                std::min(static_cast<int>(x), static_cast<int>(track.x + track.w)));
  const float ratio = track.w > 0 ? static_cast<float>(clampedX - track.x) /
                                        static_cast<float>(track.w)
                                  : 0.0f;
  const int rawValue =
      kSettingsWpmMin +
      static_cast<int>(ratio * static_cast<float>(kSettingsWpmMax - kSettingsWpmMin) + 0.5f);
  const int step = std::max<int>(1, kWpmSliderStepWpm);
  int snapped = ((rawValue + step / 2) / step) * step;
  snapped = clampIntSetting(snapped, kSettingsWpmMin, kSettingsWpmMax);

  if (reader_.wpm() != static_cast<uint16_t>(snapped)) {
    reader_.setWpm(static_cast<uint16_t>(snapped));
    renderWpmEditor();
  }
}

void App::handleWpmEditorTouch(const TouchEvent &event, uint32_t nowMs) {
  if (event.phase == TouchPhase::Start) {
    wpmEditorTouchOnBack_ =
        event.x <= kPacingSliderBackHitX1 && event.y <= kPacingSliderBackHitY1;
    if (!wpmEditorTouchOnBack_) {
      applyWpmEditorTouchX(event.x);
    }
    return;
  }

  if (wpmEditorTouchOnBack_) {
    if (event.phase == TouchPhase::End) {
      wpmEditorTouchOnBack_ = false;
      if (event.x <= kPacingSliderBackHitX1 && event.y <= kPacingSliderBackHitY1) {
        settingsSelectedIndex_ = kSettingsPacingWpmIndex;
        menuScreen_ = MenuScreen::SettingsPacing;
        rebuildSettingsMenuItems();
        renderSettings();
      }
    }
    return;
  }

  applyWpmEditorTouchX(event.x);

  if (event.phase == TouchPhase::End) {
    preferences_.putUShort(kPrefWpm, reader_.wpm());
    Serial.printf("[settings] WPM=%u interval=%lu ms\n", reader_.wpm(),
                  static_cast<unsigned long>(reader_.wordIntervalMs()));
    showGridToast(wpmEditorLabel() + ": " + String(reader_.wpm()) + " WPM", nowMs);
    renderWpmEditor();
  }
}

App::TypographyValueEditorSpec App::typographyValueEditorSpec() const {
  TypographyValueEditorSpec spec;
  switch (typographyValueEditorTarget_) {
    case TypographyValueEditorTarget::FontSize: {
      // readerFontSizeIndex_ is 0=Large/1=Medium/2=Small everywhere else in
      // the app (readerFontSizeLabel(), the actual point-size lookup, ...).
      // The slider track fills left-to-right from sliderMin to sliderMax, so
      // mapping the index straight through put Large on the left and Small
      // on the right — backwards from what users expect (small on the left,
      // large on the right). Flip only the slider's own value/labels here;
      // commitTypographyValueEditorValue() flips it back on the way in, so
      // readerFontSizeIndex_'s meaning elsewhere is untouched.
      spec.label = uiText(UiText::FontSize);
      spec.sliderMin = 0;
      spec.sliderMax = static_cast<uint16_t>(kReaderFontSizeCount - 1);
      spec.sliderValue = static_cast<uint16_t>(kReaderFontSizeCount - 1 - readerFontSizeIndex_);
      spec.step = 1;
      spec.valueLabels = {uiText(UiText::Small), uiText(UiText::Medium), uiText(UiText::Large)};
      return spec;
    }
    case TypographyValueEditorTarget::Tracking: {
      spec.label = uiText(UiText::Tracking);
      spec.sliderMin = 0;
      spec.sliderMax = static_cast<uint16_t>(kTypographyTrackingMax - kTypographyTrackingMin);
      spec.sliderValue = static_cast<uint16_t>(typographyConfig_.trackingPx - kTypographyTrackingMin);
      spec.step = 1;
      for (int value = kTypographyTrackingMin; value <= kTypographyTrackingMax; ++value) {
        spec.valueLabels.push_back((value >= 0 ? "+" : "") + String(value) + " px");
      }
      return spec;
    }
    case TypographyValueEditorTarget::Anchor: {
      const uint8_t anchorMin =
          (handednessMode_ == HandednessMode::Left) ? kLeftHandAnchorMin : kTypographyAnchorMin;
      const uint8_t anchorMax =
          (handednessMode_ == HandednessMode::Left) ? kLeftHandAnchorMax : kTypographyAnchorMax;
      spec.label = uiText(UiText::Anchor);
      spec.sliderMin = anchorMin;
      spec.sliderMax = anchorMax;
      spec.sliderValue = effectiveAnchorPercent();
      spec.step = 1;
      spec.unit = "%";
      return spec;
    }
    case TypographyValueEditorTarget::GuideWidth: {
      spec.label = uiText(UiText::GuideWidth);
      spec.sliderMin = kTypographyGuideWidthMin;
      spec.sliderMax = kTypographyGuideWidthMax;
      spec.sliderValue = typographyConfig_.guideHalfWidth;
      spec.step = kTypographyGuideWidthStep;
      spec.unit = " px";
      return spec;
    }
    case TypographyValueEditorTarget::GuideGap:
    default: {
      spec.label = uiText(UiText::GuideGap);
      spec.sliderMin = kTypographyGuideGapMin;
      spec.sliderMax = kTypographyGuideGapMax;
      spec.sliderValue = typographyConfig_.guideGap;
      spec.step = 1;
      spec.unit = " px";
      return spec;
    }
  }
}

void App::openTypographyValueEditor(TypographyValueEditorTarget target, uint32_t nowMs) {
  (void)nowMs;
  typographyValueEditorTarget_ = target;
  typographyValueEditorTouchOnBack_ = false;
  menuScreen_ = MenuScreen::TypographyValueEditor;
  renderTypographyValueEditor();
}

void App::renderTypographyValueEditor() {
  applyReaderUiOrientation();
  currentGridButtons_.clear();
  currentGridItemIndices_.clear();

  DisplayManager::Button backButton;
  backButton.icon = ui::IconId::Back;
  currentGridButtons_.push_back(backButton);
  currentGridItemIndices_.push_back(0);

  const TypographyValueEditorSpec spec = typographyValueEditorSpec();
  DisplayManager::Button slider;
  slider.kind = DisplayManager::Button::ButtonKind::Slider;
  slider.label = spec.label;
  slider.x = kPacingSliderX;
  slider.y = kPacingSliderY;
  slider.width = kPacingSliderW;
  slider.height = kPacingSliderH;
  slider.sliderMin = spec.sliderMin;
  slider.sliderMax = spec.sliderMax;
  slider.sliderValue = spec.sliderValue;
  slider.sliderUnit = spec.unit;
  slider.sliderValueLabels = spec.valueLabels;
  currentGridButtons_.push_back(slider);
  currentGridItemIndices_.push_back(1);

  applyBackButtonCornerLayout();
  display_.renderButtonGrid("", currentGridButtons_, 0, 1, activeGridToastText(millis()));
}

bool App::touchInsideTypographySliderZone(uint16_t x, uint16_t y) const {
  DisplayManager::Button geom;
  geom.x = kPacingSliderX;
  geom.y = kPacingSliderY;
  geom.width = kPacingSliderW;
  geom.height = kPacingSliderH;
  const ui::Rect track = DisplayManager::sliderTrackRectFor(geom);

  const int minX = static_cast<int>(track.x) - kTypographySliderHitToleranceX;
  const int maxX = static_cast<int>(track.x) + static_cast<int>(track.w) + kTypographySliderHitToleranceX;
  const int minY = static_cast<int>(track.y) - kTypographySliderHitToleranceY;
  const int maxY = static_cast<int>(track.y) + static_cast<int>(track.h) + kTypographySliderHitToleranceY;

  return static_cast<int>(x) >= minX && static_cast<int>(x) <= maxX &&
         static_cast<int>(y) >= minY && static_cast<int>(y) <= maxY;
}

void App::applyTypographyValueEditorTouchX(uint16_t x) {
  DisplayManager::Button geom;
  geom.x = kPacingSliderX;
  geom.y = kPacingSliderY;
  geom.width = kPacingSliderW;
  geom.height = kPacingSliderH;
  const ui::Rect track = DisplayManager::sliderTrackRectFor(geom);

  const TypographyValueEditorSpec spec = typographyValueEditorSpec();
  const int clampedX = std::max(static_cast<int>(track.x),
                                std::min(static_cast<int>(x), static_cast<int>(track.x + track.w)));
  const float ratio =
      track.w > 0 ? static_cast<float>(clampedX - track.x) / static_cast<float>(track.w) : 0.0f;
  const int range = static_cast<int>(spec.sliderMax) - static_cast<int>(spec.sliderMin);
  const int rawValue = spec.sliderMin + static_cast<int>(ratio * static_cast<float>(range) + 0.5f);
  const int step = std::max<int>(1, spec.step);
  int snapped = ((rawValue - spec.sliderMin + step / 2) / step) * step + spec.sliderMin;
  snapped = clampIntSetting(snapped, spec.sliderMin, spec.sliderMax);

  if (spec.sliderValue != static_cast<uint16_t>(snapped)) {
    commitTypographyValueEditorValue(static_cast<uint16_t>(snapped), millis());
    renderTypographyValueEditor();
  }
}

void App::commitTypographyValueEditorValue(uint16_t sliderValue, uint32_t nowMs) {
  switch (typographyValueEditorTarget_) {
    case TypographyValueEditorTarget::FontSize:
      // Inverse of the flip in typographyValueEditorSpec() — see comment there.
      readerFontSizeIndex_ = static_cast<uint8_t>(kReaderFontSizeCount - 1 - sliderValue);
      preferences_.putUChar(kPrefReaderFontSize, readerFontSizeIndex_);
      applyDisplayPreferences(nowMs);
      return;
    case TypographyValueEditorTarget::Tracking:
      typographyConfig_.trackingPx =
          static_cast<int8_t>(static_cast<int>(sliderValue) + kTypographyTrackingMin);
      preferences_.putChar(kPrefTypographyTracking, typographyConfig_.trackingPx);
      break;
    case TypographyValueEditorTarget::Anchor:
      typographyConfig_.anchorPercent = (handednessMode_ == HandednessMode::Left)
                                             ? static_cast<uint8_t>(sliderValue - kLeftHandAnchorOffset)
                                             : static_cast<uint8_t>(sliderValue);
      preferences_.putUChar(kPrefTypographyAnchor, typographyConfig_.anchorPercent);
      break;
    case TypographyValueEditorTarget::GuideWidth:
      typographyConfig_.guideHalfWidth = static_cast<uint8_t>(sliderValue);
      preferences_.putUChar(kPrefTypographyGuideWidth, typographyConfig_.guideHalfWidth);
      break;
    case TypographyValueEditorTarget::GuideGap:
      typographyConfig_.guideGap = static_cast<uint8_t>(sliderValue);
      preferences_.putUChar(kPrefTypographyGuideGap, typographyConfig_.guideGap);
      break;
  }
  applyTypographySettings(nowMs);
}

void App::handleTypographyValueEditorTouch(const TouchEvent &event, uint32_t nowMs) {
  if (event.phase == TouchPhase::Start) {
    typographyValueEditorTouchOnBack_ =
        event.x <= kPacingSliderBackHitX1 && event.y <= kPacingSliderBackHitY1;
    if (!typographyValueEditorTouchOnBack_) {
      typographyValueEditorTouchOnSlider_ = touchInsideTypographySliderZone(event.x, event.y);
      if (typographyValueEditorTouchOnSlider_) {
        applyTypographyValueEditorTouchX(event.x);
      }
    }
    return;
  }

  if (typographyValueEditorTouchOnBack_) {
    if (event.phase == TouchPhase::End) {
      typographyValueEditorTouchOnBack_ = false;
      if (event.x <= kPacingSliderBackHitX1 && event.y <= kPacingSliderBackHitY1) {
        menuScreen_ = MenuScreen::TypographyTuning;
        renderTypographyTuning();
      }
    }
    return;
  }

  if (!typographyValueEditorTouchOnSlider_) {
    // Touch started on dead space (e.g. the label text) — ignore the whole
    // gesture instead of dragging the value to wherever it happens to end.
    return;
  }

  applyTypographyValueEditorTouchX(event.x);

  if (event.phase == TouchPhase::End) {
    typographyValueEditorTouchOnSlider_ = false;
    const TypographyValueEditorSpec spec = typographyValueEditorSpec();
    const String valueText = !spec.valueLabels.empty() && spec.sliderValue < spec.valueLabels.size()
                                  ? spec.valueLabels[spec.sliderValue]
                                  : String(spec.sliderValue) + spec.unit;
    showGridToast(spec.label + ": " + valueText, nowMs);
    renderTypographyValueEditor();
  }
}

String App::firmwareUpdateMenuLabel() const {
  return tr(TrKey::FirmwareUpdate);
}

String App::uiText(UiText key) const { return Localization::text(uiLanguage_, key); }

String App::themeModeLabel() const {
  if (nightMode_) {
    return uiText(UiText::Night);
  }
  return darkMode_ ? uiText(UiText::Dark) : uiText(UiText::Light);
}

String App::phantomWordsLabel() const {
  return phantomWordsEnabled_ ? uiText(UiText::On) : uiText(UiText::Off);
}

String App::focusHighlightLabel() const {
  return typographyConfig_.focusHighlight ? uiText(UiText::On) : uiText(UiText::Off);
}

String App::focusColorLabel() const {
  switch (display_.focusColorIndex()) {
    case 0: return tr3(TrKey3::ColorRed);
    case 1: return tr3(TrKey3::ColorBlue);
    case 2: return tr3(TrKey3::ColorGreen);
    case 3: return tr3(TrKey3::ColorYellow);
    case 4: return tr3(TrKey3::ColorOrange);
    case 5: return tr3(TrKey3::ColorPurple);
    default: return tr3(TrKey3::ColorRed);
  }
}

void App::cycleFocusColor(uint32_t nowMs) {
  uint8_t next = display_.focusColorIndex() + 1;
  if (next >= 6) next = 0;
  display_.setFocusColorIndex(next);
  preferences_.putUChar(kPrefFocusColorIndex, next);
  applyDisplayPreferences(nowMs);
}

String App::uiLanguageLabel() const { return Localization::languageName(uiLanguage_); }

String App::readerModeLabel() const {
  switch (readerMode_) {
    case ReaderMode::Scroll:
      return uiText(UiText::ScrollMode);
    case ReaderMode::Rsvp:
    default:
      return uiText(UiText::RsvpMode);
  }
}

void App::showScrollSettingsPreview() {
  // Build sample words for preview
  static const char *const kScrollPreviewText[] = {
      "The", "quick", "brown", "fox", "jumps", "over",
      "the", "lazy", "dog.", "Reading", "is", "a",
      "wonderful", "way", "to", "explore", "new", "worlds.",
      "Every", "book", "opens", "a", "door", "to",
      "knowledge", "and", "imagination."
  };
  constexpr size_t kScrollPreviewWordCount = sizeof(kScrollPreviewText) / sizeof(kScrollPreviewText[0]);

  std::vector<DisplayManager::ContextWord> previewWords;
  previewWords.reserve(kScrollPreviewWordCount);
  for (size_t i = 0; i < kScrollPreviewWordCount; ++i) {
    DisplayManager::ContextWord w;
    w.text = kScrollPreviewText[i];
    w.paragraphStart = (i == 0 || i == 9 || i == 18);
    w.current = (i == 5);  // highlight "over" as current word
    previewWords.push_back(w);
  }

  // Apply current scroll display settings before rendering
  display_.setScrollFontSize(scrollFontSize_);
  display_.setScrollLineSpacing(scrollLineSpacing_);
  display_.setScrollMargin(scrollMargin_);

  // Build a label showing current setting values
  String overlayText = "Font:" + scrollFontSizeLabel() +
                       " Spacing:" + scrollLineSpacingLabel() +
                       " Margin:" + scrollMarginLabel();

  DisplayManager::ReaderChrome chrome;
  chrome.showBattery = false;
  chrome.showChapter = false;
  chrome.showProgress = false;
  chrome.showPreviousSentenceHint = false;
  chrome.showSavePointButton = false;

  // Force a fresh render by using a unique content token
  static uint32_t previewToken = 90000;
  ++previewToken;

  display_.renderScrollView(previewWords, previewToken,
                            0, 5, 0,
                            "", 0, overlayText,
                            "", chrome);

  // Show preview for 1500ms then return to menu
  delay(1500);
  rebuildSettingsMenuItems();
  renderSettings();
}

String App::scrollFontSizeLabel() const {
  return String(scrollFontSize_);
}

String App::scrollLineSpacingLabel() const {
  static const char *const labels[] = {"Compact", "Normal", "Relaxed"};
  const uint8_t idx = scrollLineSpacing_ <= 2 ? scrollLineSpacing_ : 1;
  return String(labels[idx]);
}

String App::scrollMarginLabel() const {
  static const char *const labels[] = {"Narrow", "Normal", "Wide"};
  const uint8_t idx = scrollMargin_ <= 2 ? scrollMargin_ : 1;
  return String(labels[idx]);
}

String App::pauseModeLabel() const {
  return pauseMode_ == PauseMode::Instant ? tr(TrKey::Instant)
                                          : tr(TrKey::Sentence);
}

String App::handednessLabel() const {
  return handednessMode_ == HandednessMode::Left ? tr(TrKey::LeftHand)
                                                 : tr(TrKey::RightHand);
}

String App::savePointNameModeLabel() const {
  return savePointUseCustomName_ ? tr3(TrKey3::SavePointNameCustomOption)
                                 : tr3(TrKey3::SavePointNameDefaultOption);
}

String App::navModeLabel() const {
  switch (navMode_) {
    case NavMode::DPad:
      return "D-Pad";
    case NavMode::Buttons:
      return tr3(TrKey3::ButtonsLabel);
    case NavMode::Swipe:
    default:
      return "Swipe";
  }
}

String App::readerFontSizeLabel() const {
  uint8_t levelIndex = readerFontSizeIndex_;
  if (levelIndex >= kReaderFontSizeCount) {
    levelIndex = 0;
  }

  switch (levelIndex) {
    case 0:
      return uiText(UiText::Large);
    case 1:
      return uiText(UiText::Medium);
    case 2:
    default:
      return uiText(UiText::Small);
  }
}

String App::typefaceDisplayName(DisplayManager::ReaderTypeface typeface) const {
  switch (typeface) {
    case DisplayManager::ReaderTypeface::AtkinsonHyperlegible:
      return "Atkinson";
    case DisplayManager::ReaderTypeface::OpenDyslexic:
      return "OpenDyslexic";
    case DisplayManager::ReaderTypeface::Literata:
      return "Literata";
    case DisplayManager::ReaderTypeface::Merriweather:
      return "Merriweather";
    case DisplayManager::ReaderTypeface::Lora:
      return "Lora";
    case DisplayManager::ReaderTypeface::Bitter:
      return "Bitter";
    case DisplayManager::ReaderTypeface::EBGaramond:
      return "EB Garamond";
    case DisplayManager::ReaderTypeface::Vollkorn:
      return "Vollkorn";
    case DisplayManager::ReaderTypeface::Gelasio:
      return "Gelasio";
    case DisplayManager::ReaderTypeface::PtSerif:
      return "PT Serif";
    case DisplayManager::ReaderTypeface::IbmPlexSerif:
      return "IBM Plex Serif";
    case DisplayManager::ReaderTypeface::Cardo:
      return "Cardo";
    case DisplayManager::ReaderTypeface::ZillaSlab:
      return "Zilla Slab";
    case DisplayManager::ReaderTypeface::OldStandard:
      return "Old Standard";
    case DisplayManager::ReaderTypeface::Domine:
      return "Domine";
    case DisplayManager::ReaderTypeface::Alegreya:
      return "Alegreya";
    case DisplayManager::ReaderTypeface::Newsreader:
      return "Newsreader";
    case DisplayManager::ReaderTypeface::NotoSerif:
      return "Noto Serif";
    case DisplayManager::ReaderTypeface::Spectral:
      return "Spectral";
    case DisplayManager::ReaderTypeface::Standard:
    default:
      return uiText(UiText::Standard);
  }
}

String App::readerTypefaceLabel() const { return typefaceDisplayName(typographyConfig_.typeface); }

String App::typographyTuningLabel() const {
  switch (typographyTuningSelectedIndex_) {
    case TypographyTuningBack:
      return uiText(UiText::Back);
    case TypographyTuningFontSize:
      return uiText(UiText::FontSize);
    case TypographyTuningTypeface:
      return uiText(UiText::Typeface);
    case TypographyTuningPhantomWords:
      return uiText(UiText::PhantomWords);
    case TypographyTuningFocusHighlight:
      return uiText(UiText::RedHighlight);
    case TypographyTuningTracking:
      return uiText(UiText::Tracking);
    case TypographyTuningAnchor:
      return uiText(UiText::Anchor);
    case TypographyTuningGuideWidth:
      return uiText(UiText::GuideWidth);
    case TypographyTuningGuideGap:
      return uiText(UiText::GuideGap);
    case TypographyTuningReset:
      return uiText(UiText::Reset);
    default:
      return uiText(UiText::Typography);
  }
}

String App::typographyTuningValueLabel() const {
  switch (typographyTuningSelectedIndex_) {
    case TypographyTuningBack:
      return uiText(UiText::TapToExit);
    case TypographyTuningFontSize:
      return readerFontSizeLabel();
    case TypographyTuningTypeface:
      return readerTypefaceLabel();
    case TypographyTuningPhantomWords:
      return phantomWordsLabel();
    case TypographyTuningFocusHighlight:
      return focusHighlightLabel();
    case TypographyTuningTracking:
      return String(typographyConfig_.trackingPx >= 0 ? "+" : "") +
             String(static_cast<int>(typographyConfig_.trackingPx)) + " px";
    case TypographyTuningAnchor:
      return String(static_cast<unsigned int>(effectiveAnchorPercent())) + "%";
    case TypographyTuningGuideWidth:
      return String(static_cast<unsigned int>(typographyConfig_.guideHalfWidth)) + " px";
    case TypographyTuningGuideGap:
      return String(static_cast<unsigned int>(typographyConfig_.guideGap)) + " px";
    case TypographyTuningReset:
      return uiText(UiText::TapToReset);
    default:
      return "";
  }
}

void App::openBookPicker(bool articlesOnly) {
  storage_.refreshBooks();
  restoreArchivedSavePointsForReturnedBooks();
  bookMenuItems_.clear();
  bookPickerBookIndices_.clear();
  bookMenuItems_.push_back(
      {wizardBookPickerActive_ ? tr2(TrKey2::SkipForNow) : uiText(UiText::Back), ""});

  const size_t count = storage_.bookCount();
  std::vector<size_t> sortedBookIndices;
  sortedBookIndices.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    // Library shows all items (books + articles together)
    (void)articlesOnly;
    sortedBookIndices.push_back(i);
  }

  std::stable_sort(sortedBookIndices.begin(), sortedBookIndices.end(),
                   [this](size_t leftIndex, size_t rightIndex) {
                     const bool leftCurrent =
                         usingStorageBook_ && leftIndex == currentBookIndex_;
                     const bool rightCurrent =
                         usingStorageBook_ && rightIndex == currentBookIndex_;
                     if (leftCurrent != rightCurrent) {
                       return leftCurrent;
                     }

                     const uint32_t leftRecent =
                         bookRecentSequence(storage_.bookPath(leftIndex));
                     const uint32_t rightRecent =
                         bookRecentSequence(storage_.bookPath(rightIndex));
                     const bool leftHasRecent = leftRecent > 0;
                     const bool rightHasRecent = rightRecent > 0;
                     if (leftHasRecent != rightHasRecent) {
                       return leftHasRecent;
                     }
                     if (leftRecent != rightRecent) {
                       return leftRecent > rightRecent;
                     }

                     return false;
                   });

  for (size_t bookIndex : sortedBookIndices) {
    bookPickerBookIndices_.push_back(bookIndex);
    bookMenuItems_.push_back(libraryItemForBook(bookIndex));
  }

  if (sortedBookIndices.empty()) {
    Serial.printf("[book-picker] No SD %s available\n", articlesOnly ? "articles" : "books");
  }

  menuScreen_ = MenuScreen::BookPicker;
  bookPickerSelectedIndex_ = kBookPickerBackIndex;
  if (usingStorageBook_) {
    for (size_t row = 0; row < bookPickerBookIndices_.size(); ++row) {
      if (bookPickerBookIndices_[row] == currentBookIndex_) {
        bookPickerSelectedIndex_ = row + 1;
        break;
      }
    }
  }
  renderBookPicker();
}

void App::selectBookPickerItem(uint32_t nowMs) {
  if (bookPickerSelectedIndex_ == kBookPickerBackIndex || bookMenuItems_.size() <= 1) {
    if (wizardBookPickerActive_) {
      // Krok 2.4 kreatora, "Wstecz"/brak książek — kończymy kreator bez
      // otwierania żadnej książki, zamiast wracać do Biblioteki, która i tak
      // jest pusta.
      finishWelcomeWizard(nowMs);
      return;
    }
    menuScreen_ = MenuScreen::Main;
    menuSelectedIndex_ = MenuLibrary;
    renderMainMenu();
    return;
  }

  const size_t rowIndex = bookPickerSelectedIndex_ - 1;
  if (rowIndex >= bookPickerBookIndices_.size()) {
    renderBookPicker();
    return;
  }

  const size_t bookIndex = bookPickerBookIndices_[rowIndex];
  if (wizardBookPickerActive_) {
    // "Prawie gotowe! Co dziś czytamy?" — wybór książki od razu otwiera
    // czytanie, bez przystanku na ekranie szczegółów książki.
    wizardBookPickerActive_ = false;
    preferences_.putBool(kPrefSetupDone, true);
    preferences_.putBool(kPrefTutorialDone, true);
    tutorialCompleted_ = true;
    saveReadingPosition(true);
    if (loadBookAtIndex(bookIndex, nowMs, true, true, true, true)) {
      menuScreen_ = MenuScreen::Main;
      setState(AppState::Paused, nowMs);
    } else {
      finishWelcomeWizard(nowMs);
    }
    return;
  }

  openBookDetails(bookIndex, nowMs);
}

void App::openChapterPicker() {
  chapterMenuItems_.clear();
  chapterMenuItems_.push_back(uiText(UiText::Back));

  if (chapterMarkers_.empty()) {
    chapterMenuItems_.push_back(uiText(UiText::StartOfBook));
    chapterPickerSelectedIndex_ = kChapterPickerFallbackIndex;
    Serial.println("[chapter-picker] No chapter markers found; showing start fallback");
  } else {
    for (size_t i = 0; i < chapterMarkers_.size(); ++i) {
      chapterMenuItems_.push_back(chapterMenuLabel(i));
    }

    size_t selectedChapter = 0;
    const size_t currentWordIndex = reader_.currentIndex();
    for (size_t i = 0; i < chapterMarkers_.size(); ++i) {
      if (chapterMarkers_[i].wordIndex <= currentWordIndex) {
        selectedChapter = i;
      }
    }
    chapterPickerSelectedIndex_ = selectedChapter + 1;
  }

  chapterMenuItems_.push_back(uiText(UiText::RestartBook));

  menuScreen_ = MenuScreen::ChapterPicker;
  renderChapterPicker();
}

void App::selectChapterPickerItem(uint32_t nowMs) {
  if (chapterPickerSelectedIndex_ == kChapterPickerBackIndex || chapterMenuItems_.size() <= 1) {
    // Return to book details if we came from there, otherwise main menu
    if (bookDetailsMenuItems_.size() > 0) {
      menuScreen_ = MenuScreen::BookDetails;
      renderBookDetails();
    } else {
      menuScreen_ = MenuScreen::Main;
      renderMainMenu();
    }
    return;
  }

  const size_t restartIndex = chapterMenuItems_.size() - 1;
  if (chapterPickerSelectedIndex_ == restartIndex) {
    openRestartConfirm();
    return;
  }

  if (chapterMarkers_.empty()) {
    reader_.seekTo(0);
    menuScreen_ = MenuScreen::Main;
    setState(AppState::Paused, nowMs);
    saveReadingPosition(true);
    Serial.println("[chapter-picker] jumped to start of book");
    return;
  }

  const size_t chapterIndex = chapterPickerSelectedIndex_ - 1;
  if (chapterIndex >= chapterMarkers_.size()) {
    renderChapterPicker();
    return;
  }

  // Save current position as auto save point before jumping
  saveReadingPosition(true);
  reader_.seekTo(chapterMarkers_[chapterIndex].wordIndex);
  menuScreen_ = MenuScreen::Main;
  setState(AppState::Paused, nowMs);
  saveReadingPosition(true);
  Serial.printf("[chapter-picker] jumped to %s at word %u\n",
                chapterMarkers_[chapterIndex].title.c_str(),
                static_cast<unsigned int>(chapterMarkers_[chapterIndex].wordIndex));
}

// ─── Book Details ────────────────────────────────────────────────────────────

void App::openBookDetails(size_t bookIndex, uint32_t nowMs) {
  (void)nowMs;
  bookDetailsBookIndex_ = bookIndex;
  bookDetailsMenuItems_.clear();
  bookDetailsMenuItems_.push_back(uiText(UiText::Back));

  const String title = storage_.bookDisplayName(bookIndex);
  const String author = storage_.bookAuthorName(bookIndex);
  uint8_t percent = 0;
  bookProgressPercent(bookIndex, percent);

  bookDetailsMenuItems_.push_back(title + (author.isEmpty() ? "" : " - " + author));
  bookDetailsMenuItems_.push_back(String(static_cast<unsigned int>(percent)) + "% " +
                                  tr3(TrKey3::PercentComplete));
  bookDetailsMenuItems_.push_back(tr3(TrKey3::ReadFromPlace));
  bookDetailsMenuItems_.push_back(uiText(UiText::Chapters));
  bookDetailsMenuItems_.push_back(uiText(UiText::RestartBook));
  bookDetailsMenuItems_.push_back(tr3(TrKey3::DeleteBookLabel));

  bookDetailsSelectedIndex_ = 3;  // "Czytaj od miejsca"
  menuScreen_ = MenuScreen::BookDetails;
  renderBookDetails();
}

void App::selectBookDetailsItem(uint32_t nowMs) {
  switch (bookDetailsSelectedIndex_) {
    case 0:  // Back
      menuScreen_ = MenuScreen::BookPicker;
      renderBookPicker();
      return;
    case 1:  // Title (info only)
    case 2:  // Progress (info only)
      return;
    case 3: {  // Czytaj od miejsca
      saveReadingPosition(true);
      if (!loadBookAtIndex(bookDetailsBookIndex_, nowMs, true, true, true, true)) {
        display_.renderStatus(tr3(TrKey3::ErrorLabel),
                              storage_.bookDisplayName(bookDetailsBookIndex_), "");
        delay(1400);
        renderBookDetails();
        return;
      }
      bookDetailsMenuItems_.clear();  // signal we left book details
      menuScreen_ = MenuScreen::Main;
      setState(AppState::Paused, nowMs);
      return;
    }
    case 4: {  // Rozdzialy
      // Load the book so we have chapter markers available
      saveReadingPosition(true);
      if (!loadBookAtIndex(bookDetailsBookIndex_, nowMs, true, true, true, true)) {
        display_.renderStatus(tr3(TrKey3::ErrorLabel),
                              storage_.bookDisplayName(bookDetailsBookIndex_), "");
        delay(1400);
        renderBookDetails();
        return;
      }
      openChapterPicker();
      return;
    }
    case 5:  // Restart
      saveReadingPosition(true);
      if (!loadBookAtIndex(bookDetailsBookIndex_, nowMs, true, true, true, true)) {
        display_.renderStatus(tr3(TrKey3::ErrorLabel),
                              storage_.bookDisplayName(bookDetailsBookIndex_), "");
        delay(1400);
        renderBookDetails();
        return;
      }
      openRestartConfirm();
      return;
    case 6:  // Delete book
      openBookDeleteConfirm(nowMs);
      return;
    default:
      return;
  }
}

void App::renderBookDetails() {
  renderMenuAnyMode("", bookDetailsMenuItems_, bookDetailsSelectedIndex_);
}

// ─── Book Delete Confirm ─────────────────────────────────────────────────────

void App::openBookDeleteConfirm(uint32_t nowMs) {
  (void)nowMs;
  bookDeleteConfirmMenuItems_.clear();
  bookDeleteConfirmMenuItems_.push_back(uiText(UiText::Back));

  const String title = storage_.bookDisplayName(bookDetailsBookIndex_);
  bookDeleteConfirmMenuItems_.push_back(String(tr3(TrKey3::DeleteConfirmColon)) + title);
  bookDeleteConfirmMenuItems_.push_back(tr3(TrKey3::NoGoBack));
  bookDeleteConfirmMenuItems_.push_back(tr3(TrKey3::YesDelete));

  bookDeleteConfirmSelectedIndex_ = 2;  // default to "No"
  menuScreen_ = MenuScreen::BookDeleteConfirm;
  renderItemGrid("", bookDeleteConfirmMenuItems_, bookDeleteConfirmSelectedIndex_);
}

void App::selectBookDeleteConfirmItem(uint32_t nowMs) {
  switch (bookDeleteConfirmSelectedIndex_) {
    case 0:  // Back
    case 2:  // Nie, wroc
      menuScreen_ = MenuScreen::BookDetails;
      renderBookDetails();
      return;
    case 1:  // Title (info only)
      return;
    case 3:  // Tak, usun
      executeDeleteBook(nowMs);
      return;
    default:
      return;
  }
}

void App::executeDeleteBook(uint32_t nowMs) {
  (void)nowMs;
  const String bookPath = storage_.bookPath(bookDetailsBookIndex_);
  const String bookTitle = storage_.bookDisplayName(bookDetailsBookIndex_);

  // Clear saved progress for this book
  const String positionKey = bookPositionKey(bookPath);
  const String countKey = bookWordCountKey(bookPath);
  const String recentKey = bookRecentKey(bookPath);
  if (preferences_.isKey(positionKey.c_str())) {
    preferences_.remove(positionKey.c_str());
  }
  if (preferences_.isKey(countKey.c_str())) {
    preferences_.remove(countKey.c_str());
  }
  if (preferences_.isKey(recentKey.c_str())) {
    preferences_.remove(recentKey.c_str());
  }

  // Explicit save points (bookmarks) are a separate feature from reading
  // position above — move any pointing at this book into the hidden SD
  // trash instead of leaving them stuck in the visible list.
  archiveSavePointsForDeletedBook(bookPath);

  // If we're deleting the currently loaded book, close it FIRST so the file
  // handle is released before we attempt to remove the file from SD.
  // Note: for EPUBs, currentBookPath_ points to the converted .rsvp while
  // bookPath is the .epub source. Check both paths.
  const bool deletingCurrent = usingStorageBook_ &&
      (currentBookPath_ == bookPath ||
       currentBookIndex_ == bookDetailsBookIndex_);
  if (deletingCurrent) {
    activeBookStore_.close();
    reader_.clearLoadedBook(millis());
    usingStorageBook_ = false;
    currentBookPath_ = "";
    currentBookTitle_ = "";
  }

  // Delete the book file from SD
  const bool deleted = storage_.deleteBook(bookDetailsBookIndex_);

  if (deleted) {
    display_.renderStatus(tr3(TrKey3::DeletedLabel), bookTitle, "");
  } else {
    display_.renderStatus(tr3(TrKey3::ErrorLabel),
                          tr3(TrKey3::CannotDelete), bookTitle);
  }
  delay(1400);
  flushStaleTouch();

  // If we deleted the current book, load another one
  if (deletingCurrent) {
    if (storage_.bookCount() > 0) {
      loadBookAtIndex(0, millis());
    }
  } else {
    // Update currentBookIndex_ if it shifted
    if (bookDetailsBookIndex_ < currentBookIndex_) {
      --currentBookIndex_;
    }
  }

  // Return to library
  openBookPicker(false);
}

// ─── Save Points ─────────────────────────────────────────────────────────────

void App::loadSavePoints() {
  savePoints_.clear();
  // Save points stored in preferences as: sp_count, sp_N_name, sp_N_book,
  // sp_N_title, sp_N_word, sp_N_pct
  const size_t count = preferences_.getUChar("sp_count", 0);
  for (size_t i = 0; i < count && i < kMaxSavePoints; ++i) {
    SavePoint sp;
    const String prefix = "sp_" + String(static_cast<unsigned int>(i)) + "_";
    sp.name = preferences_.getString((prefix + "name").c_str(), "");
    sp.bookPath = preferences_.getString((prefix + "book").c_str(), "");
    sp.bookTitle = preferences_.getString((prefix + "titl").c_str(), "");
    sp.wordIndex = preferences_.getUInt((prefix + "word").c_str(), 0);
    sp.progressPercent = preferences_.getUChar((prefix + "pct").c_str(), 0);
    if (!sp.bookPath.isEmpty()) {
      savePoints_.push_back(sp);
    }
  }
}

void App::persistSavePoints() {
  const size_t count = std::min(savePoints_.size(), kMaxSavePoints);
  preferences_.putUChar("sp_count", static_cast<uint8_t>(count));
  for (size_t i = 0; i < count; ++i) {
    const String prefix = "sp_" + String(static_cast<unsigned int>(i)) + "_";
    preferences_.putString((prefix + "name").c_str(), savePoints_[i].name);
    preferences_.putString((prefix + "book").c_str(), savePoints_[i].bookPath);
    preferences_.putString((prefix + "titl").c_str(), savePoints_[i].bookTitle);
    preferences_.putUInt((prefix + "word").c_str(), static_cast<uint32_t>(savePoints_[i].wordIndex));
    preferences_.putUChar((prefix + "pct").c_str(), savePoints_[i].progressPercent);
  }
  // Clear any leftover entries beyond current count
  for (size_t i = count; i < kMaxSavePoints; ++i) {
    const String prefix = "sp_" + String(static_cast<unsigned int>(i)) + "_";
    preferences_.remove((prefix + "name").c_str());
    preferences_.remove((prefix + "book").c_str());
    preferences_.remove((prefix + "titl").c_str());
    preferences_.remove((prefix + "word").c_str());
    preferences_.remove((prefix + "pct").c_str());
  }
}

namespace {
String sanitizeSavePointTrashField(String value) {
  value.replace("|", " ");
  value.replace("\n", " ");
  value.replace("\r", " ");
  return value;
}
}  // namespace

// A deleted book's explicit save points (bookmarks) used to stay in the
// visible Punkty zapisu list forever, pointing at a book that no longer
// exists. Karol chose "hidden SD trash" over a visible Archive screen: move
// them out of the normal list into a plain-text file nobody sees, and bring
// them back automatically if a book with the same path ever reappears in
// the library (restoreArchivedSavePointsForReturnedBooks()) — otherwise they
// just stay there, invisibly, instead of being destroyed outright.
void App::archiveSavePointsForDeletedBook(const String &bookPath) {
  loadSavePoints();

  // Save points store whatever path was actually open for reading, which
  // for an EPUB is the converted .rsvp cache, not the .epub source path
  // passed in here (see the deletingCurrent comment a few lines below) —
  // match either.
  const String epubCachePath = storage_.epubCacheRsvpPath(bookPath);

  std::vector<SavePoint> remaining;
  std::vector<String> trashLines;
  remaining.reserve(savePoints_.size());
  for (const SavePoint &sp : savePoints_) {
    if (sp.bookPath != bookPath && sp.bookPath != epubCachePath) {
      remaining.push_back(sp);
      continue;
    }

    String line = sanitizeSavePointTrashField(sp.bookPath);
    line += "|";
    line += sanitizeSavePointTrashField(sp.name);
    line += "|";
    line += sanitizeSavePointTrashField(sp.bookTitle);
    line += "|";
    line += String(static_cast<unsigned int>(sp.wordIndex));
    line += "|";
    line += String(static_cast<unsigned int>(sp.chapterIndex));
    line += "|";
    line += String(static_cast<unsigned int>(sp.progressPercent));
    trashLines.push_back(line);
  }

  if (trashLines.empty()) {
    return;
  }

  storage_.appendSavePointTrashLines(trashLines);
  savePoints_ = remaining;
  persistSavePoints();
  Serial.printf("[save-point] archived %u entr%s for deleted book %s\n",
                static_cast<unsigned int>(trashLines.size()), trashLines.size() == 1 ? "y" : "ies",
                bookPath.c_str());
}

void App::restoreArchivedSavePointsForReturnedBooks() {
  std::vector<String> trashLines = storage_.readSavePointTrashLines();
  if (trashLines.empty()) {
    return;
  }

  loadSavePoints();

  std::vector<String> stillTrashed;
  std::vector<SavePoint> restored;
  for (const String &line : trashLines) {
    int firstBar = line.indexOf('|');
    int secondBar = firstBar >= 0 ? line.indexOf('|', firstBar + 1) : -1;
    int thirdBar = secondBar >= 0 ? line.indexOf('|', secondBar + 1) : -1;
    int fourthBar = thirdBar >= 0 ? line.indexOf('|', thirdBar + 1) : -1;
    int fifthBar = fourthBar >= 0 ? line.indexOf('|', fourthBar + 1) : -1;
    if (fifthBar < 0) {
      continue;  // malformed line, drop it rather than looping on it forever
    }

    const String bookPath = line.substring(0, firstBar);
    if (!storage_.bookExistsAtPath(bookPath)) {
      stillTrashed.push_back(line);
      continue;
    }

    SavePoint sp;
    sp.bookPath = bookPath;
    sp.name = line.substring(firstBar + 1, secondBar);
    sp.bookTitle = line.substring(secondBar + 1, thirdBar);
    sp.wordIndex = static_cast<size_t>(line.substring(thirdBar + 1, fourthBar).toInt());
    sp.chapterIndex = static_cast<size_t>(line.substring(fourthBar + 1, fifthBar).toInt());
    sp.progressPercent = static_cast<uint8_t>(line.substring(fifthBar + 1).toInt());
    restored.push_back(sp);
  }

  if (restored.empty()) {
    return;
  }

  for (const SavePoint &sp : restored) {
    savePoints_.push_back(sp);
    if (savePoints_.size() > kMaxSavePoints) {
      savePoints_.erase(savePoints_.begin());
    }
  }
  persistSavePoints();
  storage_.writeSavePointTrashLines(stillTrashed);
  Serial.printf("[save-point] restored %u entr%s for books back in the library\n",
                static_cast<unsigned int>(restored.size()), restored.size() == 1 ? "y" : "ies");
}

void App::finishSavePointCreation(const String &name, uint32_t nowMs) {
  if (usingStorageBook_ && !currentBookPath_.isEmpty()) {
    SavePoint sp;
    sp.bookPath = currentBookPath_;
    sp.bookTitle = currentBookTitle_;
    sp.wordIndex = reader_.currentIndex();
    sp.progressPercent = readingProgressPercent();
    sp.name = name;
    loadSavePoints();
    savePoints_.insert(savePoints_.begin(), sp);
    if (savePoints_.size() > kMaxSavePoints) {
      savePoints_.pop_back();
    }
    persistSavePoints();
    Serial.printf("[save-point] created: %s word=%u\n", sp.name.c_str(),
                  static_cast<unsigned int>(sp.wordIndex));
    display_.renderStatus(uiText(UiText::SavePoints),
                          tr3(TrKey3::BookmarkAdded), name);
    delay(1200);
  }
  flushStaleTouch();
  if (savePointQuickSaveFromReader_) {
    // Quick-save from the reader: go back to reading, not to the
    // SavePointsList menu — the point was already saved, the user
    // wants to keep reading, not browse their bookmarks.
    savePointQuickSaveFromReader_ = false;
    menuScreen_ = MenuScreen::Main;
    setState(AppState::Paused, nowMs);
    return;
  }
  openSavePointsList();
}

void App::createSavePoint(uint32_t nowMs) {
  (void)nowMs;
  if (!usingStorageBook_ || currentBookPath_.isEmpty()) {
    return;
  }

  SavePoint sp;
  sp.bookPath = currentBookPath_;
  sp.bookTitle = currentBookTitle_;
  sp.wordIndex = reader_.currentIndex();
  sp.progressPercent = readingProgressPercent();

  // Build meaningful default name: chapter name + percentage
  const String chapter = currentChapterLabel();
  if (chapter.isEmpty() || chapter == currentBookTitle_) {
    sp.name = currentBookTitle_ + " " + String(static_cast<unsigned int>(sp.progressPercent)) + "%";
  } else {
    sp.name = chapter + " " + String(static_cast<unsigned int>(sp.progressPercent)) + "%";
  }

  // Add to front (newest first)
  savePoints_.insert(savePoints_.begin(), sp);
  if (savePoints_.size() > kMaxSavePoints) {
    savePoints_.pop_back();
  }
  persistSavePoints();
  Serial.printf("[save-point] created: %s word=%u\n", sp.name.c_str(),
                static_cast<unsigned int>(sp.wordIndex));
}

void App::deleteSavePoint(size_t index) {
  if (index >= savePoints_.size()) return;
  savePoints_.erase(savePoints_.begin() + static_cast<int>(index));
  persistSavePoints();
}

void App::openSavePointsList() {
  loadSavePoints();
  savePointMenuItems_.clear();
  savePointMenuItems_.push_back(uiText(UiText::Back));
  savePointMenuItems_.push_back(tr3(TrKey3::AddSavePoint));

  for (size_t i = 0; i < savePoints_.size(); ++i) {
    const auto &sp = savePoints_[i];
    String label = sp.name.isEmpty() ? sp.bookTitle : sp.name;
    savePointMenuItems_.push_back(label);
    // No colon in this label on purpose: a colon makes isDestructiveGridLabel()
    // treat it as an irreversible action requiring its own arm-then-confirm
    // tap, but this button only opens the confirm screen below — the actual
    // delete already gets its own Tak/Nie step there (same convention as
    // "Usun ksiazke" opening BookDeleteConfirm).
    savePointMenuItems_.push_back(String(tr3(TrKey3::DeleteSpace)) + label);
  }

  savePointSelectedIndex_ = savePoints_.empty() ? 1 : 2;
  menuScreen_ = MenuScreen::SavePointsList;
  renderSavePointsList();
}

void App::selectSavePointItem(uint32_t nowMs) {
  if (savePointSelectedIndex_ == 0) {
    // Back
    menuScreen_ = MenuScreen::Main;
    menuSelectedIndex_ = MenuSavePoints;
    renderMainMenu();
    return;
  }

  if (savePointSelectedIndex_ == 1) {
    // Add save point — open name entry
    if (!usingStorageBook_ || currentBookPath_.isEmpty()) {
      display_.renderStatus(uiText(UiText::SavePoints),
                            tr3(TrKey3::OpenBookFirst), "");
      delay(1400);
      renderSavePointsList();
      return;
    }
    // Generate default name
    const String defaultName = savePointDefaultName();
    savePointQuickSaveFromReader_ = false;
    if (savePointUseCustomName_) {
      openTextEntry(TextEntryPurpose::SavePointName,
                    tr3(TrKey3::NameBookmark),
                    tr3(TrKey3::EnterNamePrompt),
                    "", defaultName, "", false, 30,
                    MenuScreen::SavePointsList);
    } else {
      finishSavePointCreation(defaultName, nowMs);
    }
    return;
  }

  // Items after index 1: alternating save point / delete
  const size_t itemOffset = savePointSelectedIndex_ - 2;
  const size_t spIndex = itemOffset / 2;
  const bool isDeleteButton = (itemOffset % 2) == 1;

  if (spIndex >= savePoints_.size()) {
    return;
  }

  if (isDeleteButton) {
    openSavePointDeleteConfirm(spIndex, nowMs);
    return;
  }

  // Navigate to save point
  const SavePoint &sp = savePoints_[spIndex];
  saveReadingPosition(true);

  const int bookIdx = findBookIndexByPath(sp.bookPath);
  if (bookIdx < 0) {
    display_.renderStatus(tr3(TrKey3::ErrorLabel),
                          tr3(TrKey3::BookNotFound), sp.bookTitle);
    delay(1400);
    renderSavePointsList();
    return;
  }

  if (!loadBookAtIndex(static_cast<size_t>(bookIdx), nowMs, true, true, true, true)) {
    display_.renderStatus(tr3(TrKey3::ErrorLabel),
                          tr3(TrKey3::CannotOpen), sp.bookTitle);
    delay(1400);
    renderSavePointsList();
    return;
  }

  reader_.seekTo(sp.wordIndex);
  menuScreen_ = MenuScreen::Main;
  setState(AppState::Paused, nowMs);
  saveReadingPosition(true);
  Serial.printf("[save-point] jumped to %s word=%u\n", sp.name.c_str(),
                static_cast<unsigned int>(sp.wordIndex));
}

void App::renderSavePointsList() {
  renderMenuAnyMode("", savePointMenuItems_, savePointSelectedIndex_);
}

// ─── Save Point Delete Confirm ────────────────────────────────────────────────

void App::openSavePointDeleteConfirm(size_t index, uint32_t nowMs) {
  (void)nowMs;
  if (index >= savePoints_.size()) return;
  savePointDeleteTargetIndex_ = index;
  const auto &sp = savePoints_[index];
  const String label = sp.name.isEmpty() ? sp.bookTitle : sp.name;

  savePointDeleteConfirmMenuItems_.clear();
  savePointDeleteConfirmMenuItems_.push_back(uiText(UiText::Back));
  savePointDeleteConfirmMenuItems_.push_back(String(tr3(TrKey3::DeleteConfirmColon)) + label);
  savePointDeleteConfirmMenuItems_.push_back(tr3(TrKey3::NoGoBack));
  savePointDeleteConfirmMenuItems_.push_back(tr3(TrKey3::YesDelete));

  savePointDeleteConfirmSelectedIndex_ = 2;  // default to "No"
  menuScreen_ = MenuScreen::SavePointDeleteConfirm;
  renderItemGrid("", savePointDeleteConfirmMenuItems_, savePointDeleteConfirmSelectedIndex_);
}

void App::selectSavePointDeleteConfirmItem(uint32_t nowMs) {
  switch (savePointDeleteConfirmSelectedIndex_) {
    case 0:  // Back
    case 2:  // Nie, wroc
      menuScreen_ = MenuScreen::SavePointsList;
      openSavePointsList();
      return;
    case 1:  // Title (info only)
      return;
    case 3:  // Tak, usun
      executeDeleteSavePoint(nowMs);
      return;
    default:
      return;
  }
}

void App::executeDeleteSavePoint(uint32_t nowMs) {
  (void)nowMs;
  deleteSavePoint(savePointDeleteTargetIndex_);
  display_.renderStatus(uiText(UiText::SavePoints),
                        tr3(TrKey3::DeletedLabel), "");
  delay(800);
  // The finger that tapped "Tak, usun" may still be resting on the screen
  // when this fires; flush any stale touch before rebuilding the (now
  // shorter) list so a lingering End event can't land on whatever button
  // ends up under those same pixels (see flushStaleTouch()).
  flushStaleTouch();
  openSavePointsList();
}

// ─── Plugins ─────────────────────────────────────────────────────────────────
//
// Pluginy są teraz wbudowane w firmware (BuiltinPlugins) i "instalacja" to
// tylko lokalna flaga włącz/wyłącz w PluginLibrary (NVS) — bez WiFi, bez SD.
// Trzy ekrany:
//   PluginsHome    — [Aktywne] [Biblioteka], wejście z głównego menu.
//   PluginsActive  — tylko włączone pluginy, pełna szerokość, 3/stronę
//                    (patrz gridPagesVertically_ w renderItemGrid()).
//   PluginLibraryScreen (Biblioteka) — wszystkie pluginy, tap → PluginDetail.
//   PluginDetail   — nazwa, opis, przycisk Włącz/Wyłącz.

void App::openPluginsHome() {
  pluginsHomeMenuItems_.clear();
  pluginsHomeMenuItems_.push_back(uiText(UiText::Back));
  pluginsHomeMenuItems_.push_back(tr3(TrKey3::ActivePlugins));
  pluginsHomeMenuItems_.push_back(tr2(TrKey2::PluginLibrary));

  pluginsHomeSelectedIndex_ = 1;
  menuScreen_ = MenuScreen::PluginsHome;
  renderPluginsHome();
}

void App::selectPluginsHomeItem(uint32_t nowMs) {
  (void)nowMs;
  switch (pluginsHomeSelectedIndex_) {
    case 0:  // Back
      menuScreen_ = MenuScreen::Main;
      // Pluginy są osiągalne tylko z trybu zaawansowanego, gdzie
      // renderMainMenu() rysuje je zaraz po Ustawieniach (Wyłącz jest
      // dosunięte na sam koniec) — patrz kolejność push_back tam.
      // +1 gdy na górze menu wisi przycisk "Update" — patrz selectMenuItem().
      menuSelectedIndex_ = (MenuSettings + 1) + (otaUpdatePromptPending_ ? 1 : 0);
      renderMainMenu();
      return;
    case 1:  // Aktywne
      openPluginsActive();
      return;
    case 2:  // Biblioteka
      openPluginLibraryScreen();
      return;
    default:
      return;
  }
}

void App::renderPluginsHome() {
  renderMenuAnyMode("", pluginsHomeMenuItems_, pluginsHomeSelectedIndex_);
}

void App::openPluginsActive() {
  pluginsActiveMenuItems_.clear();
  pluginsActiveMenuItems_.push_back(uiText(UiText::Back));

  const auto enabled = pluginLibrary_.enabledEntries();
  if (enabled.empty()) {
    // Info-only tile, same non-selectable treatment as a "---" separator —
    // an empty grid with just a Back icon reads as broken, not "empty".
    pluginsActiveMenuItems_.push_back(
        tr3(TrKey3::NoActivePlugins));
  } else {
    for (const auto& entry : enabled) {
      pluginsActiveMenuItems_.push_back(entry.name);
    }
  }

  pluginsActiveSelectedIndex_ = enabled.empty() ? 0 : 1;
  menuScreen_ = MenuScreen::PluginsActive;
  renderPluginsActive();
}

void App::selectPluginsActiveItem(uint32_t nowMs) {
  // Back
  if (pluginsActiveSelectedIndex_ == 0) {
    openPluginsHome();
    return;
  }

  const auto enabled = pluginLibrary_.enabledEntries();
  if (enabled.empty()) {
    // Only the info tile is present — nothing to launch.
    return;
  }

  const size_t idx = pluginsActiveSelectedIndex_ - 1;
  if (idx >= enabled.size()) {
    return;
  }
  const auto& entry = enabled[idx];
  const char* pluginId = entry.id.c_str();

  display_.renderStatus(uiText(UiText::Plugins), tr2(TrKey2::PluginLaunch),
                        entry.name.c_str());

  PluginLoader::LoadResult result = pluginLoader_.load(pluginId);
  if (result.success) {
    Serial.printf("[plugin] launched plugin: %s\n", pluginId);
    // Plugin is now running in its own FreeRTOS task.
    // The update loop (task 6.2) will handle forwarding events.
    return;
  }

  // Load failed — show error
  Serial.printf("[plugin] load failed: %s — %s\n", pluginId, result.message);
  display_.renderStatus(uiText(UiText::Plugins), tr2(TrKey2::PluginInstallFailed),
                        result.message ? result.message : "");
  delay(2000);
  (void)nowMs;
  renderPluginsActive();
}

void App::renderPluginsActive() {
  // No battery badge here: this screen is a dense full-width grid and the
  // corner badge visually collides with the tiles.
  if (navMode_ == NavMode::Buttons) {
    renderItemGrid("", pluginsActiveMenuItems_, pluginsActiveSelectedIndex_, 0, false);
    return;
  }
  renderMenuAnyMode("", pluginsActiveMenuItems_, pluginsActiveSelectedIndex_);
}

void App::openPluginLibraryScreen() {
  pluginLibraryMenuItems_.clear();
  pluginLibraryMenuItems_.push_back(uiText(UiText::Back));

  const auto& all = pluginLibrary_.all();
  for (const auto& entry : all) {
    String label = entry.name;
    label += entry.enabled ? tr3(TrKey3::PluginEnabledTag)
                            : tr3(TrKey3::PluginDisabledTag);
    pluginLibraryMenuItems_.push_back(label);
  }

  pluginLibrarySelectedIndex_ = all.empty() ? 0 : 1;
  menuScreen_ = MenuScreen::PluginLibraryScreen;
  renderPluginLibraryScreen();
}

void App::selectPluginLibraryItem(uint32_t nowMs) {
  (void)nowMs;

  // Back
  if (pluginLibrarySelectedIndex_ == 0) {
    openPluginsHome();
    return;
  }

  const auto& all = pluginLibrary_.all();
  const size_t idx = pluginLibrarySelectedIndex_ - 1;
  if (idx >= all.size()) {
    return;
  }

  openPluginDetail(idx);
}

void App::renderPluginLibraryScreen() {
  renderMenuAnyMode("", pluginLibraryMenuItems_, pluginLibrarySelectedIndex_);
}

void App::openPluginDetail(size_t entryIndex) {
  const auto& all = pluginLibrary_.all();
  if (entryIndex >= all.size()) {
    return;
  }

  pluginDetailIndex_ = entryIndex;
  const auto& entry = all[entryIndex];

  pluginDetailMenuItems_.clear();
  pluginDetailMenuItems_.push_back(uiText(UiText::Back));

  // Description renders as a single non-interactive Label tile (see
  // annotatePluginDetailButton()), so it's always exactly one menu entry
  // here — line 2, if the text doesn't fit on one line, goes into
  // pluginDetailDescLine2_ instead of becoming a second (falsely tappable)
  // entry.
  String desc = entry.description.isEmpty() ? entry.name : entry.description;
  constexpr size_t kMaxLineChars = 35;
  pluginDetailDescLine2_ = "";

  if (desc.length() <= kMaxLineChars) {
    pluginDetailMenuItems_.push_back(desc);
  } else {
    // First line: break at last space within kMaxLineChars
    int breakPos = kMaxLineChars;
    for (int i = kMaxLineChars; i > 0; --i) {
      if (desc.charAt(i) == ' ') {
        breakPos = i;
        break;
      }
    }
    String line1 = desc.substring(0, breakPos);
    String line2 = desc.substring(breakPos);
    line2.trim();
    // Truncate line2 if still too long
    if (line2.length() > kMaxLineChars) {
      line2 = line2.substring(0, kMaxLineChars - 3) + "...";
    }
    pluginDetailMenuItems_.push_back(line1);
    pluginDetailDescLine2_ = line2;
  }

  pluginDetailMenuItems_.push_back(entry.enabled ? tr3(TrKey3::DisablePlugin)
                                                  : tr3(TrKey3::EnablePlugin));

  // Default selection to the last item (Enable/Disable button)
  pluginDetailSelectedIndex_ = pluginDetailMenuItems_.size() - 1;
  menuScreen_ = MenuScreen::PluginDetail;
  renderPluginDetail();
}

void App::selectPluginDetailItem(uint32_t nowMs) {
  (void)nowMs;

  // Back
  if (pluginDetailSelectedIndex_ == 0) {
    menuScreen_ = MenuScreen::PluginLibraryScreen;
    renderPluginLibraryScreen();
    return;
  }

  const size_t toggleButtonIndex = pluginDetailMenuItems_.size() - 1;

  // Description row — non-selectable Label tile (see
  // annotatePluginDetailButton()); this guard is generic over how many
  // entries sit between Back and the toggle, but today it's exactly one.
  if (pluginDetailSelectedIndex_ < toggleButtonIndex) {
    return;
  }

  // Włącz/Wyłącz (last item)
  if (pluginDetailSelectedIndex_ == toggleButtonIndex) {
    const auto& all = pluginLibrary_.all();
    if (pluginDetailIndex_ >= all.size()) {
      return;
    }
    const auto& entry = all[pluginDetailIndex_];
    pluginLibrary_.setEnabled(entry.id.c_str(), !entry.enabled);

    // Refresh the detail screen in place — toggling is instant and
    // reversible, no restart needed since the plugin was already built in.
    openPluginDetail(pluginDetailIndex_);
  }
}

void App::renderPluginDetail() {
  renderItemGrid("", pluginDetailMenuItems_, pluginDetailSelectedIndex_);
}

// ─── Presets ─────────────────────────────────────────────────────────────────

void App::openPresets() {
  menuScreen_ = MenuScreen::Presets;

  auto presets = presetManager_.listPresets();
  presetFilenames_.clear();
  presetFilenames_.reserve(presets.size());

  settingsMenuItems_.clear();
  settingsMenuItems_.reserve(presets.size() + 2);

  // [0] Back
  settingsMenuItems_.push_back(uiText(UiText::Back));

  // [1] Save Current / Limit reached
  if (presets.size() < PresetManager::kMaxPresets) {
    settingsMenuItems_.push_back(tr3(TrKey3::SaveCurrentPreset));
  } else {
    settingsMenuItems_.push_back(tr3(TrKey3::PresetLimitReachedParen));
  }

  // [2..] Preset names
  for (const auto &p : presets) {
    settingsMenuItems_.push_back(p.name);
    presetFilenames_.push_back(p.filename);
  }

  presetsSelectedIndex_ = 0;
  renderMenuAnyMode("", settingsMenuItems_, presetsSelectedIndex_);
}

void App::selectPresetsItem(uint32_t nowMs) {
  if (presetsSelectedIndex_ == 0) {
    // Back → return to SettingsHome
    openSettings();
    return;
  }

  if (presetsSelectedIndex_ == 1) {
    // Save Current
    if (presetFilenames_.size() < PresetManager::kMaxPresets) {
      openTextEntry(TextEntryPurpose::PresetName,
                    tr3(TrKey3::PresetNameLabel),
                    "", "", "", "", false,
                    PresetManager::kMaxPresetNameLength,
                    MenuScreen::Presets);
    }
    // If at limit, do nothing (item is informational)
    return;
  }

  // Index >= 2: open preset detail screen (Apply / Delete)
  const size_t presetIndex = presetsSelectedIndex_ - 2;
  confirmDeletePreset(presetIndex, nowMs);
}

void App::executeSavePreset(uint32_t nowMs) {
  (void)nowMs;

  String validatedName = presetManager_.validateName(textEntrySession_.value);
  textEntrySession_ = TextEntrySession();
  textEntryButtons_.clear();

  if (validatedName.isEmpty()) {
    display_.renderStatus(tr3(TrKey3::PresetsLabel),
                          tr3(TrKey3::InvalidName), "");
    delay(1000);
    openPresets();
    return;
  }

  PresetManager::SaveResult result = presetManager_.savePreset(validatedName, preferences_);

  switch (result) {
    case PresetManager::SaveResult::Ok:
      display_.renderStatus(tr3(TrKey3::PresetsLabel),
                            tr3(TrKey3::SavedLabel), validatedName);
      delay(1000);
      break;
    case PresetManager::SaveResult::LimitReached:
      display_.renderStatus(tr3(TrKey3::PresetsLabel),
                            tr3(TrKey3::LimitReachedShort), "");
      delay(1000);
      break;
    default:
      display_.renderStatus(tr3(TrKey3::PresetsLabel),
                            tr3(TrKey3::SdCardErrorLabel), "");
      delay(1000);
      break;
  }

  openPresets();
}

void App::executeRestorePreset(size_t index, uint32_t nowMs) {
  const String &filename = presetFilenames_[index];
  PresetManager::RestoreResult result = presetManager_.restorePreset(filename, preferences_);

  if (result == PresetManager::RestoreResult::Ok) {
    reloadRuntimePreferences(nowMs, true);
    const String &presetName = settingsMenuItems_[index + 2];
    display_.renderStatus("Preset",
                          tr3(TrKey3::LoadedLabel), presetName);
    delay(1200);
    openPresets();
  } else {
    display_.renderStatus(tr3(TrKey3::ErrorLabel),
                          tr3(TrKey3::PresetLoadError), "");
    delay(1400);
    openPresets();
  }
}

void App::confirmDeletePreset(size_t index, uint32_t nowMs) {
  (void)nowMs;
  presetsDeleteTargetIndex_ = index;
  menuScreen_ = MenuScreen::PresetsDeleteConfirm;

  // Build detail menu: Back + Apply + Delete
  const String presetName = settingsMenuItems_[index + 2];  // offset by Back + Save Current

  settingsMenuItems_.clear();
  settingsMenuItems_.reserve(3);
  settingsMenuItems_.push_back(uiText(UiText::Back));
  settingsMenuItems_.push_back(String(tr3(TrKey3::ApplyColon)) + presetName);
  settingsMenuItems_.push_back(String(tr3(TrKey3::DeletePresetColon)) + presetName);

  presetsSelectedIndex_ = 0;
  renderItemGrid("", settingsMenuItems_, presetsSelectedIndex_);
}

void App::executeDeletePreset(uint32_t nowMs) {
  (void)nowMs;

  const String &filename = presetFilenames_[presetsDeleteTargetIndex_];
  PresetManager::DeleteResult result = presetManager_.deletePreset(filename);

  switch (result) {
    case PresetManager::DeleteResult::Ok:
      display_.renderStatus(tr3(TrKey3::PresetsLabel),
                            tr3(TrKey3::DeletedLabel), "");
      delay(800);
      break;
    default:
      display_.renderStatus(tr3(TrKey3::PresetsLabel),
                            tr3(TrKey3::PresetDeleteFailed), "");
      delay(1000);
      break;
  }

  flushStaleTouch();
  openPresets();
}

void App::openRestartConfirm() {
  restartConfirmReturnScreen_ = menuScreen_;
  restartConfirmSelectedIndex_ = RestartConfirmNo;
  menuScreen_ = MenuScreen::RestartConfirm;
  renderRestartConfirm();
}

void App::selectRestartConfirmItem(uint32_t nowMs) {
  if (restartConfirmSelectedIndex_ != RestartConfirmYes) {
    menuScreen_ = restartConfirmReturnScreen_;
    renderMenu();
    return;
  }

  reader_.begin(nowMs);
  restartConfirmReturnScreen_ = MenuScreen::Main;
  menuScreen_ = MenuScreen::Main;
  setState(AppState::Paused, nowMs);
  saveReadingPosition(true);
  Serial.println("[restart] book restarted from beginning");
}

void App::openSdCardRepairConfirm() {
  sdCardRepairConfirmSelectedIndex_ = SdCardRepairConfirmNo;
  menuScreen_ = MenuScreen::SdCardRepairConfirm;
  renderSdCardRepairConfirm();
}

void App::selectSdCardRepairConfirmItem(uint32_t nowMs) {
  if (sdCardRepairConfirmSelectedIndex_ != SdCardRepairConfirmYes) {
    Serial.println("[sd-check] folder repair declined");
    menuScreen_ = MenuScreen::Main;
    renderMenu();
    return;
  }

  runSdCardRepair(nowMs);
}

void App::openUpdateConfirm() {
  updateConfirmSelectedIndex_ = UpdateConfirmSkip;
  menuScreen_ = MenuScreen::UpdateConfirm;
  renderUpdateConfirm();
}

void App::selectUpdateConfirmItem(uint32_t nowMs) {
  if (updateConfirmSelectedIndex_ != UpdateConfirmUpdate) {
    Serial.println("[ota] update skipped by user");
    otaUpdatePromptDismissed_ = true;
    menuScreen_ = MenuScreen::Main;
    setState(AppState::Paused, nowMs);
    return;
  }

  Serial.println("[ota] update confirmed by user");
  runFirmwareUpdate(preferredOtaConfig(), false, nowMs);
}

void App::enterCompanionSync(uint32_t nowMs) {
  if (blockNetworkActionForOtaCheck("Sync", nowMs)) {
    return;
  }

  Serial.println("[app] entering companion sync mode");
  saveReadingPosition(true);
  pausedTouch_.active = false;
  pausedTouchIntent_ = TouchIntent::None;
  wpmFeedbackVisible_ = false;
  display_.renderStatus("Sync", tr(TrKey::StartingWifi), "");

  // If auto-sync AP is already running, just transition to CompanionSync state.
  if (autoSyncActive_) {
    Serial.println("[app] reusing auto-sync AP for companion sync mode");
    autoSyncActive_ = false;
    lastCompanionSyncRenderMs_ = 0;
    setState(AppState::CompanionSync, nowMs);
    return;
  }

  OtaUpdater::Config wifiConfig = preferredOtaConfig();
  CompanionSyncManager::Config syncConfig;
  syncConfig.wifiSsid = wifiConfig.wifiSsid;
  syncConfig.wifiPassword = wifiConfig.wifiPassword;

  if (!companionSync_.begin(syncConfig)) {
    Serial.println("[app] companion sync failed");
    display_.renderStatus("Sync", tr(TrKey::CouldNotStart),
                          tr(TrKey::Returning));
    delay(1400);
    menuScreen_ = MenuScreen::Main;
    setState(AppState::Menu, nowMs);
    return;
  }

  lastCompanionSyncRenderMs_ = 0;
  setState(AppState::CompanionSync, nowMs);
}

void App::updateCompanionSync(uint32_t nowMs) {
  // Push device status to companion API (battery, SD card)
  uint32_t sdFreeKb = 0, sdTotalKb = 0;
  {
    const uint64_t totalBytes = SD_MMC.totalBytes();
    const uint64_t usedBytes = SD_MMC.usedBytes();
    sdTotalKb = static_cast<uint32_t>(totalBytes / 1024ULL);
    sdFreeKb = static_cast<uint32_t>((totalBytes - usedBytes) / 1024ULL);
  }
  companionSync_.setDeviceStatus(batteryDisplayedPercent_, sdFreeKb, sdTotalKb);
  companionSync_.update();

  if (powerButton_.isHeld() && nowMs - powerButton_.lastEdgeMs() >= kUsbTransferExitHoldMs) {
    powerButtonLongPressHandled_ = true;
    exitCompanionSync(nowMs);
    return;
  }

  // Touch anywhere = exit sync (back button)
  TouchEvent ev;
  if (touch_.poll(ev) && ev.phase == TouchPhase::End) {
    exitCompanionSync(nowMs);
    return;
  }

  if (nowMs - lastCompanionSyncRenderMs_ >= 1000) {
    lastCompanionSyncRenderMs_ = nowMs;
    if (companionSync_.hasQrCode()) {
      display_.renderStatusWithQr(tr3(TrKey3::BackWifiHeader),
                                  companionSync_.statusLine1(),
                                  companionSync_.qrCodeData(), companionSync_.qrCodeSize());
    } else {
      display_.renderStatus(tr3(TrKey3::BackSyncHeader),
                            companionSync_.statusLine1(), companionSync_.statusLine2());
    }
  }
}

void App::exitCompanionSync(uint32_t nowMs) {
  Serial.println("[app] leaving companion sync mode");
  display_.renderStatus("Sync", tr(TrKey::Stopping), "");
  companionSync_.end();
  preferences_.end();
  preferences_.begin(kPrefsNamespace, false);
  reloadRuntimePreferences(nowMs, false);
  storage_.refreshBooks();
  restoreArchivedSavePointsForReturnedBooks();
  menuScreen_ = MenuScreen::Main;
  setState(AppState::Paused, nowMs);
}

void App::runSdCardCheck(uint32_t nowMs) {
  (void)nowMs;
  Serial.println("[app] running SD card check");
  display_.renderStatus("SD", tr2(TrKey2::Starting), "");
  const StorageManager::DiagnosticResult result = storage_.diagnoseSdCard();

  if (sdCardFolderRepairNeeded(result)) {
    display_.renderStatus("SD", tr2(TrKey2::FoldersMissing), tr2(TrKey2::ConfirmRepair));
    delay(900);
    openSdCardRepairConfirm();
    return;
  }

  String detail = result.detail;
  if (detail.isEmpty() && result.mounted) {
    detail = String(static_cast<unsigned int>(result.sizeMb)) + " MB";
  }
  display_.renderStatus("SD check", result.summary, detail);
  delay(2600);

  menuScreen_ = MenuScreen::Main;
  renderMenu();
}

void App::runSdCardRepair(uint32_t nowMs) {
  Serial.println("[app] repairing SD card folder layout");
  display_.renderStatus("SD", tr2(TrKey2::RepairingFolders), tr(TrKey::PleaseWait));
  const bool repaired = storage_.repairSdCardFolders();
  if (!repaired) {
    display_.renderStatus("SD", tr2(TrKey2::FolderRepairFailed), tr2(TrKey2::FormatFat32));
    delay(2600);
    menuScreen_ = MenuScreen::Main;
    renderMenu();
    return;
  }

  // A freshly repaired/formatted card has no /fonts contents — kick off the
  // silent background download here too, not just at boot, so a card
  // formatted mid-session doesn't have to wait for the next power cycle.
  refreshFontPackComplete();
  maybeAutoDownloadFonts(nowMs);

  display_.renderStatus("SD", tr2(TrKey2::FoldersRepaired), tr2(TrKey2::CheckingCard));
  delay(900);

  const StorageManager::DiagnosticResult result = storage_.diagnoseSdCard();
  String detail = result.detail;
  if (detail.isEmpty() && result.mounted) {
    detail = String(static_cast<unsigned int>(result.sizeMb)) + " MB";
  }
  display_.renderStatus("SD check", result.summary, detail);
  delay(2600);

  menuScreen_ = MenuScreen::Main;
  renderMenu();
}

void App::enterUsbTransfer(uint32_t nowMs) {
  Serial.println("[app] entering USB transfer mode");
  saveReadingPosition(true);
  pausedTouch_.active = false;
  pausedTouchIntent_ = TouchIntent::None;
  wpmFeedbackVisible_ = false;
  const size_t resumeIndex = reader_.currentIndex();
  setState(AppState::UsbTransfer, nowMs);

  activeBookStore_.close();
  storage_.end();
  if (!usbTransfer_.begin(true)) {
    Serial.printf("[app] USB transfer failed: %s\n", usbTransfer_.statusMessage());
    display_.renderStatus("USB", tr2(TrKey2::SdNotReady), tr(TrKey::Returning));
    storageReady_ = storage_.begin();
    if (storageReady_ && usingStorageBook_ && !currentBookPath_.isEmpty()) {
      const int refreshedBookIndex = findBookIndexByPath(currentBookPath_);
      if (refreshedBookIndex >= 0 &&
          loadBookAtIndex(static_cast<size_t>(refreshedBookIndex), nowMs, false, false, false,
                          false)) {
        reader_.seekTo(resumeIndex);
      }
    }
    setState(AppState::Paused, nowMs);
    return;
  }

  const uint64_t sizeMb = usbTransfer_.cardSizeBytes() / (1024ULL * 1024ULL);
  Serial.printf("[app] USB transfer active (%llu MB). Eject from computer when finished.\n",
                sizeMb);
  display_.renderStatus(tr3(TrKey3::UsbBackHint),
                        tr3(TrKey3::ConnectUsbCable),
                        tr3(TrKey3::SdVisibleOnPhone));
}

void App::updateUsbTransfer(uint32_t nowMs) {
  if (!usbTransfer_.active()) {
    return;
  }

  // Touch anywhere = exit USB transfer (back button)
  TouchEvent ev;
  if (touch_.poll(ev) && ev.phase == TouchPhase::End) {
    powerButtonLongPressHandled_ = true;
    exitUsbTransfer(nowMs);
    return;
  }

  const bool powerExitRequested =
      powerButton_.isHeld() && nowMs - powerButton_.lastEdgeMs() >= kUsbTransferExitHoldMs;
  if (!usbTransfer_.ejected() && !powerExitRequested) {
    return;
  }

  if (powerExitRequested && !usbTransfer_.ejected()) {
    Serial.println("[app] leaving USB transfer by PWR hold; make sure host was ejected first");
  }

  if (powerExitRequested) {
    powerButtonLongPressHandled_ = true;
  }

  exitUsbTransfer(nowMs);
}

void App::exitUsbTransfer(uint32_t nowMs) {
  Serial.println("[app] USB transfer ejected; remounting SD");
  display_.renderStatus("USB", tr2(TrKey2::RemountingSd), "");
  usbTransfer_.end();

  storageReady_ = storage_.begin();
  if (storageReady_) {
    const int refreshedBookIndex = findBookIndexByPath(currentBookPath_);
    if (refreshedBookIndex >= 0) {
      const size_t resumeIndex = reader_.currentIndex();
      if (loadBookAtIndex(static_cast<size_t>(refreshedBookIndex), nowMs, false, false, false,
                          false)) {
        reader_.seekTo(resumeIndex);
      } else {
        Serial.println("[app] current indexed book unavailable after USB transfer");
        usingStorageBook_ = false;
        currentBookPath_ = "";
        currentBookTitle_ = "Demo";
        reader_.clearLoadedBook(nowMs);
        reader_.begin(nowMs);
      }
    } else if (storage_.bookCount() > 0) {
      loadBookAtIndex(0, nowMs);
    }
  } else {
    Serial.println("[app] SD remount failed after USB transfer");
  }

  menuScreen_ = MenuScreen::Main;
  setState(AppState::Paused, nowMs);
}

void App::enterStandby(uint32_t nowMs) {
  if (state_ == AppState::UsbTransfer || state_ == AppState::CompanionSync ||
      state_ == AppState::Sleeping || powerOffStarted_) {
    return;
  }

  standbyReturnState_ = state_ == AppState::Playing ? AppState::Paused : state_;
  if (standbyReturnState_ == AppState::Booting || standbyReturnState_ == AppState::Standby) {
    standbyReturnState_ = AppState::Paused;
  }

  if (state_ == AppState::Playing) {
    saveReadingPosition(true);
  }

  pausedTouch_.active = false;
  pausedTouchIntent_ = TouchIntent::None;
  touchPlayHeld_ = false;
  playLocked_ = false;
  pauseAtSentenceEndRequested_ = false;
  contextViewVisible_ = false;
  wpmFeedbackVisible_ = false;
  batteryWarningOverlayVisible_ = false;
  standbyEnteredMs_ = nowMs;
  standbyButtonsReleased_ = false;
  lastStandbyFrameMs_ = 0;
  setState(AppState::Standby, nowMs);
  Serial.println("[app] standby screensaver started");
}

void App::exitStandby(uint32_t nowMs) {
  if (state_ != AppState::Standby) {
    return;
  }

  pausedTouch_.active = false;
  pausedTouchIntent_ = TouchIntent::None;
  touchPlayHeld_ = false;
  playLocked_ = false;
  pauseAtSentenceEndRequested_ = false;
  batteryWarningOverlayVisible_ = false;
  standbyButtonsReleased_ = false;

  AppState nextState = standbyReturnState_;
  if (nextState == AppState::Booting || nextState == AppState::Playing ||
      nextState == AppState::CompanionSync || nextState == AppState::UsbTransfer ||
      nextState == AppState::Standby || nextState == AppState::Sleeping) {
    nextState = AppState::Paused;
  }

  Serial.println("[app] leaving standby");
  if (standbyScreenOffActive_) {
    display_.wakeFromSleep();
    standbyScreenOffActive_ = false;
  }
  setState(nextState, nowMs);
}

void App::seedStandbyScreensaver(uint32_t nowMs) {
  if (screensaverMode_ != ScreensaverMode::ScreenOff && standbyScreenOffActive_) {
    display_.wakeFromSleep();
    standbyScreenOffActive_ = false;
  }

  switch (screensaverMode_) {
    case ScreensaverMode::Maze:
      seedStandbyMaze(nowMs);
      return;
    case ScreensaverMode::Voronoi:
      seedStandbyVoronoi(nowMs);
      return;
    case ScreensaverMode::Stars:
      seedStandbyStars(nowMs);
      return;
    case ScreensaverMode::Matrix:
      seedStandbyMatrix(nowMs);
      return;
    case ScreensaverMode::ScreenOff:
      seedStandbyScreenOff(nowMs);
      return;
    case ScreensaverMode::Life:
    default:
      seedStandbyLife(nowMs);
      return;
  }
}

void App::stepStandbyScreensaver(uint32_t nowMs) {
  (void)nowMs;
  switch (screensaverMode_) {
    case ScreensaverMode::Maze:
      stepStandbyMaze();
      return;
    case ScreensaverMode::Voronoi:
      stepStandbyVoronoi();
      return;
    case ScreensaverMode::Stars:
      stepStandbyStars();
      return;
    case ScreensaverMode::Matrix:
      stepStandbyMatrix();
      return;
    case ScreensaverMode::ScreenOff:
      return;
    case ScreensaverMode::Life:
    default:
      stepStandbyLife();
      return;
  }
}

void App::seedStandbyLife(uint32_t nowMs) {
  const size_t cellCount =
      static_cast<size_t>(kStandbyLifeColumns) * static_cast<size_t>(kStandbyLifeRows);
  standbyLifeCells_.assign(packedLifeWordCount(cellCount), 0);
  standbyLifeNextCells_.assign(packedLifeWordCount(cellCount), 0);
  standbyScreensaverDimCells_.clear();
  standbyMazeVisited_.clear();
  standbyMazeStack_.clear();
  standbyVoronoiX_.clear();
  standbyVoronoiY_.clear();
  standbyVoronoiDx_.clear();
  standbyVoronoiDy_.clear();
  standbyLifeGeneration_ = 0;

  standbyScreensaverRng_ =
      nowMs ^ micros() ^ (static_cast<uint32_t>(reader_.currentIndex() + 1) * 2654435761UL) ^
      (static_cast<uint32_t>(batteryDisplayedPercent_) << 24);
  for (size_t i = 0; i < cellCount; ++i) {
    setPackedLifeCell(standbyLifeCells_, i, (advanceStandbyRng(standbyScreensaverRng_) >> 24) < 12);
  }

  clearAndStampPackedLifePattern(standbyLifeCells_, kStandbyLifeColumns, kStandbyLifeRows,
                                 kLifeGosperGliderGun,
                                 sizeof(kLifeGosperGliderGun) / sizeof(kLifeGosperGliderGun[0]),
                                 18, 18, 36, 9);
  clearAndStampPackedLifePattern(standbyLifeCells_, kStandbyLifeColumns, kStandbyLifeRows,
                                 kLifeGosperGliderGun,
                                 sizeof(kLifeGosperGliderGun) / sizeof(kLifeGosperGliderGun[0]),
                                 static_cast<int>(kStandbyLifeColumns) - 62,
                                 static_cast<int>(kStandbyLifeRows) - 34, 36, 9);
  clearAndStampPackedLifePattern(standbyLifeCells_, kStandbyLifeColumns, kStandbyLifeRows,
                                 kLifePulsar, sizeof(kLifePulsar) / sizeof(kLifePulsar[0]),
                                 static_cast<int>(kStandbyLifeColumns / 2) - 7,
                                 static_cast<int>(kStandbyLifeRows / 2) - 7, 13, 13);
  clearAndStampPackedLifePattern(standbyLifeCells_, kStandbyLifeColumns, kStandbyLifeRows,
                                 kLifePentadecathlon,
                                 sizeof(kLifePentadecathlon) / sizeof(kLifePentadecathlon[0]),
                                 static_cast<int>(kStandbyLifeColumns / 3),
                                 static_cast<int>(kStandbyLifeRows) - 42, 5, 10);
  clearAndStampPackedLifePattern(standbyLifeCells_, kStandbyLifeColumns, kStandbyLifeRows,
                                 kLifeLightweightSpaceship,
                                 sizeof(kLifeLightweightSpaceship) /
                                     sizeof(kLifeLightweightSpaceship[0]),
                                 static_cast<int>((kStandbyLifeColumns * 2) / 3),
                                 static_cast<int>(kStandbyLifeRows / 3), 5, 4);

  for (uint8_t i = 0; i < 10; ++i) {
    const int x =
        static_cast<int>((advanceStandbyRng(standbyScreensaverRng_) >> 8) %
                         std::max<uint16_t>(1, kStandbyLifeColumns - 6));
    const int y =
        static_cast<int>((advanceStandbyRng(standbyScreensaverRng_) >> 8) %
                         std::max<uint16_t>(1, kStandbyLifeRows - 6));
    clearAndStampPackedLifePattern(standbyLifeCells_, kStandbyLifeColumns, kStandbyLifeRows,
                                   kLifeGlider, sizeof(kLifeGlider) / sizeof(kLifeGlider[0]), x,
                                   y, 3, 3);
  }
}

void App::stepStandbyLife() {
  const size_t cellCount =
      static_cast<size_t>(kStandbyLifeColumns) * static_cast<size_t>(kStandbyLifeRows);
  const size_t wordCount = packedLifeWordCount(cellCount);
  if (standbyLifeCells_.size() != wordCount || standbyLifeNextCells_.size() != wordCount) {
    seedStandbyLife(millis());
    return;
  }

  std::fill(standbyLifeNextCells_.begin(), standbyLifeNextCells_.end(), 0);
  size_t aliveCount = 0;
  for (uint16_t y = 0; y < kStandbyLifeRows; ++y) {
    for (uint16_t x = 0; x < kStandbyLifeColumns; ++x) {
      uint8_t neighbours = 0;
      for (int8_t dy = -1; dy <= 1; ++dy) {
        for (int8_t dx = -1; dx <= 1; ++dx) {
          if (dx == 0 && dy == 0) {
            continue;
          }
          const uint16_t nx =
              static_cast<uint16_t>((static_cast<int>(x) + dx + kStandbyLifeColumns) %
                                    kStandbyLifeColumns);
          const uint16_t ny =
              static_cast<uint16_t>((static_cast<int>(y) + dy + kStandbyLifeRows) %
                                    kStandbyLifeRows);
          neighbours += packedLifeCellAlive(
              standbyLifeCells_, static_cast<size_t>(ny) * kStandbyLifeColumns + nx)
                            ? 1
                            : 0;
        }
      }

      const size_t index = static_cast<size_t>(y) * kStandbyLifeColumns + x;
      const bool alive = packedLifeCellAlive(standbyLifeCells_, index);
      const bool nextAlive = alive ? (neighbours == 2 || neighbours == 3) : (neighbours == 3);
      setPackedLifeCell(standbyLifeNextCells_, index, nextAlive);
      if (nextAlive) {
        ++aliveCount;
      }
    }
  }

  standbyLifeCells_.swap(standbyLifeNextCells_);
  ++standbyLifeGeneration_;
  if (aliveCount == 0 || aliveCount > (cellCount * 3) / 4) {
    seedStandbyLife(millis());
  }
}

void App::seedStandbyMaze(uint32_t nowMs) {
  const size_t cellCount =
      static_cast<size_t>(kStandbyLifeColumns) * static_cast<size_t>(kStandbyLifeRows);
  const uint16_t mazeColumns = std::max<uint16_t>(1, (kStandbyLifeColumns - 1) / 2);
  const uint16_t mazeRows = std::max<uint16_t>(1, (kStandbyLifeRows - 1) / 2);
  standbyLifeCells_.assign(packedLifeWordCount(cellCount), 0);
  standbyLifeNextCells_.assign(packedLifeWordCount(cellCount), 0);
  standbyScreensaverDimCells_.clear();
  standbyVoronoiX_.clear();
  standbyVoronoiY_.clear();
  standbyVoronoiDx_.clear();
  standbyVoronoiDy_.clear();
  standbyMazeVisited_.assign(static_cast<size_t>(mazeColumns) * mazeRows, 0);
  standbyMazeStack_.clear();
  standbyLifeGeneration_ = 0;
  standbyScreensaverRng_ =
      nowMs ^ micros() ^ (static_cast<uint32_t>(reader_.currentIndex() + 1) * 2246822519UL);

  const uint16_t startX = static_cast<uint16_t>((advanceStandbyRng(standbyScreensaverRng_) >> 8) %
                                               mazeColumns);
  const uint16_t startY = static_cast<uint16_t>((advanceStandbyRng(standbyScreensaverRng_) >> 8) %
                                               mazeRows);
  standbyMazeVisited_[static_cast<size_t>(startY) * mazeColumns + startX] = 1;
  standbyMazeStack_.push_back(static_cast<uint16_t>(startY * mazeColumns + startX));
  setPackedLifeCellAt(standbyLifeCells_, kStandbyLifeColumns, kStandbyLifeRows,
                      static_cast<int>(startX) * 2 + 1, static_cast<int>(startY) * 2 + 1, true);
}

void App::stepStandbyMaze() {
  const uint16_t mazeColumns = std::max<uint16_t>(1, (kStandbyLifeColumns - 1) / 2);
  const uint16_t mazeRows = std::max<uint16_t>(1, (kStandbyLifeRows - 1) / 2);
  const size_t mazeCellCount = static_cast<size_t>(mazeColumns) * mazeRows;
  if (standbyMazeVisited_.size() != mazeCellCount || standbyMazeStack_.empty()) {
    if (standbyMazeStack_.empty() && standbyLifeGeneration_ < 600) {
      ++standbyLifeGeneration_;
      return;
    }
    seedStandbyMaze(millis());
    return;
  }

  constexpr uint8_t kMazeStepsPerFrame = 32;
  for (uint8_t step = 0; step < kMazeStepsPerFrame && !standbyMazeStack_.empty(); ++step) {
    const uint16_t current = standbyMazeStack_.back();
    const uint16_t cx = current % mazeColumns;
    const uint16_t cy = current / mazeColumns;
    uint16_t candidates[4];
    uint8_t candidateCount = 0;

    auto addCandidate = [&](int nx, int ny) {
      if (nx < 0 || ny < 0 || nx >= static_cast<int>(mazeColumns) ||
          ny >= static_cast<int>(mazeRows)) {
        return;
      }
      const uint16_t encoded = static_cast<uint16_t>(ny * mazeColumns + nx);
      if (standbyMazeVisited_[encoded] == 0) {
        candidates[candidateCount++] = encoded;
      }
    };

    addCandidate(static_cast<int>(cx) + 1, cy);
    addCandidate(static_cast<int>(cx) - 1, cy);
    addCandidate(cx, static_cast<int>(cy) + 1);
    addCandidate(cx, static_cast<int>(cy) - 1);

    if (candidateCount == 0) {
      standbyMazeStack_.pop_back();
      continue;
    }

    const uint16_t next = candidates[(advanceStandbyRng(standbyScreensaverRng_) >> 16) %
                                     candidateCount];
    const uint16_t nx = next % mazeColumns;
    const uint16_t ny = next / mazeColumns;
    standbyMazeVisited_[next] = 1;
    standbyMazeStack_.push_back(next);

    const int displayCx = static_cast<int>(cx) * 2 + 1;
    const int displayCy = static_cast<int>(cy) * 2 + 1;
    const int displayNx = static_cast<int>(nx) * 2 + 1;
    const int displayNy = static_cast<int>(ny) * 2 + 1;
    setPackedLifeCellAt(standbyLifeCells_, kStandbyLifeColumns, kStandbyLifeRows, displayNx,
                        displayNy, true);
    setPackedLifeCellAt(standbyLifeCells_, kStandbyLifeColumns, kStandbyLifeRows,
                        (displayCx + displayNx) / 2, (displayCy + displayNy) / 2, true);
  }

  if (standbyMazeStack_.empty()) {
    standbyLifeGeneration_ = 0;
  } else {
    ++standbyLifeGeneration_;
  }
}

void App::seedStandbyVoronoi(uint32_t nowMs) {
  const size_t cellCount =
      static_cast<size_t>(kStandbyLifeColumns) * static_cast<size_t>(kStandbyLifeRows);
  const size_t wordCount = packedLifeWordCount(cellCount);
  standbyLifeCells_.assign(wordCount, 0);
  standbyLifeNextCells_.assign(wordCount, 0);
  standbyScreensaverDimCells_.assign(wordCount, 0);
  standbyMazeVisited_.clear();
  standbyMazeStack_.clear();
  standbyLifeGeneration_ = 0;
  standbyScreensaverRng_ =
      nowMs ^ micros() ^ (static_cast<uint32_t>(reader_.currentIndex() + 1) * 3266489917UL) ^
      0x51a7f00dUL;

  constexpr size_t kVoronoiSiteCount = 15;
  standbyVoronoiX_.assign(kVoronoiSiteCount, 0);
  standbyVoronoiY_.assign(kVoronoiSiteCount, 0);
  standbyVoronoiDx_.assign(kVoronoiSiteCount, 0);
  standbyVoronoiDy_.assign(kVoronoiSiteCount, 0);
  for (size_t i = 0; i < kVoronoiSiteCount; ++i) {
    standbyVoronoiX_[i] = static_cast<int16_t>(
        ((advanceStandbyRng(standbyScreensaverRng_) >> 8) % kStandbyLifeColumns) * 16);
    standbyVoronoiY_[i] = static_cast<int16_t>(
        ((advanceStandbyRng(standbyScreensaverRng_) >> 8) % kStandbyLifeRows) * 16);

    const int16_t dx =
        static_cast<int16_t>(4 + ((advanceStandbyRng(standbyScreensaverRng_) >> 24) % 7));
    const int16_t dy =
        static_cast<int16_t>(3 + ((advanceStandbyRng(standbyScreensaverRng_) >> 24) % 6));
    standbyVoronoiDx_[i] =
        (advanceStandbyRng(standbyScreensaverRng_) & 1U) != 0 ? dx : static_cast<int16_t>(-dx);
    standbyVoronoiDy_[i] =
        (advanceStandbyRng(standbyScreensaverRng_) & 1U) != 0 ? dy : static_cast<int16_t>(-dy);
  }
  renderStandbyVoronoi();
}

void App::renderStandbyVoronoi() {
  const size_t cellCount =
      static_cast<size_t>(kStandbyLifeColumns) * static_cast<size_t>(kStandbyLifeRows);
  const size_t wordCount = packedLifeWordCount(cellCount);
  standbyLifeCells_.assign(wordCount, 0);
  standbyScreensaverDimCells_.assign(wordCount, 0);
  if (standbyVoronoiX_.empty()) {
    return;
  }

  for (uint16_t y = 0; y < kStandbyLifeRows; ++y) {
    const int32_t cellY = static_cast<int32_t>(y) * 16 + 8;
    for (uint16_t x = 0; x < kStandbyLifeColumns; ++x) {
      const int32_t cellX = static_cast<int32_t>(x) * 16 + 8;
      int32_t nearest = INT32_MAX;
      int32_t secondNearest = INT32_MAX;
      for (size_t i = 0; i < standbyVoronoiX_.size(); ++i) {
        const int32_t dx = cellX - standbyVoronoiX_[i];
        const int32_t dy = cellY - standbyVoronoiY_[i];
        const int32_t distance = dx * dx + dy * dy;
        if (distance < nearest) {
          secondNearest = nearest;
          nearest = distance;
        } else if (distance < secondNearest) {
          secondNearest = distance;
        }
      }

      const size_t index = static_cast<size_t>(y) * kStandbyLifeColumns + x;
      const int32_t gap = secondNearest - nearest;
      if (nearest < 1200 || gap < 190) {
        setPackedLifeCell(standbyLifeCells_, index, true);
      } else if (gap < 580 + nearest / 180) {
        setPackedLifeCell(standbyScreensaverDimCells_, index, true);
      }
    }
  }
}

void App::stepStandbyVoronoi() {
  constexpr size_t kVoronoiSiteCount = 15;
  if (standbyVoronoiX_.size() != kVoronoiSiteCount ||
      standbyVoronoiY_.size() != kVoronoiSiteCount ||
      standbyVoronoiDx_.size() != kVoronoiSiteCount ||
      standbyVoronoiDy_.size() != kVoronoiSiteCount) {
    seedStandbyVoronoi(millis());
    return;
  }

  const int16_t maxX = static_cast<int16_t>((kStandbyLifeColumns - 1) * 16);
  const int16_t maxY = static_cast<int16_t>((kStandbyLifeRows - 1) * 16);
  for (size_t i = 0; i < standbyVoronoiX_.size(); ++i) {
    int16_t nextX = static_cast<int16_t>(standbyVoronoiX_[i] + standbyVoronoiDx_[i]);
    int16_t nextY = static_cast<int16_t>(standbyVoronoiY_[i] + standbyVoronoiDy_[i]);
    if (nextX < 0 || nextX > maxX) {
      standbyVoronoiDx_[i] = static_cast<int16_t>(-standbyVoronoiDx_[i]);
      nextX = std::max<int16_t>(0, std::min<int16_t>(maxX, nextX));
    }
    if (nextY < 0 || nextY > maxY) {
      standbyVoronoiDy_[i] = static_cast<int16_t>(-standbyVoronoiDy_[i]);
      nextY = std::max<int16_t>(0, std::min<int16_t>(maxY, nextY));
    }
    standbyVoronoiX_[i] = nextX;
    standbyVoronoiY_[i] = nextY;
  }

  ++standbyLifeGeneration_;
  if (standbyLifeGeneration_ > 2400) {
    seedStandbyVoronoi(millis());
    return;
  }
  renderStandbyVoronoi();
}

void App::seedStandbyScreenOff(uint32_t nowMs) {
  (void)nowMs;
  standbyLifeCells_.clear();
  standbyLifeNextCells_.clear();
  standbyScreensaverDimCells_.clear();
  standbyMazeVisited_.clear();
  standbyMazeStack_.clear();
  standbyVoronoiX_.clear();
  standbyVoronoiY_.clear();
  standbyVoronoiDx_.clear();
  standbyVoronoiDy_.clear();
  standbyStarsX_.clear();
  standbyStarsY_.clear();
  standbyStarsSpeed_.clear();
  standbyStarsBright_.clear();
  standbyMatrixColumns_.clear();
  standbyMatrixHeads_.clear();
  standbyMatrixTrails_.clear();
  standbyLifeGeneration_ = 0;
  standbyScreenOffActive_ = true;
  display_.prepareForSleep();
}

void App::seedStandbyStars(uint32_t nowMs) {
  const size_t cellCount =
      static_cast<size_t>(kStandbyLifeColumns) * static_cast<size_t>(kStandbyLifeRows);
  const size_t wordCount = packedLifeWordCount(cellCount);
  standbyLifeCells_.assign(wordCount, 0);
  standbyLifeNextCells_.assign(wordCount, 0);
  standbyScreensaverDimCells_.assign(wordCount, 0);
  standbyMazeVisited_.clear();
  standbyMazeStack_.clear();
  standbyLifeGeneration_ = 0;

  standbyScreensaverRng_ =
      nowMs ^ micros() ^ (static_cast<uint32_t>(reader_.currentIndex() + 1) * 2246822519UL) ^
      0x57A25EEDUL;

  // Initialize 60 stars at random positions with random speeds and brightness
  constexpr size_t kStarCount = 60;
  standbyStarsX_.resize(kStarCount);
  standbyStarsY_.resize(kStarCount);
  standbyStarsSpeed_.resize(kStarCount);
  standbyStarsBright_.resize(kStarCount);

  for (size_t i = 0; i < kStarCount; ++i) {
    standbyStarsX_[i] = static_cast<int16_t>(
        (advanceStandbyRng(standbyScreensaverRng_) >> 8) % kStandbyLifeColumns);
    standbyStarsY_[i] = static_cast<int16_t>(
        (advanceStandbyRng(standbyScreensaverRng_) >> 8) % kStandbyLifeRows);
    standbyStarsSpeed_[i] = static_cast<int8_t>(
        1 + ((advanceStandbyRng(standbyScreensaverRng_) >> 24) % 3));
    standbyStarsBright_[i] = static_cast<uint8_t>(
        (advanceStandbyRng(standbyScreensaverRng_) >> 16) % 2);
  }

  // Render initial frame
  std::fill(standbyLifeCells_.begin(), standbyLifeCells_.end(), 0);
  for (size_t i = 0; i < kStarCount; ++i) {
    const size_t idx = static_cast<size_t>(standbyStarsY_[i]) * kStandbyLifeColumns +
                       static_cast<size_t>(standbyStarsX_[i]);
    if (idx < cellCount) {
      setPackedLifeCell(standbyLifeCells_, idx, true);
      if (standbyStarsBright_[i]) {
        setPackedLifeCell(standbyScreensaverDimCells_, idx, true);
      }
    }
  }
}

void App::stepStandbyStars() {
  constexpr size_t kStarCount = 60;
  const size_t cellCount =
      static_cast<size_t>(kStandbyLifeColumns) * static_cast<size_t>(kStandbyLifeRows);

  if (standbyStarsX_.size() != kStarCount) return;

  std::fill(standbyLifeCells_.begin(), standbyLifeCells_.end(), 0);
  std::fill(standbyScreensaverDimCells_.begin(), standbyScreensaverDimCells_.end(), 0);

  for (size_t i = 0; i < kStarCount; ++i) {
    // Move star down by its speed (simulating falling stars / starfield)
    standbyStarsY_[i] += standbyStarsSpeed_[i];
    if (standbyStarsY_[i] >= static_cast<int16_t>(kStandbyLifeRows)) {
      // Respawn at top with new random x
      standbyStarsY_[i] = 0;
      standbyStarsX_[i] = static_cast<int16_t>(
          (advanceStandbyRng(standbyScreensaverRng_) >> 8) % kStandbyLifeColumns);
      standbyStarsSpeed_[i] = static_cast<int8_t>(
          1 + ((advanceStandbyRng(standbyScreensaverRng_) >> 24) % 3));
      standbyStarsBright_[i] = static_cast<uint8_t>(
          (advanceStandbyRng(standbyScreensaverRng_) >> 16) % 2);
    }

    const size_t idx = static_cast<size_t>(standbyStarsY_[i]) * kStandbyLifeColumns +
                       static_cast<size_t>(standbyStarsX_[i]);
    if (idx < cellCount) {
      setPackedLifeCell(standbyLifeCells_, idx, true);
      if (standbyStarsBright_[i]) {
        setPackedLifeCell(standbyScreensaverDimCells_, idx, true);
      }
    }

    // Draw a short trail behind
    for (int8_t t = 1; t <= 2; ++t) {
      const int16_t trailY = standbyStarsY_[i] - t * standbyStarsSpeed_[i];
      if (trailY >= 0 && trailY < static_cast<int16_t>(kStandbyLifeRows)) {
        const size_t trailIdx = static_cast<size_t>(trailY) * kStandbyLifeColumns +
                                static_cast<size_t>(standbyStarsX_[i]);
        if (trailIdx < cellCount) {
          setPackedLifeCell(standbyScreensaverDimCells_, trailIdx, true);
        }
      }
    }
  }

  // Occasionally twinkle: randomly toggle a few dim cells
  for (uint8_t t = 0; t < 4; ++t) {
    const size_t twinkleIdx =
        (advanceStandbyRng(standbyScreensaverRng_) >> 8) % cellCount;
    setPackedLifeCell(standbyScreensaverDimCells_, twinkleIdx,
                      !packedLifeCellAlive(standbyScreensaverDimCells_, twinkleIdx));
  }

  standbyLifeGeneration_++;
}

void App::seedStandbyMatrix(uint32_t nowMs) {
  const size_t cellCount =
      static_cast<size_t>(kStandbyLifeColumns) * static_cast<size_t>(kStandbyLifeRows);
  const size_t wordCount = packedLifeWordCount(cellCount);
  standbyLifeCells_.assign(wordCount, 0);
  standbyLifeNextCells_.assign(wordCount, 0);
  standbyScreensaverDimCells_.assign(wordCount, 0);
  standbyMazeVisited_.clear();
  standbyMazeStack_.clear();
  standbyLifeGeneration_ = 0;

  standbyScreensaverRng_ =
      nowMs ^ micros() ^ (static_cast<uint32_t>(reader_.currentIndex() + 1) * 3266489917UL) ^
      0xAA7E1CEDUL;

  // Initialize column raindrop heads
  const uint16_t numCols = kStandbyLifeColumns;
  standbyMatrixColumns_.resize(numCols);
  standbyMatrixHeads_.resize(numCols);
  standbyMatrixTrails_.resize(numCols);

  for (uint16_t c = 0; c < numCols; ++c) {
    standbyMatrixHeads_[c] = static_cast<uint8_t>(
        (advanceStandbyRng(standbyScreensaverRng_) >> 8) % kStandbyLifeRows);
    standbyMatrixTrails_[c] = static_cast<uint8_t>(
        4 + ((advanceStandbyRng(standbyScreensaverRng_) >> 16) % 8));
    // Speed: 0 = slow (skip some frames), 1 = normal, 2 = fast
    standbyMatrixColumns_[c] = static_cast<uint8_t>(
        (advanceStandbyRng(standbyScreensaverRng_) >> 24) % 3);
  }
}

void App::stepStandbyMatrix() {
  const size_t cellCount =
      static_cast<size_t>(kStandbyLifeColumns) * static_cast<size_t>(kStandbyLifeRows);
  const uint16_t numCols = kStandbyLifeColumns;
  const uint16_t numRows = kStandbyLifeRows;

  if (standbyMatrixHeads_.size() != numCols) return;

  std::fill(standbyLifeCells_.begin(), standbyLifeCells_.end(), 0);
  std::fill(standbyScreensaverDimCells_.begin(), standbyScreensaverDimCells_.end(), 0);

  standbyLifeGeneration_++;

  for (uint16_t c = 0; c < numCols; ++c) {
    // Determine if this column should advance this frame based on speed
    const uint8_t speed = standbyMatrixColumns_[c];
    bool shouldAdvance = true;
    if (speed == 0) {
      shouldAdvance = (standbyLifeGeneration_ % 3) == 0;
    } else if (speed == 1) {
      shouldAdvance = (standbyLifeGeneration_ % 2) == 0;
    }

    if (shouldAdvance) {
      standbyMatrixHeads_[c]++;
      if (standbyMatrixHeads_[c] >= numRows + standbyMatrixTrails_[c]) {
        // Respawn from top with new trail length and speed
        standbyMatrixHeads_[c] = 0;
        standbyMatrixTrails_[c] = static_cast<uint8_t>(
            4 + ((advanceStandbyRng(standbyScreensaverRng_) >> 16) % 8));
        standbyMatrixColumns_[c] = static_cast<uint8_t>(
            (advanceStandbyRng(standbyScreensaverRng_) >> 24) % 3);
      }
    }

    // Draw the head (bright pixel)
    const int16_t headY = static_cast<int16_t>(standbyMatrixHeads_[c]);
    if (headY >= 0 && headY < numRows) {
      const size_t headIdx = static_cast<size_t>(headY) * numCols + c;
      setPackedLifeCell(standbyLifeCells_, headIdx, true);
    }

    // Draw the trail (dimmer pixels)
    const uint8_t trail = standbyMatrixTrails_[c];
    for (uint8_t t = 1; t <= trail; ++t) {
      const int16_t trailY = headY - static_cast<int16_t>(t);
      if (trailY >= 0 && trailY < numRows) {
        const size_t trailIdx = static_cast<size_t>(trailY) * numCols + c;
        if (t <= 2) {
          // Near-head trail: bright
          setPackedLifeCell(standbyLifeCells_, trailIdx, true);
        } else {
          // Far trail: dim
          setPackedLifeCell(standbyScreensaverDimCells_, trailIdx, true);
        }
      }
    }
  }

  // Add occasional random bright flickers for the "digital rain" effect
  for (uint8_t f = 0; f < 3; ++f) {
    const size_t flickerIdx =
        (advanceStandbyRng(standbyScreensaverRng_) >> 8) % cellCount;
    setPackedLifeCell(standbyScreensaverDimCells_, flickerIdx, true);
  }
}

void App::openScreensaverSettings() {
  screensaverSettingsSelectedIndex_ = kScreensaverSettingsStyleIndex;
  menuScreen_ = MenuScreen::ScreensaverSettings;
  rebuildSettingsMenuItems();
  renderSettings();
}

void App::selectScreensaverSettingsItem(uint32_t nowMs) {
  switch (settingsSelectedIndex_) {
    case kScreensaverSettingsBackIndex:
      settingsSelectedIndex_ = kSettingsDisplayScreensaverIndex;
      menuScreen_ = MenuScreen::SettingsDisplay;
      rebuildSettingsMenuItems();
      renderSettings();
      return;
    case kScreensaverSettingsStyleIndex:
      // Cycle through all screensaver modes
      switch (screensaverMode_) {
        case ScreensaverMode::Life:
          screensaverMode_ = ScreensaverMode::Maze;
          break;
        case ScreensaverMode::Maze:
          screensaverMode_ = ScreensaverMode::Voronoi;
          break;
        case ScreensaverMode::Voronoi:
          screensaverMode_ = ScreensaverMode::Stars;
          break;
        case ScreensaverMode::Stars:
          screensaverMode_ = ScreensaverMode::Matrix;
          break;
        case ScreensaverMode::Matrix:
          screensaverMode_ = ScreensaverMode::ScreenOff;
          break;
        case ScreensaverMode::ScreenOff:
        default:
          screensaverMode_ = ScreensaverMode::Life;
          break;
      }
      preferences_.putUChar(kPrefScreensaverMode, static_cast<uint8_t>(screensaverMode_));
      rebuildSettingsMenuItems();
      showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
      renderSettings();
      return;
    case kScreensaverSettingsTimeoutIndex:
      screensaverTimeoutIndex_ =
          (screensaverTimeoutIndex_ + 1) % kScreensaverTimeoutCount;
      preferences_.putUChar(kPrefScreensaverTimeout, screensaverTimeoutIndex_);
      rebuildSettingsMenuItems();
      showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
      renderSettings();
      return;
    case kScreensaverSettingsAutoOffIndex:
      screensaverAutoOffIndex_ =
          (screensaverAutoOffIndex_ + 1) % kScreensaverAutoOffCount;
      preferences_.putUChar(kPrefScreensaverAutoOff, screensaverAutoOffIndex_);
      rebuildSettingsMenuItems();
      showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
      renderSettings();
      return;
    case kScreensaverSettingsSleepGuardIndex:
      screensaverSleepGuardIndex_ =
          (screensaverSleepGuardIndex_ + 1) % kScreensaverSleepGuardCount;
      preferences_.putUChar(kPrefScreensaverSleepGuard, screensaverSleepGuardIndex_);
      rebuildSettingsMenuItems();
      showGridToast(settingsMenuItems_[settingsSelectedIndex_], nowMs);
      renderSettings();
      return;
    case kScreensaverSettingsPreviewIndex:
      // Enter standby to preview the screensaver
      enterStandby(nowMs);
      return;
    default:
      return;
  }
}

void App::renderScreensaverSettings() {
  renderSettings();
}

String App::screensaverTimeoutLabel() const {
  if (screensaverTimeoutIndex_ >= kScreensaverTimeoutCount) return "5 min";
  const uint16_t minutes = kScreensaverTimeoutMinutes[screensaverTimeoutIndex_];
  return String(minutes) + " min";
}

String App::screensaverAutoOffLabel() const {
  if (screensaverAutoOffIndex_ >= kScreensaverAutoOffCount) return tr(TrKey::Never);
  if (screensaverAutoOffIndex_ == 0) return tr(TrKey::Never);
  const uint16_t minutes = kScreensaverAutoOffMinutes[screensaverAutoOffIndex_];
  if (minutes >= 60) {
    return String(minutes / 60) + "h";
  }
  return String(minutes) + " min";
}

String App::screensaverSleepGuardLabel() const {
  if (screensaverSleepGuardIndex_ >= kScreensaverSleepGuardCount) return tr(TrKey::No);
  if (screensaverSleepGuardIndex_ == 0) return tr(TrKey::No);
  const uint16_t minutes = kScreensaverSleepGuardMinutes[screensaverSleepGuardIndex_];
  if (minutes >= 60) {
    return String(minutes / 60) + "h";
  }
  return String(minutes) + " min";
}

void App::updateStandbyScreensaver(uint32_t nowMs, bool force) {
  if (state_ != AppState::Standby) {
    return;
  }

  // Auto power-off: check if standby has been active long enough
  if (screensaverAutoOffIndex_ > 0) {
    const uint32_t autoOffMs =
        static_cast<uint32_t>(kScreensaverAutoOffMinutes[screensaverAutoOffIndex_]) * 60000UL;
    if (nowMs - standbyEnteredMs_ >= autoOffMs) {
      Serial.println("[app] screensaver auto power-off triggered");
      enterPowerOff(nowMs);
      return;
    }
  }

  if (screensaverMode_ == ScreensaverMode::ScreenOff) {
    if (!standbyScreenOffActive_) {
      seedStandbyScreenOff(nowMs);
    }
    lastStandbyFrameMs_ = nowMs;
    return;
  }

  if (!force && nowMs - lastStandbyFrameMs_ < kStandbyFrameMs) {
    return;
  }

  if (!force) {
    stepStandbyScreensaver(nowMs);
  } else if (standbyLifeCells_.empty()) {
    seedStandbyScreensaver(nowMs);
  }

  lastStandbyFrameMs_ = nowMs;
  
  // Hint text: fade in/out every 10 seconds
  // Cycle: 10s total. First 1s = fade in, 1s-3s = visible, 3s-4s = fade out, 4s-10s = hidden
  const uint32_t standbyElapsed = nowMs - standbyEnteredMs_;
  const uint32_t hintCycleMs = standbyElapsed % 10000UL;
  uint8_t hintAlpha = 0;
  String hintText;
  if (hintCycleMs < 1000) {
    // Fade in: 0→255 over 1 second
    hintAlpha = static_cast<uint8_t>((hintCycleMs * 255UL) / 1000UL);
  } else if (hintCycleMs < 3000) {
    // Fully visible
    hintAlpha = 255;
  } else if (hintCycleMs < 4000) {
    // Fade out: 255→0 over 1 second
    hintAlpha = static_cast<uint8_t>(255 - ((hintCycleMs - 3000UL) * 255UL) / 1000UL);
  }
  if (hintAlpha > 0) {
    hintText = tr(TrKey::ScreensaverHint);
  }

  // Style name at the top for the first ~2s of standby: full brightness,
  // then a quick fade — so glancing at the screensaver (including the
  // dedicated Settings > Screensaver > Preview button) tells you which
  // style is active instead of showing an unlabeled animation.
  String styleLabel;
  uint8_t styleLabelAlpha = 0;
  if (standbyElapsed < 1600) {
    styleLabelAlpha = 255;
  } else if (standbyElapsed < 2000) {
    styleLabelAlpha = static_cast<uint8_t>(255 - ((standbyElapsed - 1600UL) * 255UL) / 400UL);
  }
  if (styleLabelAlpha > 0) {
    styleLabel = screensaverModeLabel();
  }

  display_.renderLifeScreensaver(standbyLifeCells_, kStandbyLifeColumns, kStandbyLifeRows,
                                 standbyLifeGeneration_,
                                 standbyScreensaverDimCells_.empty() ? nullptr
                                                                      : &standbyScreensaverDimCells_,
                                 hintText, hintAlpha, styleLabel, styleLabelAlpha);
}

void App::enterPowerOff(uint32_t nowMs) {
  if (powerOffStarted_) {
    return;
  }

  powerOffStarted_ = true;
  Serial.println("[app] powering off; hold PWR to start again");
  saveReadingPosition(true);
  pausedTouch_.active = false;
  pausedTouchIntent_ = TouchIntent::None;
  touchPlayHeld_ = false;
  touchPlayPendingRelease_ = false;
  contextViewVisible_ = false;
  wpmFeedbackVisible_ = false;
  menuScreen_ = MenuScreen::Main;
  state_ = AppState::Sleeping;

  display_.renderStatus("OFF", tr2(TrKey2::ReleasePwr), tr2(TrKey2::HoldPwrToStart));
  delay(300);
  display_.prepareForSleep();

  activeBookStore_.close();
  storage_.end();
  touch_.end();
  touchInitialized_ = false;
  Serial.flush();

  BoardConfig::holdBacklightOffForDeepSleep();
  BoardConfig::releaseBatteryPowerHold();

  const uint32_t waitStartMs = millis();
  while (powerButton_.isHeld() && millis() - waitStartMs < kPowerOffReleaseWaitMs) {
    powerButton_.update(millis());
    delay(10);
  }

  BoardConfig::enablePwrButtonExt0Wakeup();
  esp_deep_sleep_start();
}

void App::enterSleep(uint32_t nowMs) {
  Serial.println("[app] entering light sleep; press BOOT to wake");
  saveReadingPosition(true);
  setState(AppState::Sleeping, nowMs);
  Serial.flush();
  delay(200);

  display_.prepareForSleep();
  activeBookStore_.close();
  storage_.end();
  touch_.end();
  touchInitialized_ = false;

  BoardConfig::lightSleepUntilBootButton();
  wakeFromSleep();
}

void App::wakeFromSleep() {
  const uint32_t nowMs = millis();
  Serial.println("[app] woke from light sleep");

  BoardConfig::begin();
  button_.begin();
  powerButton_.begin();
  bootButtonReleasedSinceBoot_ = !button_.isHeld();
  bootButtonLongPressHandled_ = false;
  powerButtonReleasedSinceBoot_ = !powerButton_.isHeld();
  powerButtonLongPressHandled_ = false;
  powerTapPending_ = false;
  powerOffStarted_ = false;
  updateBatteryStatus(nowMs, true);
  storage_.setStatusCallback(&App::handleStorageStatus, this);
  pausedTouch_.active = false;
  pausedTouchIntent_ = TouchIntent::None;
  wpmFeedbackVisible_ = false;
  menuScreen_ = MenuScreen::Main;
  lastStateLogMs_ = nowMs;
  state_ = AppState::Paused;

  const bool displayReady = display_.wakeFromSleep();
  touchInitialized_ = touch_.begin();
  storageReady_ = storage_.begin();

  if (storageReady_ && usingStorageBook_ && !currentBookPath_.isEmpty()) {
    const size_t resumeIndex = reader_.currentIndex();
    const int refreshedBookIndex = findBookIndexByPath(currentBookPath_);
    if (refreshedBookIndex >= 0 &&
        loadBookAtIndex(static_cast<size_t>(refreshedBookIndex), nowMs, false, false, false,
                        false)) {
      reader_.seekTo(resumeIndex);
    } else {
      Serial.println("[app] current indexed book unavailable after wake");
      usingStorageBook_ = false;
      currentBookPath_ = "";
      currentBookTitle_ = "Demo";
      reader_.clearLoadedBook(nowMs);
      reader_.begin(nowMs);
    }
  }

  if (displayReady) {
    renderActiveReader(nowMs);
  }
}

bool App::restoreSavedBook(uint32_t nowMs) {
  const String savedPath = preferences_.getString(kPrefBookPath, "");
  if (savedPath.isEmpty()) {
    return false;
  }

  const int bookIndex = findBookIndexByPath(savedPath);
  if (bookIndex < 0) {
    Serial.printf("[app] saved book not found: %s\n", savedPath.c_str());
    return false;
  }

  if (!loadBookAtIndex(static_cast<size_t>(bookIndex), nowMs, true, false, false, false)) {
    return false;
  }

  Serial.printf("[app] restored %s at word %u\n", savedPath.c_str(),
                static_cast<unsigned int>(reader_.currentIndex()));
  return true;
}

bool App::prepareBootBookLoad() {
  pendingBootBookIndex_ = 0;
  pendingBootBookLegacyFallback_ = false;

  if (!storageReady_ || storage_.bookCount() == 0) {
    return false;
  }

  const String savedPath = preferences_.getString(kPrefBookPath, "");
  if (!savedPath.isEmpty()) {
    const int savedBookIndex = findBookIndexByPath(savedPath);
    if (savedBookIndex >= 0) {
      pendingBootBookIndex_ = static_cast<size_t>(savedBookIndex);
      pendingBootBookLegacyFallback_ = true;
      Serial.printf("[app] deferred saved book load: %s\n", savedPath.c_str());
      return true;
    }

    Serial.printf("[app] saved book not found: %s\n", savedPath.c_str());
  }

  pendingBootBookIndex_ = 0;
  pendingBootBookLegacyFallback_ = false;
  Serial.println("[app] deferred first book load");
  return true;
}

void App::loadPendingBootBook(uint32_t nowMs) {
  if (!pendingBootBookLoad_) {
    return;
  }

  // Called from two places: updateState()'s Booting branch (while the boot
  // splash is still on screen, right before it hands off to the reader) and
  // the fallback check every frame thereafter for the rare case a caller
  // reaches Paused some other way first. Only the Booting case can hide the
  // load behind the splash — by the time we're already in Paused, something
  // has to be on screen, so the placeholder is unavoidable there.
  const bool bootTimeLoad = state_ == AppState::Booting;
  if (!bootTimeLoad && state_ != AppState::Paused) {
    return;
  }

  pendingBootBookLoad_ = false;
  if (bootTimeLoad) {
    suppressBootStorageStatusRender_ = true;
  } else {
    display_.renderStatus(tr(TrKey::LoadingBook), currentBookTitle_,
                          tr(TrKey::PleaseWait));
  }

  const uint32_t startedMs = millis();
  // allowIndexBuild must stay true regardless of pendingBootBookLegacyFallback_:
  // on a genuinely first-ever boot (fresh SD, no cached index, no saved book
  // path) tying it to that flag made the very first deferred load always fail
  // — there was no index yet to "check", and building one wasn't allowed. Every
  // other loadBookAtIndex() call site in the app (picking a book, book details,
  // ...) already passes true unconditionally; boot should behave the same way.
  const bool loaded = loadBookAtIndex(pendingBootBookIndex_, nowMs,
                                      pendingBootBookLegacyFallback_, true, false,
                                      false);
  suppressBootStorageStatusRender_ = false;
  const uint32_t elapsedMs = millis() - startedMs;
  Serial.printf("[app] deferred book load %s in %lu ms\n", loaded ? "ok" : "failed",
                static_cast<unsigned long>(elapsedMs));

  if (loaded) {
    usingStorageBook_ = true;
  } else {
    usingStorageBook_ = false;
    chapterMarkers_.clear();
    paragraphStarts_.clear();
    currentBookPath_ = "";
    currentBookTitle_ = "Demo";
    reader_.begin(millis());
    invalidateContextPreviewWindow();
    Serial.println("[app] using built-in demo text");
  }

  // At boot, the caller (updateState()) still has to run setState(Paused/
  // Playing) right after this returns — that call renders the real reader
  // screen once, so the splash is replaced exactly once and never by a
  // placeholder. Outside of boot, this function is the end of the line, so
  // it has to render itself.
  if (!bootTimeLoad) {
    renderActiveReader(millis());
  }
}

void App::saveReadingPosition(bool force) {
  if (!usingStorageBook_ || currentBookPath_.isEmpty()) {
    return;
  }

  const size_t wordIndex = reader_.currentIndex();
  if (!force && wordIndex == lastSavedWordIndex_) {
    return;
  }

  preferences_.putString(kPrefBookPath, currentBookPath_);
  preferences_.putUInt(bookPositionKey(currentBookPath_).c_str(), static_cast<uint32_t>(wordIndex));
  preferences_.putUInt(bookWordCountKey(currentBookPath_).c_str(),
                       static_cast<uint32_t>(reader_.wordCount()));
  preferences_.putUInt(kPrefLegacyWordIndex, static_cast<uint32_t>(wordIndex));
  preferences_.putUShort(kPrefWpm, reader_.wpm());
  markBookRecent(currentBookPath_);
  lastSavedWordIndex_ = wordIndex;
  Serial.printf("[app] saved position word=%u book=%s\n", static_cast<unsigned int>(wordIndex),
                currentBookPath_.c_str());
}

bool App::loadBookAtIndex(size_t index, uint32_t nowMs, bool allowLegacyPositionFallback,
                          bool allowIndexBuild, bool allowEpubConversion,
                          bool rebuildTimeEstimate) {
  BookMetadata book;
  String loadedPath;
  size_t loadedIndex = index;
  const String initialLabel = storage_.bookDisplayName(index);
  renderStorageStatus("Opening book", initialLabel.c_str(),
                      allowIndexBuild ? "Checking index" : "Checking saved index", 5);
  if (!storage_.loadIndexedBook(index, activeBookStore_, book, &loadedPath, &loadedIndex,
                                allowIndexBuild, allowEpubConversion)) {
    return false;
  }

  const String loadedTitle = book.title.isEmpty() ? displayNameForPath(loadedPath) : book.title;
  renderStorageStatus("Opening book", loadedTitle.c_str(), "Loading word cache", 70);

  const bool keepingExistingTimeCache =
      !rebuildTimeEstimate && timeEstimateCacheValid_ && currentBookPath_ == loadedPath;
  reader_.setWordSource(&activeBookStore_, nowMs);
  if (reader_.wordCount() == 0 || reader_.currentWord().isEmpty()) {
    Serial.printf("[app] failed to read first indexed word from %s\n", loadedPath.c_str());
    activeBookStore_.close();
    reader_.clearLoadedBook(nowMs);
    renderStorageStatus("Book open failed", loadedTitle.c_str(), "Word cache unreadable", 100);
    return false;
  }

  chapterMarkers_ = std::move(book.chapters);
  paragraphStarts_ = std::move(book.paragraphStarts);
  invalidateContextPreviewWindow();
  currentBookIndex_ = loadedIndex;
  currentBookPath_ = loadedPath;
  currentBookTitle_ = loadedTitle;
  lastSavedWordIndex_ = static_cast<size_t>(-1);
  usingStorageBook_ = true;
  preferences_.putString(kPrefBookPath, currentBookPath_);
  markBookRecent(currentBookPath_);

  const uint32_t savedWordIndex =
      savedWordIndexForBook(currentBookPath_, allowLegacyPositionFallback);
  if (savedWordIndex != kNoSavedWordIndex) {
    // Check if saved position belongs to this exact book version by comparing
    // word counts. If the count differs, the book content changed (e.g. a new
    // book was uploaded to the same path) — discard the stale position.
    const String countKey = bookWordCountKey(currentBookPath_);
    const uint32_t savedWordCount = preferences_.getUInt(countKey.c_str(), 0);
    const uint32_t currentWordCount = static_cast<uint32_t>(reader_.wordCount());
    if (savedWordCount != 0 && savedWordCount != currentWordCount) {
      // Word count mismatch — book content changed, reset position to start
      preferences_.remove(bookPositionKey(currentBookPath_).c_str());
      Serial.printf("[app] word count mismatch (saved=%u current=%u), resetting position for %s\n",
                    static_cast<unsigned int>(savedWordCount),
                    static_cast<unsigned int>(currentWordCount),
                    currentBookPath_.c_str());
    } else if (savedWordIndex < reader_.wordCount()) {
      renderStorageStatus("Opening book", currentBookTitle_.c_str(), "Restoring position", 78);
      reader_.seekTo(savedWordIndex);
      lastSavedWordIndex_ = reader_.currentIndex();
      Serial.printf("[app] restored book position word=%u key=%s\n",
                    static_cast<unsigned int>(reader_.currentIndex()),
                    bookPositionKey(currentBookPath_).c_str());
    }
  }

  // Update the word count key to the current book's word count
  preferences_.putUInt(bookWordCountKey(currentBookPath_).c_str(),
                       static_cast<uint32_t>(reader_.wordCount()));

  if (rebuildTimeEstimate) {
    rebuildTimeEstimateCache();
  } else if (!keepingExistingTimeCache) {
    invalidateTimeEstimateCache();
  } else {
    renderStorageStatus("Opening book", currentBookTitle_.c_str(), "Using cached estimate", 92);
  }

  lastProgressSaveMs_ = nowMs;
  Serial.printf("[app] loaded SD book[%u/%u]: %s (%u chapters, %u paragraphs)\n",
                static_cast<unsigned int>(loadedIndex + 1),
                static_cast<unsigned int>(storage_.bookCount()), loadedPath.c_str(),
                static_cast<unsigned int>(chapterMarkers_.size()),
                static_cast<unsigned int>(paragraphStarts_.size()));
  return true;
}

String App::bookPositionKey(const String &bookPath) const {
  char key[10];
  std::snprintf(key, sizeof(key), "p%08lx", static_cast<unsigned long>(hashBookPath(bookPath)));
  return String(key);
}

String App::bookWordCountKey(const String &bookPath) const {
  char key[10];
  std::snprintf(key, sizeof(key), "c%08lx", static_cast<unsigned long>(hashBookPath(bookPath)));
  return String(key);
}

String App::bookRecentKey(const String &bookPath) const {
  char key[10];
  std::snprintf(key, sizeof(key), "r%08lx", static_cast<unsigned long>(hashBookPath(bookPath)));
  return String(key);
}

uint32_t App::nextRecentSequence() {
  uint32_t sequence = preferences_.getUInt(kPrefRecentSeq, 0);
  if (sequence == 0xFFFFFFFEUL) {
    sequence = 0;
  }
  ++sequence;
  preferences_.putUInt(kPrefRecentSeq, sequence);
  return sequence;
}

uint32_t App::bookRecentSequence(const String &bookPath) {
  return preferences_.getUInt(bookRecentKey(bookPath).c_str(), 0);
}

void App::markBookRecent(const String &bookPath) {
  if (bookPath.isEmpty()) {
    return;
  }

  preferences_.putUInt(bookRecentKey(bookPath).c_str(), nextRecentSequence());
}

uint32_t App::savedWordIndexForBook(const String &bookPath, bool allowLegacyFallback) {
  const String key = bookPositionKey(bookPath);
  if (preferences_.isKey(key.c_str())) {
    return preferences_.getUInt(key.c_str(), 0);
  }

  if (allowLegacyFallback && preferences_.isKey(kPrefLegacyWordIndex)) {
    const uint32_t legacyWordIndex = preferences_.getUInt(kPrefLegacyWordIndex, 0);
    preferences_.putUInt(key.c_str(), legacyWordIndex);
    Serial.printf("[app] migrated legacy position word=%u to key=%s\n",
                  static_cast<unsigned int>(legacyWordIndex), key.c_str());
    return legacyWordIndex;
  }

  return kNoSavedWordIndex;
}

bool App::bookProgressPercent(size_t bookIndex, uint8_t &percent) {
  size_t wordIndex = 0;
  size_t wordCount = 0;

  if (usingStorageBook_ && bookIndex == currentBookIndex_) {
    wordIndex = reader_.currentIndex();
    wordCount = reader_.wordCount();
  } else {
    const String path = storage_.bookPath(bookIndex);
    const String positionKey = bookPositionKey(path);
    const String countKey = bookWordCountKey(path);
    if (!preferences_.isKey(positionKey.c_str()) || !preferences_.isKey(countKey.c_str())) {
      return false;
    }

    wordIndex = preferences_.getUInt(positionKey.c_str(), 0);
    wordCount = preferences_.getUInt(countKey.c_str(), 0);
  }

  if (wordCount <= 1) {
    return false;
  }

  wordIndex = std::min(wordIndex, wordCount - 1);
  const size_t progress = (wordIndex * static_cast<size_t>(100)) / (wordCount - 1);
  percent = static_cast<uint8_t>(std::min(static_cast<size_t>(100), progress));
  return true;
}

int App::findBookIndexByPath(const String &path) const {
  for (size_t i = 0; i < storage_.bookCount(); ++i) {
    if (storage_.bookPath(i) == path) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void App::renderMenu() {
  applyReaderUiOrientation();

  if (menuScreen_ == MenuScreen::SettingsHome || menuScreen_ == MenuScreen::SettingsDisplay ||
      menuScreen_ == MenuScreen::SettingsPacing || menuScreen_ == MenuScreen::WifiSettings ||
      menuScreen_ == MenuScreen::SettingsConnectivity ||
      menuScreen_ == MenuScreen::SettingsAbout || menuScreen_ == MenuScreen::ScreensaverSettings ||
      menuScreen_ == MenuScreen::WelcomeLanguage ||
      menuScreen_ == MenuScreen::WelcomeTheme ||
      menuScreen_ == MenuScreen::WelcomeHighlightColor ||
      menuScreen_ == MenuScreen::WelcomeReadingMode) {
    renderSettings();
  } else if (menuScreen_ == MenuScreen::Presets) {
    renderMenuAnyMode("", settingsMenuItems_, presetsSelectedIndex_);
  } else if (menuScreen_ == MenuScreen::PresetsDeleteConfirm) {
    // 3-item confirm dialog (Back/Apply/Delete) — always the grid, like the
    // other confirm dialogs (see renderMenuAnyMode()'s doc comment).
    renderItemGrid("", settingsMenuItems_, presetsSelectedIndex_);
  } else if (menuScreen_ == MenuScreen::WifiNetworks) {
    renderWifiNetworks();
  } else if (menuScreen_ == MenuScreen::TextEntry) {
    renderTextEntry();
  } else if (menuScreen_ == MenuScreen::TypographyTuning) {
    renderTypographyTuning();
  } else if (menuScreen_ == MenuScreen::PacingDelayEditor) {
    renderPacingDelayEditor();
  } else if (menuScreen_ == MenuScreen::WpmEditor) {
    renderWpmEditor();
  } else if (menuScreen_ == MenuScreen::TypographyValueEditor) {
    renderTypographyValueEditor();
  } else if (menuScreen_ == MenuScreen::BookPicker) {
    renderBookPicker();
  } else if (menuScreen_ == MenuScreen::BookDetails) {
    renderBookDetails();
  } else if (menuScreen_ == MenuScreen::BookDeleteConfirm) {
    renderItemGrid("", bookDeleteConfirmMenuItems_, bookDeleteConfirmSelectedIndex_);
  } else if (menuScreen_ == MenuScreen::ChapterPicker) {
    renderChapterPicker();
  } else if (menuScreen_ == MenuScreen::SavePointsList) {
    renderSavePointsList();
  } else if (menuScreen_ == MenuScreen::SavePointDeleteConfirm) {
    renderItemGrid("", savePointDeleteConfirmMenuItems_, savePointDeleteConfirmSelectedIndex_);
  } else if (menuScreen_ == MenuScreen::PluginsHome) {
    renderPluginsHome();
  } else if (menuScreen_ == MenuScreen::PluginsActive) {
    renderPluginsActive();
  } else if (menuScreen_ == MenuScreen::PluginLibraryScreen) {
    renderPluginLibraryScreen();
  } else if (menuScreen_ == MenuScreen::PluginDetail) {
    renderPluginDetail();
  } else if (menuScreen_ == MenuScreen::TypographyFontPicker) {
    renderTypographyFontPicker();
  } else if (menuScreen_ == MenuScreen::RestartConfirm) {
    renderRestartConfirm();
  } else if (menuScreen_ == MenuScreen::TypographyResetConfirm) {
    renderTypographyResetConfirm();
  } else if (menuScreen_ == MenuScreen::SdCardRepairConfirm) {
    renderSdCardRepairConfirm();
  } else if (menuScreen_ == MenuScreen::UpdateConfirm) {
    renderUpdateConfirm();
  } else if (menuScreen_ == MenuScreen::WelcomeConnect) {
    renderWelcomeConnect();
  } else if (menuScreen_ == MenuScreen::WelcomeAppPairing) {
    renderWelcomeAppPairing();
  } else if (menuScreen_ == MenuScreen::WelcomeConfigureInApp) {
    renderWelcomeTimedMessage(tr3(TrKey3::WelcomeConfigureInAppLine1),
                              tr3(TrKey3::WelcomeConfigureInAppLine2));
  } else if (menuScreen_ == MenuScreen::WelcomeReadingModePreview) {
    renderWelcomeReadingModePreview();
  } else if (menuScreen_ == MenuScreen::WelcomeLoading) {
    renderWelcomeLoading(millis());
  } else if (menuScreen_ == MenuScreen::WelcomeSuper) {
    renderWelcomeTimedMessage(tr3(TrKey3::WelcomeSuperTitle));
  } else if (menuScreen_ == MenuScreen::WelcomeConfigureIntro) {
    renderWelcomeTimedMessage(tr3(TrKey3::WelcomeConfigureTitle));
  } else if (menuScreen_ == MenuScreen::TutorialStep1 ||
             menuScreen_ == MenuScreen::TutorialStep2 ||
             menuScreen_ == MenuScreen::TutorialStep3 ||
             menuScreen_ == MenuScreen::TutorialStep4 ||
             menuScreen_ == MenuScreen::TutorialStep5) {
    renderTutorialStep();
  } else {
    renderMainMenu();
  }
}

void App::renderMainMenu() {
  std::vector<String> items;
  items.reserve(MenuItemCount + 1);

  // Show update button at top when available (and versions differ)
  if (otaUpdatePromptPending_) {
    String current = otaUpdater_.currentVersion();
    int dashPos = current.indexOf('-');
    String currentBase = (dashPos > 0) ? current.substring(0, dashPos) : current;
    if (pendingUpdateNewVersion_ != currentBase) {
      items.push_back(String(">> Update ") + pendingUpdateNewVersion_);
    } else {
      otaUpdatePromptPending_ = false;
    }
  }

  // mainMenuOrder_ mówi selectMenuItem(), który MenuItem odpowiada której
  // pozycji na ekranie — w trybie zaawansowanym Wyłącz siedzi po Pluginach,
  // więc kolejności nie da się już wyprowadzić ze stałego enuma.
  mainMenuOrder_.clear();
  items.push_back(uiText(UiText::Read));
  mainMenuOrder_.push_back(MenuRead);
  items.push_back(uiText(UiText::Library));
  mainMenuOrder_.push_back(MenuLibrary);
  items.push_back(uiText(UiText::SavePoints));
  mainMenuOrder_.push_back(MenuSavePoints);
  items.push_back(uiText(UiText::Settings));
  mainMenuOrder_.push_back(MenuSettings);
  if (devModeEnabled()) {
    items.push_back(uiText(UiText::Plugins));
    mainMenuOrder_.push_back(MenuPlugins);
  }
  items.push_back(uiText(UiText::PowerOff));
  mainMenuOrder_.push_back(MenuPowerOff);
  renderMenuAnyMode("", items, menuSelectedIndex_);
}

// Ile pozycji faktycznie rysuje renderMainMenu() — potrzebne nawigacji
// (moveMenuSelection/D-Pad), żeby kursor nie wchodził na nieistniejący
// wiersz i żeby zawijanie listy działało na realnej długości.
size_t App::mainMenuItemCount() {
  size_t count = devModeEnabled() ? MenuItemCount : MenuItemCount - 1;
  if (otaUpdatePromptPending_) {
    ++count;  // przycisk ">> Update" doklejany na górze
  }
  return count;
}

void App::renderSettings() {
  if (settingsMenuItems_.empty()) {
    rebuildSettingsMenuItems();
  }

  // Show "?" indicator on selected item only if help is available
  std::vector<String> renderItems = settingsMenuItems_;
  if (showHelpHints_ && settingsSelectedIndex_ < renderItems.size() && settingsSelectedIndex_ > 0) {
    bool hasHelp = false;
    if (menuScreen_ == MenuScreen::SettingsDisplay) {
      hasHelp = HelpTexts::getDisplayHelp(settingsSelectedIndex_ - 1) != nullptr;
    } else if (menuScreen_ == MenuScreen::SettingsPacing) {
      hasHelp = (readerMode_ == ReaderMode::Scroll)
        ? HelpTexts::getPacingScrollHelp(settingsSelectedIndex_ - 1) != nullptr
        : HelpTexts::getPacingHelp(settingsSelectedIndex_ - 1) != nullptr;
    }
    if (hasHelp) {
      renderItems[settingsSelectedIndex_] += " ?";
    }
  }

  String title;
  switch (menuScreen_) {
    case MenuScreen::WelcomeLanguage:
      title = tr3(TrKey3::WelcomeLanguageTitle);
      break;
    case MenuScreen::WelcomeTheme:
      title = tr3(TrKey3::WelcomeThemeTitle);
      break;
    case MenuScreen::WelcomeHighlightColor:
      title = tr3(TrKey3::WelcomeHighlightColorTitle);
      break;
    case MenuScreen::WelcomeReadingMode:
      title = tr3(TrKey3::WelcomeReadingModeTitle);
      break;
    default:
      break;
  }
  renderMenuAnyMode(title, renderItems, settingsSelectedIndex_);
}

void App::renderTypographyTuning() {
  if (kTypographyPreviewWordCount == 0) {
    display_.renderStatus(uiText(UiText::Typography), uiText(UiText::NoSamples), "");
    return;
  }

  if (typographyPreviewSampleIndex_ >= kTypographyPreviewWordCount) {
    typographyPreviewSampleIndex_ = 0;
  }
  if (typographyTuningSelectedIndex_ >= TypographyTuningItemCount) {
    typographyTuningSelectedIndex_ = TypographyTuningFontSize;
  }

  const size_t index = typographyPreviewSampleIndex_;
  const size_t beforeIndex =
      index == 0 ? kTypographyPreviewWordCount - 1 : index - 1;
  const size_t afterIndex =
      (index + 1 >= kTypographyPreviewWordCount) ? 0 : index + 1;
  const String beforeText = phantomWordsEnabled_ ? kTypographyPreviewWords[beforeIndex] : "";
  const String afterText = phantomWordsEnabled_ ? kTypographyPreviewWords[afterIndex] : "";
  const String line1 = typographyTuningLabel() + ": " + typographyTuningValueLabel();
  // Position among the TypographyTuningItemCount swipeable settings (Back
  // included, so this always shows something like "3/10") — makes the
  // horizontal-swipe paging discoverable the same way page dots do on grid
  // screens, since this single-item-at-a-time screen has no dots to show.
  const String title =
      uiText(UiText::Typography) + " " +
      String(static_cast<unsigned int>(typographyTuningSelectedIndex_) + 1) + "/" +
      String(static_cast<unsigned int>(TypographyTuningItemCount));
  String line2 = uiText(UiText::SwipeMoreSettings);
  if (typographyTuningSelectedIndex_ == TypographyTuningBack) {
    line2 = uiText(UiText::TapExitSample);
  } else if (typographyTuningSelectedIndex_ == TypographyTuningPhantomWords ||
             typographyTuningSelectedIndex_ == TypographyTuningFocusHighlight) {
    line2 = uiText(UiText::TapToggleSample);
  } else if (typographyTuningSelectedIndex_ == TypographyTuningReset) {
    line2 = uiText(UiText::TapToReset);
  }

  display_.renderTypographyPreview(beforeText,
                                   kTypographyPreviewWords[index],
                                   afterText,
                                   readerFontSizeIndex_, title, line1, line2);
}

void App::renderTypographyResetConfirm() {
  std::vector<String> items;
  items.reserve(TypographyResetConfirmItemCount + kTypographyResetConfirmHeaderRows);
  items.push_back(uiText(UiText::ResetTypographyQuestion));
  items.push_back(uiText(UiText::NoKeepSettings));
  items.push_back(uiText(UiText::YesReset));

  renderItemGrid(items[0], items,
                 typographyResetConfirmSelectedIndex_ + kTypographyResetConfirmHeaderRows,
                 kTypographyResetConfirmHeaderRows);
}

void App::renderBookPicker() {
  renderMenuAnyModeLibrary(bookMenuItems_, bookPickerSelectedIndex_);
}

void App::renderChapterPicker() {
  renderMenuAnyMode("", chapterMenuItems_, chapterPickerSelectedIndex_);
}

void App::renderRestartConfirm() {
  std::vector<String> items;
  items.reserve(RestartConfirmItemCount);
  items.push_back(uiText(UiText::AreYouSure));
  items.push_back(uiText(UiText::NoKeepPlace));
  items.push_back(uiText(UiText::YesRestart));

  renderItemGrid(items[0], items, restartConfirmSelectedIndex_ + kRestartConfirmHeaderRows,
                 kRestartConfirmHeaderRows);
}

void App::renderSdCardRepairConfirm() {
  std::vector<String> items;
  items.reserve(SdCardRepairConfirmItemCount + kSdCardRepairConfirmHeaderRows);
  items.push_back(tr2(TrKey2::RepairFolders));
  items.push_back(tr2(TrKey2::NotNow));
  items.push_back(tr2(TrKey2::CreateFolders));

  renderItemGrid(items[0], items,
                 sdCardRepairConfirmSelectedIndex_ + kSdCardRepairConfirmHeaderRows,
                 kSdCardRepairConfirmHeaderRows);
}

void App::renderUpdateConfirm() {
  std::vector<String> items;
  items.reserve(UpdateConfirmItemCount + kUpdateConfirmHeaderRows);
  items.push_back(tr2(TrKey2::UpdateAvailable));
  items.push_back(pendingUpdateCurrentVersion_ + " -> " + pendingUpdateNewVersion_);
  items.push_back(tr2(TrKey2::SkipForNow));
  items.push_back(tr2(TrKey2::Update));

  renderItemGrid(items[0] + ": " + items[1], items,
                 updateConfirmSelectedIndex_ + kUpdateConfirmHeaderRows,
                 kUpdateConfirmHeaderRows);
}

bool App::updateChapterTransition(uint32_t nowMs) {
  if (!chapterTransitionVisible_) {
    return false;
  }

  if (nowMs < chapterTransitionUntilMs_) {
    return true;
  }

  chapterTransitionVisible_ = false;
  reader_.start(nowMs);
  renderActiveReader(nowMs);
  return true;
}

bool App::maybeStartChapterTransition(size_t previousWordIndex, size_t currentWordIndex,
                                      uint32_t nowMs) {
  if (chapterMarkers_.empty() || currentWordIndex <= previousWordIndex) {
    return false;
  }

  for (size_t i = 0; i < chapterMarkers_.size(); ++i) {
    const size_t chapterWordIndex = chapterMarkers_[i].wordIndex;
    if (chapterWordIndex == 0 || chapterWordIndex <= previousWordIndex ||
        chapterWordIndex > currentWordIndex) {
      continue;
    }

    chapterTransitionIndex_ = i;
    chapterTransitionVisible_ = true;
    chapterTransitionUntilMs_ = nowMs + kChapterTransitionMs;
    contextViewVisible_ = false;
    wpmFeedbackVisible_ = false;
    reader_.seekTo(chapterWordIndex);
    renderChapterTransition();
    Serial.printf("[chapter] transition %u/%u word=%u title=%s\n",
                  static_cast<unsigned int>(i + 1),
                  static_cast<unsigned int>(chapterMarkers_.size()),
                  static_cast<unsigned int>(chapterWordIndex),
                  chapterMarkers_[i].title.c_str());
    return true;
  }

  return false;
}

void App::renderChapterTransition() {
  if (!chapterTransitionVisible_ || chapterTransitionIndex_ >= chapterMarkers_.size()) {
    return;
  }

  applyReaderUiOrientation();
  const String title = String("CHAPTER ") + String(chapterTransitionIndex_ + 1);
  String subtitle = chapterMarkers_[chapterTransitionIndex_].title;
  if (subtitle.length() > 42) {
    subtitle = subtitle.substring(0, 42) + "...";
  }
  display_.renderStatus(title, subtitle, "");
}

DisplayManager::LibraryItem App::libraryItemForBook(size_t bookIndex) {
  DisplayManager::LibraryItem item;
  item.title = storage_.bookDisplayName(bookIndex);
  item.subtitle = storage_.bookAuthorName(bookIndex);

  uint8_t percent = 0;
  const bool hasProgress = bookProgressPercent(bookIndex, percent);
  if (hasProgress) {
    if (!item.subtitle.isEmpty()) {
      item.subtitle += " - ";
    }
    item.subtitle += String(percent) + "%";
  }

  if (item.subtitle.isEmpty() && usingStorageBook_ && bookIndex == currentBookIndex_) {
    item.subtitle = uiText(UiText::CurrentBook);
  }

  return item;
}

String App::chapterMenuLabel(size_t chapterIndex) const {
  if (chapterIndex >= chapterMarkers_.size()) {
    return "";
  }

  String label = String(chapterIndex + 1) + " " + chapterMarkers_[chapterIndex].title;
  if (label.length() > 36) {
    label = label.substring(0, 36) + "...";
  }

  const size_t currentIndex = reader_.currentIndex();
  const size_t startIndex = chapterMarkers_[chapterIndex].wordIndex;
  const size_t endIndex = (chapterIndex + 1 < chapterMarkers_.size())
                              ? chapterMarkers_[chapterIndex + 1].wordIndex
                              : reader_.wordCount();
  if (currentIndex >= startIndex && currentIndex < endIndex) {
    label += " *";
  }
  return label;
}

size_t App::currentChapterIndex() const {
  if (chapterMarkers_.empty()) {
    return static_cast<size_t>(-1);
  }

  size_t currentChapter = 0;
  const size_t currentIndex = reader_.currentIndex();
  for (size_t i = 0; i < chapterMarkers_.size(); ++i) {
    if (chapterMarkers_[i].wordIndex <= currentIndex) {
      currentChapter = i;
    }
  }

  return currentChapter;
}

String App::currentChapterLabel() const {
  const size_t chapterIndex = currentChapterIndex();
  if (chapterIndex >= chapterMarkers_.size()) {
    return currentBookTitle_.isEmpty() ? uiText(UiText::Start) : currentBookTitle_;
  }

  return chapterMarkers_[chapterIndex].title;
}

String App::currentFooterMetricLabel() const {
  if (footerMetricMode_ == FooterMetricMode::Percentage) {
    return String(readingProgressPercent()) + "%";
  }

  const size_t wordCount = reader_.wordCount();
  if (wordCount == 0) {
    return "0%";
  }

  const size_t currentIndex = std::min(reader_.currentIndex(), wordCount - 1);
  size_t endIndex = wordCount;
  const bool generatingEstimate = accurateTimeEstimateEnabled_ && timeEstimateBuildInProgress_ &&
                                  timeEstimateBuildMatchesCurrentBook();
  const int generatingPercent =
      generatingEstimate
          ? static_cast<int>((timeEstimateBuildNextBlock_ * 100UL) /
                             std::max<size_t>(1, timeEstimateBuildBlockCount_))
          : 0;

  if (footerMetricMode_ == FooterMetricMode::ChapterTime) {
    const size_t chapterIndex = currentChapterIndex();
    if (chapterIndex < chapterMarkers_.size() && chapterIndex + 1 < chapterMarkers_.size()) {
      endIndex = chapterMarkers_[chapterIndex + 1].wordIndex;
    }
    if (generatingEstimate) {
      return String("CH ") + String(generatingPercent) + "% gen";
    }
    return String("CH ") +
           formatReadingTimeRemaining(estimatedReadingTimeRemainingMs(currentIndex, endIndex));
  }

  if (generatingEstimate) {
    return String("BOOK ") + String(generatingPercent) + "% gen";
  }
  return String("BOOK ") +
         formatReadingTimeRemaining(estimatedReadingTimeRemainingMs(currentIndex, endIndex));
}

String App::currentBatteryLabel() const {
  if (!batteryPresent_ || !batterySampleInitialized_) {
    return "";
  }

  if (batteryLabelMode_ == BatteryLabelMode::TimeRemaining) {
    return batteryTimeRemainingLabel();
  }

  if (batteryLabelMode_ == BatteryLabelMode::Voltage) {
    return batteryVoltageLabel();
  }

  return String(static_cast<unsigned int>(batteryDisplayedPercent_)) + "%";
}

String App::footerMetricModeLabel() const {
  switch (footerMetricMode_) {
    case FooterMetricMode::ChapterTime:
      return tr(TrKey::ChapterTime);
    case FooterMetricMode::BookTime:
      return tr(TrKey::BookTime);
    case FooterMetricMode::Percentage:
    default:
      return tr(TrKey::PercentRead);
  }
}

String App::batteryLabelModeLabel() const {
  switch (batteryLabelMode_) {
    case BatteryLabelMode::TimeRemaining:
      return tr(TrKey::TimeRemaining);
    case BatteryLabelMode::Voltage:
      return tr(TrKey::Voltage);
    case BatteryLabelMode::Percent:
    default:
      return tr(TrKey::Percentage);
  }
}

String App::screensaverModeLabel() const {
  switch (screensaverMode_) {
    case ScreensaverMode::Maze:
      return tr(TrKey::Maze);
    case ScreensaverMode::Voronoi:
      return "Voronoi";
    case ScreensaverMode::Stars:
      return tr(TrKey::Stars);
    case ScreensaverMode::Matrix:
      return tr(TrKey::MatrixRain);
    case ScreensaverMode::ScreenOff:
      return tr(TrKey::ScreenOff);
    case ScreensaverMode::Life:
    default:
      return tr(TrKey::Life);
  }
}

String App::batteryTimeRemainingLabel() const {
  if (batteryRuntimeEstimateReady_) {
    return formatBatteryTimeRemaining(batteryRuntimeMinutesRemaining_);
  }

  const uint32_t estimatedMinutes =
      (static_cast<uint32_t>(batteryDisplayedPercent_) * kNominalBatteryRuntimeMinutes) / 100UL;
  return formatBatteryTimeRemaining(estimatedMinutes);
}

String App::batteryVoltageLabel() const { return String(batteryFilteredVoltage_, 2) + "V"; }

String App::formatBatteryTimeRemaining(uint32_t minutes) const {
  if (minutes < 1) {
    return "0m";
  }

  if (minutes < 60) {
    return String(minutes) + "m";
  }

  const uint32_t hours = minutes / 60;
  const uint32_t remainder = minutes % 60;
  if (hours >= 10 || remainder < 10) {
    return String(hours) + "h";
  }

  return String(hours) + "h" + String(remainder / 10) + "0";
}

uint32_t App::estimatedReadingTimeRemainingMs(size_t startIndex, size_t endIndex) const {
  const size_t wordCount = reader_.wordCount();
  if (wordCount == 0 || reader_.wpm() == 0) {
    return 0;
  }

  startIndex = std::min(startIndex, wordCount);
  endIndex = std::min(endIndex, wordCount);
  if (endIndex <= startIndex) {
    return 0;
  }

  const uint32_t baseMs = static_cast<uint32_t>(
      (static_cast<uint64_t>(endIndex - startIndex) * 60000ULL) /
      static_cast<uint64_t>(reader_.wpm()));

  if (!accurateTimeEstimateEnabled_ || !timeEstimateCacheValid_) {
    return baseMs;
  }

  return baseMs + estimatedPacingBonusMs(startIndex, endIndex);
}

uint32_t App::estimatedPacingBonusMs(size_t startIndex, size_t endIndex) const {
  if (!timeEstimateCacheValid_ || wordBonusBlockPrefixSumMs_.empty() ||
      endIndex <= startIndex) {
    return 0;
  }

  const size_t wordCount = reader_.wordCount();
  startIndex = std::min(startIndex, wordCount);
  endIndex = std::min(endIndex, wordCount);
  if (endIndex <= startIndex) {
    return 0;
  }

  const size_t firstFullBlock = (startIndex + kTimeEstimateBlockWords - 1) /
                                kTimeEstimateBlockWords;
  const size_t lastFullBlockEnd = endIndex / kTimeEstimateBlockWords;
  uint32_t bonusMs = 0;

  if (firstFullBlock < lastFullBlockEnd &&
      lastFullBlockEnd < wordBonusBlockPrefixSumMs_.size()) {
    const size_t startPartialEnd =
        std::min(endIndex, firstFullBlock * kTimeEstimateBlockWords);
    for (size_t i = startIndex; i < startPartialEnd; ++i) {
      bonusMs += reader_.wordPacingBonusMsAt(i);
    }

    bonusMs += wordBonusBlockPrefixSumMs_[lastFullBlockEnd] -
               wordBonusBlockPrefixSumMs_[firstFullBlock];

    const size_t endPartialStart = lastFullBlockEnd * kTimeEstimateBlockWords;
    for (size_t i = endPartialStart; i < endIndex; ++i) {
      bonusMs += reader_.wordPacingBonusMsAt(i);
    }
    return bonusMs;
  }

  for (size_t i = startIndex; i < endIndex; ++i) {
    bonusMs += reader_.wordPacingBonusMsAt(i);
  }
  return bonusMs;
}

void App::invalidateTimeEstimateCache() {
  cancelTimeEstimateBuild();
  timeEstimateCacheValid_ = false;
  std::vector<uint32_t>().swap(wordBonusBlockPrefixSumMs_);
}

void App::rebuildTimeEstimateCache() {
  invalidateTimeEstimateCache();
  pacingCacheDirty_ = false;
  if (!accurateTimeEstimateEnabled_) {
    if (!currentBookTitle_.isEmpty()) {
      renderStorageStatus("Reading time", currentBookTitle_.c_str(), "Fast estimate enabled",
                          100);
    }
    return;
  }

  const size_t n = reader_.wordCount();
  if (n == 0) {
    return;
  }

  const String label = currentBookTitle_.isEmpty() ? String("Current book") : currentBookTitle_;
  timeEstimateBuildWordCount_ = n;
  timeEstimateBuildBlockCount_ =
      (timeEstimateBuildWordCount_ + kTimeEstimateBlockWords - 1) / kTimeEstimateBlockWords;
  if (timeEstimateBuildBlockCount_ == 0) {
    return;
  }

  wordBonusBlockPrefixSumMs_.assign(timeEstimateBuildBlockCount_ + 1, 0);
  timeEstimateBuildBookPath_ = currentBookPath_;
  timeEstimateBuildNextBlock_ = 0;
  timeEstimateBuildRunningMs_ = 0;
  timeEstimateBuildStartedMs_ = millis();
  timeEstimateBuildLastLogMs_ = timeEstimateBuildStartedMs_;
  timeEstimateBuildInProgress_ = true;

  const String detail = String(static_cast<unsigned int>(n)) + " words in background";
  renderStorageStatus("Reading time", label.c_str(), detail.c_str(), 0);
  Serial.printf("[time-est] background build started words=%u blocks=%u book=%s\n",
                static_cast<unsigned int>(timeEstimateBuildWordCount_),
                static_cast<unsigned int>(timeEstimateBuildBlockCount_),
                currentBookPath_.c_str());
}

void App::cancelTimeEstimateBuild() {
  timeEstimateBuildInProgress_ = false;
  timeEstimateBuildBookPath_ = "";
  timeEstimateBuildWordCount_ = 0;
  timeEstimateBuildBlockCount_ = 0;
  timeEstimateBuildNextBlock_ = 0;
  timeEstimateBuildRunningMs_ = 0;
  timeEstimateBuildStartedMs_ = 0;
  timeEstimateBuildLastLogMs_ = 0;
}

bool App::timeEstimateBuildMatchesCurrentBook() const {
  return timeEstimateBuildInProgress_ && timeEstimateBuildBookPath_ == currentBookPath_ &&
         timeEstimateBuildWordCount_ == reader_.wordCount();
}

void App::updateTimeEstimateBuild(uint32_t nowMs) {
  if (!timeEstimateBuildInProgress_) {
    return;
  }

  if (!accurateTimeEstimateEnabled_ || !timeEstimateBuildMatchesCurrentBook()) {
    Serial.println("[time-est] background build cancelled");
    invalidateTimeEstimateCache();
    return;
  }

  if (state_ == AppState::Playing || state_ == AppState::CompanionSync ||
      state_ == AppState::UsbTransfer || state_ == AppState::Standby ||
      state_ == AppState::Sleeping) {
    return;
  }

  size_t processedBlocks = 0;
  while (timeEstimateBuildNextBlock_ < timeEstimateBuildBlockCount_ &&
         processedBlocks < kTimeEstimateBlocksPerUpdate) {
    const size_t block = timeEstimateBuildNextBlock_;
    wordBonusBlockPrefixSumMs_[block] = timeEstimateBuildRunningMs_;
    const size_t blockStart = block * kTimeEstimateBlockWords;
    const size_t blockEnd =
        std::min(timeEstimateBuildWordCount_, blockStart + kTimeEstimateBlockWords);
    for (size_t i = blockStart; i < blockEnd; ++i) {
      timeEstimateBuildRunningMs_ += reader_.wordPacingBonusMsAt(i);
    }
    ++timeEstimateBuildNextBlock_;
    ++processedBlocks;
    delay(0);
  }

  if (timeEstimateBuildNextBlock_ >= timeEstimateBuildBlockCount_) {
    wordBonusBlockPrefixSumMs_[timeEstimateBuildBlockCount_] = timeEstimateBuildRunningMs_;
    timeEstimateCacheValid_ = true;
    const uint32_t elapsedMs = millis() - timeEstimateBuildStartedMs_;
    Serial.printf("[time-est] background cached %u words in %u blocks bonus=%lums took=%lums\n",
                  static_cast<unsigned int>(timeEstimateBuildWordCount_),
                  static_cast<unsigned int>(timeEstimateBuildBlockCount_),
                  static_cast<unsigned long>(timeEstimateBuildRunningMs_),
                  static_cast<unsigned long>(elapsedMs));
    cancelTimeEstimateBuild();
    if (state_ == AppState::Paused || state_ == AppState::Playing) {
      renderActiveReader(nowMs);
    } else if (state_ == AppState::Menu) {
      renderMenu();
    }
    return;
  }

  if (nowMs - timeEstimateBuildLastLogMs_ >= kTimeEstimateProgressLogMs) {
    const int progress =
        static_cast<int>((timeEstimateBuildNextBlock_ * 100UL) /
                         std::max<size_t>(1, timeEstimateBuildBlockCount_));
    Serial.printf("[time-est] background progress %u/%u blocks (%d%%)\n",
                  static_cast<unsigned int>(timeEstimateBuildNextBlock_),
                  static_cast<unsigned int>(timeEstimateBuildBlockCount_), progress);
    timeEstimateBuildLastLogMs_ = nowMs;
    if (state_ == AppState::Paused) {
      renderActiveReader(nowMs);
    }
  }
}

String App::timeEstimateModeLabel() const {
  return uiText(accurateTimeEstimateEnabled_ ? UiText::TimeEstimateAccurate
                                             : UiText::TimeEstimateFast);
}

String App::formatReadingTimeRemaining(uint32_t remainingMs) const {
  const uint32_t totalSeconds = remainingMs / 1000UL;
  if (totalSeconds < 60UL) {
    return "0m";
  }

  const uint32_t totalMinutes = totalSeconds / 60UL;
  if (totalMinutes < 60UL) {
    return String(totalMinutes) + "m";
  }

  const uint32_t totalHours = totalMinutes / 60UL;
  const uint32_t minutes = totalMinutes % 60UL;
  if (totalHours < 24UL) {
    if (minutes == 0) {
      return String(totalHours) + "h";
    }
    return String(totalHours) + "h" + String(minutes) + "m";
  }

  const uint32_t days = totalHours / 24UL;
  const uint32_t hours = totalHours % 24UL;
  if (hours == 0) {
    return String(days) + "d";
  }
  return String(days) + "d" + String(hours) + "h";
}

String App::savePointDefaultName() const {
  const size_t count = reader_.wordCount();
  const size_t index = count > 1 ? std::min(reader_.currentIndex(), count - 1) : 0;
  const float percent = count > 1 ? (100.0f * static_cast<float>(index)) /
                                        static_cast<float>(count - 1)
                                  : 0.0f;
  char percentBuf[8];
  snprintf(percentBuf, sizeof(percentBuf), "%.1f%%", percent);

  const String chapter = currentChapterLabel();
  const String label = (chapter.isEmpty() || chapter == currentBookTitle_)
                            ? currentBookTitle_.substring(0, 16)
                            : chapter.substring(0, 20);
  return String(percentBuf) + " " + label;
}

uint8_t App::readingProgressPercent() const {
  const size_t count = reader_.wordCount();
  if (count <= 1) {
    return 0;
  }

  const size_t index = std::min(reader_.currentIndex(), count - 1);
  const size_t percent = (index * 100UL) / (count - 1);
  return static_cast<uint8_t>(std::min(static_cast<size_t>(100), percent));
}

void App::applyUiOrientation(BoardConfig::UiOrientation orientation) {
  touch_.setUiOrientation(orientation);
  display_.setUiOrientation(orientation);
}

void App::applyReaderUiOrientation() {
  applyUiOrientation(readerUiOrientation());
}

BoardConfig::UiOrientation App::readerUiOrientation() const {
  return uiRotated180() ? BoardConfig::UiOrientation::LandscapeFlipped
                        : BoardConfig::UiOrientation::Landscape;
}

bool App::scrollModeEnabled() const { return readerMode_ == ReaderMode::Scroll; }

bool App::uiRotated180() const {
  // Always keep the same screen orientation regardless of handedness.
  // Left/right hand setting only affects touch zones and anchor offset,
  // not physical screen rotation.
  return BoardConfig::UI_ROTATED_180;
}

uint8_t App::effectiveAnchorPercent() const {
  return handednessMode_ == HandednessMode::Left
             ? static_cast<uint8_t>(typographyConfig_.anchorPercent + kLeftHandAnchorOffset)
             : typographyConfig_.anchorPercent;
}

DisplayManager::TypographyConfig App::effectiveTypographyConfig() const {
  DisplayManager::TypographyConfig config = typographyConfig_;
  config.anchorPercent = effectiveAnchorPercent();
  return config;
}

uint32_t App::currentReaderContentToken() const {
  return hashBookPath(currentBookPath_.isEmpty() ? String("__demo__") : currentBookPath_);
}

size_t App::phantomBeforeCharTarget() const {
  uint8_t levelIndex = readerFontSizeIndex_;
  if (levelIndex >= kReaderFontSizeCount) {
    levelIndex = 0;
  }
  return kPhantomBeforeCharTargets[levelIndex];
}

size_t App::phantomAfterCharTarget() const {
  uint8_t levelIndex = readerFontSizeIndex_;
  if (levelIndex >= kReaderFontSizeCount) {
    levelIndex = 0;
  }
  return kPhantomAfterCharTargets[levelIndex];
}

String App::collectPhantomBeforeText(size_t currentIndex, size_t charTarget) const {
  if (currentIndex == 0 || charTarget == 0) {
    return "";
  }

  size_t startIndex = currentIndex;
  size_t totalChars = 0;
  while (startIndex > 0 && totalChars < charTarget) {
    --startIndex;
    const String word = reader_.wordAt(startIndex);
    totalChars += word.length();
    if (startIndex + 1 < currentIndex) {
      ++totalChars;
    }
  }

  String text;
  for (size_t index = startIndex; index < currentIndex; ++index) {
    if (!text.isEmpty()) {
      text += ' ';
    }
    text += reader_.wordAt(index);
  }
  return text;
}

String App::collectPhantomAfterText(size_t currentIndex, size_t charTarget) const {
  const size_t wordCount = reader_.wordCount();
  if (wordCount == 0 || currentIndex + 1 >= wordCount || charTarget == 0) {
    return "";
  }

  size_t endIndex = currentIndex + 1;
  size_t totalChars = 0;
  while (endIndex < wordCount && totalChars < charTarget) {
    const String word = reader_.wordAt(endIndex);
    totalChars += word.length();
    if (endIndex > currentIndex + 1) {
      ++totalChars;
    }
    ++endIndex;
  }

  String text;
  for (size_t index = currentIndex + 1; index < endIndex; ++index) {
    if (!text.isEmpty()) {
      text += ' ';
    }
    text += reader_.wordAt(index);
  }
  return text;
}

String App::phantomBeforeText() const {
  const size_t wordCount = reader_.wordCount();
  if (wordCount == 0) {
    return "";
  }

  const size_t currentIndex = std::min(reader_.currentIndex(), wordCount - 1);
  return collectPhantomBeforeText(currentIndex, phantomBeforeCharTarget());
}

String App::phantomAfterText() const {
  const size_t wordCount = reader_.wordCount();
  if (wordCount == 0) {
    return "";
  }

  const size_t currentIndex = std::min(reader_.currentIndex(), wordCount - 1);
  return collectPhantomAfterText(currentIndex, phantomAfterCharTarget());
}

void App::renderActiveReader(uint32_t nowMs) {
  if (pendingBootBookLoad_) {
    display_.renderStatus(tr(TrKey::LoadingBook), currentBookTitle_,
                        tr(TrKey::PleaseWait));
    return;
  }

  if (!ensureCurrentBookWordAvailable(nowMs)) {
    return;
  }

  if (chapterTransitionVisible_) {
    renderChapterTransition();
    return;
  }

  applyReaderUiOrientation();
  if (scrollModeEnabled()) {
    if (wpmFeedbackVisible_) {
      renderScrollReader(nowMs, String(reader_.wpm()) + " WPM");
    } else {
      renderScrollReader(nowMs);
    }
    return;
  }

  if (contextViewVisible_) {
    renderContextPreview();
  } else if (wpmFeedbackVisible_) {
    renderWpmFeedback(nowMs);
  } else {
    renderReaderWord();
  }
}

bool App::ensureCurrentBookWordAvailable(uint32_t nowMs) {
  if (!usingStorageBook_ || reader_.wordCount() == 0 || !reader_.currentWord().isEmpty()) {
    return true;
  }

  handleCurrentBookReadFailure(nowMs, "Word cache unreadable");
  return false;
}

void App::handleCurrentBookReadFailure(uint32_t nowMs, const char *detail) {
  const String failedTitle = currentBookTitle_.isEmpty() ? String("Current book") : currentBookTitle_;
  const String failedPath = currentBookPath_;
  const bool articlesOnly =
      currentBookIndex_ < storage_.bookCount() && storage_.bookIsArticle(currentBookIndex_);

  Serial.printf("[app] active book read failed word=%u book=%s detail=%s\n",
                static_cast<unsigned int>(reader_.currentIndex()), failedPath.c_str(),
                detail == nullptr ? "" : detail);

  saveReadingPosition(true);
  activeBookStore_.close();
  reader_.clearLoadedBook(nowMs);
  chapterMarkers_.clear();
  paragraphStarts_.clear();
  currentBookPath_ = "";
  currentBookTitle_ = "Demo";
  usingStorageBook_ = false;
  contextViewVisible_ = false;
  wpmFeedbackVisible_ = false;
  invalidateContextPreviewWindow();
  invalidateTimeEstimateCache();

  setState(AppState::Menu, nowMs);
  display_.renderStatus("Book read failed", failedTitle,
                        detail == nullptr ? "Reopen from library" : detail);
  delay(1800);
  openBookPicker(articlesOnly);
}

void App::renderReaderWord() {
  applyReaderUiOrientation();
  contextViewVisible_ = false;
  const String beforeText = phantomWordsEnabled_ ? phantomBeforeText() : "";
  const String afterText = phantomWordsEnabled_ ? phantomAfterText() : "";
  const DisplayManager::ReaderChrome chrome = readerChrome();
  const bool showReaderFooter = readerFooterVisible();
  const String footerMetricLabel = readerFooterStatusLabel();
  display_.renderPhantomRsvpWord(beforeText, reader_.currentWord(), afterText,
                                 readerFontSizeIndex_, currentChapterLabel(),
                                 readingProgressPercent(), showReaderFooter, footerMetricLabel,
                                 chrome);
}

bool App::isParagraphStart(size_t wordIndex) const {
  if (wordIndex == 0) {
    return true;
  }

  return std::binary_search(paragraphStarts_.begin(), paragraphStarts_.end(), wordIndex);
}

size_t App::paragraphStartAtOrBefore(size_t wordIndex) const {
  if (wordIndex == 0 || paragraphStarts_.empty()) {
    return 0;
  }

  const auto it = std::upper_bound(paragraphStarts_.begin(), paragraphStarts_.end(), wordIndex);
  if (it == paragraphStarts_.begin()) {
    return 0;
  }

  return *std::prev(it);
}

size_t App::contextPreviewAnchorIndex(size_t currentIndex) const {
  if (currentIndex <= kContextPreviewAnchorLeadWords) {
    return 0;
  }

  const size_t anchorTarget = currentIndex - kContextPreviewAnchorLeadWords;
  const size_t paragraphStart = paragraphStartAtOrBefore(anchorTarget);
  if (anchorTarget - paragraphStart <= kContextPreviewMaxParagraphSnapWords) {
    return paragraphStart;
  }

  return anchorTarget;
}

void App::updateContextPreviewWindow(size_t currentIndex) {
  const size_t wordCount = reader_.wordCount();
  if (wordCount == 0) {
    contextPreviewWords_.clear();
    contextPreviewWindowValid_ = false;
    contextPreviewCurrentLocalIndex_ = static_cast<size_t>(-1);
    return;
  }

  size_t startIndex = contextPreviewStartIndex_;
  size_t endIndex = 0;
  bool rebuildWindow = !contextPreviewWindowValid_ || contextPreviewWords_.empty();
  if (!rebuildWindow) {
    endIndex = std::min(wordCount, startIndex + kContextPreviewWindowWords);
    rebuildWindow = currentIndex < startIndex || currentIndex >= endIndex ||
                    (currentIndex + 1 >= endIndex && endIndex < wordCount);
  }

  if (rebuildWindow) {
    startIndex = contextPreviewAnchorIndex(currentIndex);
    endIndex = std::min(wordCount, startIndex + kContextPreviewWindowWords);
    contextPreviewStartIndex_ = startIndex;
    contextPreviewWords_.clear();
    contextPreviewWords_.reserve(endIndex - startIndex);
    bool anyWordLoadFailed = false;
    for (size_t index = startIndex; index < endIndex; ++index) {
      DisplayManager::ContextWord word;
      word.text = reader_.wordAt(index);
      if (word.text.isEmpty()) {
        // A real empty word shouldn't occur in a valid word index — treat it
        // as a transient SD read glitch (IndexedBookStore::wordAt() failing
        // mid-window) rather than genuine content, so the next tick retries
        // instead of freezing a blank window in as "valid".
        anyWordLoadFailed = true;
      }
      word.paragraphStart = isParagraphStart(index);
      word.current = index == currentIndex;
      contextPreviewWords_.push_back(word);
    }
    // Leave contextPreviewWindowValid_ false on a failed load so the very
    // next call retries the SD read instead of locking in a blank window
    // (see DisplayManager::renderScrollView's renderKey word fingerprint for
    // why a later successful retry must still differ from this one).
    contextPreviewWindowValid_ = !anyWordLoadFailed;
    contextPreviewCurrentLocalIndex_ =
        currentIndex >= startIndex ? currentIndex - startIndex : static_cast<size_t>(-1);
    return;
  }

  const size_t nextLocalIndex = currentIndex - startIndex;
  if (contextPreviewCurrentLocalIndex_ < contextPreviewWords_.size()) {
    contextPreviewWords_[contextPreviewCurrentLocalIndex_].current = false;
  }
  if (nextLocalIndex < contextPreviewWords_.size()) {
    contextPreviewWords_[nextLocalIndex].current = true;
    contextPreviewCurrentLocalIndex_ = nextLocalIndex;
  } else {
    contextPreviewCurrentLocalIndex_ = static_cast<size_t>(-1);
  }
}

void App::invalidateContextPreviewWindow() {
  contextPreviewWindowValid_ = false;
  contextPreviewWords_.clear();
  contextPreviewCurrentLocalIndex_ = static_cast<size_t>(-1);
}

void App::renderContextPreview() {
  applyReaderUiOrientation();
  const size_t wordCount = reader_.wordCount();
  if (wordCount == 0) {
    renderReaderWord();
    return;
  }

  const size_t currentIndex = std::min(reader_.currentIndex(), wordCount - 1);
  updateContextPreviewWindow(currentIndex);

  contextViewVisible_ = true;
  const DisplayManager::ReaderChrome chrome = readerChrome();
  display_.renderScrollView(contextPreviewWords_, currentReaderContentToken(),
                            contextPreviewStartIndex_, currentIndex, 0,
                            currentChapterLabel(), readingProgressPercent(), "",
                            readerFooterStatusLabel(), chrome);
}

void App::renderScrollReader(uint32_t nowMs, const String &overlayText) {
  applyReaderUiOrientation();
  contextViewVisible_ = false;
  const size_t wordCount = reader_.wordCount();
  if (wordCount == 0) {
    renderReaderWord();
    return;
  }

  const size_t currentIndex = std::min(reader_.currentIndex(), wordCount - 1);
  updateContextPreviewWindow(currentIndex);

  uint16_t scrollProgressPermille = 0;
  if (state_ == AppState::Playing && currentIndex + 1 < wordCount) {
    const uint32_t durationMs = reader_.currentWordDurationMs();
    if (durationMs > 0) {
      const uint32_t elapsedMs = reader_.elapsedInCurrentWordMs(nowMs);
      scrollProgressPermille = static_cast<uint16_t>(
          std::min<uint32_t>(1000UL, (elapsedMs * 1000UL) / durationMs));
    }
  }

  const DisplayManager::ReaderChrome chrome = readerChrome();
  display_.renderScrollView(contextPreviewWords_, currentReaderContentToken(),
                            contextPreviewStartIndex_, currentIndex, scrollProgressPermille,
                            currentChapterLabel(), readingProgressPercent(), overlayText,
                            readerFooterStatusLabel(), chrome);
}

void App::renderWpmFeedback(uint32_t nowMs) {
  if (!ensureCurrentBookWordAvailable(nowMs)) {
    return;
  }

  applyReaderUiOrientation();
  wpmFeedbackVisible_ = true;
  wpmFeedbackUntilMs_ = nowMs + kWpmFeedbackMs;
  if (scrollModeEnabled()) {
    renderScrollReader(nowMs, String(reader_.wpm()) + " WPM");
    return;
  }

  contextViewVisible_ = false;
  const String beforeText = phantomWordsEnabled_ ? phantomBeforeText() : "";
  const String afterText = phantomWordsEnabled_ ? phantomAfterText() : "";
  const DisplayManager::ReaderChrome chrome = readerChrome();
  const String footerMetricLabel = readerFooterStatusLabel();
  display_.renderPhantomRsvpWordWithWpm(beforeText, reader_.currentWord(), afterText,
                                        readerFontSizeIndex_, reader_.wpm(),
                                        currentChapterLabel(), readingProgressPercent(),
                                        readerFooterVisible(), footerMetricLabel, chrome);
}

void App::renderStorageStatus(const char *title, const char *line1, const char *line2,
                              int progressPercent) {
  if (suppressBootStorageStatusRender_) {
    return;
  }
  applyReaderUiOrientation();
  display_.renderProgress(title == nullptr ? "SD" : title, line1 == nullptr ? "" : line1,
                          line2 == nullptr ? "" : line2, progressPercent);
}

void App::handleStorageStatus(void *context, const char *title, const char *line1,
                              const char *line2, int progressPercent) {
  if (context == nullptr) {
    return;
  }

  static_cast<App *>(context)->renderStorageStatus(title, line1, line2, progressPercent);
  delay(0);
}
