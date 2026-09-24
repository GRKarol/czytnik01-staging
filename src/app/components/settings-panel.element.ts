import { LitElement, css, html, svg } from "lit";
import { customElement, state } from "lit/decorators.js";
import {
  deviceApi,
  onDeviceApiChange,
  type DeviceSettings,
  type Theme,
  type Language,
  type ReaderHand,
  type ReaderMode,
  type PauseBehaviour,
  type Typeface,
  type FooterMetric,
  type BatteryLabel,
  type WifiStationConfig,
  type DeviceInfo,
} from "../device/api";
import { setLang } from "../i18n/index";
import { deviceLangToSupported } from "../i18n/lang-map";
import "./help-panel.element";
import "./setting-tooltip.element";

const THEME_LABEL: Record<Theme, string> = {
  light: "Jasny",
  dark: "Ciemny",
  night: "Nocny",
};
const LANG_LABEL: Record<Language, string> = {
  pl: "Polski",
  en: "English",
  de: "Deutsch",
  es: "Español",
  fr: "Français",
  it: "Italiano",
};
const HAND_LABEL: Record<ReaderHand, string> = { right: "Prawa", left: "Lewa" };
const MODE_LABEL: Record<ReaderMode, string> = { rsvp: "RSVP", scroll: "Przewijanie" };
const PAUSE_LABEL: Record<PauseBehaviour, string> = {
  tap: "Tap",
  "long-press": "Przytrzymanie",
  auto: "Auto",
};

const FONT_SIZE_LABEL: Record<number, string> = {
  0: "0",
  1: "1",
  2: "2",
  3: "3",
  4: "4",
  5: "5",
  6: "6",
  7: "7",
  8: "8",
};

const LINE_SPACING_LABEL: Record<number, string> = {
  0: "Compact",
  1: "Normal",
  2: "Relaxed",
};

const MARGIN_LABEL: Record<number, string> = {
  0: "Narrow",
  1: "Normal",
  2: "Wide",
};

const TYPEFACE_LABEL: Record<Typeface, string> = {
  standard: "Standard",
  open_dyslexic: "OpenDyslexic",
  atkinson: "Atkinson",
};

const RSVP_FONT_SIZE_LABEL: Record<number, string> = {
  0: "S",
  1: "M",
  2: "L",
};

const FOOTER_METRIC_LABEL: Record<FooterMetric, string> = {
  percentage: "Procent",
  chapter_time: "Czas rozdziału",
  book_time: "Czas książki",
};

const BATTERY_LABEL_LABEL: Record<BatteryLabel, string> = {
  percent: "Procent",
  time_remaining: "Czas",
  voltage: "Napięcie",
};

type SettingsSubView = "settings" | "help";

const ico = (d: string) => svg`
  <svg width="15" height="15" viewBox="0 0 24 24" fill="none"
       stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
    <path d=${d}/>
  </svg>
`;
const icoBook = svg`
  <svg width="15" height="15" viewBox="0 0 24 24" fill="none"
       stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
    <path d="M4 4h6a4 4 0 0 1 4 4v12a3 3 0 0 0-3-3H4z"/>
    <path d="M20 4h-6a4 4 0 0 0-4 4v12a3 3 0 0 1 3-3h7z"/>
  </svg>
`;
const icoGauge = svg`
  <svg width="15" height="15" viewBox="0 0 24 24" fill="none"
       stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
    <path d="M12 12l4-4"/>
    <path d="M3 12a9 9 0 1 1 18 0"/>
  </svg>
`;
const icoType = ico("M5 4h14M12 4v16M9 20h6");
const icoSun = svg`
  <svg width="15" height="15" viewBox="0 0 24 24" fill="none"
       stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
    <circle cx="12" cy="12" r="4"/>
    <path d="M12 2v2M12 20v2M4 12H2M22 12h-2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>
  </svg>
`;
const icoActivity = ico("M3 12h4l2 8 4-16 2 8h6");
const icoGlobe = svg`
  <svg width="15" height="15" viewBox="0 0 24 24" fill="none"
       stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
    <circle cx="12" cy="12" r="9"/>
    <path d="M3 12h18M12 3a14 14 0 0 1 0 18a14 14 0 0 1 0-18"/>
  </svg>
`;
const icoCode = ico("M8 5L3 12l5 7M16 5l5 7-5 7");
const icoWifi = svg`
  <svg width="15" height="15" viewBox="0 0 24 24" fill="none"
       stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
    <path d="M2 8.5a17 17 0 0 1 20 0"/>
    <path d="M5 12a13 13 0 0 1 14 0"/>
    <path d="M8.5 15.5a8 8 0 0 1 7 0"/>
    <circle cx="12" cy="19" r="1.2" fill="currentColor"/>
  </svg>
`;

