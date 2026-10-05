import { LitElement, css, html, nothing, type TemplateResult } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import {
  deviceApi,
  onDeviceApiChange,
  FOCUS_COLOR_CUSTOM,
  TYPEFACE_NAMES,
  type DeviceInfo,
  type DeviceSettings,
  type Language,
  type ReaderDeviceSettings,
  type Typeface,
  type WifiStationConfig,
} from "../device/api";
import { HttpDeviceApi } from "../device/http-api";
import { APP_VERSION } from "../../shared/config";
import { setSoftColors, softColors } from "../ui/reader-look";
import { LANG_NAMES, SUPPORTED_LANGS, chooseLang, chosenLang, tr, type SupportedLang } from "../i18n/index";
import { icons } from "../ui/icons";
import { onBack } from "../ui/back-nav";
import { rangeFill, rgb565, sharedStyles } from "../ui/theme";
import "./help-panel.element";
import "./updates-panel.element";

type Page = "root" | "reading" | "look" | "display" | "menu" | "power" | "connect" | "updates" | "language" | "help" | "about";

/**
 * Więcej: the reader's settings as category pages, like the reader's own
 * Ustawienia/Motywy/Urządzenie tabs, plus updates, language and help.
 * Every change goes to the reader at once (optimistic, rolled back on error).
 */
@customElement("settings-panel")
export class SettingsPanel extends LitElement {
  /** Firmware the reader reported on connect (for About). */
  @property({ attribute: false }) readerFirmware = "";
  @property({ attribute: false }) connection = "";

  @state() private page: Page = "root";
  @state() private settings: DeviceSettings | null = null;
  @state() private saving = false;
  @state() private error = "";
  @state() private tapCount = 0;
  @state() private justUnlocked = false;
  private tapResetTimer: number | null = null;
  private unsubApi: (() => void) | null = null;
  private unsubBack: (() => void) | null = null;

  @state() private wifi: WifiStationConfig | null = null;
  @state() private wifiSsidInput = "";
  @state() private wifiPasswordInput = "";
  @state() private wifiBusy = false;
  @state() private wifiError = "";
  @state() private wifiTimeoutMinutes = 0;

  @state() private deviceInfo: DeviceInfo | null = null;
  @state() private logLines: string[] = [];
  @state() private showLogs = false;
  @state() private diagBusy = false;

  connectedCallback(): void {
    super.connectedCallback();
    void this.load();
    this.unsubApi = onDeviceApiChange(() => void this.load());
    this.unsubBack = onBack(() => {
      if (this.page === "root") return false;
      this.go("root");
      return true;
    });
  }

  disconnectedCallback(): void {
    super.disconnectedCallback();
    if (this.tapResetTimer) window.clearTimeout(this.tapResetTimer);
    this.unsubApi?.();
    this.unsubBack?.();
  }

  private get onReader(): boolean {
    return deviceApi.current instanceof HttpDeviceApi;
  }

  // ─── Render ──────────────────────────────────────────────────────────────

  render() {
    if (this.page === "help") {
      return html`<help-panel @help-close=${() => this.go("root")} @restart-tutorial=${this.handleRestartTutorial}></help-panel>`;
    }
    if (!this.settings) return html`<p class="muted">${tr("set.loading")}</p>`;
    const body = this.renderPage(this.settings);
    if (this.page === "root") return body;
    return html`
      <button class="back" @click=${() => this.go("root")}>${icons.chevronLeft()} ${tr("nav.more")}</button>
      <h2 class="page-title">${this.pageTitle(this.page)}</h2>
      ${this.error ? html`<p class="error">${this.error}</p>` : nothing}
      ${body}
      ${this.saving ? html`<p class="muted small saving">${tr("common.saving")}</p>` : nothing}
    `;
  }

  private go(page: Page): void {
    this.page = page;
    this.error = "";
    document.querySelector("czytnik-app")?.shadowRoot?.querySelector("main")?.scrollTo({ top: 0 });
    if (page === "connect") void this.loadNetwork();
  }

  private pageTitle(page: Page): string {
    const titles: Record<Page, string> = {
      root: tr("nav.more"),
      reading: tr("set.reading"),
      look: tr("set.look"),
      display: tr("set.display"),
      menu: tr("set.menu"),
      power: tr("set.power"),
      connect: tr("set.connect"),
      updates: tr("set.updates"),
      language: tr("set.language"),
      help: tr("set.help"),
      about: tr("set.about"),
    };
    return titles[page];
  }

