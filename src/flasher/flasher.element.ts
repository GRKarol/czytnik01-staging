import { LitElement, css, html, svg } from "lit";
import { customElement, state } from "lit/decorators.js";
import { APP_URL, BRAND_NAME, FIRMWARE_MANIFEST_URL, FLASHER_URL } from "../shared/config";
import { setUpCard, type CardSetupProgress } from "./card-setup";

const iconFlower = (s = 80) => svg`
  <svg width=${s} height=${s} viewBox="0 0 100 100" aria-hidden="true">
    <g transform="translate(50 50)">
      ${[0, 60, 120, 180, 240, 300].map(
        (a) => svg`
        <ellipse cx="0" cy="-22" rx="14" ry="22" fill="currentColor" opacity="0.85"
                 transform="rotate(${a})"/>
      `,
      )}
      <circle r="10" fill="#fff2bf"/>
      <circle r="6" fill="#ffd66e"/>
    </g>
  </svg>
`;

@customElement("czytnik-flasher")
export class CzytnikFlasher extends LitElement {
  @state() private serialSupported = "serial" in navigator;
  @state() private isSecure = window.isSecureContext;
  @state() private card: CardSetupProgress | null = null;
  @state() private cardError = "";
  @state() private cardBusy = false;

  private async prepareCard() {
    this.cardError = "";
    this.cardBusy = true;
    try {
      await setUpCard(FLASHER_URL, (progress) => {
        this.card = progress;
      });
    } catch (error) {
      const message = (error as Error).message || String(error);
      // Closing the port picker is not an error worth a red box.
      if (!/No port selected|cancel/i.test(message)) this.cardError = message;
      if (this.card?.phase !== "done") this.card = null;
    } finally {
      this.cardBusy = false;
    }
  }

  private cardStatus() {
    const card = this.card;
    if (!card) return "";
    const mb = (bytes: number) => (bytes / 1048576).toFixed(1);
    switch (card.phase) {
      case "connect":
        return "Łączę z czytnikiem...";
      case "wake":
        return "Czytnik jeszcze nie odpowiada. Jeśli ekran jest ciemny, przytrzymaj PWR, aż się zaświeci.";
      case "format":
        return "Formatuję kartę...";
      case "fonts":
        return `Wgrywam czcionki: ${card.filesDone}/${card.filesTotal} (${mb(card.bytesDone)} z ${mb(card.bytesTotal)} MB)`;
      case "done":
        return "Gotowe. Karta ma wszystkie czcionki, kreator na czytniku pominie ich pobieranie.";
    }
  }