function legend(icon: unknown, text: string) {
  return html`<span class="legend-ico">${icon}</span><span class="legend-text">${text}</span>`;
}

@customElement("settings-panel")
export class SettingsPanel extends LitElement {
  @state() private settings: DeviceSettings | null = null;
  @state() private saving = false;
  @state() private error = "";
  @state() private tapCount = 0;
  @state() private justUnlocked = false;
  @state() private subView: SettingsSubView = "settings";
  private tapResetTimer: number | null = null;
  private unsubApi: (() => void) | null = null;

  // ─── Sieć: stacja WiFi + auto-off ────────────────────────────────────────
  @state() private wifi: WifiStationConfig | null = null;
  @state() private wifiSsidInput = "";
  @state() private wifiPasswordInput = "";
  @state() private wifiBusy = false;
  @state() private wifiError = "";
  @state() private wifiTimeoutMinutes = 0;

  // ─── Diagnostyka + logi (developer) ──────────────────────────────────────
  @state() private deviceInfo: DeviceInfo | null = null;
  @state() private logLines: string[] = [];
  @state() private showLogs = false;
  @state() private diagBusy = false;

  private get effectiveMode(): ReaderMode {
    const m = this.settings?.readerMode;
    return m === "scroll" ? "scroll" : "rsvp";
  }

  connectedCallback(): void {
    super.connectedCallback();
    void this.load();
    void this.loadNetwork();
    this.unsubApi = onDeviceApiChange(() => {
      void this.load();
      void this.loadNetwork();
    });
  }

  disconnectedCallback(): void {
    super.disconnectedCallback();
    if (this.tapResetTimer) window.clearTimeout(this.tapResetTimer);
    this.unsubApi?.();
  }

