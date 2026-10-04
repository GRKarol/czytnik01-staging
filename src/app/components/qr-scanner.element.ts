import { LitElement, css, html } from "lit";
import { tr } from "../i18n/index";
import { customElement, state } from "lit/decorators.js";
import jsQR from "jsqr";

/**
 * Full-screen camera view that reads one QR code and fires `qr-result`
 * with its text (`detail.text`). `qr-cancel` when the user closes it or the
 * camera cannot start. Decoding runs in the app (jsQR), so it works on
 * phones without Google services and with no internet (the phone may
 * already be half-way onto the reader's offline network).
 */
@customElement("qr-scanner")
export class QrScanner extends LitElement {
  @state() private problem = "";

  private stream: MediaStream | null = null;
  private timer: number | null = null;
  private canvas = document.createElement("canvas");

  connectedCallback(): void {
    super.connectedCallback();
    void this.start();
  }

  disconnectedCallback(): void {
    super.disconnectedCallback();
    this.stop();
  }

  render() {
    return html`
      <div class="overlay" role="dialog" aria-modal="true" aria-label="QR">
        <video playsinline muted></video>
        <div class="frame"></div>
        <p class="hint">
          ${this.problem || tr("qr.aim")}
        </p>
        <button class="close" @click=${this.cancel}>${tr("common.cancel")}</button>
      </div>
    `;
  }

  private async start() {
    if (!navigator.mediaDevices?.getUserMedia) {
      this.problem = tr("qr.noCamera");
      return;
    }
    try {
      this.stream = await navigator.mediaDevices.getUserMedia({
        video: { facingMode: { ideal: "environment" } },
        audio: false,
      });
    } catch {
      this.problem = tr("qr.denied");
      return;
    }
    await this.updateComplete;
    const video = this.renderRoot.querySelector("video");
    if (!video || !this.stream) return;
    video.srcObject = this.stream;
    await video.play().catch(() => undefined);
    this.timer = window.setInterval(() => this.scanFrame(video), 200);
  }

  private scanFrame(video: HTMLVideoElement) {
    const width = video.videoWidth;
    const height = video.videoHeight;
    if (!width || !height) return;
    // Half resolution is plenty for a code filling a good part of the view
    // and keeps each pass short on slower phones.
    const scale = Math.min(1, 640 / Math.max(width, height));
    this.canvas.width = Math.round(width * scale);
    this.canvas.height = Math.round(height * scale);
    const context = this.canvas.getContext("2d", { willReadFrequently: true });
    if (!context) return;
    context.drawImage(video, 0, 0, this.canvas.width, this.canvas.height);
    const image = context.getImageData(0, 0, this.canvas.width, this.canvas.height);
    const code = jsQR(image.data, image.width, image.height, { inversionAttempts: "attemptBoth" });
    if (code?.data) {
      this.stop();
      this.dispatchEvent(new CustomEvent("qr-result", { detail: { text: code.data }, bubbles: true, composed: true }));
    }
  }

  private cancel = () => {
    this.stop();
    this.dispatchEvent(new CustomEvent("qr-cancel", { bubbles: true, composed: true }));
  };

  private stop() {
    if (this.timer !== null) {
      window.clearInterval(this.timer);
      this.timer = null;
    }
    this.stream?.getTracks().forEach((track) => track.stop());
    this.stream = null;
  }

  static styles = css`
    .overlay {
      position: fixed;
      inset: 0;
      z-index: 1000;
      background: #000;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
    }
    video {
      position: absolute;
      inset: 0;
      width: 100%;
      height: 100%;
      object-fit: cover;
    }
    .frame {
      position: relative;
      width: min(70vw, 70vh);
      aspect-ratio: 1;
      border: 3px solid rgba(255, 255, 255, 0.9);
      border-radius: 18px;
      box-shadow: 0 0 0 100vmax rgba(0, 0, 0, 0.45);
    }
    .hint {
      position: relative;
      margin: 24px 24px 0;
      color: #fff;
      text-align: center;
      font-size: 16px;
      line-height: 1.4;
    }
    .close {
      position: relative;
      margin-top: 20px;
      min-height: 48px;
      padding: 0 28px;
      border: none;
      border-radius: 24px;
      background: #fff;
      color: #111;
      font-size: 16px;
      font-weight: 600;
    }
  `;
}
