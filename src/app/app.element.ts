import { LitElement, css, html, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { keyed } from "lit/directives/keyed.js";
import { unsafeHTML } from "lit/directives/unsafe-html.js";
import { BRAND_NAME } from "../shared/config";
import type { DeviceLink } from "./device/device-link";
import { WifiLink, helloDevice } from "./device/wifi-link";
import {
  isNativeApp,
  joinNetworkFromQr,
  joinReaderNetwork,
  openWifiSettings,
  readerNetworkFromQr,
  pinToReaderNetwork,
  unpinReaderNetwork,
} from "./device/network-pin";
import { BluetoothLink } from "./device/bluetooth-link";
import { SerialLink } from "./device/serial-link";
import { dandelionIcon } from "./components/flower-icon";
import "./components/converter-panel.element";
import "./components/library-panel.element";
import "./components/settings-panel.element";
import "./components/plugins-panel.element";
import "./components/onboarding.element";
import "./components/pwa-install-dialog.element";
import "./components/tutorial-wizard.element";
import "./components/qr-scanner.element";
import {
  deviceApi,
  onDeviceApiChange,
  setDeviceApi,
  type Book,
  type DeviceInfo,
  type DeviceSettings,
} from "./device/api";
import { HttpDeviceApi } from "./device/http-api";
import { getTutorialStatus } from "./onboarding/onboarding-store";
import { followReaderLang, getLang, onLangChange, tr } from "./i18n/index";
import { deviceLangToSupported } from "./i18n/lang-map";
import { icons } from "./ui/icons";
import { sharedStyles, themeTokens } from "./ui/theme";
import { applyLook, lookFromSettings, saveLook, savedLook } from "./ui/reader-look";
import { applyFont, fontFromSettings, saveFont, savedFont } from "./ui/reader-font";
import { decodePicture, readerCoverColor, readerInitials } from "./books/pictures";

type View = "home" | "library" | "converter" | "plugins" | "more";
type Transport = "wifi" | "bluetooth" | "serial";

/** Static strings with <b>/<code> markup only, never user data. */
const trHtml = (key: string) => unsafeHTML(tr(key));

@customElement("czytnik-app")
export class CzytnikApp extends LitElement {
  @state() private view: View = "home";
  @state() private connecting = false;
  @state() private connected = false;
  @state() private error: string | null = null;
  @state() private chosenTransport: Transport | null = null;
  @state() private showAdvanced = false;
  @state() private devMode = false;
  @state() private showTutorial = false;
  @state() private readerFirmware = "";
  @state() private joinUnsupported = false;
  // QR fallback: offered once "Connect to reader" has gone 20 s without the
  // phone finding the reader's network.
  @state() private qrFallbackVisible = false;
  @state() private scanningQr = false;
  private qrFallbackTimer: ReturnType<typeof setTimeout> | null = null;


  // Start tab once connected: the open book and the reader's state.
  @state() private books: Book[] = [];
  @state() private currentCover = "";
  @state() private info: DeviceInfo | null = null;

  private link: DeviceLink | null = null;
  private unsubApi: (() => void) | null = null;
  private unsubLang: (() => void) | null = null;
  private heartbeatTimer: ReturnType<typeof setInterval> | null = null;

  connectedCallback(): void {
    super.connectedCallback();
    applyLook(this, savedLook());
    applyFont(this, savedFont());
    this.refreshFromReader();
    this.unsubApi = onDeviceApiChange(() => this.refreshFromReader());
    this.unsubLang = onLangChange(() => this.requestUpdate());
    this.addEventListener("device-settings-changed", this.onSettingsChanged as EventListener);
    this.addEventListener("tutorial-close", this.handleTutorialClose);
    this.addEventListener("restart-tutorial", this.handleRestartTutorial);
    this.addEventListener("open-view", this.onOpenView as EventListener);
    this.handleSharedFile();
    // Already on the reader's Wi-Fi (or back from the Wi-Fi settings):
    // connect without asking.
    void this.autoConnect();
    document.addEventListener("visibilitychange", this.onVisibilityChange);
  }

  disconnectedCallback(): void {
    super.disconnectedCallback();
    this.stopHeartbeat();
    this.unsubApi?.();
    this.unsubLang?.();
    this.removeEventListener("device-settings-changed", this.onSettingsChanged as EventListener);
    this.removeEventListener("tutorial-close", this.handleTutorialClose);
    this.removeEventListener("restart-tutorial", this.handleRestartTutorial);
    this.removeEventListener("open-view", this.onOpenView as EventListener);
    document.removeEventListener("visibilitychange", this.onVisibilityChange);
  }

  private onVisibilityChange = () => {
    if (document.visibilityState === "visible") void this.autoConnect();
  };

  private onOpenView = (e: CustomEvent<View>) => this.switchView(e.detail);

  /**
   * Silent check for a reader at 192.168.4.1. The native app pins itself to
   * the current Wi-Fi first (without that Android may send the request over
   * mobile data) and lets go again when nobody answers.
   */
  private autoConnect = async () => {
    if (this.connected || this.connecting) return;
    if (this.chosenTransport && this.chosenTransport !== "wifi") return;
    const pinned = await pinToReaderNetwork();
    const hello = await helloDevice();
    if (!hello) {
      if (pinned) await unpinReaderNetwork();
      return;
    }
    if (this.connected || this.connecting) return;
    this.chosenTransport = "wifi";
    await this.connect();
  };

  private handleTutorialClose = () => {
    this.showTutorial = false;
  };

  private handleRestartTutorial = () => {
    this.showTutorial = true;
  };

  private onSettingsChanged = (e: CustomEvent<DeviceSettings | undefined>) => {
    if (e.detail) this.adoptSettings(e.detail);
    else void this.refreshFromReader();
  };

  /** Language, colors and the DEV badge follow the reader's settings. */
  private adoptSettings(s: DeviceSettings): void {
    this.setDevMode(s.devMode);
    if (!this.onReader) return;
    followReaderLang(deviceLangToSupported(s.language));
    const look = lookFromSettings(s);
    if (look) {
      saveLook(look);
      applyLook(this, look);
    }
    const font = fontFromSettings(s);
    if (font) {
      saveFont(font);
      applyFont(this, font);
    }
  }

  private async refreshFromReader(): Promise<void> {
    try {
      this.adoptSettings(await deviceApi.getSettings());
    } catch {
      this.setDevMode(false);
    }
    if (this.onReader) void this.loadOverview();
  }

  /** Plugins is a tab of the reader's advanced mode only. */
  private setDevMode(on: boolean): void {
    this.devMode = on;
    if (!on && this.view === "plugins") this.view = "home";
  }

  private get onReader(): boolean {
    return deviceApi.current instanceof HttpDeviceApi;
  }

  /** The open book (with its cover) and the reader's battery and card. */
  private async loadOverview(): Promise<void> {
    try {
      this.books = await deviceApi.listBooks();
    } catch {
      /* the Start card shows what it has */
    }
    try {
      this.info = await deviceApi.getDeviceInfo();
    } catch {
      this.info = null;
    }
    const current = this.books.find((b) => b.current);
    this.currentCover = "";
    if (current?.hasCover) {
      try {
        const blob = await deviceApi.getBookPicture(current.name, "cover");
        const canvas = blob ? await decodePicture(blob) : null;
        if (canvas) this.currentCover = canvas.toDataURL();
      } catch {
        /* the colored default stays */
      }
    }
  }

  /** Web Share Target: a file shared to the app goes to the reader. */
  private async handleSharedFile(): Promise<void> {
    const params = new URLSearchParams(window.location.search);
    const sharedId = params.get("shared");
    if (!sharedId) return;
    const url = new URL(window.location.href);
    url.searchParams.delete("shared");
    window.history.replaceState(null, "", url.toString());
    this.view = "library";
    try {
      const entry = await this.readSharedFileFromDb(sharedId);
      if (!entry) return;
      const file = entry.file as File;
      const name = (entry.name as string) || file.name;
      await deviceApi.uploadBook(file, name);
      await this.removeSharedFileFromDb(sharedId);
    } catch (err) {
      this.error = tr("share.err", { error: err instanceof Error ? err.message : String(err) });
    }
  }

  private readSharedFileFromDb(id: string): Promise<Record<string, unknown> | null> {
    return new Promise((resolve, reject) => {
      const request = indexedDB.open("flower-share", 1);
      request.onupgradeneeded = () => {
        const db = request.result;
        if (!db.objectStoreNames.contains("pending-files")) db.createObjectStore("pending-files");
      };
      request.onsuccess = () => {
        const db = request.result;
        const get = db.transaction("pending-files", "readonly").objectStore("pending-files").get(id);
        get.onsuccess = () => {
          db.close();
          resolve(get.result ?? null);
        };
        get.onerror = () => {
          db.close();
          reject(get.error);
        };
      };
      request.onerror = () => reject(request.error);
    });
  }

  private removeSharedFileFromDb(id: string): Promise<void> {
    return new Promise((resolve, reject) => {
      const request = indexedDB.open("flower-share", 1);
      request.onsuccess = () => {
        const db = request.result;
        const tx = db.transaction("pending-files", "readwrite");
        tx.objectStore("pending-files").delete(id);
        tx.oncomplete = () => {
          db.close();
          resolve();
        };
        tx.onerror = () => {
          db.close();
          reject(tx.error);
        };
      };
      request.onerror = () => reject(request.error);
    });
  }

  // ─── Render ──────────────────────────────────────────────────────────────

  render() {
    // Keyed on the language: a switch rebuilds every screen in the new one.
    return keyed(
      getLang(),
      html`
        <onboarding-wizard></onboarding-wizard>
        <pwa-install-dialog></pwa-install-dialog>
        ${this.showTutorial ? html`<tutorial-wizard></tutorial-wizard>` : nothing}

        <header>
          <button class="brand" @click=${() => this.switchView("home")}>
            <span class="mark">${dandelionIcon(26)}</span>
            <strong>${BRAND_NAME}</strong>
          </button>
          <div class="badges">
            ${this.devMode ? html`<span class="badge dev">${tr("status.dev")}</span>` : nothing}
            <span class=${this.connected ? "status on" : "status"}>
              <span class="dot"></span>
              ${this.connected ? tr("status.connected") : tr("status.offline")}
            </span>
          </div>
        </header>

        <main><div class="view">${keyed(this.view, this.renderView())}</div></main>

        <nav>
          ${this.navButton("home", tr("nav.home"), icons.home())}
          ${this.navButton("library", tr("nav.books"), icons.books())}
          ${this.navButton("converter", tr("nav.convert"), icons.convert())}
          ${this.devMode ? this.navButton("plugins", tr("nav.plugins"), icons.plugins()) : nothing}
          ${this.navButton("more", tr("nav.more"), icons.more())}
        </nav>
      `,
    );
  }

  private navButton(v: View, label: string, ico: unknown) {
    return html`
      <button class=${this.view === v ? "on" : ""} @click=${() => this.switchView(v)}>
        <span class="ico">${ico}</span>
        <span class="label">${label}</span>
      </button>
    `;
  }

  private switchView(v: View): void {
    this.view = v;
    if (v === "home" && this.onReader) void this.loadOverview();
  }

  private renderView() {
    switch (this.view) {
      case "home":
        return this.renderHome();
      case "library":
        return html`<h1>${tr("nav.books")}</h1><library-panel></library-panel>`;
      case "converter":
        return html`<h1>${tr("nav.convert")}</h1><converter-panel></converter-panel>`;
      case "plugins":
        return html`<h1>${tr("nav.plugins")}</h1><plugins-panel></plugins-panel>`;
      case "more":
        return html`<h1>${tr("nav.more")}</h1>
          <settings-panel .readerFirmware=${this.readerFirmware} .connection=${this.link?.transport.label ?? ""}></settings-panel>`;
    }
  }

  // ─── Start ───────────────────────────────────────────────────────────────

  private renderHome() {
    return html`
      ${!this.connected ? this.renderConnectChoice() : this.renderConnectedHome()}
      ${this.error ? html`<p class="error">${this.error}</p>` : nothing}
    `;
  }

  private renderConnectChoice() {
    if (this.chosenTransport) return this.renderConnecting();
    const btSupported = BluetoothLink.isSupported();
    const serialSupported = SerialLink.isSupported();
    return html`
      <section class="hero">
        <span class="hero-mark">${dandelionIcon(72)}</span>
        <h2>${tr("home.offline.title")}</h2>
        <p>${tr("home.offline.lead")}</p>
      </section>

      <div class="list">
        <button class="item choice" @click=${() => this.pickTransport("wifi")}>
          <span class="tile-ico">${icons.wifi(22)}</span>
          <span class="label">
            <span class="title">Wi-Fi <em class="pill">${tr("home.recommended")}</em></span>
            <small>${tr("home.wifi.desc")}</small>
          </span>
          <span class="chev">${icons.chevronRight()}</span>
        </button>
        <button class="item choice" ?disabled=${!btSupported} @click=${() => this.pickTransport("bluetooth")}>
          <span class="tile-ico">${icons.bluetooth(22)}</span>
          <span class="label">
            <span class="title">Bluetooth</span>
            <small>${btSupported ? tr("home.bt.desc") : tr("home.bt.unsupported")}</small>
          </span>
          <span class="chev">${icons.chevronRight()}</span>
        </button>
        ${this.showAdvanced
          ? html`<button class="item choice" ?disabled=${!serialSupported} @click=${() => this.pickTransport("serial")}>
              <span class="tile-ico">${icons.usb(22)}</span>
              <span class="label">
                <span class="title">USB</span>
                <small>${serialSupported ? tr("home.usb.desc") : tr("home.usb.unsupported")}</small>
              </span>
              <span class="chev">${icons.chevronRight()}</span>
            </button>`
          : nothing}
      </div>
      <button class="btn quiet small center" @click=${() => (this.showAdvanced = !this.showAdvanced)}>
        ${this.showAdvanced ? tr("home.advanced.hide") : tr("home.advanced.show")}
      </button>
    `;
  }

  private renderConnecting() {
    const label = this.chosenTransport === "wifi" ? "Wi-Fi" : this.chosenTransport === "bluetooth" ? "Bluetooth" : "USB";
    const nativeJoin = this.chosenTransport === "wifi" && isNativeApp() && !this.joinUnsupported;
    return html`
      <section class="card">
        <h2 class="card-title">${tr("connect.title", { method: label })}</h2>
        ${this.chosenTransport === "wifi"
          ? html`
              <ol class="steps">
                <li>${trHtml("connect.step.open")}</li>
                ${nativeJoin
                  ? html`<li>${trHtml("connect.step.join")}</li>
                      ${this.qrFallbackVisible
                        ? html`<li class="callout">
                            <span>${tr("connect.qr.fallback")}</span>
                            <button class="btn small" ?disabled=${this.scanningQr} @click=${this.openQrScanner}>
                              ${tr("connect.qr.scan")}
                            </button>
                          </li>`
                        : nothing}`
                  : html`
                      <li>
                        <span>${trHtml("connect.step.manual")}</span>
                        <button class="btn small" @click=${() => void openWifiSettings()}>${tr("connect.openWifi")}</button>
                      </li>
                      <li class="callout"><span>${trHtml("connect.noInternet")}</span></li>
                      <li>${trHtml("connect.return")}</li>
                    `}
              </ol>
            `
          : html`<p class="muted">
              ${this.chosenTransport === "bluetooth" ? tr("connect.pickBt") : tr("connect.pickUsb")}
            </p>`}
        <div class="row">
          <button class="btn quiet" @click=${this.cancelChoice}>${tr("common.back")}</button>
          <button class="btn primary" ?disabled=${this.connecting} @click=${nativeJoin ? this.joinAndConnect : this.connect}>
            ${this.connecting ? tr("connect.connecting") : nativeJoin ? tr("connect.join") : tr("connect.check")}
          </button>
        </div>
        ${this.scanningQr
          ? html`<qr-scanner @qr-result=${this.onQrResult} @qr-cancel=${() => (this.scanningQr = false)}></qr-scanner>`
          : nothing}
      </section>
    `;
  }

  private renderConnectedHome() {
    const current = this.books.find((b) => b.current);
    const title = current ? current.title || current.name.replace(/^.*\//, "").replace(/\.[^.]+$/, "") : "";
    const percent = current?.progressPercent ?? 0;
    return html`
      <section class="now">
        <span class="section-title">${tr("home.reading")}</span>
        ${current
          ? html`<button class="now-card" @click=${() => this.switchView("library")}>
              <span
                class="cover"
                style=${this.currentCover
                  ? `background-image:url(${this.currentCover})`
                  : `background:${readerCoverColor(current.name)}`}
                >${this.currentCover ? nothing : readerInitials(title)}</span
              >
              <span class="now-text">
                <strong>${title}</strong>
                <small>${current.author || tr("common.unknownAuthor")}</small>
                <span class="progress"><span style="width:${percent}%"></span></span>
                <small class="pct">${tr("home.read", { n: percent })}</small>
              </span>
            </button>`
          : html`<p class="notice">${tr("home.noBook")}</p>`}
      </section>

      <section class="tiles">
        ${this.tile(icons.upload(24), tr("home.tile.send"), tr("home.tile.send.desc"), "library")}
        ${this.tile(icons.convert(24), tr("nav.convert"), tr("home.tile.convert.desc"), "converter")}
        ${this.tile(icons.books(24), tr("home.tile.library"), tr("home.tile.library.desc", { n: this.books.length }), "library")}
        ${this.tile(icons.more(24), tr("home.tile.settings"), tr("home.tile.settings.desc"), "more")}
      </section>

      ${this.info
        ? html`<section class="device">
            <span class="section-title">${tr("home.device")}</span>
            <div class="stats">
              <div><span>${icons.battery(18)}</span><strong>${this.info.batteryPercent}%</strong><small>${tr("home.battery")}</small></div>
              <div>
                <span>${icons.sd(18)}</span><strong>${formatKb(this.info.sdFreeKb)}</strong>
                <small>${tr("home.cardFree")}</small>
              </div>
              <div><span>${icons.chip(18)}</span><strong>${this.info.firmwareVersion || this.readerFirmware}</strong><small>${tr("home.firmware")}</small></div>
            </div>
          </section>`
        : nothing}

      <button class="btn quiet center" @click=${this.disconnect}>${tr("home.disconnect")}</button>
    `;
  }

  private tile(icon: unknown, title: string, desc: string, view: View) {
    return html`<button class="tile" @click=${() => this.switchView(view)}>
      <span class="tile-ico">${icon}</span>
      <strong>${title}</strong>
      <small>${desc}</small>
    </button>`;
  }

  // ─── Connection ──────────────────────────────────────────────────────────

  private pickTransport(t: Transport) {
    this.chosenTransport = t;
    this.error = null;
  }

  private cancelChoice = () => {
    this.chosenTransport = null;
    this.error = null;
    this.resetQrFallback();
  };

  private resetQrFallback() {
    if (this.qrFallbackTimer !== null) {
      clearTimeout(this.qrFallbackTimer);
      this.qrFallbackTimer = null;
    }
    this.qrFallbackVisible = false;
    this.scanningQr = false;
  }

  /**
   * Android 10+: the system dialog joins "Flower-…" and pins the app to it,
   * no trip to the Wi-Fi settings. Older Android falls back to the manual
   * steps.
   */
  private joinAndConnect = async () => {
    this.error = null;
    this.connecting = true;
    // The clock starts at the first press and keeps running across retries:
    // 20 s without the reader's network brings up the QR scanner option.
    if (this.qrFallbackTimer === null && !this.qrFallbackVisible) {
      this.qrFallbackTimer = setTimeout(() => {
        this.qrFallbackTimer = null;
        if (!this.connected) this.qrFallbackVisible = true;
      }, 20000);
    }
    const joined = await joinReaderNetwork();
    if (joined.replaced) {
      // The QR scanner took over with the exact network; it finishes the job.
      return;
    }
    this.connecting = false;
    if (!joined.supported) {
      this.joinUnsupported = true;
      return;
    }
    if (!joined.connected) {
      this.error = tr("connect.err.join");
      return;
    }
    this.resetQrFallback();
    await this.connect();
  };

  private openQrScanner = () => {
    this.error = null;
    this.scanningQr = true;
  };

  private onQrResult = async (event: CustomEvent<{ text: string }>) => {
    this.scanningQr = false;
    const network = readerNetworkFromQr(event.detail.text);
    if (!network) {
      this.error = tr("connect.err.qr");
      return;
    }
    this.connecting = true;
    const joined = await joinNetworkFromQr(network.ssid, network.password);
    this.connecting = false;
    if (!joined.connected) {
      this.error = tr("connect.err.qrJoin", { ssid: network.ssid });
      return;
    }
    this.resetQrFallback();
    await this.connect();
  };

  private connect = async () => {
    if (!this.chosenTransport) return;
    this.error = null;
    this.connecting = true;
    try {
      this.link =
        this.chosenTransport === "wifi"
          ? new WifiLink()
          : this.chosenTransport === "bluetooth"
            ? new BluetoothLink()
            : new SerialLink();
      await this.link.connect();
      this.connected = true;
      this.readerFirmware = this.link instanceof WifiLink ? (this.link.hello?.firmwareVersion ?? "") : "";
      if (getTutorialStatus() === "not_seen") this.showTutorial = true;
      // Wi-Fi switches every screen to the reader's HTTP API (Bluetooth and
      // USB have no such API yet).
      if (this.chosenTransport === "wifi") {
        setDeviceApi(new HttpDeviceApi());
        this.startHeartbeat();
      }
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
      this.link = null;
    } finally {
      this.connecting = false;
    }
  };

  private disconnect = async () => {
    this.stopHeartbeat();
    await this.link?.disconnect();
    this.link = null;
    this.connected = false;
    this.readerFirmware = "";
    this.chosenTransport = null;
    this.books = [];
    this.info = null;
    this.view = "home";
    const { MockDeviceApi } = await import("./device/api");
    setDeviceApi(new MockDeviceApi());
  };

  /**
   * hello every 8 s (docs/flower-companion-api.md): a reader that restarted
   * or left the App screen shows as disconnected right away, not at the
   * next tap.
   */
  private startHeartbeat(): void {
    this.stopHeartbeat();
    this.heartbeatTimer = setInterval(() => void this.checkHeartbeat(), 8000);
  }

  private stopHeartbeat(): void {
    if (this.heartbeatTimer !== null) {
      clearInterval(this.heartbeatTimer);
      this.heartbeatTimer = null;
    }
  }

  private checkHeartbeat = async () => {
    if (!this.connected || this.chosenTransport !== "wifi") return;
    if (this.link && !this.link.connected) {
      await this.handleLinkLost();
      return;
    }
    for (let attempt = 0; attempt < 3; attempt++) {
      if (await helloDevice()) return;
    }
    await this.handleLinkLost();
  };

  private handleLinkLost = async () => {
    this.stopHeartbeat();
    await this.link?.disconnect().catch(() => {});
    this.link = null;
    this.connected = false;
    this.readerFirmware = "";
    this.chosenTransport = null;
    this.books = [];
    this.info = null;
    this.error = tr("connect.err.lost");
    const { MockDeviceApi } = await import("./device/api");
    setDeviceApi(new MockDeviceApi());
  };

  // ─── Styles ──────────────────────────────────────────────────────────────

  static styles = [
    themeTokens,
    sharedStyles,
    css`
      :host {
        display: flex;
        flex-direction: column;
        height: 100vh;
        height: 100dvh;
        overflow: hidden;
        background: var(--bg);
        color: var(--text);
        font-family: var(--font);
      }

      header {
        flex: 0 0 auto;
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 10px;
        padding: calc(10px + var(--safe-top)) 16px 10px;
        background: var(--bg);
      }
      .brand {
        display: flex;
        align-items: center;
        gap: 8px;
        padding: 4px 0;
        border: 0;
        background: transparent;
        color: var(--text);
        cursor: pointer;
      }
      .brand .mark {
        color: var(--accent-text);
        display: grid;
      }
      .brand strong {
        font-size: 1.2rem;
        font-weight: 600;
        letter-spacing: -0.01em;
      }
      .badges {
        display: flex;
        align-items: center;
        gap: 6px;
        min-width: 0;
      }
      .badge.dev {
        padding: 4px 8px;
        border-radius: 6px;
        background: #ff7a45;
        color: #fff;
        font-size: 0.7rem;
        font-weight: 700;
      }
      .status {
        display: inline-flex;
        align-items: center;
        gap: 7px;
        padding: 6px 12px;
        border-radius: var(--radius-pill);
        background: var(--surface);
        color: var(--muted);
        font-size: 0.78rem;
        font-weight: 600;
        white-space: nowrap;
      }
      .status .dot {
        width: 8px;
        height: 8px;
        border-radius: 50%;
        background: currentColor;
      }
      .status.on {
        color: var(--ok);
      }

      main {
        flex: 1 1 auto;
        min-height: 0;
        overflow-y: auto;
        overflow-x: hidden;
        -webkit-overflow-scrolling: touch;
        overscroll-behavior: contain;
      }
      .view {
        display: flex;
        flex-direction: column;
        gap: 14px;
        max-width: 640px;
        margin: 0 auto;
        padding: 4px 16px 24px;
        animation: view-in 0.22s cubic-bezier(0.22, 1, 0.36, 1);
      }
      @keyframes view-in {
        from {
          opacity: 0;
          transform: translateY(6px);
        }
        to {
          opacity: 1;
          transform: none;
        }
      }
      @media (prefers-reduced-motion: reduce) {
        .view {
          animation: none;
        }
      }
      h1 {
        margin: 6px 2px 0;
        font-size: 1.7rem;
        font-weight: 700;
        letter-spacing: -0.02em;
      }

      .hero {
        display: flex;
        flex-direction: column;
        align-items: center;
        text-align: center;
        gap: 10px;
        padding: 18px 8px 6px;
      }
      .hero-mark {
        color: var(--accent-text);
      }
      .hero h2 {
        font-size: 1.6rem;
        font-weight: 700;
        letter-spacing: -0.02em;
      }
      .hero p {
        max-width: 34ch;
        color: var(--text-2);
        line-height: 1.5;
      }

      .tile-ico {
        flex: 0 0 auto;
        width: 42px;
        height: 42px;
        display: grid;
        place-items: center;
        border-radius: 12px;
        background: var(--accent-soft);
        color: var(--accent-text);
      }
      .choice {
        min-height: 72px;
      }
      .choice:disabled {
        opacity: 0.5;
        cursor: default;
      }
      .choice .title,
      .item .title {
        display: flex;
        align-items: center;
        gap: 8px;
        font-weight: 600;
      }
      .chev {
        color: var(--muted);
        display: grid;
      }
      .pill {
        padding: 3px 9px;
        border-radius: var(--radius-pill);
        background: var(--surface-3);
        color: var(--text-2);
        font-size: 0.72rem;
        font-style: normal;
        font-weight: 600;
        white-space: nowrap;
      }
      .pill.on,
      em.pill {
        background: var(--accent-soft);
        color: var(--accent-text);
      }
      .center {
        align-self: center;
      }

      .card-title {
        font-size: 1.2rem;
      }
      .steps {
        margin: 0;
        padding: 0;
        list-style: none;
        counter-reset: step;
        display: flex;
        flex-direction: column;
        gap: 12px;
        color: var(--text-2);
        line-height: 1.5;
      }
      .steps li {
        position: relative;
        padding-left: 38px;
        display: flex;
        flex-direction: column;
        align-items: flex-start;
        gap: 8px;
      }
      .steps li:not(.callout)::before {
        counter-increment: step;
        content: counter(step);
        position: absolute;
        left: 0;
        top: -1px;
        width: 26px;
        height: 26px;
        display: grid;
        place-items: center;
        border-radius: 50%;
        background: var(--accent);
        color: var(--on-accent);
        font-size: 0.8rem;
        font-weight: 700;
      }
      .steps li.callout {
        padding: 12px 14px;
        border-radius: var(--radius-sm);
        background: var(--surface-2);
      }
      .steps b {
        color: var(--text);
      }
      code {
        padding: 1px 6px;
        border-radius: 6px;
        background: var(--surface-3);
        color: var(--text);
        font-family: var(--font-mono);
        font-size: 0.88em;
      }

      .now {
        display: flex;
        flex-direction: column;
        gap: 8px;
      }
      .now-card {
        display: flex;
        align-items: center;
        gap: 14px;
        padding: 14px;
        border: 0;
        border-radius: var(--radius-lg);
        background: var(--surface);
        color: var(--text);
        text-align: left;
        cursor: pointer;
      }
      .cover {
        position: relative;
        flex: 0 0 auto;
        width: 62px;
        height: 78px;
        display: grid;
        place-items: center;
        padding-left: 8px;
        border-radius: 7px;
        background-size: cover;
        background-position: center;
        color: #fff;
        font-size: 1.2rem;
        font-weight: 600;
        overflow: hidden;
      }
      .cover::after {
        content: "";
        position: absolute;
        top: 0;
        bottom: 0;
        left: 7px;
        width: 2px;
        background: rgba(8, 8, 10, 0.62);
      }
      .now-text {
        flex: 1 1 auto;
        min-width: 0;
        display: flex;
        flex-direction: column;
        gap: 4px;
      }
      .now-text strong {
        font-size: 1.05rem;
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
      }
      .now-text small {
        color: var(--muted);
        font-size: 0.82rem;
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
      }
      .now-text .progress {
        margin-top: 6px;
      }
      .now-text .pct {
        color: var(--accent-text);
        font-weight: 600;
      }

      .tiles {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 10px;
      }
      .tile {
        display: flex;
        flex-direction: column;
        align-items: flex-start;
        gap: 4px;
        min-height: 122px;
        padding: 14px;
        border: 0;
        border-radius: var(--radius-lg);
        background: var(--surface);
        color: var(--text);
        text-align: left;
        cursor: pointer;
      }
      .tile:active {
        background: var(--surface-2);
      }
      .tile .tile-ico {
        margin-bottom: 6px;
      }
      .tile strong {
        font-size: 0.98rem;
      }
      .tile small {
        color: var(--muted);
        font-size: 0.8rem;
        line-height: 1.35;
      }

      .device {
        display: flex;
        flex-direction: column;
        gap: 8px;
      }
      .stats {
        display: grid;
        grid-template-columns: repeat(3, 1fr);
        gap: 1px;
        border-radius: var(--radius-lg);
        background: var(--line);
        overflow: hidden;
      }
      .stats div {
        display: flex;
        flex-direction: column;
        align-items: center;
        gap: 4px;
        min-width: 0;
        padding: 14px 6px;
        background: var(--surface);
        text-align: center;
      }
      .stats span {
        color: var(--accent-text);
        display: grid;
      }
      .stats strong {
        max-width: 100%;
        font-size: 0.95rem;
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
      }
      .stats small {
        color: var(--muted);
        font-size: 0.74rem;
      }

      .feed {
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
        color: var(--text-2);
        font-size: 0.86rem;
      }

      nav {
        flex: 0 0 auto;
        display: grid;
        grid-template-columns: repeat(5, 1fr);
        padding: 6px 4px calc(6px + var(--safe-bottom));
        background: var(--bg);
        border-top: 1px solid var(--line);
      }
      nav button {
        display: flex;
        flex-direction: column;
        align-items: center;
        gap: 3px;
        min-width: 0;
        padding: 4px 2px;
        border: 0;
        background: transparent;
        color: var(--muted);
        cursor: pointer;
      }
      nav .ico {
        width: 56px;
        height: 30px;
        display: grid;
        place-items: center;
        border-radius: var(--radius-pill);
        transition: background 0.15s ease;
      }
      nav .label {
        max-width: 100%;
        font-size: 0.7rem;
        font-weight: 600;
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
      }
      nav button.on {
        color: var(--text);
      }
      nav button.on .ico {
        background: var(--accent);
        color: var(--on-accent);
      }
    `,
  ];
}

function formatKb(kb: number): string {
  if (kb < 1024) return `${kb} kB`;
  if (kb < 1024 * 1024) return `${(kb / 1024).toFixed(0)} MB`;
  return `${(kb / 1024 / 1024).toFixed(1)} GB`;
}

declare global {
  interface HTMLElementTagNameMap {
    "czytnik-app": CzytnikApp;
  }
}