  render() {
    if (this.subView === "help") {
      return html`
        <help-panel
          @help-close=${this.handleHelpClose}
          @restart-tutorial=${this.handleRestartTutorial}
        ></help-panel>
      `;
    }

    if (!this.settings) {
      return html`<p class="muted">Wczytuję ustawienia z urządzenia…</p>`;
    }
    const s = this.settings;
    return html`
      <div class="brand" @click=${this.onBrandTap}>
        <strong>Flower</strong>
        <span>Ustawienia urządzenia</span>
        ${this.tapCount > 0 && this.tapCount < 10 && !s.devMode
          ? html`<small class="tap-hint">${10 - this.tapCount} aby odblokować…</small>`
          : ""}
        ${this.justUnlocked
          ? html`<small class="tap-hint ok">Tryb developera włączony</small>`
          : ""}
      </div>

      ${this.error ? html`<p class="error">${this.error}</p>` : ""}

      <fieldset class="group">
        <legend>${legend(icoBook, "Tryb czytania")}</legend>
        ${this.segmented(
          "readerMode",
          s.readerMode,
          ["rsvp", "scroll"],
          MODE_LABEL,
          "Tryb",
          "readingMode",
        )}
      </fieldset>

      ${this.effectiveMode === "rsvp"
        ? html`
            <fieldset class="group">
              <legend>${legend(icoGauge, "Ustawienia RSVP")}</legend>
              ${this.segmented(
                "pauseBehaviour",
                s.pauseBehaviour,
                ["tap", "long-press", "auto"],
                PAUSE_LABEL,
                "Pauza",
                "pauseBehaviour",
              )}
              ${this.slider("baseWpm", "Tempo", s.baseWpm, 50, 1000, 25, "WPM", "baseWpm")}
              ${this.slider(
                "longWordDelayMs",
                "Długie słowa",
                s.longWordDelayMs,
                0,
                600,
                50,
                "ms",
                "longWordDelay",
              )}
              ${this.slider(
                "complexWordDelayMs",
                "Złożone słowa",
                s.complexWordDelayMs,
                0,
                600,
                50,
                "ms",
                "complexWordDelay",
              )}
              ${this.slider(
                "punctuationDelayMs",
                "Interpunkcja",
                s.punctuationDelayMs,
                0,
                600,
                50,
                "ms",
                "punctuationDelay",
              )}
              ${this.toggle("phantomWords", "Słowa widma", s.phantomWords)}
            </fieldset>

            <fieldset class="group">
              <legend>${legend(icoType, "Typografia RSVP")}</legend>
              ${this.segmented(
                "fontSizeIndex",
                s.fontSizeIndex,
                [0, 1, 2],
                RSVP_FONT_SIZE_LABEL,
                "Rozmiar czcionki",
              )}
              ${this.segmented(
                "typeface",
                s.typeface,
                ["standard", "open_dyslexic", "atkinson"],
                TYPEFACE_LABEL,
                "Krój czcionki",
              )}
              ${this.toggle("focusHighlight", "Podświetlenie fokusowe", s.focusHighlight)}
              ${this.slider("tracking", "Tracking (odstępy)", s.tracking, -2, 3, 1, "")}
              ${this.slider("anchorPercent", "Pozycja kotwicy", s.anchorPercent, 30, 40, 1, "%")}
              ${this.slider("guideWidth", "Szerokość prowadnicy", s.guideWidth, 12, 30, 1, "px")}
              ${this.slider("guideGap", "Przerwa prowadnicy", s.guideGap, 2, 8, 1, "px")}
            </fieldset>
          `
        : html`
            <fieldset class="group">
              <legend>${legend(icoType, "Ustawienia Scroll")}</legend>
              ${this.segmented(
                "scrollFontSize",
                s.scrollFontSize,
                [0, 1, 2, 3, 4, 5, 6, 7, 8],
                FONT_SIZE_LABEL,
                "Rozmiar czcionki",
              )}
              ${this.segmented(
                "scrollLineSpacing",
                s.scrollLineSpacing,
                [0, 1, 2],
                LINE_SPACING_LABEL,
                "Interlinia",
              )}
              ${this.segmented(
                "scrollMargin",
                s.scrollMargin,
                [0, 1, 2],
                MARGIN_LABEL,
                "Marginesy",
              )}
            </fieldset>
          `}

      <fieldset class="group">
        <legend>${legend(icoSun, "Wyświetlanie")}</legend>
        ${this.segmented(
          "theme",
          s.theme,
          ["light", "dark", "night"],
          THEME_LABEL,
          undefined,
          "theme",
        )}
        ${this.slider("brightness", "Jasność", s.brightness, 10, 100, 5, "%", "brightness")}
        ${this.segmented(
          "readerHand",
          s.readerHand,
          ["right", "left"],
          HAND_LABEL,
          "Dłoń",
          "readerHand",
        )}
      </fieldset>

      <fieldset class="group">
        <legend>${legend(icoActivity, "HUD podczas czytania")}</legend>
        ${this.toggle(
          "showBatteryWhileReading",
          "Bateria",
          s.showBatteryWhileReading,
          "readingBattery",
        )}
        ${this.toggle(
          "showChapterWhileReading",
          "Rozdział",
          s.showChapterWhileReading,
          "readingChapter",
        )}
        ${this.toggle(
          "showPercentWhileReading",
          "Procent",
          s.showPercentWhileReading,
          "readingPercent",
        )}
        ${this.segmented(
          "footerMetric",
          s.footerMetric,
          ["percentage", "chapter_time", "book_time"],
          FOOTER_METRIC_LABEL,
          "Metryka stopki",
        )}
        ${this.segmented(
          "batteryLabel",
          s.batteryLabel,
          ["percent", "time_remaining", "voltage"],
          BATTERY_LABEL_LABEL,
          "Etykieta baterii",
        )}
      </fieldset>

      <fieldset class="group">
        <legend>${legend(icoGlobe, "Język")}</legend>
        <label class="select">
          <span>Język interfejsu</span>
          <select
            @change=${(e: Event) =>
              this.put({ language: (e.target as HTMLSelectElement).value as Language })}
          >
            ${(Object.keys(LANG_LABEL) as Language[]).map(
              (l) =>
                html`<option value=${l} ?selected=${l === s.language}>${LANG_LABEL[l]}</option>`,
            )}
          </select>
        </label>
      </fieldset>

      <fieldset class="group">
        <legend>${legend(icoWifi, "Sieć")}</legend>
        <p class="muted small">
          Podłącz czytnik do domowego WiFi — nie będzie już wymagał trybu AP, żeby zsynchronizować
          się z internetem (aktualizacje, RSS).
        </p>
        ${this.wifi?.configured
          ? html`<p class="muted small">
              Zapisana sieć: <strong>${this.wifi.ssid}</strong>${this.wifi.passwordSet
                ? " (z hasłem)"
                : " (bez hasła)"}
            </p>`
          : html`<p class="muted small">Czytnik nie ma jeszcze zapisanej sieci domowej.</p>`}
        <label class="select">
          <span>SSID</span>
          <input
            type="text"
            .value=${this.wifiSsidInput}
            placeholder="Nazwa sieci WiFi"
            @input=${(e: Event) => (this.wifiSsidInput = (e.target as HTMLInputElement).value)}
          />
        </label>
        <label class="select">
          <span>Hasło</span>
          <input
            type="password"
            .value=${this.wifiPasswordInput}
            placeholder=${this.wifi?.passwordSet ? "•••••••• (zostaw puste bez zmian)" : "Hasło"}
            @input=${(e: Event) =>
              (this.wifiPasswordInput = (e.target as HTMLInputElement).value)}
          />
        </label>
        ${this.wifiError ? html`<p class="error">${this.wifiError}</p>` : ""}
        <div class="wifi-actions">
          <button class="mini-cta" ?disabled=${this.wifiBusy} @click=${this.saveWifiStation}>
            Zapisz sieć
          </button>
          <button
            class="mini-cta ghost"
            ?disabled=${this.wifiBusy || !this.wifi?.configured}
            @click=${this.forgetWifiStation}
          >
            Zapomnij
          </button>
        </div>
        <label class="slider">
          <span
            >Auto-wyłączenie WiFi/AP<small
              >${this.wifiTimeoutMinutes === 0 ? "nigdy" : `${this.wifiTimeoutMinutes} min`}</small
            ></span
          >
          <input
            type="range"
            min="0"
            max="60"
            step="5"
            .value=${String(this.wifiTimeoutMinutes)}
            @change=${(e: Event) =>
              this.setWifiTimeoutMinutes(Number((e.target as HTMLInputElement).value))}
          />
        </label>
      </fieldset>

      ${s.devMode
        ? html`
            <fieldset class="group dev">
              <legend>${legend(icoCode, "Developer")}</legend>
              <p class="muted small">
                Te opcje są ukryte przed klientem. Włączasz je tylko z aplikacji — na samym
                urządzeniu też nic nie widzi, dopóki tu jest „On".
              </p>
              ${this.toggle("devMode", "Tryb developera", s.devMode)}
              <p class="muted small">
                Po wyłączeniu trybu developera advanced ustawienia (OTA owner, Auto OTA, RSS feed
                editor, etc.) znikają zarówno z urządzenia jak i z tej aplikacji.
              </p>

              <div class="dev-block">
                <div class="dev-block-head">
                  <strong>Diagnostyka urządzenia</strong>
                  <button class="mini-cta ghost" ?disabled=${this.diagBusy} @click=${this.refreshDiagnostics}>
                    Odśwież
                  </button>
                </div>
                ${this.deviceInfo
                  ? html`
                      <ul class="diag-list">
                        <li><span>Firmware</span><strong>${this.deviceInfo.firmwareVersion}</strong></li>
                        <li><span>Tryb sieci</span><strong>${this.deviceInfo.mode === "station" ? "Stacja WiFi" : "Access Point"}</strong></li>
                        <li><span>SSID</span><strong>${this.deviceInfo.networkSsid || "—"}</strong></li>
                        <li><span>Bateria</span><strong>${this.deviceInfo.batteryPercent}%</strong></li>
                        <li>
                          <span>Karta SD</span>
                          <strong
                            >${formatKb(this.deviceInfo.sdFreeKb)} wolne /
                            ${formatKb(this.deviceInfo.sdTotalKb)}</strong
                          >
                        </li>
                      </ul>
                    `
                  : html`<p class="muted small">Kliknij „Odśwież", żeby pobrać stan z urządzenia.</p>`}
              </div>

              <div class="dev-block">
                <div class="dev-block-head">
                  <strong>Logi urządzenia</strong>
                  <button class="mini-cta ghost" @click=${this.toggleLogs}>
                    ${this.showLogs ? "Ukryj" : "Pokaż"}
                  </button>
                </div>
                ${this.showLogs
                  ? html`
                      <div class="log-actions">
                        <button class="mini-cta ghost" ?disabled=${this.diagBusy} @click=${this.refreshLogs}>
                          Odśwież
                        </button>
                        <button class="mini-cta ghost" ?disabled=${this.diagBusy} @click=${this.clearLogs}>
                          Wyczyść
                        </button>
                      </div>
                      <pre class="log-view">${this.logLines.length
                        ? this.logLines.join("\n")
                        : "(pusto)"}</pre>
                    `
                  : ""}
              </div>
            </fieldset>
          `
        : ""}

      <button class="help-link" @click=${this.openHelp}>
        <svg
          width="20"
          height="20"
          viewBox="0 0 20 20"
          fill="none"
          stroke="currentColor"
          stroke-width="1.8"
          stroke-linecap="round"
          stroke-linejoin="round"
          aria-hidden="true"
        >
          <circle cx="10" cy="10" r="8" />
          <path d="M7.5 7.5a2.5 2.5 0 0 1 4.5 1.5c0 1.5-2 2-2 3" />
          <circle cx="10" cy="14.5" r="0.5" fill="currentColor" />
        </svg>
        <span>Pomoc / Przewodnik</span>
      </button>

      ${this.saving ? html`<p class="muted small">Zapisuję…</p>` : ""}
    `;
  }

