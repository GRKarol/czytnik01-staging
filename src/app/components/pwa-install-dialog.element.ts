import { LitElement, css, html } from "lit";
import { customElement, state } from "lit/decorators.js";
import { unsafeHTML } from "lit/directives/unsafe-html.js";
import { tr } from "../i18n/index";
import { dandelionIcon } from "./flower-icon";

export interface BeforeInstallPromptEvent extends Event {
  prompt(): Promise<void>;
  userChoice: Promise<{ outcome: "accepted" | "dismissed" }>;
}

const DISMISS_KEY = "flower.installPrompt.dismissedAt";
const DISMISS_TTL_MS = 7 * 24 * 60 * 60 * 1000; // 7 days
const SHOW_DELAY_MS = 30_000; // 30 seconds

@customElement("pwa-install-dialog")
export class PwaInstallDialog extends LitElement {
  @state() private deferredPrompt: BeforeInstallPromptEvent | null = null;
  @state() private visible = false;
  @state() private isIos = false;
  @state() private isStandalone = false;

  private showTimer: ReturnType<typeof setTimeout> | null = null;

  get installAvailable(): boolean {
    return this.deferredPrompt !== null;
  }

  connectedCallback(): void {
    super.connectedCallback();
    this.isStandalone =
      window.matchMedia("(display-mode: standalone)").matches ||
      (navigator as any).standalone === true;
    this.isIos = /iPad|iPhone|iPod/.test(navigator.userAgent) && !("MSStream" in window);

    if (this.isStandalone) return;

    window.addEventListener("beforeinstallprompt", this.handleBeforeInstallPrompt);
    window.addEventListener("appinstalled", this.handleAppInstalled);
  }

  disconnectedCallback(): void {
    super.disconnectedCallback();
    window.removeEventListener("beforeinstallprompt", this.handleBeforeInstallPrompt);
    window.removeEventListener("appinstalled", this.handleAppInstalled);
    if (this.showTimer) {
      clearTimeout(this.showTimer);
      this.showTimer = null;
    }
  }

  /** Programmatic trigger for onboarding wizard */
  async triggerInstall(): Promise<"accepted" | "dismissed" | "unavailable"> {
    if (!this.deferredPrompt) return "unavailable";
    await this.deferredPrompt.prompt();
    const { outcome } = await this.deferredPrompt.userChoice;
    if (outcome === "accepted") {
      this.deferredPrompt = null;
      this.visible = false;
    }
    return outcome;
  }

  private handleBeforeInstallPrompt = (e: Event) => {
    e.preventDefault();
    this.deferredPrompt = e as BeforeInstallPromptEvent;
    this.dispatchEvent(new CustomEvent("pwa-install-available", { bubbles: true, composed: true }));
    this.scheduleShow();
  };

  private handleAppInstalled = () => {
    this.deferredPrompt = null;
    this.visible = false;
    if (this.showTimer) {
      clearTimeout(this.showTimer);
      this.showTimer = null;
    }
  };

  private scheduleShow(): void {
    if (this.isStandalone) return;
    if (this.isDismissedRecently()) return;
    this.showTimer = setTimeout(() => {
      this.visible = true;
    }, SHOW_DELAY_MS);
  }

  private isDismissedRecently(): boolean {
    const raw = localStorage.getItem(DISMISS_KEY);
    if (!raw) return false;
    const ts = Number(raw);
    if (isNaN(ts)) return false;
    return Date.now() - ts < DISMISS_TTL_MS;
  }

  private handleInstallClick = async () => {
    const outcome = await this.triggerInstall();
    if (outcome === "dismissed") this.dismiss();
  };

  private dismiss = () => {
    localStorage.setItem(DISMISS_KEY, String(Date.now()));
    this.visible = false;
  };