  private renderPage(s: DeviceSettings): TemplateResult {
    switch (this.page) {
      case "root":
        return this.renderHome(s);
      case "reading":
        return this.renderReading(s);
      case "look":
        return this.renderLook(s);
      case "display":
        return this.renderDisplay(s);
      case "menu":
        return this.renderMenu(s);
      case "power":
        return this.renderPower(s);
      case "connect":
        return this.renderConnect(s);
      case "updates":
        return html`<updates-panel .currentFw=${this.readerFirmware}></updates-panel>`;
      case "language":
        return this.renderLanguage(s);
      case "about":
        return this.renderAbout(s);
      default:
        return html``;
    }
  }

  private renderHome(s: DeviceSettings) {
    const row = (page: Page, icon: TemplateResult, title: string, desc?: string) => html`
      <button class="item" @click=${() => this.go(page)}>
        <span class="row-ico">${icon}</span>
        <span class="label">${title}${desc ? html`<small>${desc}</small>` : nothing}</span>
        <span class="chev">${icons.chevronRight()}</span>
      </button>
    `;
    return html`
      ${!this.onReader ? html`<p class="notice">${tr("set.sample")}</p>` : nothing}
      ${this.onReader && !s.device ? html`<p class="notice">${tr("set.needsUpdate")}</p>` : nothing}
      ${this.error ? html`<p class="error">${this.error}</p>` : nothing}
      <span class="section-title">${tr("set.group.reader")}</span>
      <div class="list">
        ${row("reading", icons.gauge(), tr("set.reading"), tr("set.reading.desc"))}
        ${row("look", icons.type(), tr("set.look"), tr("set.look.desc"))}
        ${row("display", icons.sun(), tr("set.display"), tr("set.display.desc"))}
        ${s.device ? row("menu", icons.palette(), tr("set.menu"), tr("set.menu.desc")) : nothing}
        ${s.device ? row("power", icons.moon(), tr("set.power"), tr("set.power.desc")) : nothing}
        ${row("connect", icons.wifi(20), tr("set.connect"), tr("set.connect.desc"))}
        ${row("updates", icons.update(20), tr("set.updates"), tr("set.updates.desc"))}
      </div>
      <span class="section-title">${tr("set.group.app")}</span>
      <div class="list">
        ${this.onReader
          ? this.switchRow(tr("set.advanced"), s.devMode, (v) => this.put({ devMode: v }), tr("set.advanced.desc"))
          : nothing}
        ${this.switchRow(tr("set.softColors"), softColors(), (v) => this.setSoft(v), tr("set.softColors.desc"))}
        ${row("language", icons.globe(), tr("set.language"), LANG_NAMES[chosenLang() ?? (s.language as SupportedLang)] ?? "")}
        ${row("help", icons.help(), tr("set.help"))}
        ${row("about", icons.chip(), tr("set.about"), `${tr("set.appVersion")} ${APP_VERSION}`)}
      </div>
    `;
  }

  // ─── Reading ─────────────────────────────────────────────────────────────

  private renderReading(s: DeviceSettings) {
    const rsvp = s.readerMode !== "scroll";
    return html`
      <div class="list">
        ${this.segRow(tr("set.mode"), s.readerMode, [
          ["rsvp", tr("set.mode.rsvp")],
          ["scroll", tr("set.mode.scroll")],
        ], (v) => this.put({ readerMode: v }))}
        ${rsvp
          ? html`
              ${this.sliderRow(tr("set.wpm"), s.baseWpm, 50, 1000, 25, (v) => `${v} ${tr("set.wpm.unit")}`, (v) => this.put({ baseWpm: v }))}
              ${this.segRow(tr("set.pause"), s.pauseBehaviour === "auto" ? "auto" : "tap", [
                ["tap", tr("set.pause.sentence")],
                ["auto", tr("set.pause.instant")],
              ], (v) => this.put({ pauseBehaviour: v }))}
              ${this.switchRow(tr("set.phantom"), s.phantomWords, (v) => this.put({ phantomWords: v }), tr("set.phantom.desc"))}
            `
          : html`
              ${this.sliderRow(tr("set.scrollSize"), s.scrollFontSize, 0, 8, 1, (v) => String(v + 1), (v) => this.put({ scrollFontSize: v }))}
              ${this.segRow(tr("set.lineSpacing"), s.scrollLineSpacing, [
                [0, tr("set.compact")],
                [1, tr("set.normal")],
                [2, tr("set.relaxed")],
              ], (v) => this.put({ scrollLineSpacing: v }))}
              ${this.segRow(tr("set.margins"), s.scrollMargin, [
                [0, tr("set.narrow")],
                [1, tr("set.normal")],
                [2, tr("set.wide")],
              ], (v) => this.put({ scrollMargin: v }))}
            `}
      </div>
      ${rsvp
        ? html`
            <span class="section-title">${tr("set.delays")}</span>
            <div class="list">
              ${this.sliderRow(tr("set.longWords"), s.longWordDelayMs, 0, 600, 50, (v) => `+${v} ms`, (v) => this.put({ longWordDelayMs: v }))}
              ${this.sliderRow(tr("set.complexWords"), s.complexWordDelayMs, 0, 600, 50, (v) => `+${v} ms`, (v) => this.put({ complexWordDelayMs: v }))}
              ${this.sliderRow(tr("set.punctuation"), s.punctuationDelayMs, 0, 600, 50, (v) => `+${v} ms`, (v) => this.put({ punctuationDelayMs: v }))}
            </div>
          `
        : nothing}
    `;
  }