  // ─── help-panel navigation ────────────────────────────────────────────────

  private openHelp(): void {
    this.subView = "help";
  }

  private handleHelpClose(): void {
    this.subView = "settings";
  }

  private handleRestartTutorial(): void {
    this.subView = "settings";
    this.dispatchEvent(new CustomEvent("restart-tutorial", { bubbles: true, composed: true }));
  }

  // ─── helpers UI ───────────────────────────────────────────────────────────

  private toggle(key: keyof DeviceSettings, label: string, value: boolean, tooltipKey?: string) {
    return html`
      <label class="toggle">
        <span class="label-with-tooltip"
          >${label}${tooltipKey
            ? html`<setting-tooltip settingKey=${tooltipKey}></setting-tooltip>`
            : ""}</span
        >
        <input
          type="checkbox"
          ?checked=${value}
          @change=${(e: Event) =>
            this.put({ [key]: (e.target as HTMLInputElement).checked } as Partial<DeviceSettings>)}
        />
      </label>
    `;
  }

  private slider(
    key: keyof DeviceSettings,
    label: string,
    value: number,
    min: number,
    max: number,
    step: number,
    unit: string,
    tooltipKey?: string,
  ) {
    return html`
      <label class="slider">
        <span
          >${label}${tooltipKey
            ? html`<setting-tooltip settingKey=${tooltipKey}></setting-tooltip>`
            : ""}<small>${value} ${unit}</small></span
        >
        <input
          type="range"
          min=${min}
          max=${max}
          step=${step}
          .value=${String(value)}
          @input=${(e: Event) =>
            this.put({
              [key]: Number((e.target as HTMLInputElement).value),
            } as Partial<DeviceSettings>)}
        />
      </label>
    `;
  }

