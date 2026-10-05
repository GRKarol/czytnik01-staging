import { LitElement, css, html, nothing, type TemplateResult } from "lit";
import { customElement, state } from "lit/decorators.js";
import { Directory, Filesystem } from "@capacitor/filesystem";
import { Share } from "@capacitor/share";
import { deviceApi, onDeviceApiChange, type PluginFile, type PluginInfo } from "../device/api";
import { HttpDeviceApi } from "../device/http-api";
import { isNativeApp } from "../device/network-pin";
import { onLangChange, tr } from "../i18n/index";
import { icons } from "../ui/icons";
import { onBack } from "../ui/back-nav";
import { sharedStyles } from "../ui/theme";

/**
 * Plugins tab: the reader's plugins in the reader's order, each switchable
 * from here, reorderable, and opening a page of its own. What a page offers
 * beyond the switch comes from what the reader reports for the plugin
 * (files: list, play, save to the phone, delete) plus the per-id sections in
 * renderFunctions() (RSS feeds). A new plugin gets its page for free; one
 * with phone-side functions adds a case there.
 */
@customElement("plugins-panel")
export class PluginsPanel extends LitElement {
  @state() private plugins: PluginInfo[] = [];
  @state() private loading = false;
  @state() private error = "";
  @state() private ordering = false;
  @state() private openId: string | null = null;

  // Files of the open plugin.
  @state() private files: PluginFile[] = [];
  @state() private filesMime = "";
  @state() private filesLoading = false;
  @state() private busyFile = "";
  @state() private confirmDelete = "";
  @state() private playing = "";
  @state() private playUrl = "";

  // RSS page.
  @state() private rssFeeds: string[] = [];
  @state() private rssBusy = false;
  @state() private newFeedUrl = "";

  private unsubApi: (() => void) | null = null;
  private unsubLang: (() => void) | null = null;
  private unsubBack: (() => void) | null = null;

  connectedCallback(): void {
    super.connectedCallback();
    this.unsubApi = onDeviceApiChange(() => void this.load());
    this.unsubLang = onLangChange(() => this.requestUpdate());
    this.unsubBack = onBack(() => {
      if (this.confirmDelete) {
        this.confirmDelete = "";
        return true;
      }
      if (this.openId === null) return false;
      this.closePlugin();
      return true;
    });
    void this.load();
  }

  disconnectedCallback(): void {
    super.disconnectedCallback();
    this.unsubApi?.();
    this.unsubLang?.();
    this.unsubBack?.();
    this.stopPlaying();
  }

  private get onReader(): boolean {
    return deviceApi.current instanceof HttpDeviceApi;
  }

  private async load(): Promise<void> {
    this.loading = true;
    this.error = "";
    try {
      this.plugins = await deviceApi.getPlugins();
    } catch (err) {
      this.plugins = [];
      this.error = this.message(err);
    } finally {
      this.loading = false;
    }
  }

  private message(err: unknown): string {
    return err instanceof Error ? err.message : String(err);
  }

  // ─── Names ───────────────────────────────────────────────────────────────

  /** The app's own translation where it has one, the reader's text otherwise. */
  private nameOf(p: PluginInfo): string {
    const key = `plugin.${p.id}.name`;
    const text = tr(key);
    return text === key ? p.name : text;
  }

  private descOf(p: PluginInfo): string {
    const key = `plugin.${p.id}.desc`;
    const text = tr(key);
    return text === key ? (p.description ?? "") : text;
  }

  // ─── Render ──────────────────────────────────────────────────────────────

  render() {
    const open = this.openId ? this.plugins.find((p) => p.id === this.openId) : undefined;
    return html`
      ${this.error ? html`<p class="error">${this.error}</p>` : nothing}
      ${open ? this.renderDetail(open) : this.renderList()}
    `;
  }