  // ─── Reading look ────────────────────────────────────────────────────────

  private renderLook(s: DeviceSettings) {
    const d = s.device;
    const o = s.options;
    const focusCss =
      d && o
        ? rgb565(d.focusColor === FOCUS_COLOR_CUSTOM ? d.focusRgb : (o.focusColors[d.focusColor] ?? 0x001f))
        : "var(--accent)";
    const available = s.typefacesAvailable;
    return html`
      ${this.renderSample(s, focusCss)}
      <div class="list">
        <label class="item stack">
          <span class="head"><span>${tr("set.typeface")}</span></span>
          <select
            class="input"
            @change=${(e: Event) => {
              const index = Number((e.target as HTMLSelectElement).value);
              const legacy = (["standard", "open_dyslexic", "atkinson"] as Typeface[])[index];
              this.put(legacy ? { typefaceIndex: index, typeface: legacy } : { typefaceIndex: index });
            }}
          >
            ${TYPEFACE_NAMES.map((name, index) =>
              !available || available.includes(index) || index === s.typefaceIndex
                ? html`<option value=${index} ?selected=${index === s.typefaceIndex}>${name}</option>`
                : nothing,
            )}
            ${s.typefaceIndex >= TYPEFACE_NAMES.length
              ? html`<option value=${s.typefaceIndex} selected>${tr("set.typeface.newer")}</option>`
              : nothing}
          </select>
          ${available && available.length < TYPEFACE_NAMES.length
            ? html`<small class="muted small">${tr("set.typeface.sd", { have: available.length, all: TYPEFACE_NAMES.length })}</small>`
            : nothing}
        </label>
        ${this.segRow(tr("set.wordSize"), s.fontSizeIndex, [
          [0, "S"],
          [1, "M"],
          [2, "L"],
        ], (v) => this.put({ fontSizeIndex: v }))}
        ${this.switchRow(tr("set.focusHighlight"), s.focusHighlight, (v) => this.put({ focusHighlight: v }), tr("set.focusHighlight.desc"))}
        ${d && o
          ? html`<div class="item stack">
              <span class="head"><span>${tr("set.focusColor")}</span></span>
              <div class="swatches">
                ${o.focusColors.map(
                  (c, i) => html`<button
                    class=${d.focusColor === i ? "swatch on" : "swatch"}
                    style="background:${rgb565(c)}"
                    aria-label=${tr("set.focusColor")}
                    @click=${() => this.putDevice({ focusColor: i })}
                  >${d.focusColor === i ? icons.check(18) : nothing}</button>`,
                )}
                ${d.focusColor === FOCUS_COLOR_CUSTOM
                  ? html`<span class="swatch on" style="background:${rgb565(d.focusRgb)}">${icons.check(18)}</span>`
                  : nothing}
              </div>
              ${d.focusColor === FOCUS_COLOR_CUSTOM ? html`<small class="muted small">${tr("set.focusColor.custom")}</small>` : nothing}
            </div>`
          : nothing}
      </div>
      <div class="list">
        ${this.sliderRow(tr("set.tracking"), s.tracking, -2, 3, 1, (v) => (v > 0 ? `+${v}` : String(v)), (v) => this.put({ tracking: v }))}
        ${this.sliderRow(tr("set.anchor"), s.anchorPercent, 30, 40, 1, (v) => `${v}%`, (v) => this.put({ anchorPercent: v }))}
        ${this.sliderRow(tr("set.guideWidth"), s.guideWidth, 12, 30, 1, (v) => `${v} px`, (v) => this.put({ guideWidth: v }))}
        ${this.sliderRow(tr("set.guideGap"), s.guideGap, 2, 8, 1, (v) => `${v} px`, (v) => this.put({ guideGap: v }))}
      </div>
    `;
  }