  private segmented<K extends keyof DeviceSettings>(
    key: K,
    current: DeviceSettings[K],
    options: ReadonlyArray<DeviceSettings[K]>,
    labels: Record<string, string>,
    title?: string,
    tooltipKey?: string,
  ) {
    const showHeader = title || tooltipKey;
    return html`
      <label class="seg">
        ${showHeader
          ? html`<span class="label-with-tooltip"
              >${title ?? ""}${tooltipKey
                ? html`<setting-tooltip settingKey=${tooltipKey}></setting-tooltip>`
                : ""}</span
            >`
          : ""}
        <div class="seg-buttons">
          ${options.map(
            (opt) => html`
              <button
                class=${opt === current ? "active" : ""}
                @click=${() => this.put({ [key]: opt } as Partial<DeviceSettings>)}
              >
                ${labels[opt as string]}
              </button>
            `,
          )}
        </div>
      </label>
    `;
  }

  // ─── network ──────────────────────────────────────────────────────────────

  private async load() {
    try {
      this.settings = await deviceApi.getSettings();
      // Sync i18n module with device language on initial load
      if (this.settings) {
        setLang(deviceLangToSupported(this.settings.language));
      }
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
    }
  }

  private async put(patch: Partial<DeviceSettings>) {
    if (!this.settings) return;
    // Optimistic update — natychmiast aktualizuj UI, w razie czego cofnij.
    const previous = this.settings;
    this.settings = { ...previous, ...patch };
    this.saving = true;
    this.error = "";

    // Sync i18n when language changes
    if ("language" in patch && patch.language) {
      setLang(deviceLangToSupported(patch.language));
    }

    try {
      this.settings = await deviceApi.putSettings(patch);
      // Powiedz rodzicowi (app.element.ts) żeby odświeżył DEV badge w header.
      if ("devMode" in patch && previous.devMode !== this.settings.devMode) {
        this.dispatchEvent(
          new CustomEvent("device-settings-changed", {
            bubbles: true,
            composed: true,
            detail: this.settings,
          }),
        );
      }
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
      this.settings = previous;
      // Revert i18n on failure
      if ("language" in patch) {
        setLang(deviceLangToSupported(previous.language));
      }
    } finally {
      this.saving = false;
    }
  }