  private renderList(): TemplateResult {
    if (this.loading && this.plugins.length === 0) {
      return html`<p class="muted">${tr("common.loading")}</p>`;
    }
    if (this.plugins.length === 0) {
      return html`<p class="notice">${tr("plugins.none")}</p>`;
    }
    const last = this.plugins.length - 1;
    return html`
      <p class="muted">${tr("plugins.lead")}</p>
      <div class="row end">
        <button class="btn small ${this.ordering ? "primary" : ""}" @click=${() => (this.ordering = !this.ordering)}>
          ${this.ordering ? tr("plugins.orderDone") : tr("plugins.order")}
        </button>
      </div>
      <div class="list">
        ${this.plugins.map(
          (p, i) => html`<div class="item plugin">
            <button class="open" ?disabled=${this.ordering} @click=${() => this.openPlugin(p.id)}>
              <span class="tile-ico ${p.active ? "on" : ""}">${icons.plugins(20)}</span>
              <span class="label">
                <span class="title">${this.nameOf(p)}</span>
                <small class="clamp">${this.descOf(p)}</small>
              </span>
            </button>
            ${this.ordering
              ? html`<span class="moves">
                  <button class="icon-btn" ?disabled=${i === 0} @click=${() => this.move(i, -1)} aria-label=${tr("plugins.up")}>
                    <span class="up">${icons.chevronLeft(18)}</span>
                  </button>
                  <button class="icon-btn" ?disabled=${i === last} @click=${() => this.move(i, 1)} aria-label=${tr("plugins.down")}>
                    <span class="down">${icons.chevronLeft(18)}</span>
                  </button>
                </span>`
              : html`<input
                  class="switch"
                  type="checkbox"
                  aria-label=${this.nameOf(p)}
                  .checked=${p.active}
                  @change=${(e: Event) => this.setActive(p, (e.target as HTMLInputElement).checked)}
                />`}
          </div>`,
        )}
      </div>
      <p class="muted small">${tr("plugins.orderHint")}</p>
    `;
  }

  private renderDetail(p: PluginInfo): TemplateResult {
    return html`
      <button class="btn quiet small back" @click=${this.closePlugin}>${icons.chevronLeft(18)} ${tr("nav.plugins")}</button>
      <h2>${this.nameOf(p)}</h2>
      ${this.descOf(p) ? html`<p class="muted">${this.descOf(p)}</p>` : nothing}
      <div class="list">
        <label class="item">
          <span class="label">${tr("plugins.onReader")}<small>${tr("plugins.onReader.desc")}</small></span>
          <input class="switch" type="checkbox" .checked=${p.active}
            @change=${(e: Event) => this.setActive(p, (e.target as HTMLInputElement).checked)} />
        </label>
      </div>
      <span class="section-title">${tr("plugins.functions")}</span>
      ${this.renderFunctions(p)}
    `;
  }

  /** What the phone can do with this plugin. */
  private renderFunctions(p: PluginInfo): TemplateResult {
    const parts: TemplateResult[] = [];
    if (p.files) parts.push(this.renderFiles(p));
    if (p.id === "rss") parts.push(this.renderRss());
    if (parts.length === 0) {
      return html`<p class="notice">${tr("plugins.noFunctions")}</p>`;
    }
    return html`${parts}`;
  }

  private renderFiles(p: PluginInfo): TemplateResult {
    if (!this.onReader) {
      return html`<p class="notice">${tr("plugins.files.needWifi")}</p>`;
    }
    const audio = this.filesMime.startsWith("audio/");
    return html`
      <div class="row between">
        <strong>${tr(`plugin.${p.id}.files`) === `plugin.${p.id}.files` ? tr("plugins.files") : tr(`plugin.${p.id}.files`)}</strong>
        <button class="btn small" ?disabled=${this.filesLoading} @click=${() => this.loadFiles(p.id)}>${tr("common.refresh")}</button>
      </div>
      ${this.playUrl && audio
        ? html`<div class="player">
            <span>${this.playing}</span>
            <audio src=${this.playUrl} controls autoplay @ended=${this.stopPlaying}></audio>
          </div>`
        : nothing}
      <div class="list">
        ${this.filesLoading && this.files.length === 0
          ? html`<div class="item"><span class="label"><small>${tr("common.loading")}</small></span></div>`
          : this.files.length === 0
            ? html`<div class="item"><span class="label"><small>${tr("plugins.files.none")}</small></span></div>`
            : this.files.map((f) =>
                this.confirmDelete === f.name
                  ? html`<div class="item">
                      <span class="label">${tr("plugins.files.confirm", { name: f.name })}</span>
                      <span class="actions">
                        <button class="btn small danger" ?disabled=${this.busyFile === f.name} @click=${() => this.deleteFile(p.id, f.name)}>
                          ${tr("plugins.files.delete")}
                        </button>
                        <button class="btn small" @click=${() => (this.confirmDelete = "")}>${tr("common.cancel")}</button>
                      </span>
                    </div>`
                  : html`<div class="item">
                      <span class="label">
                        <span class="title">${f.name}</span>
                        <small>${this.fileDetail(f, audio)}</small>
                      </span>
                      <span class="actions">
                        ${audio
                          ? html`<button class="icon-btn" ?disabled=${!!this.busyFile} @click=${() => this.play(p.id, f.name)} aria-label=${tr("plugins.files.play")}>
                              ${icons.play(18)}
                            </button>`
                          : nothing}
                        <button class="icon-btn" ?disabled=${!!this.busyFile} @click=${() => this.save(p.id, f.name)} aria-label=${tr("plugins.files.save")}>
                          ${icons.download(18)}
                        </button>
                        <button class="icon-btn" ?disabled=${!!this.busyFile} @click=${() => (this.confirmDelete = f.name)} aria-label=${tr("plugins.files.delete")}>
                          ${icons.trash(18)}
                        </button>
                      </span>
                    </div>`,
              )}
      </div>
      ${this.busyFile && !this.confirmDelete ? html`<p class="muted small">${tr("plugins.files.fetching", { name: this.busyFile })}</p>` : nothing}
    `;
  }

