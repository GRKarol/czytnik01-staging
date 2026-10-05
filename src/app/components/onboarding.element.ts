import { LitElement, css, html, svg, type TemplateResult } from "lit";
import { customElement, state } from "lit/decorators.js";
import { unsafeHTML } from "lit/directives/unsafe-html.js";
import { LANG_NAMES, SUPPORTED_LANGS, chooseLang, chosenLang, setLang, tr, type SupportedLang } from "../i18n/index";
import type { PwaInstallDialog } from "./pwa-install-dialog.element";
import { dandelionIcon } from "./flower-icon";

const STORAGE_KEY = "flower.onboarded.v1";
// Set once the language question on the first launch has an answer.
const LANG_ASKED_KEY = "flower.langAsked.v1";

// The tapped language, before Dalej confirms it. Module-level: the preview
// switch rebuilds the whole app, this element included.
let pendingLang: SupportedLang | null = null;

function langAsked(): boolean {
  try {
    return !!localStorage.getItem(LANG_ASKED_KEY) || chosenLang() !== null;
  } catch {
    return true;
  }
}

/**
 * Pełnoekranowy pierwszorazowy wizard. Pokazuje się raz po otwarciu
 * PWA. Trzy ekrany: powitanie / dodaj do ekranu głównego / połącz
 * urządzenie. Na iOS pokazujemy ilustrowaną instrukcję Share Sheet bo
 * Safari nie wspiera `beforeinstallprompt`.
 *
 * Po przejściu wszystkich ekranów zapisuje flagę w localStorage —
 * następnym razem wizard się nie pokaże.
 */
@customElement("onboarding-wizard")
export class OnboardingWizard extends LitElement {
  @state() private step = 0;
  @state() private dismissed = false;
  @state() private installAvailable = false;
  @state() private isStandalone = false;
  @state() private askLang = false;

  connectedCallback(): void {
    super.connectedCallback();
    this.dismissed = !!localStorage.getItem(STORAGE_KEY);
    this.askLang = !langAsked();

    this.isStandalone =
      window.matchMedia("(display-mode: standalone)").matches ||
      (navigator as any).standalone === true;

    window.addEventListener("pwa-install-available", this.handleInstallAvailable);
  }

  disconnectedCallback(): void {
    super.disconnectedCallback();
    window.removeEventListener("pwa-install-available", this.handleInstallAvailable);
  }

  private handleInstallAvailable = () => {
    this.installAvailable = true;
  };

  render() {
    if (this.askLang) return this.renderLanguage();
    if (this.dismissed) return null;
    return html`
      <div class="overlay">
        <div class="card">
          <div class="dots">
            ${[0, 1, 2].map(
              (i) => html`<span class=${i === this.step ? "dot active" : "dot"}></span>`,
            )}
          </div>

          <div class="stage">${this.renderStep()}</div>

          <div class="footer">
            ${this.step > 0
              ? html`<button class="link" @click=${() => (this.step -= 1)}>${tr("common.back")}</button>`
              : html`<button class="link" @click=${this.skip}>${tr("common.skip")}</button>`}
            <button class="cta" @click=${this.next}>
              ${this.step < 2 ? tr("common.next") : tr("onb.start")}
            </button>
          </div>
        </div>
      </div>
    `;
  }

  /** First launch: the language comes before anything else, no skipping. */
  private renderLanguage(): TemplateResult {
    return html`
      <div class="overlay">
        <div class="card">
          <div class="stage">
            <div class="hero">${this.flower(96)}</div>
            <h2>${tr("onb.lang.title")}</h2>
            <p>${tr("onb.lang.text")}</p>
            <div class="langs">
              ${SUPPORTED_LANGS.map(
                (lang) =>
                  html`<button class=${lang === pendingLang ? "lang on" : "lang"} @click=${() => this.previewLang(lang)}>
                    ${LANG_NAMES[lang]}
                  </button>`,
              )}
            </div>
          </div>
          <div class="footer end">
            <button class="cta" ?disabled=${pendingLang === null} @click=${this.confirmLang}>${tr("common.next")}</button>
          </div>
        </div>
      </div>
    `;
  }