  /** A word as the reader shows it: focus letter in the highlight color, neighbours dimmed. */
  private renderSample(s: DeviceSettings, focusCss: string) {
    const [before, word, after] = tr("set.sampleWords").split(" ");
    // Optimal recognition point, as the reader picks it (by word length).
    const n = word.length;
    const focus = n <= 1 ? 0 : n <= 5 ? 1 : n <= 9 ? 2 : n <= 13 ? 3 : 4;
    return html`<div class="sample ${s.theme}">
      <span class="phantom">${s.phantomWords ? before : ""}</span>
      <span class="word size-${s.fontSizeIndex}"
        >${word.slice(0, focus)}<b style=${s.focusHighlight ? `color:${focusCss}` : ""}>${word[focus]}</b>${word.slice(focus + 1)}</span
      >
      <span class="phantom">${s.phantomWords ? after : ""}</span>
    </div>`;
  }

  // ─── Screen ──────────────────────────────────────────────────────────────

  private renderDisplay(s: DeviceSettings) {
    const d = s.device;
    return html`
      <div class="list">
        ${this.segRow(tr("set.theme"), s.theme, [
          ["light", tr("set.theme.light")],
          ["dark", tr("set.theme.dark")],
          ["night", tr("set.theme.night")],
        ], (v) => this.put({ theme: v }))}
        ${this.sliderRow(tr("set.brightness"), Math.max(10, s.brightness), 10, 100, 1, (v) => `${v}%`, (v) => this.put({ brightness: v }))}
        ${this.segRow(tr("set.hand"), s.readerHand, [
          ["right", tr("set.hand.right")],
          ["left", tr("set.hand.left")],
        ], (v) => this.put({ readerHand: v }))}
        ${this.selectRow(tr("set.readerLanguage"), s.language, SUPPORTED_LANGS.map((l) => [l, LANG_NAMES[l]] as [Language, string]), (v) =>
          this.put({ language: v }),
        )}
      </div>
      <span class="section-title">${tr("set.hud")}</span>
      <div class="list">
        ${this.switchRow(tr("set.hud.battery"), s.showBatteryWhileReading, (v) => this.put({ showBatteryWhileReading: v }))}
        ${this.switchRow(tr("set.hud.chapter"), s.showChapterWhileReading, (v) => this.put({ showChapterWhileReading: v }))}
        ${this.switchRow(tr("set.hud.progress"), s.showPercentWhileReading, (v) => this.put({ showPercentWhileReading: v }))}
        ${this.selectRow(tr("set.footer"), s.footerMetric, (["percentage", "chapter_time", "book_time"] as const).map((m) => [m, tr(`set.footer.${m}`)] as [typeof m, string]), (v) =>
          this.put({ footerMetric: v }),
        )}
        ${this.selectRow(tr("set.batteryLabel"), s.batteryLabel, (["percent", "time_remaining", "voltage"] as const).map((m) => [m, tr(`set.batteryLabel.${m}`)] as [typeof m, string]), (v) =>
          this.put({ batteryLabel: v }),
        )}
        ${d
          ? this.selectRow(tr("set.batteryStyle"), d.batteryStyle, [0, 1, 2, 3].map((i) => [i, tr(`set.batteryStyle.${i}`)] as [number, string]), (v) =>
              this.putDevice({ batteryStyle: v }),
            )
          : nothing}
      </div>
    `;
  }

  // ─── Menu and theme ──────────────────────────────────────────────────────

  private renderMenu(s: DeviceSettings) {
    const d = s.device!;
    const o = s.options!;
    return html`
      <span class="section-title">${tr("set.palette")}</span>
      <div class="palettes">
        ${o.palettes.map((p, i) => {
          const [bg, fg, ac] = p.c ?? [s.theme === "light" ? 0xdeda : 0x0000, s.theme === "light" ? 0x0000 : 0xffff, o.focusColors[d.focusColor] ?? 0x001f];
          return html`<button class=${d.menuPalette === i ? "palette on" : "palette"} @click=${() => this.putDevice({ menuPalette: i })}>
            <span class="pal-preview" style="background:${rgb565(bg)};color:${rgb565(fg)}">
              <span class="pal-line" style="background:${rgb565(fg)}"></span>
              <span class="pal-chip" style="background:${rgb565(ac)}"></span>
            </span>
            <span class="pal-name">${i === 0 ? tr("set.palette.classic").split(":")[0] : p.n}</span>
          </button>`;
        })}
      </div>
      ${d.menuPalette === 0 ? html`<p class="muted small">${tr("set.palette.classic")}</p>` : nothing}
      <div class="list">
        ${d.menuPalette !== 0
          ? this.switchRow(tr("set.ownAccent"), d.menuOwnAccent, (v) => this.putDevice({ menuOwnAccent: v }), tr("set.ownAccent.desc"))
          : nothing}
        ${this.selectRow(tr("set.menuFont"), d.menuFont, [[-1, tr("set.menuFont.follow")] as [number, string], ...o.menuFonts.map((f, i) => [i, f] as [number, string])], (v) =>
          this.putDevice({ menuFont: v }),
        )}
        ${this.selectRow(tr("set.layout"), d.menuLayout, [0, 1, 2, 3].map((i) => [i, tr(`set.layout.${i}`)] as [number, string]), (v) =>
          this.putDevice({ menuLayout: v }),
        )}
        ${this.selectRow(tr("set.librarySort"), d.librarySort, [0, 1, 2, 3].map((i) => [i, tr(`set.librarySort.${i}`)] as [number, string]), (v) =>
          this.putDevice({ librarySort: v }),
        )}
        ${this.switchRow(tr("set.helpHints"), d.helpHints, (v) => this.putDevice({ helpHints: v }))}
        ${this.switchRow(tr("set.savePointNames"), d.savePointNames, (v) => this.putDevice({ savePointNames: v }))}
      </div>
    `;
  }

