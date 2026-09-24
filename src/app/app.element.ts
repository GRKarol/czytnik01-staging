import { LitElement, css, html, svg } from "lit";
import { customElement, state } from "lit/decorators.js";
import { keyed } from "lit/directives/keyed.js";
import { BRAND_NAME, DEVICE_LABEL, APP_VERSION } from "../shared/config";
import type { DeviceLink } from "./device/device-link";
import { WifiLink } from "./device/wifi-link";
import { BluetoothLink } from "./device/bluetooth-link";
import { SerialLink } from "./device/serial-link";
import { dandelionIcon } from "./components/flower-icon";
import "./components/converter-panel.element";
import "./components/updates-panel.element";
import "./components/library-panel.element";
import "./components/settings-panel.element";
import "./components/onboarding.element";
import "./components/pwa-install-dialog.element";
import "./components/tutorial-wizard.element";
import {
  deviceApi,
  onDeviceApiChange,
  setDeviceApi,
  type DeviceSettings,
  type PluginInfo,
} from "./device/api";
import { HttpDeviceApi, pingDevice } from "./device/http-api";
import { getTutorialStatus } from "./onboarding/onboarding-store";

type View = "home" | "library" | "converter" | "plugins" | "updates" | "settings";
type Transport = "wifi" | "bluetooth" | "serial";