  // ─── Sieć: stacja WiFi + auto-off ────────────────────────────────────────

  private async loadNetwork(): Promise<void> {
    try {
      this.wifi = await deviceApi.getWifiStation();
      this.wifiSsidInput = this.wifi.ssid;
    } catch {
      /* urządzenie nie odpowiada — zostaw poprzedni stan */
    }
    try {
      this.wifiTimeoutMinutes = Math.round((await deviceApi.setWifiTimeoutSeconds()) / 60);
    } catch {
      /* ignored */
    }
  }

  private saveWifiStation = async () => {
    const ssid = this.wifiSsidInput.trim();
    if (!ssid) {
      this.wifiError = "Podaj nazwę sieci (SSID).";
      return;
    }
    this.wifiBusy = true;
    this.wifiError = "";
    try {
      this.wifi = await deviceApi.setWifiStation(ssid, this.wifiPasswordInput);
      this.wifiPasswordInput = "";
    } catch (err) {
      this.wifiError = err instanceof Error ? err.message : String(err);
    } finally {
      this.wifiBusy = false;
    }
  };

  private forgetWifiStation = async () => {
    this.wifiBusy = true;
    this.wifiError = "";
    try {
      this.wifi = await deviceApi.clearWifiStation();
      this.wifiSsidInput = "";
      this.wifiPasswordInput = "";
    } catch (err) {
      this.wifiError = err instanceof Error ? err.message : String(err);
    } finally {
      this.wifiBusy = false;
    }
  };

  private setWifiTimeoutMinutes = async (minutes: number) => {
    this.wifiTimeoutMinutes = minutes;
    try {
      await deviceApi.setWifiTimeoutSeconds(minutes * 60);
    } catch (err) {
      this.wifiError = err instanceof Error ? err.message : String(err);
    }
  };

  // ─── Diagnostyka + logi (developer) ──────────────────────────────────────

  private refreshDiagnostics = async () => {
    this.diagBusy = true;
    try {
      this.deviceInfo = await deviceApi.getDeviceInfo();
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
    } finally {
      this.diagBusy = false;
    }
  };

  private toggleLogs = async () => {
    this.showLogs = !this.showLogs;
    if (this.showLogs) await this.refreshLogs();
  };