  // ─── Screensaver and power ───────────────────────────────────────────────

  private renderPower(s: DeviceSettings) {
    const d = s.device!;
    const o = s.options!;
    const minutes = (list: number[], zeroNever: boolean) =>
      list.map((m, i) => [i, m === 0 && zeroNever ? tr("common.never") : tr("common.minutes", { n: m })] as [number, string]);
    return html`
      <div class="list">
        ${this.selectRow(tr("set.saver"), d.screensaverMode, o.screensaverModes.map((m) => [m, tr(`set.saver.${m}`)] as [number, string]), (v) =>
          this.putDevice({ screensaverMode: v }),
        )}
        ${this.selectRow(tr("set.saverAfter"), d.screensaverTimeout, minutes(o.screensaverTimeoutMin, true), (v) =>
          this.putDevice({ screensaverTimeout: v }),
        )}
        ${this.selectRow(tr("set.autoOff"), d.screensaverAutoOff, minutes(o.screensaverAutoOffMin, true), (v) =>
          this.putDevice({ screensaverAutoOff: v }), tr("set.autoOff.desc"),
        )}
        ${this.selectRow(tr("set.sleepGuard"), d.sleepGuard, minutes(o.sleepGuardMin, true), (v) =>
          this.putDevice({ sleepGuard: v }), tr("set.sleepGuard.desc"),
        )}
      </div>
    `;
  }

  // ─── Connectivity ────────────────────────────────────────────────────────

  private renderConnect(s: DeviceSettings) {
    const d = s.device;
    return html`
      <span class="section-title">${tr("set.homeWifi")}</span>
      <div class="list">
        <div class="item stack">
          <small class="muted small">${tr("set.homeWifi.desc")}</small>
          <span class="wifi-state">
            ${this.wifi?.configured ? tr("set.homeWifi.saved", { ssid: this.wifi.ssid }) : tr("set.homeWifi.none")}
          </span>
          <label class="field">
            ${tr("set.ssid")}
            <input class="input" type="text" .value=${this.wifiSsidInput} autocomplete="off"
              @input=${(e: Event) => (this.wifiSsidInput = (e.target as HTMLInputElement).value)} />
          </label>
          <label class="field">
            ${tr("set.password")}
            <input class="input" type="password" .value=${this.wifiPasswordInput}
              placeholder=${this.wifi?.passwordSet ? tr("set.password.keep") : ""}
              @input=${(e: Event) => (this.wifiPasswordInput = (e.target as HTMLInputElement).value)} />
          </label>
          ${this.wifiError ? html`<p class="error">${this.wifiError}</p>` : nothing}
          <div class="row">
            <button class="btn" ?disabled=${this.wifiBusy || !this.wifi?.configured} @click=${this.forgetWifiStation}>${tr("set.forget")}</button>
            <button class="btn primary" ?disabled=${this.wifiBusy} @click=${this.saveWifiStation}>${tr("set.saveNetwork")}</button>
          </div>
        </div>
      </div>
      <div class="list">
        ${d ? this.switchRow(tr("set.autoUpdate"), d.autoUpdate, (v) => this.putDevice({ autoUpdate: v }), tr("set.autoUpdate.desc")) : nothing}
        ${d ? this.switchRow(tr("set.bluetooth"), d.bluetooth, (v) => this.putDevice({ bluetooth: v }), tr("set.bluetooth.desc")) : nothing}
        ${d
          ? this.selectRow(tr("set.wifiSession"), d.wifiSession ?? 0, [0, 1, 2, 3, 4].map((i) => [i, tr(`set.wifiSession.${i}`)] as [number, string]), (v) =>
              this.putDevice({ wifiSession: v }),
            )
          : nothing}
        ${this.sliderRow(
          tr("set.wifiTimeout"),
          this.wifiTimeoutMinutes,
          0,
          60,
          5,
          (v) => (v === 0 ? tr("common.never") : tr("common.minutes", { n: v })),
          (v) => void this.setWifiTimeoutMinutes(v),
        )}
      </div>
    `;
  }