  /** A tap shows the app in that language; Dalej keeps it. */
  private previewLang(lang: SupportedLang) {
    pendingLang = lang;
    this.requestUpdate();
    // Not remembered yet; rebuilds the app (this element too) in it.
    setLang(lang);
  }

  private confirmLang = () => {
    const lang = pendingLang;
    if (lang === null) return;
    try {
      localStorage.setItem(LANG_ASKED_KEY, lang);
    } catch {
      /* asked again next launch */
    }
    pendingLang = null;
    this.askLang = false;
    chooseLang(lang);
  };

  private renderStep(): TemplateResult {
    switch (this.step) {
      case 0:
        return html`
          <div class="hero">${this.flower(120)}</div>
          <h2>${tr("onb.hello.title")}</h2>
          <p>${tr("onb.hello.text")}</p>
        `;
      case 1: {
        // If standalone → skip install step entirely (proceed to step 2)
        if (this.isStandalone) {
          this.step = 2;
          return this.renderStep();
        }

        const isIos = /iPad|iPhone|iPod/.test(navigator.userAgent) && !("MSStream" in window);

        if (isIos) {
          // iOS → show numbered Share Sheet instructions (at least 2 steps)
          return html`
            <div class="hero soft">${this.iconAdd()}</div>
            <h2>${tr("onb.ios.title")}</h2>
            <ol class="ios-steps">
              <li>${unsafeHTML(tr("onb.ios.step1"))}</li>
              <li>${unsafeHTML(tr("onb.ios.step2"))}</li>
            </ol>
          `;
        }

        // Check if beforeinstallprompt is available via pwa-install-dialog
        if (this.installAvailable) {
          return html`
            <div class="hero soft">${this.iconAdd()}</div>
            <h2>${tr("onb.install.title")}</h2>
            <p>${tr("onb.install.text")}</p>
            <button class="cta" @click=${this.handleOnboardingInstall}>${tr("onb.install.btn")}</button>
          `;
        }

        // No prompt available and not iOS → skip install step
        this.step = 2;
        return this.renderStep();
      }
      case 2:
        return html`
          <div class="hero soft">${this.iconWifi()}</div>
          <h2>${tr("onb.connect.title")}</h2>
          <p>${unsafeHTML(tr("onb.connect.text"))}</p>
          <p class="hint">${unsafeHTML(tr("onb.connect.hint"))}</p>
        `;
      default:
        return html``;
    }
  }

  private next = () => {
    if (this.step < 2) {
      this.step += 1;
      return;
    }
    this.finish();
  };

  private skip = () => this.finish();

  private finish() {
    localStorage.setItem(STORAGE_KEY, new Date().toISOString());
    this.dismissed = true;
  }

  private handleOnboardingInstall = async () => {
    const dialog = document.querySelector("pwa-install-dialog") as PwaInstallDialog | null;
    if (dialog) {
      await dialog.triggerInstall();
    }
    // Regardless of outcome (accepted, dismissed, unavailable), proceed to next step
    this.step += 1;
  };

  // ─── ikony ────────────────────────────────────────────────────────────────

  private flower(s: number) {
    return dandelionIcon(s);
  }

  private iconAdd() {
    return svg`
      <svg width="90" height="90" viewBox="0 0 100 100" fill="none"
           stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round">
        <rect x="22" y="22" width="56" height="56" rx="12"/>
        <line x1="50" y1="38" x2="50" y2="62"/>
        <line x1="38" y1="50" x2="62" y2="50"/>
      </svg>
    `;
  }

  private iconWifi() {
    return svg`
      <svg width="100" height="100" viewBox="0 0 100 100" fill="none"
           stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round">
        <path d="M14 38a48 48 0 0 1 72 0"/>
        <path d="M26 52a32 32 0 0 1 48 0"/>
        <path d="M38 66a16 16 0 0 1 24 0"/>
        <circle cx="50" cy="80" r="4" fill="currentColor"/>
      </svg>
    `;
  }

