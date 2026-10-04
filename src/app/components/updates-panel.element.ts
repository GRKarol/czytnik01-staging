import { LitElement, css, html, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import {
  fetchLatestRelease,
  pickFirmwareAsset,
  downloadAsset,
  isNewer,
  type ReleaseInfo,
} from "../updates/releases";
import { deviceApi } from "../device/api";
import { formatDate, onLangChange, tr } from "../i18n/index";
import { OTA_RELEASES_REPO } from "../../shared/config";
import { isNativeApp } from "../device/network-pin";
import { Directory, Filesystem } from "@capacitor/filesystem";
import { Share } from "@capacitor/share";

type Stage =
  | "idle"
  | "checking"
  | "found"
  | "none"
  | "error"
  | "downloading"
  | "downloaded"
  | "installing"
  | "installed";

@customElement("updates-panel")
export class UpdatesPanel extends LitElement {
  @state() private stage: Stage = "idle";
  @state() private release: ReleaseInfo | null = null;
  @state() private error = "";
  @state() private progress = 0;
  /** Firmware of the connected reader (from /api/hello); empty = unknown. */
  @property({ attribute: false }) currentFw = "";
  private downloaded: Blob | null = null;
  private unsubLang: (() => void) | null = null;

  connectedCallback(): void {
    super.connectedCallback();
    this.unsubLang = onLangChange(() => this.requestUpdate());
  }

  disconnectedCallback(): void {
    super.disconnectedCallback();
    this.unsubLang?.();
  }

  render() {
    return html`
      <div class="head">
        <strong>${tr("up.title")}</strong>
        <span class="muted">${tr("up.source", { repo: OTA_RELEASES_REPO })}</span>
      </div>

      ${this.renderStage()}
    `;
  }

  private renderStage() {
    switch (this.stage) {
      case "idle":
        return html`
          <p class="muted">${tr("up.lead")}</p>
          <button class="cta" @click=${this.check}>${tr("up.check")}</button>
        `;
      case "checking":
        return html`<p class="muted">${tr("up.checking")}</p>`;
      case "none":
        return html`
          <p class="muted">${tr("up.none")}</p>
          <button class="cta ghost" @click=${this.check}>${tr("up.checkAgain")}</button>
        `;
      case "error":
        return html`
          <p class="error">${this.error}</p>
          <button class="cta ghost" @click=${this.check}>${tr("common.retry")}</button>
        `;
      case "found":
      case "downloading":
      case "downloaded":
      case "installing":
      case "installed":
        return this.renderRelease();
    }
  }

  private renderRelease() {
    const r = this.release!;
    const asset = pickFirmwareAsset(r);
    const tag = r.tag.replace(/^v/, "");
    // Only a release newer than the reader is offered for install: a reader
    // on a newer or test build must not be pushed back to an older one.
    const newer = this.currentFw ? isNewer(tag, this.currentFw) : true;
    const readerAhead = !!this.currentFw && isNewer(this.currentFw, tag);
    const offerInstall = newer;
    const date = formatDate(r.publishedAt);

    return html`
      <article class="release">
        <header>
          <div>
            <h4>${r.name}</h4>
            <small>${tag} · ${date}${r.isPrerelease ? ` · ${tr("up.prerelease")}` : ""}</small>
          </div>
          ${newer
            ? html`<span class="badge ok">${tr("up.available")}</span>`
            : html`<span class="badge">${readerAhead ? tr("up.older") : tr("up.current")}</span>`}
        </header>
        ${this.currentFw
          ? html`<p class="muted">
              ${tr("up.reader", { v: this.currentFw })}
              ${readerAhead ? tr("up.readerAhead") : newer ? "" : tr("up.readerSame")}
            </p>`
          : nothing}

        ${r.body
          ? html`<pre class="changelog">${trimChangelog(r.body)}</pre>`
          : html`<p class="muted">${tr("up.noNotes")}</p>`}

        ${asset
          ? html`
              <div class="asset">
                <span><strong>${asset.name}</strong> · ${formatBytes(asset.size)}</span>
                ${this.stage === "downloading" || this.stage === "installing"
                  ? html`<progress max="100" value=${this.progress}></progress>`
                  : ""}
                ${this.stage === "downloaded"
                  ? html`<span class="ok">${tr("up.downloaded")}</span>`
                  : ""}
                ${this.stage === "installed"
                  ? html`<span class="ok">${tr("up.installed")}</span>`
                  : ""}
              </div>
              <div class="row">
                ${this.stage === "found" && offerInstall
                  ? html`<button class="cta" @click=${() => this.download(asset)}>
                      ${tr("up.download")}
                    </button>`
                  : ""}
                ${this.stage === "downloaded"
                  ? html`<button class="cta" @click=${this.install}>
                        ${tr("up.install")}
                      </button>
                      <button class="cta ghost" @click=${this.savePhone}>
                        ${tr("up.savePhone")}
                      </button>`
                  : ""}
                ${this.stage === "installing"
                  ? html`<span class="muted">${tr("up.sendingPct", { n: this.progress })}</span>`
                  : ""}
              </div>
            `
          : html`<p class="muted">${tr("up.noBin")}</p>`}

        <a class="link" href=${r.htmlUrl} target="_blank" rel="noopener noreferrer">
          ${tr("up.github")} ↗
        </a>
      </article>
    `;
  }

  // ─── actions ──────────────────────────────────────────────────────────────

  private check = async () => {
    this.error = "";
    this.stage = "checking";
    try {
      const r = await fetchLatestRelease();
      if (!r) {
        this.stage = "none";
        return;
      }
      this.release = r;
      this.stage = "found";
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
      this.stage = "error";
    }
  };

  private download = async (asset: ReturnType<typeof pickFirmwareAsset>) => {
    if (!asset) return;
    this.stage = "downloading";
    this.progress = 0;
    try {
      this.downloaded = await downloadAsset(asset, (loaded, total) => {
        this.progress = total ? Math.round((loaded / total) * 100) : 0;
      });
      this.stage = "downloaded";
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
      this.stage = "error";
    }
  };

  private savePhone = async () => {
    if (!this.downloaded || !this.release) return;
    const asset = pickFirmwareAsset(this.release);
    if (!asset) return;
    if (isNativeApp()) {
      // A WebView ignores <a download>; hand the file to Android's share
      // sheet instead (Pliki, Dysk, komunikator…).
      try {
        const { uri } = await Filesystem.writeFile({
          path: asset.name,
          data: await blobToBase64(this.downloaded),
          directory: Directory.Cache,
        });
        await Share.share({ title: asset.name, files: [uri] });
      } catch (err) {
        const message = err instanceof Error ? err.message : String(err);
        if (!/cancel/i.test(message)) this.error = message;
      }
      return;
    }
    const url = URL.createObjectURL(this.downloaded);
    const a = document.createElement("a");
    a.href = url;
    a.download = asset.name;
    a.click();
    URL.revokeObjectURL(url);
  };

  private install = async () => {
    if (!this.downloaded) return;
    this.stage = "installing";
    this.progress = 0;
    this.error = "";
    try {
      await deviceApi.installOta(this.downloaded, (loaded, total) => {
        this.progress = total ? Math.round((loaded / total) * 100) : 0;
      });
      this.stage = "installed";
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
      this.stage = "error";
    }
  };

  static styles = css`
    :host {
      display: block;
      display: flex;
      flex-direction: column;
      gap: 12px;
    }
    .head {
      display: flex;
      flex-direction: column;
      gap: 2px;
    }
    .head strong {
      font-family: var(--fr);
      font-size: 1.1rem;
    }
    .muted {
      color: var(--muted);
      font: 0.9rem/1.5 var(--ns);
      margin: 0;
    }
    .error {
      color: var(--err);
      font: 0.9rem var(--ns);
      margin: 0;
    }
    .cta {
      padding: 12px 18px;
      border: 1px solid var(--accent);
      border-radius: var(--radius-sm, 9px);
      color: #fff;
      background: var(--accent);
      font: 700 0.85rem var(--mn);
      letter-spacing: 0.02em;
      cursor: pointer;
      transition: background 0.15s ease;
    }
    .cta:hover {
      background: var(--accent-deep);
      border-color: var(--accent-deep);
    }
    .cta.ghost {
      background: transparent;
      color: var(--accent);
      border: 1px solid var(--accent);
    }
    .cta:disabled {
      opacity: 0.55;
      cursor: not-allowed;
    }
    .release {
      display: flex;
      flex-direction: column;
      gap: 10px;
      padding: 14px;
      border: 1px solid var(--line);
      border-radius: var(--radius, 13px);
      background: var(--paper-tint);
    }
    .release header {
      display: flex;
      justify-content: space-between;
      align-items: flex-start;
      gap: 12px;
    }
    .release h4 {
      margin: 0;
      font-family: var(--fr);
      font-size: 1.15rem;
    }
    .release small {
      color: var(--muted);
      font: 0.78rem var(--mn);
    }
    .badge {
      padding: 3px 8px;
      border-radius: var(--radius-pill, 999px);
      background: var(--sky-2);
      color: var(--ink-soft);
      font: 600 0.68rem var(--mn);
    }
    .badge.ok {
      background: rgba(47, 122, 77, 0.14);
      color: var(--green);
    }
    .changelog {
      margin: 0;
      padding: 10px 12px;
      background: #fff;
      border: 1px solid var(--line);
      border-radius: var(--radius-sm, 9px);
      font: 0.82rem/1.55 var(--mn);
      color: var(--ink-soft);
      white-space: pre-wrap;
      max-height: 200px;
      overflow-y: auto;
    }
    .asset {
      display: flex;
      flex-direction: column;
      gap: 6px;
      font: 0.88rem var(--ns);
      color: var(--muted);
    }
    .asset strong {
      color: var(--ink);
    }
    .asset progress {
      width: 100%;
      height: 6px;
    }
    .row {
      display: flex;
      gap: 8px;
      flex-wrap: wrap;
    }
    .row .cta {
      flex: 1 1 auto;
    }
    .ok {
      color: var(--ok);
      font: 600 0.85rem var(--ns);
    }
    .link {
      align-self: flex-start;
      color: var(--accent);
      font: 600 0.85rem var(--mn);
      text-decoration: none;
    }
    .link:hover {
      text-decoration: underline;
    }
  `;
}

declare global {
  interface HTMLElementTagNameMap {
    "updates-panel": UpdatesPanel;
  }
}

function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} kB`;
  return `${(n / 1024 / 1024).toFixed(2)} MB`;
}

function trimChangelog(text: string): string {
  const lines = text.split("\n");
  return lines.slice(0, 12).join("\n") + (lines.length > 12 ? "\n…" : "");
}

function blobToBase64(blob: Blob): Promise<string> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(String(reader.result).replace(/^data:[^,]*,/, ""));
    reader.onerror = () => reject(reader.error);
    reader.readAsDataURL(blob);
  });
}