  // ─── Language ────────────────────────────────────────────────────────────

  private renderLanguage(s: DeviceSettings) {
    const chosen = chosenLang();
    const option = (value: SupportedLang | null, label: string) => html`
      <button class="item" @click=${() => chooseLang(value)}>
        <span class="label">${label}</span>
        ${chosen === value ? html`<span class="tick">${icons.check()}</span>` : nothing}
      </button>
    `;
    return html`
      <span class="section-title">${tr("set.appLanguage")}</span>
      <div class="list">
        ${option(null, `${tr("set.appLanguage.auto")} (${LANG_NAMES[s.language as SupportedLang] ?? s.language})`)}
        ${SUPPORTED_LANGS.map((l) => option(l, LANG_NAMES[l]))}
      </div>
      <div class="list">
        ${this.selectRow(tr("set.readerLanguage"), s.language, SUPPORTED_LANGS.map((l) => [l, LANG_NAMES[l]] as [Language, string]), (v) =>
          this.put({ language: v }),
        )}
      </div>
    `;
  }

  // ─── About ───────────────────────────────────────────────────────────────

  private renderAbout(s: DeviceSettings) {
    return html`
      <div class="list">
        <button class="item brand-row" @click=${this.onBrandTap}>
          <span class="label">${tr("set.appVersion")}
            ${this.tapCount > 0 && this.tapCount < 10 && !s.devMode ? html`<small>${tr("set.devTaps", { n: 10 - this.tapCount })}</small>` : nothing}
            ${this.justUnlocked ? html`<small class="ok-text">${tr("set.devOn")}</small>` : nothing}
          </span>
          <span class="value">${APP_VERSION}</span>
        </button>
        <div class="item"><span class="label">${tr("set.readerVersion")}</span><span class="value">${this.readerFirmware || "—"}</span></div>
        <div class="item"><span class="label">${tr("set.connection")}</span><span class="value">${this.connection || "—"}</span></div>
      </div>
      ${s.devMode ? this.renderDeveloper(s) : nothing}
    `;
  }

  private renderDeveloper(s: DeviceSettings) {
    return html`
      <span class="section-title">${tr("set.dev")}</span>
      <div class="list">
        ${this.switchRow(tr("set.devMode"), s.devMode, (v) => this.put({ devMode: v }), tr("set.devMode.desc"))}
        <div class="item stack">
          <span class="head"><span>${tr("set.diag")}</span>
            <button class="btn small" ?disabled=${this.diagBusy} @click=${this.refreshDiagnostics}>${tr("common.refresh")}</button></span>
          ${this.deviceInfo
            ? html`<ul class="diag">
                <li><span>${tr("set.diag.mode")}</span><strong>${this.deviceInfo.mode === "station" ? tr("set.diag.station") : tr("set.diag.ap")}</strong></li>
                <li><span>${tr("set.diag.network")}</span><strong>${this.deviceInfo.networkSsid || "—"}</strong></li>
                <li><span>${tr("home.battery")}</span><strong>${this.deviceInfo.batteryPercent}%</strong></li>
                <li><span>${tr("set.diag.card")}</span><strong>${tr("set.diag.cardFree", { free: formatKb(this.deviceInfo.sdFreeKb), total: formatKb(this.deviceInfo.sdTotalKb) })}</strong></li>
              </ul>`
            : nothing}
        </div>
        <div class="item stack">
          <span class="head"><span>${tr("set.logs")}</span>
            <button class="btn small" @click=${this.toggleLogs}>${this.showLogs ? tr("set.logs.hide") : tr("set.logs.show")}</button></span>
          ${this.showLogs
            ? html`<div class="row">
                  <button class="btn small" ?disabled=${this.diagBusy} @click=${this.refreshLogs}>${tr("common.refresh")}</button>
                  <button class="btn small" ?disabled=${this.diagBusy} @click=${this.clearLogs}>${tr("set.logs.clear")}</button>
                </div>
                <pre class="log">${this.logLines.length ? this.logLines.join("\n") : tr("set.logs.empty")}</pre>`
            : nothing}
        </div>
      </div>
    `;
  }

  // ─── Row builders ────────────────────────────────────────────────────────

  private switchRow(label: string, value: boolean, set: (v: boolean) => void, desc?: string) {
    return html`<label class="item">
      <span class="label">${label}${desc ? html`<small>${desc}</small>` : nothing}</span>
      <input class="switch" type="checkbox" .checked=${value} @change=${(e: Event) => set((e.target as HTMLInputElement).checked)} />
    </label>`;
  }