  /** Size, and for the dictaphone's WAV (16 kHz, 16-bit mono) its length. */
  private fileDetail(f: PluginFile, audio: boolean): string {
    const kb = Math.max(1, Math.round(f.size / 1024));
    const size = kb >= 1024 ? `${(kb / 1024).toFixed(1)} MB` : `${kb} KB`;
    if (!audio || !f.name.toLowerCase().endsWith(".wav")) return size;
    const seconds = Math.max(0, Math.round((f.size - 44) / 32000));
    return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, "0")} · ${size}`;
  }

  private renderRss(): TemplateResult {
    return html`
      <strong>${tr("rss.title")}</strong>
      <div class="list">
        ${this.rssFeeds.length === 0
          ? html`<div class="item"><span class="label"><small>${tr("rss.none")}</small></span></div>`
          : this.rssFeeds.map(
              (url, i) => html`<div class="item">
                <span class="label"><span class="feed">${url}</span></span>
                <button class="icon-btn" ?disabled=${this.rssBusy} @click=${() => this.removeRssFeed(i)} aria-label=${tr("rss.remove")}>
                  ${icons.close(18)}
                </button>
              </div>`,
            )}
        <div class="item">
          <input
            class="input"
            type="url"
            placeholder="https://example.com/rss.xml"
            .value=${this.newFeedUrl}
            @input=${(e: Event) => (this.newFeedUrl = (e.target as HTMLInputElement).value)}
          />
          <button class="btn small primary" ?disabled=${this.rssBusy} @click=${this.addRssFeed}>${tr("rss.add")}</button>
        </div>
      </div>
    `;
  }

  // ─── Actions ─────────────────────────────────────────────────────────────

  private async setActive(p: PluginInfo, active: boolean): Promise<void> {
    this.error = "";
    const previous = this.plugins;
    this.plugins = this.plugins.map((x) => (x.id === p.id ? { ...x, active } : x));
    try {
      this.plugins = await deviceApi.setPluginActive(p.id, active);
    } catch (err) {
      this.plugins = previous;
      this.error = this.message(err);
    }
  }

  private async move(index: number, delta: -1 | 1): Promise<void> {
    const target = index + delta;
    if (target < 0 || target >= this.plugins.length) return;
    const next = [...this.plugins];
    [next[index], next[target]] = [next[target], next[index]];
    const previous = this.plugins;
    this.plugins = next;
    this.error = "";
    try {
      this.plugins = await deviceApi.setPluginOrder(next.map((p) => p.id));
    } catch (err) {
      this.plugins = previous;
      this.error = this.message(err);
    }
  }

  private openPlugin(id: string): void {
    this.openId = id;
    this.files = [];
    this.confirmDelete = "";
    const p = this.plugins.find((x) => x.id === id);
    if (p?.files && this.onReader) void this.loadFiles(id);
    if (id === "rss") void this.loadRss();
  }

  private closePlugin = () => {
    this.stopPlaying();
    this.openId = null;
  };

  private async loadFiles(id: string): Promise<void> {
    this.filesLoading = true;
    this.error = "";
    try {
      const data = await deviceApi.listPluginFiles(id);
      this.files = data.files;
      this.filesMime = data.mime;
    } catch (err) {
      this.error = this.message(err);
    } finally {
      this.filesLoading = false;
    }
  }

  private async play(id: string, name: string): Promise<void> {
    this.stopPlaying();
    this.busyFile = name;
    this.error = "";
    try {
      const blob = await deviceApi.getPluginFile(id, name);
      this.playUrl = URL.createObjectURL(blob);
      this.playing = name;
    } catch (err) {
      this.error = this.message(err);
    } finally {
      this.busyFile = "";
    }
  }

  private stopPlaying = () => {
    if (this.playUrl) URL.revokeObjectURL(this.playUrl);
    this.playUrl = "";
    this.playing = "";
  };

  private async save(id: string, name: string): Promise<void> {
    this.busyFile = name;
    this.error = "";
    try {
      const blob = await deviceApi.getPluginFile(id, name);
      if (isNativeApp()) {
        // A WebView ignores <a download>: Android's share sheet saves it
        // (Pliki, Dysk, komunikator…).
        const { uri } = await Filesystem.writeFile({
          path: name,
          data: await blobToBase64(blob),
          directory: Directory.Cache,
        });
        await Share.share({ title: name, files: [uri] });
      } else {
        const url = URL.createObjectURL(blob);
        const a = document.createElement("a");
        a.href = url;
        a.download = name;
        a.click();
        URL.revokeObjectURL(url);
      }
    } catch (err) {
      const message = this.message(err);
      if (!/cancel/i.test(message)) this.error = message;
    } finally {
      this.busyFile = "";
    }
  }

  private async deleteFile(id: string, name: string): Promise<void> {
    this.busyFile = name;
    this.error = "";
    try {
      await deviceApi.deletePluginFile(id, name);
      if (this.playing === name) this.stopPlaying();
      this.files = this.files.filter((f) => f.name !== name);
      this.confirmDelete = "";
    } catch (err) {
      this.error = this.message(err);
    } finally {
      this.busyFile = "";
    }
  }

  private async loadRss(): Promise<void> {
    try {
      this.rssFeeds = await deviceApi.getRssFeeds();
    } catch (err) {
      this.error = this.message(err);
    }
  }

  private addRssFeed = async () => {
    const url = this.newFeedUrl.trim();
    if (!url) return;
    this.rssBusy = true;
    this.error = "";
    try {
      this.rssFeeds = await deviceApi.setRssFeeds([...this.rssFeeds, url]);
      this.newFeedUrl = "";
    } catch (err) {
      this.error = this.message(err);
    } finally {
      this.rssBusy = false;
    }
  };

  private removeRssFeed = async (index: number) => {
    this.rssBusy = true;
    this.error = "";
    try {
      this.rssFeeds = await deviceApi.setRssFeeds(this.rssFeeds.filter((_, i) => i !== index));
    } catch (err) {
      this.error = this.message(err);
    } finally {
      this.rssBusy = false;
    }
  };

  static styles = [
    sharedStyles,
    css`
      :host {
        display: grid;
        gap: 12px;
      }
      .row {
        display: flex;
        align-items: center;
        gap: 8px;
      }
      .row.end {
        justify-content: flex-end;
      }
      .row.between {
        justify-content: space-between;
      }
      .item.plugin {
        gap: 8px;
      }
      .open {
        display: flex;
        align-items: center;
        gap: 12px;
        flex: 1;
        min-width: 0;
        padding: 0;
        border: 0;
        background: none;
        color: inherit;
        text-align: left;
        cursor: pointer;
      }
      .open:disabled {
        cursor: default;
      }
      .tile-ico {
        display: grid;
        place-items: center;
        flex: none;
        width: 38px;
        height: 38px;
        border-radius: var(--radius-sm);
        background: var(--surface-3);
        color: var(--muted);
      }
      .tile-ico.on {
        background: var(--accent-soft);
        color: var(--accent-text);
      }
      .title {
        display: block;
        font-weight: 600;
      }
      .clamp {
        display: -webkit-box;
        -webkit-line-clamp: 2;
        -webkit-box-orient: vertical;
        overflow: hidden;
      }
      .moves,
      .actions {
        display: flex;
        gap: 4px;
        flex: none;
      }
      .up {
        display: inline-flex;
        transform: rotate(90deg);
      }
      .down {
        display: inline-flex;
        transform: rotate(-90deg);
      }
      .back {
        justify-self: start;
      }
      .player {
        display: grid;
        gap: 6px;
        padding: 10px 12px;
        border-radius: var(--radius);
        background: var(--surface);
      }
      .player audio {
        width: 100%;
      }
      .feed {
        word-break: break-all;
      }
      .input {
        flex: 1;
        min-width: 0;
        padding: 10px 12px;
        border: 1px solid var(--outline);
        border-radius: var(--radius-sm);
        background: var(--surface-2);
        color: var(--text);
        font: inherit;
      }
      .small {
        font-size: 0.85rem;
      }
      .error {
        color: var(--err);
      }
    `,
  ];
}

function blobToBase64(blob: Blob): Promise<string> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(String(reader.result).split(",")[1] ?? "");
    reader.onerror = () => reject(reader.error);
    reader.readAsDataURL(blob);
  });
}