const iconHome = (s = 24) => svg`
  <svg width=${s} height=${s} viewBox="0 0 24 24" fill="none"
       stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">
    <path d="M3 11l9-8 9 8v10a2 2 0 0 1-2 2h-4v-7H10v7H5a2 2 0 0 1-2-2z"/>
  </svg>
`;
const iconBook = (s = 24) => svg`
  <svg width=${s} height=${s} viewBox="0 0 24 24" fill="none"
       stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">
    <path d="M4 4h6a4 4 0 0 1 4 4v12a3 3 0 0 0-3-3H4z"/>
    <path d="M20 4h-6a4 4 0 0 0-4 4v12a3 3 0 0 1 3-3h7z"/>
  </svg>
`;
const iconConvert = (s = 24) => svg`
  <svg width=${s} height=${s} viewBox="0 0 24 24" fill="none"
       stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">
    <path d="M5 9h11l-3-3"/>
    <path d="M19 15H8l3 3"/>
  </svg>
`;
const iconPlug = (s = 24) => svg`
  <svg width=${s} height=${s} viewBox="0 0 24 24" fill="none"
       stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">
    <path d="M9 7V3M15 7V3"/>
    <rect x="7" y="7" width="10" height="8" rx="2"/>
    <path d="M12 15v4"/>
  </svg>
`;
const iconUpdate = (s = 24) => svg`
  <svg width=${s} height=${s} viewBox="0 0 24 24" fill="none"
       stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">
    <path d="M21 12a9 9 0 1 1-3-6.7"/>
    <path d="M21 4v5h-5"/>
  </svg>
`;
const iconGear = (s = 24) => svg`
  <svg width=${s} height=${s} viewBox="0 0 24 24" fill="none"
       stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">
    <circle cx="12" cy="12" r="3"/>
    <path d="M19 12a7 7 0 0 0-.1-1.2l2-1.6-2-3.5-2.4.9a7 7 0 0 0-2-1.2L14 3h-4l-.5 2.4a7 7 0 0 0-2 1.2L5 5.7l-2 3.5 2 1.6A7 7 0 0 0 5 12c0 .4 0 .8.1 1.2l-2 1.6 2 3.5 2.4-.9a7 7 0 0 0 2 1.2L10 21h4l.5-2.4a7 7 0 0 0 2-1.2l2.4.9 2-3.5-2-1.6c.1-.4.1-.8.1-1.2z"/>
  </svg>
`;
const iconWifi = (s = 28) => svg`
  <svg width=${s} height=${s} viewBox="0 0 24 24" fill="none"
       stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
    <path d="M2 8.5a17 17 0 0 1 20 0"/>
    <path d="M5 12a13 13 0 0 1 14 0"/>
    <path d="M8.5 15.5a8 8 0 0 1 7 0"/>
    <circle cx="12" cy="19" r="1.2" fill="currentColor"/>
  </svg>
`;
const iconBt = (s = 28) => svg`
  <svg width=${s} height=${s} viewBox="0 0 24 24" fill="none"
       stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
    <path d="M7 7l10 10-5 4V3l5 4L7 17"/>
  </svg>
`;
const iconUsb = (s = 28) => svg`
  <svg width=${s} height=${s} viewBox="0 0 24 24" fill="none"
       stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
    <circle cx="12" cy="4" r="1.5"/>
    <path d="M12 5.5V20"/>
    <path d="M12 14l-4-4h3V8"/>
    <path d="M12 12l4-2h-3V8"/>
    <rect x="9" y="20" width="6" height="2" rx="1"/>
  </svg>
`;
const iconFlower = dandelionIcon;

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

  @state() private plugins: PluginInfo[] = [];
  @state() private pluginsLoading = false;
  @state() private pluginsError = "";
  @state() private rssFeeds: string[] = [];
  @state() private rssBusy = false;
  @state() private newFeedUrl = "";

  private link: DeviceLink | null = null;
  private unsubApi: (() => void) | null = null;
  private heartbeatTimer: ReturnType<typeof setInterval> | null = null;

  connectedCallback(): void {
    super.connectedCallback();
    this.refreshDevMode();
    this.unsubApi = onDeviceApiChange(() => this.refreshDevMode());
    this.addEventListener("device-settings-changed", () => this.refreshDevMode());
    this.addEventListener("tutorial-close", this.handleTutorialClose);
    this.addEventListener("restart-tutorial", this.handleRestartTutorial);
    // Handle Web Share Target: if we were opened via share intent with a file
    this.handleSharedFile();
  }

  disconnectedCallback(): void {
    super.disconnectedCallback();
    this.stopHeartbeat();
    this.unsubApi?.();
    this.removeEventListener("tutorial-close", this.handleTutorialClose);
    this.removeEventListener("restart-tutorial", this.handleRestartTutorial);
  }

  private handleTutorialClose = () => {
    this.showTutorial = false;
  };

  private handleRestartTutorial = () => {
    this.showTutorial = true;
  };

  /** Sprawdza w API czy dev mode jest włączony — odświeża badge w header. */
  private async refreshDevMode(): Promise<void> {
    try {
      const s: DeviceSettings = await deviceApi.getSettings();
      this.devMode = s.devMode;
    } catch {
      this.devMode = false;
    }
  }

  /** Handle Web Share Target: read shared file from IndexedDB and upload */
  private async handleSharedFile(): Promise<void> {
    const params = new URLSearchParams(window.location.search);
    const sharedId = params.get("shared");
    if (!sharedId) return;

    // Clean the URL
    const url = new URL(window.location.href);
    url.searchParams.delete("shared");
    window.history.replaceState(null, "", url.toString());

    // Switch to library view
    this.view = "library";

    try {
      const entry = await this.readSharedFileFromDb(sharedId);
      if (!entry) return;

      const file = entry.file as File;
      const name = (entry.name as string) || file.name;

      // Upload via deviceApi (mock or real depending on connection)
      await deviceApi.uploadBook(file, name);

      // Clean up from IndexedDB
      await this.removeSharedFileFromDb(sharedId);
    } catch (err) {
      this.error = `Nie udało się wgrać udostępnionego pliku: ${err instanceof Error ? err.message : String(err)}`;
    }
  }

  private readSharedFileFromDb(id: string): Promise<Record<string, unknown> | null> {
    return new Promise((resolve, reject) => {
      const request = indexedDB.open("flower-share", 1);
      request.onupgradeneeded = () => {
        const db = request.result;
        if (!db.objectStoreNames.contains("pending-files")) {
          db.createObjectStore("pending-files");
        }
      };
      request.onsuccess = () => {
        const db = request.result;
        const tx = db.transaction("pending-files", "readonly");
        const store = tx.objectStore("pending-files");
        const get = store.get(id);
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

  render() {
    return html`
      <onboarding-wizard></onboarding-wizard>
      <pwa-install-dialog></pwa-install-dialog>
      ${this.showTutorial ? html`<tutorial-wizard></tutorial-wizard>` : ""}

      <header>
        <div class="brand">
          <span class="flower">${iconFlower(28)}</span>
          <span>
            <strong>${BRAND_NAME}</strong>
            <small>v${APP_VERSION}</small>
          </span>
        </div>
        <div class="badges">
          ${this.devMode ? html`<span class="badge dev">DEV</span>` : ""}
          <div class=${`pill ${this.connected ? "ok" : ""}`}>
            <span class="dot"></span>
            ${this.connected ? `Połączono · ${this.link?.transport.label}` : "Brak połączenia"}
          </div>
        </div>
      </header>

      <main><div class="view-anim">${keyed(this.view, this.renderView())}</div></main>

      <nav>
        ${this.navButton("home", "Start", iconHome())}
        ${this.navButton("library", "Książki", iconBook())}
        ${this.navButton("converter", "Konwerter", iconConvert())}
        ${this.navButton("plugins", "Pluginy", iconPlug())}
        ${this.navButton("updates", "Aktualizacje", iconUpdate())}
        ${this.navButton("settings", "Więcej", iconGear())}
      </nav>
    `;
  }

  private navButton(v: View, label: string, ico: unknown, disabled = false) {
    return html`
      <button
        class=${this.view === v ? "active" : ""}
        ?disabled=${disabled}
        @click=${() => this.switchView(v)}
      >
        <span class="ico">${ico}</span>
        ${label}
      </button>
    `;
  }

  private switchView(v: View): void {
    this.view = v;
    if (v === "plugins") void this.loadPlugins();
  }

  private renderView() {
    switch (this.view) {
      case "home":
        return this.renderHome();
      case "library":
        return this.renderLibrary();
      case "converter":
        return this.renderConverter();
      case "plugins":
        return this.renderPlugins();
      case "updates":
        return this.renderUpdates();
      case "settings":
        return this.renderSettings();
    }
  }

  // ─── Home ──────────────────────────────────────────────────────────────────

  private renderHome() {
    return html`
      <section class="hero">
        <div class="hero-flower">${iconFlower(96)}</div>
        <h2>Cześć!</h2>
        <p>
          To aplikacja Twojego ${DEVICE_LABEL.toLowerCase()}a <strong>${BRAND_NAME}</strong>.
          Wysyłaj książki, instaluj pluginy i aktualizuj urządzenie — wszystko bezprzewodowo.
        </p>
      </section>

      ${!this.connected ? this.renderConnectChoice() : this.renderConnectedActions()}
      ${this.error ? html`<p class="error">${this.error}</p>` : ""}
    `;
  }

  private renderConnectChoice() {
    if (this.chosenTransport) return this.renderConnecting();

    const btSupported = BluetoothLink.isSupported();
    const serialSupported = SerialLink.isSupported();

    return html`
      <section class="card">
        <h3>Połącz urządzenie</h3>
        <p class="muted">Wybierz sposób połączenia z ${DEVICE_LABEL.toLowerCase()}em.</p>

        <button class="choice" @click=${() => this.pickTransport("wifi")}>
          <span class="choice-ico">${iconWifi()}</span>
          <span class="choice-body">
            <strong>WiFi</strong>
            <span>Polecane. Działa na iPhonie i Androidzie. Szybki transfer książek.</span>
          </span>
        </button>

        <button
          class="choice"
          ?disabled=${!btSupported}
          @click=${() => this.pickTransport("bluetooth")}
        >
          <span class="choice-ico">${iconBt()}</span>
          <span class="choice-body">
            <strong>Bluetooth</strong>
            <span>
              ${btSupported
                ? "Bonus dla Androida. Idealny do drobnych komend."
                : "Niewspierany w tej przeglądarce (iOS nie obsługuje Web Bluetooth)."}
            </span>
          </span>
        </button>

        <button class="link-button" @click=${() => (this.showAdvanced = !this.showAdvanced)}>
          ${this.showAdvanced ? "Schowaj tryb zaawansowany" : "Tryb zaawansowany"}
        </button>

        ${this.showAdvanced
          ? html`
              <button
                class="choice subtle"
                ?disabled=${!serialSupported}
                @click=${() => this.pickTransport("serial")}
              >
                <span class="choice-ico">${iconUsb()}</span>
                <span class="choice-body">
                  <strong>USB</strong>
                  <span>
                    ${serialSupported
                      ? "Diagnostyka / serwis. Wymaga kabla USB-C i Chrome/Edge na desktopie."
                      : "Web Serial niewspierany — użyj Chrome lub Edge na desktopie."}
                  </span>
                </span>
              </button>
            `
          : ""}
      </section>
    `;
  }

  private renderConnecting() {
    const label =
      this.chosenTransport === "wifi"
        ? "WiFi"
        : this.chosenTransport === "bluetooth"
          ? "Bluetooth"
          : "USB";
    return html`
      <section class="card">
        <h3>Łączenie przez ${label}…</h3>
        ${this.chosenTransport === "wifi"
          ? html`
              <ol class="steps">
                <li>Włącz urządzenie i poczekaj, aż wyświetli kod sieci (np. <code>Flower-AB12</code>).</li>
                <li>
                  Otwórz ustawienia WiFi telefonu i wybierz tę sieć.
                  <button class="cta ghost small" @click=${this.openSystemWifiSettings}>
                    Otwórz ustawienia WiFi
                  </button>
                </li>
                <li class="callout">
                  Telefon prawdopodobnie ostrzeże „Połączono, brak internetu" —
                  to normalne, czytnik nie ma dostępu do internetu, tylko
                  lokalne WiFi. Wybierz <strong>„Połącz mimo to"</strong> albo
                  <strong>„Zostań połączony"</strong> — inaczej telefon sam się
                  rozłączy i przeskoczy na inną sieć.
                </li>
                <li>Wróć tutaj i naciśnij „Sprawdź połączenie".</li>
              </ol>
            `
          : this.chosenTransport === "bluetooth"
            ? html`<p class="muted">Wybierz urządzenie w okienku przeglądarki.</p>`
            : html`<p class="muted">Wybierz port USB w okienku przeglądarki.</p>`}

        <div class="row">
          <button class="cta" ?disabled=${this.connecting} @click=${this.connect}>
            ${this.connecting ? "Łączenie…" : "Sprawdź połączenie"}
          </button>
          <button class="cta ghost" @click=${this.cancelChoice}>Wróć</button>
        </div>
      </section>
    `;
  }

  private renderConnectedActions() {
    return html`
      <section class="card">
        <h3>Co robimy?</h3>
        <div class="grid">
          <button class="tile" @click=${() => (this.view = "library")}>
            <span class="tile-ico">${iconBook(28)}</span>
            <strong>Książki</strong>
            <span>Wyślij nowe, zarządzaj biblioteką.</span>
          </button>
          <button class="tile" @click=${() => (this.view = "converter")}>
            <span class="tile-ico">${iconConvert(28)}</span>
            <strong>Konwerter</strong>
            <span>EPUB · PDF · MOBI · TXT → .rsvp</span>
          </button>
          <button class="tile" @click=${() => this.switchView("plugins")}>
            <span class="tile-ico">${iconPlug(28)}</span>
            <strong>Pluginy</strong>
            <span>Klepsydra, dyktafon i więcej.</span>
          </button>
          <button class="tile" @click=${() => (this.view = "updates")}>
            <span class="tile-ico">${iconUpdate(28)}</span>
            <strong>Aktualizacje</strong>
            <span>Sprawdź nowe wersje firmware.</span>
          </button>
        </div>
        <button class="cta ghost" @click=${this.disconnect}>Rozłącz</button>
      </section>
    `;
  }

  // ─── Pozostałe ekrany — szkielety, logika idzie w kolejnych rundach ─────

  private renderLibrary() {
    return html`
      <section class="card">
        <h3>${iconBook(22)} Książki</h3>
        <library-panel></library-panel>
      </section>
    `;
  }

  private renderConverter() {
    return html`
      <section class="card">
        <h3>${iconConvert(22)} Konwerter</h3>
        <p class="muted">
          Wybierz plik z telefonu — przekonwertujemy go na format
          <code>.rsvp</code> w przeglądarce, bez wysyłania nigdzie.
        </p>
        <converter-panel></converter-panel>
      </section>
    `;
  }

  private renderPlugins() {
    const focusTimer = this.plugins.find((p) => p.id === "focus-timer");
    const rss = this.plugins.find((p) => p.id === "rss");
    return html`
      <section class="card">
        <h3>${iconPlug(22)} Pluginy</h3>
        <p class="muted">
          Dodatkowe funkcje wgrane na urządzeniu. Nowe pluginy pojawiają się tu sukcesywnie.
        </p>
        ${this.pluginsError ? html`<p class="error">${this.pluginsError}</p>` : ""}
        ${!this.connected
          ? html`<p class="muted">Połącz się z czytnikiem, żeby zobaczyć realny stan pluginów.</p>`
          : this.pluginsLoading
            ? html`<p class="muted">Wczytuję…</p>`
            : html`
                <div class="plugin-list">
                  ${this.pluginCard(
                    "Klepsydra (Focus Timer)",
                    "Sesja czytania z timerem.",
                    focusTimer?.active ? "Aktywny" : "Niedostępny",
                  )}
                  ${this.pluginCard(
                    "RSS Feeds",
                    "Artykuły z Twoich subskrypcji trafiają na czytnik.",
                    rss?.active ? "Aktywny" : "Niedostępny",
                  )}
                  ${this.pluginCard(
                    "Dyktafon",
                    "Notatki głosowe podczas czytania — nie jest jeszcze zarejestrowany w firmware.",
                    "Niedostępne",
                  )}
                  ${this.pluginCard("Odtwarzacz muzyki", "Cicha muzyka tła z SD.", "Wkrótce")}
                </div>

                ${rss?.active ? this.renderRssEditor() : ""}
              `}
      </section>
    `;
  }

  private pluginCard(name: string, tagline: string, badge: string) {
    return html`
      <div class="plugin">
        <span class="plugin-ico">${iconFlower(36)}</span>
        <div class="plugin-body">
          <strong>${name}</strong>
          <span>${tagline}</span>
        </div>
        <span class=${`badge ${badge === "Aktywny" ? "ok" : ""}`}>${badge}</span>
      </div>
    `;
  }

  private renderRssEditor() {
    return html`
      <div class="rss-editor">
        <strong>Kanały RSS</strong>
        ${this.rssFeeds.length === 0
          ? html`<p class="muted">Brak dodanych kanałów.</p>`
          : html`<ul class="rss-list">
              ${this.rssFeeds.map(
                (url, i) => html`
                  <li>
                    <span>${url}</span>
                    <button
                      class="del"
                      ?disabled=${this.rssBusy}
                      @click=${() => this.removeRssFeed(i)}
                      aria-label="Usuń kanał"
                    >
                      ✕
                    </button>
                  </li>
                `,
              )}
            </ul>`}
        <div class="rss-add">
          <input
            type="url"
            placeholder="https://przyklad.pl/rss.xml"
            .value=${this.newFeedUrl}
            @input=${(e: Event) => (this.newFeedUrl = (e.target as HTMLInputElement).value)}
          />
          <button class="cta ghost small" ?disabled=${this.rssBusy} @click=${this.addRssFeed}>
            Dodaj
          </button>
        </div>
      </div>
    `;
  }

  private async loadPlugins(): Promise<void> {
    if (!this.connected) return;
    this.pluginsLoading = true;
    this.pluginsError = "";
    try {
      this.plugins = await deviceApi.getPlugins();
      this.rssFeeds = await deviceApi.getRssFeeds();
    } catch (err) {
      this.pluginsError = err instanceof Error ? err.message : String(err);
    } finally {
      this.pluginsLoading = false;
    }
  }

  private addRssFeed = async () => {
    const url = this.newFeedUrl.trim();
    if (!url) return;
    this.rssBusy = true;
    this.pluginsError = "";
    try {
      this.rssFeeds = await deviceApi.setRssFeeds([...this.rssFeeds, url]);
      this.newFeedUrl = "";
    } catch (err) {
      this.pluginsError = err instanceof Error ? err.message : String(err);
    } finally {
      this.rssBusy = false;
    }
  };

  private removeRssFeed = async (index: number) => {
    this.rssBusy = true;
    this.pluginsError = "";
    try {
      const next = this.rssFeeds.filter((_, i) => i !== index);
      this.rssFeeds = await deviceApi.setRssFeeds(next);
    } catch (err) {
      this.pluginsError = err instanceof Error ? err.message : String(err);
    } finally {
      this.rssBusy = false;
    }
  };

  private renderUpdates() {
    return html`
      <section class="card">
        <h3>${iconUpdate(22)} Aktualizacje</h3>
        <updates-panel></updates-panel>
      </section>
    `;
  }

  private renderSettings() {
    return html`
      <section class="card">
        <h3>${iconGear(22)} Więcej</h3>
        <settings-panel></settings-panel>
        <ul class="settings-list">
          <li><strong>Wersja aplikacji</strong><span>${APP_VERSION}</span></li>
          <li><strong>Marka</strong><span>${BRAND_NAME}</span></li>
          <li>
            <strong>Połączenie</strong>
            <span>${this.link?.transport.label ?? "—"}</span>
          </li>
        </ul>
      </section>
    `;
  }

  // ─── Logika połączenia ─────────────────────────────────────────────────────

  private pickTransport(t: Transport) {
    this.chosenTransport = t;
    this.error = null;
  }

  private cancelChoice = () => {
    this.chosenTransport = null;
    this.error = null;
  };

  /**
   * Best-effort otwarcie systemowych ustawień WiFi przez Android intent URI.
   * Działa w Chrome i w TWA (bo TWA to Chrome pod maską) — nie ma
   * standardowego web API do tego, więc na innych przeglądarkach/platformach
   * (iOS, desktop) ten link po prostu nic nie zrobi zamiast crashować.
   */
  private openSystemWifiSettings = () => {
    window.location.href = "intent:#Intent;action=android.settings.WIFI_SETTINGS;end";
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

      // Show tutorial wizard if not yet seen (after first device connection)
      if (getTutorialStatus() === "not_seen") {
        this.showTutorial = true;
      }

      // Przełącz API komponentów na real HTTP — biblioteka i ustawienia
      // od teraz gadają z urządzeniem zamiast z mockiem.
      // (BLE i USB jeszcze nie mają back-end API, więc dopiero WiFi to robi.)
      if (this.chosenTransport === "wifi") {
        const reachable = await pingDevice();
        if (reachable) setDeviceApi(new HttpDeviceApi());
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
    this.chosenTransport = null;
    this.view = "home";
    // Wróć do mocka — szybkie testy bez podłączonego urządzenia dalej działają.
    const { MockDeviceApi } = await import("./device/api");
    setDeviceApi(new MockDeviceApi());
  };

  /**
   * Doc `docs/flower-companion-api.md` §"Zasady niezawodności" mówi: hello
   * co 8s, timeout 3s, max 2 retry — nigdzie w kliencie to nie było
   * zaimplementowane. Bez tego prawdziwy rozłącz (restart po OTA, wyjście
   * z zasięgu WiFi) jest wykrywany dopiero gdy user coś kliknie i dostanie
   * wyjątek — pill "Połączono" w headerze kłamie do tego czasu.
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

    // WifiLink flips jej własny `connected` na false gdy WebSocket dostanie
    // "close" (np. reboot urządzenia po OTA) — sprawdź to najpierw, zanim
    // w ogóle uderzymy w sieć.
    if (this.link && !this.link.connected) {
      await this.handleLinkLost();
      return;
    }

    for (let attempt = 0; attempt < 3; attempt++) {
      if (await pingDevice()) return;
    }
    await this.handleLinkLost();
  };

  private handleLinkLost = async () => {
    this.stopHeartbeat();
    await this.link?.disconnect().catch(() => {});
    this.link = null;
    this.connected = false;
    this.chosenTransport = null;
    this.error = "Połączenie z czytnikiem zerwane. Sprawdź WiFi i połącz ponownie.";
    const { MockDeviceApi } = await import("./device/api");
    setDeviceApi(new MockDeviceApi());
  };

  // ─── Style ─────────────────────────────────────────────────────────────────

  static styles = css`
    :host {
      /* Paleta zainspirowana flower.theworkpc.com — ciepły papier zamiast
         jaskrawego błękitu, atrament zamiast granatu, przygaszony
         niebiesko-zielony akcent. Nazwy zmiennych zostają te same, więc
         wszystkie komponenty potomne (shadow DOM, var(--...) dziedziczy się
         przez granicę) przestylowują się automatycznie. */
      --ink: #23201b;
      --ink-soft: #6b665d;
      --muted: #9a948a;
      --sky-1: #ece5d7;
      --sky-2: #f0e9dd;
      --sky-3: #f5f0e7;
      --paper: #f8f4ec;
      --paper-tint: #fbf8f2;
      --accent: #1488d8;
      --accent-deep: #106bab;
      --green: #2f7a4d;
      --bloom-yellow: #e3b355;
      --bloom-pink: #d1889b;
      --ok: #2f7a4d;
      --err: #b8443a;
      --line: rgba(35, 32, 27, 0.14);
      --shadow: 0 1px 0 rgba(255, 255, 255, 0.6) inset, 0 20px 40px -26px rgba(35, 32, 27, 0.35);
      --shadow-sm: 0 1px 0 rgba(255, 255, 255, 0.5) inset, 0 8px 16px -10px rgba(35, 32, 27, 0.3);
      --radius-lg: 18px;
      --radius: 13px;
      --radius-sm: 9px;
      --radius-pill: 999px;
      --fr: "Fraunces", Georgia, serif;
      --ns: "Newsreader", Georgia, serif;
      --mn: "JetBrains Mono", var(--mn);
      display: flex;
      flex-direction: column;
      position: relative;
      height: 100vh;
      height: 100dvh;
      overflow: hidden;
      color: var(--ink);
      font-family: var(--ns);
      background: linear-gradient(180deg, var(--sky-1) 0%, var(--sky-2) 45%, var(--sky-3) 100%);
    }

    /* Papierowa faktura — subtelny szum, ten sam trik co na referencyjnej
       stronie (feTurbulence w inline SVG), zero requestów sieciowych. */
    :host::before {
      content: "";
      position: fixed;
      inset: 0;
      z-index: 0;
      pointer-events: none;
      mix-blend-mode: multiply;
      opacity: 0.4;
      background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='180' height='180'%3E%3Cfilter id='n'%3E%3CfeTurbulence type='fractalNoise' baseFrequency='0.9' numOctaves='2' stitchTiles='stitch'/%3E%3C/filter%3E%3Crect width='100%25' height='100%25' filter='url(%23n)' opacity='0.09'/%3E%3C/svg%3E");
    }

    header,
    main,
    nav {
      position: relative;
      z-index: 1;
    }

    header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 8px;
      padding: 14px 18px 16px;
      padding-top: calc(14px + env(safe-area-inset-top));
      background: rgba(248, 244, 236, 0.78);
      backdrop-filter: blur(16px);
      box-shadow: 0 1px 0 var(--line);
      flex: 0 0 auto;
    }

    .brand {
      display: flex;
      align-items: center;
      gap: 10px;
    }

    .brand .flower {
      color: var(--accent);
      display: grid;
      place-items: center;
    }

    .brand strong {
      font-family: var(--fr);
      font-weight: 500;
      font-size: 1.25rem;
      letter-spacing: -0.01em;
      line-height: 1;
      display: block;
    }

    .brand small {
      font-family: var(--mn);
      font-size: 0.7rem;
      color: var(--muted);
      letter-spacing: 0.06em;
    }

    .badges {
      display: flex;
      align-items: center;
      gap: 6px;
    }
    .badge {
      padding: 4px 8px;
      border: 1px solid currentColor;
      font: 700 0.68rem/1 var(--mn);
      letter-spacing: 0.1em;
      text-transform: uppercase;
    }
    .badge.dev {
      background: #ff7a45;
      color: #fff;
      border-color: #ff7a45;
    }
    .pill {
      display: inline-flex;
      align-items: center;
      gap: 7px;
      padding: 5px 10px;
      border: 1px solid var(--line);
      border-radius: var(--radius-pill);
      background: transparent;
      color: var(--muted);
      font: 600 0.72rem/1 var(--mn);
      letter-spacing: 0.03em;
      text-transform: uppercase;
    }
    .pill.ok {
      border-color: rgba(47, 122, 77, 0.35);
      color: var(--green);
    }
    .pill .dot {
      width: 7px;
      height: 7px;
      border-radius: 50%;
      background: currentColor;
      box-shadow: 0 0 6px currentColor;
    }

    main {
      flex: 1 1 auto;
      display: flex;
      flex-direction: column;
      padding: 20px;
      overflow-y: auto;
      -webkit-overflow-scrolling: touch;
    }

    .view-anim {
      display: flex;
      flex-direction: column;
      gap: 16px;
      animation: view-in 0.26s cubic-bezier(0.22, 1, 0.36, 1);
    }

    @keyframes view-in {
      from {
        opacity: 0;
        transform: translateY(8px);
      }
      to {
        opacity: 1;
        transform: none;
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .view-anim {
        animation: none;
      }
    }

    .hero {
      display: flex;
      flex-direction: column;
      align-items: center;
      text-align: center;
      gap: 10px;
      padding: 16px 20px 4px;
    }

    .hero-flower {
      color: var(--accent);
    }

    .hero h2 {
      margin: 0;
      font-family: var(--fr);
      font-weight: 300;
      font-size: 2.1rem;
      letter-spacing: -0.02em;
    }

    .hero p {
      margin: 0;
      color: var(--ink-soft);
      max-width: 36ch;
      line-height: 1.5;
      font-size: 1rem;
      font-family: var(--ns);
    }

    .card {
      display: flex;
      flex-direction: column;
      gap: 12px;
      padding: 20px;
      border: 1px solid var(--line);
      border-radius: var(--radius-lg);
      background: var(--paper);
      box-shadow: var(--shadow);
    }

    .card h3 {
      display: flex;
      align-items: center;
      gap: 8px;
      margin: 0;
      font-family: var(--fr);
      font-weight: 400;
      font-size: 1.25rem;
      color: var(--ink);
    }

    .muted {
      color: var(--muted);
      line-height: 1.5;
      margin: 0;
      font-family: var(--ns);
    }

    code {
      font-family: var(--mn);
      padding: 0.1em 0.4em;
      border-radius: 0.4em;
      background: var(--sky-2);
      color: var(--accent-deep);
      font-size: 0.92em;
    }

    .choice {
      display: flex;
      align-items: center;
      gap: 14px;
      padding: 14px;
      border: 1px solid var(--line);
      border-radius: var(--radius);
      background: var(--paper-tint);
      color: var(--ink);
      font: inherit;
      cursor: pointer;
      text-align: left;
      box-shadow: var(--shadow-sm);
      transition: border-color 0.15s ease, transform 0.15s ease;
    }
    .choice:hover:not(:disabled) {
      border-color: var(--accent);
    }
    .choice:active:not(:disabled) {
      background: var(--sky-2);
      transform: scale(0.99);
    }
    .choice:disabled {
      opacity: 0.5;
      cursor: not-allowed;
    }
    .choice.subtle {
      background: transparent;
      box-shadow: none;
    }
    .choice-ico {
      flex: 0 0 auto;
      width: 44px;
      height: 44px;
      display: grid;
      place-items: center;
      border: 1px solid var(--line);
      border-radius: var(--radius-sm);
      background: var(--sky-2);
      color: var(--accent);
    }
    .choice-body {
      display: flex;
      flex-direction: column;
      gap: 2px;
    }
    .choice-body strong {
      font-family: var(--fr);
      font-size: 1.05rem;
    }
    .choice-body span {
      font-family: var(--ns);
      font-size: 0.85rem;
      color: var(--muted);
      line-height: 1.4;
    }

    .link-button {
      align-self: flex-start;
      background: transparent;
      border: 0;
      color: var(--accent);
      padding: 4px 0;
      cursor: pointer;
      font:
        600 0.88rem/1 var(--mn);
    }

    .steps {
      margin: 0;
      padding-left: 1.2rem;
      color: var(--ink-soft);
      font-family: var(--ns);
      line-height: 1.5;
    }
    .steps li {
      margin: 8px 0;
    }
    .steps .cta.small {
      display: block;
      margin-top: 6px;
      padding: 8px 14px;
      font-size: 0.85rem;
    }
    .steps .callout {
      background: rgba(46, 142, 255, 0.08);
      border: 1px solid rgba(46, 142, 255, 0.25);
      border-radius: var(--radius-sm);
      padding: 8px 10px;
      list-style: none;
      margin-left: -1.2rem;
    }

    .row {
      display: flex;
      gap: 10px;
    }
    .row .cta {
      flex: 1 1 0;
    }

    .cta {
      padding: 14px 20px;
      border: 1px solid var(--accent);
      border-radius: var(--radius-sm);
      color: #fff;
      background: var(--accent);
      font: 700 0.88rem/1 var(--mn);
      letter-spacing: 0.02em;
      cursor: pointer;
      box-shadow: var(--shadow-sm);
      transition: background 0.2s ease, color 0.2s ease, transform 0.15s ease;
    }
    .cta:hover {
      background: var(--accent-deep);
      border-color: var(--accent-deep);
    }
    .cta:active:not(:disabled) {
      background: var(--ink);
      border-color: var(--ink);
      transform: scale(0.98);
    }
    .cta:disabled {
      opacity: 0.55;
      cursor: not-allowed;
    }
    .cta.ghost {
      background: transparent;
      color: var(--accent);
      border: 1px solid var(--accent);
    }
    .cta.ghost:hover {
      background: var(--accent);
      color: #fff;
    }

    .grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 10px;
    }

    .tile {
      display: flex;
      flex-direction: column;
      align-items: flex-start;
      gap: 4px;
      padding: 14px;
      border: 1px solid var(--line);
      border-radius: var(--radius);
      background: var(--paper-tint);
      color: var(--ink);
      cursor: pointer;
      text-align: left;
      font: inherit;
      min-height: 100px;
      box-shadow: var(--shadow-sm);
      transition: border-color 0.15s ease, background 0.15s ease, transform 0.15s ease;
    }
    .tile:hover {
      border-color: var(--accent);
    }
    .tile:active {
      background: var(--sky-2);
      transform: scale(0.99);
    }
    .tile-ico {
      width: 36px;
      height: 36px;
      display: grid;
      place-items: center;
      border: 1px solid var(--line);
      border-radius: var(--radius-sm);
      background: var(--sky-2);
      color: var(--accent);
      margin-bottom: 4px;
    }
    .tile strong {
      font-family: var(--fr);
      font-size: 1rem;
    }
    .tile span {
      font-family: var(--ns);
      font-size: 0.78rem;
      color: var(--muted);
      line-height: 1.35;
    }

    .plugin-list {
      display: flex;
      flex-direction: column;
      gap: 8px;
    }
    .plugin {
      display: flex;
      align-items: center;
      gap: 12px;
      padding: 12px;
      border: 1px solid var(--line);
      border-radius: var(--radius);
      background: var(--paper-tint);
    }
    .plugin-ico {
      color: var(--bloom-pink);
      flex: 0 0 auto;
    }
    .plugin-body {
      flex: 1 1 auto;
      display: flex;
      flex-direction: column;
      gap: 1px;
    }
    .plugin-body strong {
      font-family: var(--fr);
      font-size: 1rem;
    }
    .plugin-body span {
      font-family: var(--ns);
      font-size: 0.82rem;
      color: var(--muted);
    }
    .badge {
      padding: 3px 8px;
      border-radius: var(--radius-pill);
      background: var(--sky-2);
      color: var(--ink-soft);
      font: 600 0.68rem/1 var(--mn);
    }
    .badge.ok {
      background: rgba(45, 122, 77, 0.14);
      color: var(--green);
    }

    .cta.small {
      padding: 9px 16px;
      font-size: 0.8rem;
    }

    .rss-editor {
      display: flex;
      flex-direction: column;
      gap: 10px;
      padding: 14px;
      border: 1px solid var(--line);
      border-radius: var(--radius);
      background: var(--paper-tint);
      font-family: var(--ns);
    }
    .rss-list {
      list-style: none;
      margin: 0;
      padding: 0;
      display: flex;
      flex-direction: column;
      gap: 6px;
    }
    .rss-list li {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 10px;
      padding: 8px 10px;
      background: #fff;
      border: 1px solid var(--line);
      border-radius: var(--radius-sm);
    }
    .rss-list li span {
      font-size: 0.82rem;
      color: var(--ink-soft);
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }
    .rss-add {
      display: flex;
      gap: 8px;
    }
    .rss-add input {
      flex: 1 1 auto;
      padding: 10px 12px;
      border: 1px solid var(--line);
      border-radius: var(--radius-sm);
      background: #fff;
      font: 0.88rem var(--ns);
      color: var(--ink);
    }
    .del {
      width: 28px;
      height: 28px;
      flex: 0 0 auto;
      border: 0;
      border-radius: 50%;
      background: rgba(228, 77, 101, 0.1);
      color: var(--err);
      font-size: 0.8rem;
      cursor: pointer;
    }

    .settings-list {
      list-style: none;
      margin: 0;
      padding: 0;
      display: flex;
      flex-direction: column;
      gap: 0;
    }
    .settings-list li {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 12px 0;
      border-bottom: 1px solid var(--line);
      font-family: var(--ns);
    }
    .settings-list li:last-child {
      border-bottom: 0;
    }
    .settings-list strong {
      font-weight: 600;
    }
    .settings-list span {
      color: var(--muted);
    }

    .error {
      color: var(--err);
      font-size: 0.9rem;
      font-family: var(--ns);
    }

    nav {
      display: grid;
      grid-template-columns: repeat(6, 1fr);
      flex: 0 0 auto;
      padding-bottom: env(safe-area-inset-bottom);
      border-top: 1px solid var(--line);
      background: rgba(248, 244, 236, 0.9);
      backdrop-filter: blur(16px);
    }
    nav button {
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      gap: 4px;
      min-height: 52px;
      padding: 10px 1px 8px;
      background: transparent;
      border: 0;
      border-top: 2px solid transparent;
      color: var(--muted);
      font: 600 0.56rem/1.05 var(--mn);
      cursor: pointer;
      letter-spacing: -0.01em;
      white-space: nowrap;
      transition: color 0.15s ease, border-color 0.15s ease;
    }
    nav button .ico {
      width: 24px;
      height: 24px;
      display: grid;
      place-items: center;
    }
    nav button.active {
      color: var(--accent);
      border-top-color: var(--accent);
    }
    nav button:active:not(:disabled) {
      background: var(--sky-2);
    }
    nav button:disabled {
      opacity: 0.3;
    }
  `;
}

declare global {
  interface HTMLElementTagNameMap {
    "czytnik-app": CzytnikApp;
  }
}