  private segRow<T extends string | number>(label: string, value: T, options: Array<[T, string]>, set: (v: T) => void) {
    return html`<div class="item stack">
      <span class="head"><span>${label}</span></span>
      <div class="seg">
        ${options.map(([v, text]) => html`<button class=${v === value ? "on" : ""} @click=${() => set(v)}>${text}</button>`)}
      </div>
    </div>`;
  }

  private selectRow<T extends string | number>(label: string, value: T, options: Array<[T, string]>, set: (v: T) => void, desc?: string) {
    return html`<label class="item">
      <span class="label">${label}${desc ? html`<small>${desc}</small>` : nothing}</span>
      <select
        class="pick"
        @change=${(e: Event) => {
          const raw = (e.target as HTMLSelectElement).value;
          const match = options.find(([v]) => String(v) === raw);
          if (match) set(match[0]);
        }}
      >
        ${options.map(([v, text]) => html`<option value=${String(v)} ?selected=${v === value}>${text}</option>`)}
      </select>
    </label>`;
  }

  /** Slider: the value follows the finger, the reader gets it on release. */
  private sliderRow(label: string, value: number, min: number, max: number, step: number, format: (v: number) => string, set: (v: number) => void) {
    return html`<div class="item stack">
      <span class="head"><span>${label}</span><span class="value">${format(value)}</span></span>
      <input
        type="range"
        min=${min}
        max=${max}
        step=${step}
        .value=${String(value)}
        style=${rangeFill(value, min, max)}
        @input=${(e: Event) => {
          const input = e.target as HTMLInputElement;
          input.style.cssText = rangeFill(Number(input.value), min, max);
          const valueEl = input.parentElement?.querySelector(".value");
          if (valueEl) valueEl.textContent = format(Number(input.value));
        }}
        @change=${(e: Event) => set(Number((e.target as HTMLInputElement).value))}
      />
    </div>`;
  }

  // ─── Data ────────────────────────────────────────────────────────────────

  private async load() {
    try {
      this.settings = await deviceApi.getSettings();
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
    }
  }

  private setSoft(on: boolean) {
    setSoftColors(on);
    this.requestUpdate();
    // The shell redraws its colors from the reader's settings.
    this.dispatchEvent(new CustomEvent("device-settings-changed", { bubbles: true, composed: true, detail: this.settings ?? undefined }));
  }

  private async put(patch: Partial<DeviceSettings>) {
    if (!this.settings) return;
    const previous = this.settings;
    this.settings = { ...previous, ...patch };
    this.saving = true;
    this.error = "";
    try {
      this.settings = await deviceApi.putSettings(patch);
      // The shell follows the reader's language, colors and DEV badge.
      this.dispatchEvent(new CustomEvent("device-settings-changed", { bubbles: true, composed: true, detail: this.settings }));
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
      this.settings = previous;
    } finally {
      this.saving = false;
    }
  }

  private putDevice(patch: Partial<ReaderDeviceSettings>) {
    const device = this.settings?.device;
    if (!device) return;
    void this.put({ device: { ...device, ...patch } });
  }

