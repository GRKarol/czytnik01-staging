import { LitElement, css, html } from "lit";
import { customElement, state } from "lit/decorators.js";
import {
  fetchLatestRelease,
  pickFirmwareAsset,
  downloadAsset,
  isNewer,
  type ReleaseInfo,
} from "../updates/releases";
import { deviceApi } from "../device/api";

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
  /** Aktualna wersja firmware na urządzeniu (na razie nieznana). */
  @state() private currentFw: string | null = null;
  private downloaded: Blob | null = null;

  render() {
    return html`
      <div class="head">
        <strong>Aktualizacje firmware</strong>
        <span class="muted">repo: GRKarol/czytnik01 · ostatni release</span>
      </div>

      ${this.renderStage()}
    `;
  }

  private renderStage() {
    switch (this.stage) {
      case "idle":
        return html`
          <p class="muted">
            Sprawdzimy GitHub Releases tego repo i — jeśli będzie nowsza wersja — pobierzemy ją
            do telefonu. Wysłanie na urządzenie wymaga firmware z endpointem OTA (zrobimy to
            w fazie 3).
          </p>
          <button class="cta" @click=${this.check}>Sprawdź aktualizacje</button>
        `;
      case "checking":
        return html`<p class="muted">Łączę z GitHubem…</p>`;
      case "none":
        return html`
          <p class="muted">
            Brak opublikowanych releasów. Karol musi najpierw zrobić release na GitHubie
            (Settings → Releases → Draft a new release) z plikiem .bin.
          </p>
          <button class="cta ghost" @click=${this.check}>Sprawdź ponownie</button>
        `;
      case "error":
        return html`
          <p class="error">${this.error}</p>
          <button class="cta ghost" @click=${this.check}>Spróbuj ponownie</button>
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
    const newer = this.currentFw ? isNewer(tag, this.currentFw) : true;
    const date = new Date(r.publishedAt).toLocaleDateString("pl-PL");

    return html`
      <article class="release">
        <header>
          <div>
            <h4>${r.name}</h4>
            <small>${tag} · ${date}${r.isPrerelease ? " · pre-release" : ""}</small>
          </div>
          ${newer
            ? html`<span class="badge ok">Dostępna</span>`
            : html`<span class="badge">Aktualna</span>`}
        </header>

        ${r.body
          ? html`<pre class="changelog">${trimChangelog(r.body)}</pre>`
          : html`<p class="muted">Brak opisu wersji.</p>`}

        ${asset
          ? html`
              <div class="asset">
                <span><strong>${asset.name}</strong> · ${formatBytes(asset.size)}</span>
                ${this.stage === "downloading" || this.stage === "installing"
                  ? html`<progress max="100" value=${this.progress}></progress>`
                  : ""}
                ${this.stage === "downloaded"
                  ? html`<span class="ok">Pobrano</span>`
                  : ""}
                ${this.stage === "installed"
                  ? html`<span class="ok">Zainstalowano · urządzenie się restartuje</span>`
                  : ""}
              </div>
              <div class="row">
                ${this.stage === "found"
                  ? html`<button class="cta" @click=${() => this.download(asset)}>
                      Pobierz na telefon
                    </button>`
                  : ""}
                ${this.stage === "downloaded"
                  ? html`<button class="cta" @click=${this.install}>
                        Wyślij na urządzenie
                      </button>
                      <button class="cta ghost" @click=${this.savePhone}>
                        Zapisz plik na telefonie
                      </button>`
                  : ""}
                ${this.stage === "installing"
                  ? html`<span class="muted">Wysyłam ${this.progress}%…</span>`
                  : ""}
              </div>
            `
          : html`<p class="muted">Brak pliku .bin w tym release.</p>`}

        <a class="link" href=${r.htmlUrl} target="_blank" rel="noopener noreferrer">
          Otwórz na GitHubie ↗
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

  private savePhone = () => {
    if (!this.downloaded || !this.release) return;
    const asset = pickFirmwareAsset(this.release);
    if (!asset) return;
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