  private refreshLogs = async () => {
    this.diagBusy = true;
    try {
      const tail = await deviceApi.getLogTail(80);
      this.logLines = tail.lines;
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
    } finally {
      this.diagBusy = false;
    }
  };

  private clearLogs = async () => {
    this.diagBusy = true;
    try {
      await deviceApi.clearLog();
      this.logLines = [];
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
    } finally {
      this.diagBusy = false;
    }
  };

  // ─── 10-tap unlock ────────────────────────────────────────────────────────

  private onBrandTap = () => {
    if (!this.settings) return;
    if (this.settings.devMode) return; // już odblokowane

    this.tapCount += 1;
    if (this.tapResetTimer) window.clearTimeout(this.tapResetTimer);
    this.tapResetTimer = window.setTimeout(() => {
      this.tapCount = 0;
    }, 1500);

    if (this.tapCount >= 10) {
      this.tapCount = 0;
      void this.put({ devMode: true });
      this.justUnlocked = true;
      window.setTimeout(() => (this.justUnlocked = false), 3000);
    }
  };

  static styles = css`
    :host {
      display: block;
      display: flex;
      flex-direction: column;
      gap: 14px;
    }
    .muted {
      color: var(--muted);
      margin: 0;
      font: 0.92rem/1.5 var(--ns);
    }
    .small {
      font-size: 0.8rem;
    }
    .error {
      color: var(--err);
      font: 0.9rem var(--ns);
      margin: 0;
    }
    .brand {
      display: flex;
      flex-direction: column;
      gap: 2px;
      padding: 14px;
      border: 1px solid var(--line);
      border-radius: var(--radius, 13px);
      background: var(--paper-tint);
      cursor: pointer;
      user-select: none;
      -webkit-user-select: none;
    }
    .brand strong {
      font-family: var(--fr);
      font-weight: 500;
      font-size: 1.4rem;
      color: var(--accent);
    }
    .brand span {
      font: 0.85rem var(--ns);
      color: var(--muted);
    }
    .tap-hint {
      margin-top: 4px;
      font: 600 0.7rem var(--mn);
      letter-spacing: 0.02em;
      color: var(--accent);
    }
    .tap-hint.ok {
      color: var(--ok);
    }
    fieldset.group {
      margin: 0;
      padding: 16px;
      border: 1px solid var(--line);
      border-radius: var(--radius, 13px);
      background: var(--paper-tint);
      display: flex;
      flex-direction: column;
      gap: 12px;
    }
    fieldset.group.dev {
      border-color: var(--accent);
      background: rgba(46, 142, 255, 0.05);
    }
    legend {
      display: flex;
      align-items: center;
      gap: 7px;
      padding: 0 4px;
    }
    .legend-ico {
      width: 22px;
      height: 22px;
      flex: 0 0 auto;
      display: grid;
      place-items: center;
      border: 1px solid var(--line);
      border-radius: var(--radius-sm, 9px);
      background: var(--sky-2);
      color: var(--accent);
    }
    .legend-text {
      font: 700 0.72rem var(--mn);
      letter-spacing: 0.1em;
      text-transform: uppercase;
      color: var(--green);
    }
    .toggle {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      font: 0.95rem var(--ns);
    }
    .toggle input {
      width: 44px;
      height: 26px;
      appearance: none;
      border-radius: 999px;
      background: var(--line);
      position: relative;
      cursor: pointer;
      transition: background 0.15s;
    }
    .toggle input:checked {
      background: var(--accent);
    }
    .toggle input::before {
      content: "";
      position: absolute;
      top: 3px;
      left: 3px;
      width: 20px;
      height: 20px;
      border-radius: 50%;
      background: #fff;
      transition: transform 0.15s;
    }
    .toggle input:checked::before {
      transform: translateX(18px);
    }
    .slider {
      display: flex;
      flex-direction: column;
      gap: 6px;
      font: 0.95rem var(--ns);
    }
    .slider span {
      display: flex;
      justify-content: space-between;
      align-items: baseline;
    }
    .slider small {
      color: var(--muted);
      font: 0.82rem var(--mn);
    }
    .slider input[type="range"] {
      width: 100%;
      accent-color: var(--accent);
    }
    .seg {
      display: flex;
      flex-direction: column;
      gap: 6px;
      font: 0.95rem var(--ns);
    }
    .seg-buttons {
      display: grid;
      grid-auto-flow: column;
      grid-auto-columns: 1fr;
      gap: 4px;
      border: 1px solid var(--line);
      border-radius: var(--radius-sm, 9px);
      background: transparent;
      overflow: hidden;
    }
    .seg-buttons button {
      padding: 8px 10px;
      border: 0;
      border-right: 1px solid var(--line);
      background: transparent;
      color: var(--ink-soft);
      font: 600 0.78rem var(--mn);
      letter-spacing: 0.02em;
      cursor: pointer;
      transition: background 0.15s ease, color 0.15s ease;
    }
    .seg-buttons button:last-child {
      border-right: 0;
    }
    .seg-buttons button.active {
      background: var(--accent);
      color: #fff;
    }
    .select {
      display: flex;
      flex-direction: column;
      gap: 6px;
      font: 0.95rem var(--ns);
    }
    .select select,
    .select input[type="text"],
    .select input[type="password"] {
      padding: 10px 12px;
      border: 1px solid var(--line);
      border-radius: var(--radius-sm, 9px);
      background: #fff;
      font: 0.95rem var(--ns);
      color: var(--ink);
    }
    .wifi-actions,
    .log-actions {
      display: flex;
      gap: 8px;
    }
    .mini-cta {
      flex: 1 1 auto;
      padding: 9px 14px;
      border: 1px solid var(--accent);
      border-radius: var(--radius-sm, 9px);
      color: #fff;
      background: var(--accent);
      font: 700 0.8rem var(--mn);
      letter-spacing: 0.02em;
      cursor: pointer;
      transition: background 0.15s ease;
    }
    .mini-cta:active:not(:disabled) {
      background: var(--accent-deep);
    }
    .mini-cta:disabled {
      opacity: 0.55;
      cursor: not-allowed;
    }
    .mini-cta.ghost {
      background: transparent;
      color: var(--accent);
      border: 1px solid var(--accent);
    }
    .dev-block {
      display: flex;
      flex-direction: column;
      gap: 8px;
      padding-top: 10px;
      border-top: 1px dashed var(--line);
    }
    .dev-block-head {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 10px;
    }
    .dev-block-head strong {
      font: 700 0.8rem var(--mn);
      color: var(--ink-soft);
    }
    .dev-block-head .mini-cta {
      flex: 0 0 auto;
    }
    .diag-list {
      list-style: none;
      margin: 0;
      padding: 0;
      display: flex;
      flex-direction: column;
      gap: 4px;
      font: 0.8rem var(--mn);
    }
    .diag-list li {
      display: flex;
      justify-content: space-between;
      gap: 10px;
      color: var(--muted);
    }
    .diag-list strong {
      color: var(--ink);
    }
    .log-view {
      margin: 0;
      max-height: 220px;
      overflow-y: auto;
      padding: 10px 12px;
      background: #fff;
      border: 1px solid var(--line);
      border-radius: var(--radius-sm, 9px);
      font: 0.72rem/1.5 var(--mn);
      color: var(--ink-soft);
      white-space: pre-wrap;
      word-break: break-all;
    }
    .help-link {
      display: flex;
      align-items: center;
      gap: 10px;
      padding: 14px;
      border: 1px solid var(--line);
      border-radius: var(--radius, 13px);
      background: var(--paper-tint);
      color: var(--ink);
      font: 600 0.95rem var(--ns);
      cursor: pointer;
      transition: border-color 0.15s;
    }
    .help-link:hover {
      border-color: var(--accent);
    }
    .help-link:active {
      background: var(--sky-2);
    }
    .help-link svg {
      flex-shrink: 0;
      color: var(--accent);
    }
    .label-with-tooltip {
      display: inline-flex;
      align-items: center;
      gap: 6px;
    }
  `;
}

declare global {
  interface HTMLElementTagNameMap {
    "settings-panel": SettingsPanel;
  }
}

function formatKb(kb: number): string {
  if (kb < 1024) return `${kb} kB`;
  if (kb < 1024 * 1024) return `${(kb / 1024).toFixed(1)} MB`;
  return `${(kb / 1024 / 1024).toFixed(2)} GB`;
}