  static styles = css`
    :host {
      position: fixed;
      inset: 0;
      z-index: 100;
      pointer-events: none;
    }
    .overlay {
      position: absolute;
      inset: 0;
      display: grid;
      place-items: end center;
      padding: 12px;
      padding-bottom: calc(12px + env(safe-area-inset-bottom));
      background: rgba(35, 32, 27, 0.55);
      backdrop-filter: blur(8px);
      pointer-events: auto;
    }
    .card {
      width: 100%;
      max-width: 460px;
      padding: 24px 22px;
      padding-bottom: calc(20px + env(safe-area-inset-bottom));
      background: #f8f4ec;
      border: 1px solid rgba(35, 32, 27, 0.14);
      border-bottom: 0;
      box-shadow: 0 -16px 40px rgba(0, 0, 0, 0.18);
      color: #23201b;
      display: flex;
      flex-direction: column;
      gap: 14px;
      font-family: "Newsreader", Georgia, serif;
    }
    .dots {
      display: flex;
      gap: 6px;
      justify-content: center;
      margin-bottom: 2px;
    }
    .dot {
      width: 7px;
      height: 7px;
      border-radius: 50%;
      background: #e4ddd0;
      transition: width 0.2s;
    }
    .dot.active {
      width: 22px;
      border-radius: 999px;
      background: #1488d8;
    }
    .stage {
      display: flex;
      flex-direction: column;
      align-items: center;
      text-align: center;
      gap: 12px;
      padding: 6px 0;
    }
    .hero {
      color: #1488d8;
    }
    .hero.soft {
      color: #1488d8;
      padding: 12px;
      border: 1px solid rgba(35, 32, 27, 0.14);
      background: #f0e9dd;
    }
    h2 {
      margin: 0;
      font-family: "Fraunces", Georgia, serif;
      font-weight: 400;
      font-size: 1.7rem;
      letter-spacing: -0.02em;
    }
    p {
      margin: 0;
      max-width: 38ch;
      color: #6b665d;
      font: 1rem/1.55 "Newsreader", Georgia, serif;
    }
    .hint {
      font-style: italic;
      font-size: 0.9rem;
      color: #9a948a;
    }
    .footer {
      display: flex;
      gap: 10px;
      align-items: center;
      justify-content: space-between;
      padding-top: 4px;
    }
    .link {
      background: transparent;
      border: 0;
      color: #9a948a;
      font: 600 0.78rem "JetBrains Mono", monospace;
      letter-spacing: 0.02em;
      cursor: pointer;
      padding: 8px 4px;
    }
    .langs {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 8px;
      width: 100%;
      margin-top: 8px;
    }
    .lang {
      padding: 14px 10px;
      border: 1px solid #1488d8;
      border-radius: 10px;
      color: inherit;
      background: transparent;
      font-family: inherit;
      font-size: 1rem;
      font-weight: 600;
      cursor: pointer;
    }
    .lang:active,
    .lang.on {
      background: #1488d8;
      color: #fff;
    }
    .footer.end {
      justify-content: flex-end;
    }
    .cta:disabled {
      opacity: 0.4;
      cursor: default;
    }
    .cta {
      padding: 12px 22px;
      border: 1px solid #1488d8;
      color: #fff;
      background: #1488d8;
      font: 700 0.85rem "JetBrains Mono", monospace;
      letter-spacing: 0.02em;
      cursor: pointer;
    }
    .cta:hover {
      background: #106bab;
      border-color: #106bab;
    }
    code {
      padding: 0.12em 0.38em;
      background: #f0e9dd;
      color: #106bab;
      font-family: "JetBrains Mono", monospace;
      font-size: 0.9em;
    }
    strong {
      color: #23201b;
    }
    .ios-steps {
      margin: 0;
      padding-left: 1.4rem;
      text-align: left;
      color: #6b665d;
      font: 0.95rem/1.6 "Newsreader", Georgia, serif;
    }
    .ios-steps li {
      margin-bottom: 6px;
    }
  `;
}

declare global {
  interface HTMLElementTagNameMap {
    "onboarding-wizard": OnboardingWizard;
  }
}