  render() {
    if (this.isStandalone) return null;
    if (!this.visible) return null;

    return html`
      <div class="backdrop" @click=${this.dismiss}>
        <div class="dialog" @click=${(e: Event) => e.stopPropagation()}>
          <button class="close" @click=${this.dismiss} aria-label=${tr("common.close")}>✕</button>
          <div class="icon">${this.flowerIcon()}</div>
          <h2>Flower</h2>
          ${this.isIos ? this.renderIosInstructions() : this.renderChromePrompt()}
        </div>
      </div>
    `;
  }

  private renderChromePrompt() {
    return html`
      <p>${tr("pwa.question")}</p>
      <button class="cta" @click=${this.handleInstallClick}>${tr("onb.install.btn")}</button>
    `;
  }

  private renderIosInstructions() {
    return html`
      <p>${tr("pwa.ios")}</p>
      <ol>
        <li>${unsafeHTML(tr("onb.ios.step1"))}</li>
        <li>${unsafeHTML(tr("onb.ios.step2"))}</li>
        <li>${unsafeHTML(tr("pwa.ios.step3"))}</li>
      </ol>
    `;
  }

  private flowerIcon() {
    return dandelionIcon(64);
  }

  static styles = css`
    :host {
      position: fixed;
      inset: 0;
      z-index: 200;
      pointer-events: none;
    }

    .backdrop {
      position: absolute;
      inset: 0;
      display: grid;
      place-items: center;
      background: rgba(35, 32, 27, 0.55);
      backdrop-filter: blur(8px);
      pointer-events: auto;
      animation: fadeIn 300ms ease both;
    }

    .dialog {
      position: relative;
      width: calc(100% - 32px);
      max-width: 380px;
      padding: 32px 24px 28px;
      background: #f8f4ec;
      border: 1px solid rgba(35, 32, 27, 0.14);
      box-shadow: 0 20px 50px rgba(0, 0, 0, 0.22);
      display: flex;
      flex-direction: column;
      align-items: center;
      text-align: center;
      gap: 14px;
      animation: slideUp 300ms ease both;
    }

    .close {
      position: absolute;
      top: 12px;
      right: 12px;
      width: 32px;
      height: 32px;
      display: grid;
      place-items: center;
      border: 0;
      border-radius: 50%;
      background: rgba(107, 124, 151, 0.12);
      color: #9a948a;
      font-size: 1rem;
      cursor: pointer;
      line-height: 1;
    }
    .close:hover {
      background: rgba(107, 124, 151, 0.22);
    }

    .icon {
      color: #1488d8;
      filter: drop-shadow(0 8px 16px rgba(20, 136, 216, 0.25));
    }

    h2 {
      margin: 0;
      font-family: "Fraunces", Georgia, serif;
      font-weight: 400;
      font-size: 1.6rem;
      letter-spacing: -0.02em;
      color: #23201b;
    }

    p {
      margin: 0;
      color: #6b665d;
      font: 1rem/1.5 "Newsreader", Georgia, serif;
      max-width: 32ch;
    }

    ol {
      margin: 0;
      padding-left: 1.2rem;
      color: #6b665d;
      font: 0.95rem/1.6 "Newsreader", Georgia, serif;
      text-align: left;
    }

    ol li {
      margin-bottom: 4px;
    }

    strong {
      color: #23201b;
    }

    .cta {
      margin-top: 4px;
      padding: 14px 28px;
      border: 1px solid #1488d8;
      color: #fff;
      background: #1488d8;
      font: 700 0.88rem/1 "JetBrains Mono", monospace;
      letter-spacing: 0.02em;
      cursor: pointer;
    }
    .cta:hover {
      background: #106bab;
      border-color: #106bab;
    }

    @keyframes fadeIn {
      from {
        opacity: 0;
      }
      to {
        opacity: 1;
      }
    }

    @keyframes slideUp {
      from {
        opacity: 0;
        transform: translateY(20px);
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }
  `;
}

declare global {
  interface HTMLElementTagNameMap {
    "pwa-install-dialog": PwaInstallDialog;
  }
}