  render() {
    return html`
      <main>
        <section class="hero card">
          <div class="hero-art">${iconFlower(96)}</div>
          <p class="eyebrow">Instalator firmware</p>
          <h1>${BRAND_NAME}</h1>
          <p class="lead">
            Podłącz urządzenie kablem USB i zainstaluj firmware z poziomu przeglądarki —
            bez VS Code, bez PlatformIO, bez terminala.
          </p>
        </section>

        <section class="card">
          <h2>Krok 1 — Zainstaluj firmware</h2>
          <p>Podłącz urządzenie i naciśnij przycisk poniżej.</p>

          <esp-web-install-button manifest=${FIRMWARE_MANIFEST_URL}>
            <button slot="activate" class="cta">Zainstaluj firmware</button>
            <span slot="unsupported">
              Twoja przeglądarka nie obsługuje Web Serial. Użyj Chrome lub Edge na desktopie.
            </span>
            <span slot="not-allowed">
              Strona musi być otwarta przez HTTPS lub localhost.
            </span>
          </esp-web-install-button>

          ${!this.serialSupported
            ? html`<p class="warning">
                Twoja przeglądarka nie obsługuje Web Serial. Otwórz tę stronę w Chrome lub Edge
                na komputerze.
              </p>`
            : ""}
          ${!this.isSecure
            ? html`<p class="warning">
                Strona nie jest otwarta po HTTPS. Flashowanie zadziała tylko z HTTPS lub
                localhost.
              </p>`
            : ""}
        </section>

        <section class="card">
          <h2>Krok 2 — Przygotuj kartę microSD</h2>
          <p>
            Włóż kartę microSD do czytnika i zostaw kabel podłączony. Zamknij okno instalatora z
            kroku 1, a potem naciśnij przycisk poniżej i wybierz ten sam port. Flasher sformatuje
            kartę i wgra na nią wszystkie czcionki (około 9 MB, kilkadziesiąt sekund).
          </p>
          <p class="warning">Formatowanie usuwa wszystko, co jest na karcie.</p>
          <button
            class="cta"
            ?disabled=${this.cardBusy || !this.serialSupported}
            @click=${() => this.prepareCard()}
          >
            ${this.cardBusy ? "Trwa przygotowanie..." : "Przygotuj kartę"}
          </button>
          ${this.card
            ? html`<div class="progress">
                  <div
                    class="bar"
                    style="width: ${this.card.bytesTotal
                      ? Math.round((this.card.bytesDone * 100) / this.card.bytesTotal)
                      : 0}%"
                  ></div>
                </div>
                <p class="status">${this.cardStatus()}</p>`
            : ""}
          ${this.cardError ? html`<p class="warning error">${this.cardError}</p>` : ""}
          <p class="note">
            Jeśli pominiesz ten krok i włożysz własną kartę, czytnik w kreatorze zaproponuje jej
            sformatowanie i sam pobierze potrzebne pliki przez Wi-Fi. Zajmie to około 10 minut.
          </p>
        </section>

        <section class="card">
          <h2>Krok 3 — Otwórz aplikację</h2>
          <p>
            Po zainstalowaniu firmware otwórz aplikację ${BRAND_NAME} i połącz się z
            urządzeniem przez WiFi lub Bluetooth. Aplikację możesz dodać do ekranu
            głównego telefonu (PWA).
          </p>
          <a class="cta ghost" href=${APP_URL}>Przejdź do aplikacji</a>
        </section>

        <section class="card steps">
          <h2>Zanim zaczniesz</h2>
          <ol>
            <li>Użyj Chrome lub Edge na macOS, Windows lub Linux.</li>
            <li>Podłącz urządzenie kablem USB z transmisją danych.</li>
            <li>
              Jeśli instalator nie może się połączyć, przytrzymaj <code>BOOT</code>, naciśnij
              reset i spróbuj ponownie.
            </li>
          </ol>
        </section>

        <footer>${BRAND_NAME} &middot; web flasher</footer>
      </main>
    `;
  }

  static styles = css`
    :host {
      --ink: #0c2340;
      --ink-soft: #3d5278;
      --muted: #6b7c97;
      --sky-1: #e8f4fd;
      --sky-2: #d1e9fb;
      --sky-3: #b8dcff;
      --paper: #ffffff;
      --accent: #2e8eff;
      --accent-deep: #1f6fd4;
      --line: #d9e6f6;
      --esp-tools-button-color: var(--accent);
      --esp-tools-button-text-color: #ffffff;
      --esp-tools-button-border-radius: 999px;
      display: block;
      min-height: 100vh;
      color: var(--ink);
      font-family:
        "Iowan Old Style", "Hoefler Text", Georgia, ui-serif, serif;
      background:
        radial-gradient(circle at 14% 16%, rgba(255, 214, 110, 0.35), transparent 32rem),
        radial-gradient(circle at 88% 10%, rgba(255, 182, 200, 0.3), transparent 28rem),
        linear-gradient(160deg, var(--sky-1) 0%, var(--sky-2) 55%, var(--sky-3) 100%);
    }

    main {
      width: min(960px, calc(100% - 32px));
      margin: 0 auto;
      padding: 44px 0 44px;
      display: grid;
      gap: 20px;
    }

    .hero {
      display: flex;
      flex-direction: column;
      align-items: center;
      text-align: center;
      gap: 12px;
      padding: clamp(28px, 5vw, 48px);
    }

    .hero-art {
      color: var(--accent);
      filter: drop-shadow(0 10px 22px rgba(46, 142, 255, 0.28));
    }

    .eyebrow {
      margin: 0;
      color: var(--accent-deep);
      font-family: ui-sans-serif, system-ui, sans-serif;
      font-weight: 700;
      font-size: 0.76rem;
      letter-spacing: 0.22em;
      text-transform: uppercase;
    }

    h1 {
      margin: 0;
      font-family: "Iowan Old Style", Georgia, ui-serif, serif;
      font-size: clamp(3rem, 9vw, 5.5rem);
      line-height: 0.9;
      letter-spacing: -0.03em;
    }

    .lead {
      max-width: 56ch;
      margin: 0;
      color: var(--ink-soft);
      font-size: clamp(1rem, 1.6vw, 1.18rem);
      line-height: 1.55;
      font-family: ui-sans-serif, system-ui, sans-serif;
    }

    .card {
      padding: 24px;
      border: 1px solid var(--line);
      border-radius: 22px;
      background: var(--paper);
      box-shadow: 0 12px 26px rgba(46, 142, 255, 0.14);
    }

    .card h2 {
      margin: 0 0 10px;
      font-family: "Iowan Old Style", Georgia, ui-serif, serif;
      font-size: 1.2rem;
    }

    .card p,
    .card li {
      color: var(--ink-soft);
      line-height: 1.5;
      font-family: ui-sans-serif, system-ui, sans-serif;
    }

    esp-web-install-button {
      display: inline-block;
      margin-top: 8px;
    }

    .cta {
      display: inline-block;
      margin-top: 6px;
      padding: 13px 22px;
      border: 0;
      border-radius: 999px;
      color: #fff;
      background: var(--accent);
      font: 700 0.95rem/1 ui-sans-serif, system-ui, sans-serif;
      text-decoration: none;
      cursor: pointer;
      box-shadow: 0 10px 22px rgba(46, 142, 255, 0.28);
    }
    .cta:hover {
      background: var(--accent-deep);
    }
    .cta.ghost {
      background: transparent;
      color: var(--accent);
      border: 1.5px solid var(--accent);
      box-shadow: none;
    }

    .warning {
      margin-top: 12px;
      padding: 12px 14px;
      border-left: 3px solid var(--accent);
      border-radius: 12px;
      background: rgba(46, 142, 255, 0.08);
      color: var(--accent-deep);
      font-size: 0.92rem;
      font-family: ui-sans-serif, system-ui, sans-serif;
    }

    ol {
      padding-left: 1.35rem;
    }

    .cta[disabled] {
      opacity: 0.6;
      cursor: progress;
    }

    .progress {
      margin-top: 16px;
      height: 10px;
      border-radius: 999px;
      background: var(--sky-2);
      overflow: hidden;
    }
    .progress .bar {
      height: 100%;
      background: var(--accent);
      transition: width 0.2s ease;
    }

    .status,
    .note {
      font-size: 0.92rem;
    }
    .note {
      color: var(--muted);
    }
    .warning.error {
      border-left-color: #d64545;
      background: rgba(214, 69, 69, 0.08);
      color: #a32f2f;
    }

    code {
      padding: 0.12em 0.38em;
      border-radius: 0.4em;
      background: var(--sky-2);
      color: var(--accent-deep);
      font-family: ui-monospace, SFMono-Regular, monospace;
    }

    footer {
      text-align: center;
      color: var(--muted);
      font: 0.88rem/1.4 ui-sans-serif, system-ui, sans-serif;
    }

    @media (max-width: 760px) {
      main {
        padding-top: 18px;
      }
      .card {
        padding: 20px;
        border-radius: 18px;
      }
    }
  `;
}

declare global {
  interface HTMLElementTagNameMap {
    "czytnik-flasher": CzytnikFlasher;
  }
}