  private async loadNetwork(): Promise<void> {
    try {
      this.wifi = await deviceApi.getWifiStation();
      this.wifiSsidInput = this.wifi.ssid;
    } catch {
      /* the reader didn't answer, keep what we had */
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
      this.wifiError = tr("set.ssidNeeded");
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

  private handleRestartTutorial(): void {
    this.page = "root";
    this.dispatchEvent(new CustomEvent("restart-tutorial", { bubbles: true, composed: true }));
  }

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
      this.logLines = (await deviceApi.getLogTail(80)).lines;
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

  // Ten taps on the app version unlock developer mode.
  private onBrandTap = () => {
    if (!this.settings || this.settings.devMode) return;
    this.tapCount += 1;
    if (this.tapResetTimer) window.clearTimeout(this.tapResetTimer);
    this.tapResetTimer = window.setTimeout(() => (this.tapCount = 0), 1500);
    if (this.tapCount >= 10) {
      this.tapCount = 0;
      void this.put({ devMode: true });
      this.justUnlocked = true;
      window.setTimeout(() => (this.justUnlocked = false), 3000);
    }
  };

  static styles = [
    sharedStyles,
    css`
      :host {
        display: flex;
        flex-direction: column;
        gap: 12px;
      }
      .back {
        align-self: flex-start;
        display: inline-flex;
        align-items: center;
        gap: 2px;
        margin-left: -6px;
        padding: 6px 8px 6px 2px;
        border: 0;
        border-radius: var(--radius-sm);
        background: transparent;
        color: var(--accent-text);
        font-size: 0.95rem;
        font-weight: 600;
        cursor: pointer;
      }
      .page-title {
        margin: -4px 2px 2px;
        font-size: 1.35rem;
        font-weight: 700;
      }
      .row-ico {
        flex: 0 0 auto;
        width: 36px;
        height: 36px;
        display: grid;
        place-items: center;
        border-radius: 10px;
        background: var(--accent-soft);
        color: var(--accent-text);
      }
      .chev,
      .tick {
        color: var(--muted);
        display: grid;
      }
      .tick {
        color: var(--accent-text);
      }
      .pick {
        flex: 0 1 auto;
        max-width: 55%;
        min-height: 38px;
        padding: 0 30px 0 12px;
        border: 0;
        border-radius: var(--radius-sm);
        background: var(--surface-2)
          url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='12' viewBox='0 0 24 24' fill='none' stroke='%238a8d98' stroke-width='2.4' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='M6 9l6 6 6-6'/%3E%3C/svg%3E")
          no-repeat right 10px center;
        appearance: none;
        -webkit-appearance: none;
        color: var(--text);
        font-size: 0.88rem;
        text-overflow: ellipsis;
      }
      select.input {
        appearance: none;
        -webkit-appearance: none;
      }
      .saving {
        text-align: center;
      }
      .wifi-state {
        font-weight: 600;
      }

      .sample {
        display: flex;
        align-items: baseline;
        justify-content: center;
        gap: 14px;
        padding: 26px 12px;
        border-radius: var(--radius-lg);
        background: #000;
        color: #fff;
        overflow: hidden;
      }
      .sample.light {
        background: rgb(222, 218, 214);
        color: #000;
      }
      .sample.night {
        color: rgb(255, 156, 0);
      }
      .sample .word {
        font-size: 1.9rem;
        font-weight: 500;
        letter-spacing: 0.01em;
      }
      .sample .word.size-1 {
        font-size: 2.3rem;
      }
      .sample .word.size-2 {
        font-size: 2.8rem;
      }
      .sample .word b {
        font-weight: inherit;
      }
      .sample .phantom {
        opacity: 0.35;
        font-size: 1.1rem;
      }

      .swatches {
        display: flex;
        flex-wrap: wrap;
        gap: 10px;
      }
      .swatch {
        width: 40px;
        height: 40px;
        display: grid;
        place-items: center;
        border: 0;
        border-radius: 50%;
        color: #fff;
        cursor: pointer;
        box-shadow: inset 0 0 0 1px rgba(255, 255, 255, 0.15);
      }
      .swatch.on {
        box-shadow:
          0 0 0 2px var(--bg),
          0 0 0 4px var(--text);
      }

      .palettes {
        display: grid;
        grid-template-columns: repeat(auto-fill, minmax(96px, 1fr));
        gap: 10px;
      }
      .palette {
        display: flex;
        flex-direction: column;
        gap: 6px;
        padding: 6px;
        border: 0;
        border-radius: var(--radius);
        background: var(--surface);
        color: var(--text);
        cursor: pointer;
      }
      .palette.on {
        box-shadow: inset 0 0 0 2px var(--accent);
      }
      .pal-preview {
        position: relative;
        height: 52px;
        border-radius: 9px;
        overflow: hidden;
        box-shadow: inset 0 0 0 1px rgba(127, 127, 127, 0.25);
      }
      .pal-line {
        position: absolute;
        left: 10px;
        top: 14px;
        width: 46%;
        height: 5px;
        border-radius: 3px;
        opacity: 0.85;
      }
      .pal-chip {
        position: absolute;
        left: 10px;
        bottom: 10px;
        width: 34px;
        height: 12px;
        border-radius: 6px;
      }
      .pal-name {
        font-size: 0.8rem;
        font-weight: 600;
        text-align: center;
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
      }

      .diag {
        margin: 0;
        padding: 0;
        list-style: none;
        display: flex;
        flex-direction: column;
        gap: 6px;
        font-size: 0.85rem;
      }
      .diag li {
        display: flex;
        justify-content: space-between;
        gap: 10px;
        color: var(--muted);
      }
      .diag strong {
        color: var(--text);
        text-align: right;
      }
      .log {
        margin: 0;
        max-height: 240px;
        overflow: auto;
        padding: 10px 12px;
        border-radius: var(--radius-sm);
        background: var(--surface-2);
        color: var(--text-2);
        font: 0.72rem/1.5 var(--font-mono);
        white-space: pre-wrap;
        word-break: break-all;
      }
    `,
  ];
}

function formatKb(kb: number): string {
  if (kb < 1024) return `${kb} kB`;
  if (kb < 1024 * 1024) return `${(kb / 1024).toFixed(1)} MB`;
  return `${(kb / 1024 / 1024).toFixed(2)} GB`;
}

declare global {
  interface HTMLElementTagNameMap {
    "settings-panel": SettingsPanel;
  }
}
