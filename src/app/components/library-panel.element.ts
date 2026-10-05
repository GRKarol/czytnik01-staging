import { LitElement, css, html, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { deviceApi, onDeviceApiChange, type Book, type DeviceCapabilities } from "../device/api";
import { HttpDeviceApi } from "../device/http-api";
import { extractEpubCover } from "../converter/epub";
import { decodePicture, readerCoverColor, readerInitials } from "../books/pictures";
import { onLangChange, tr } from "../i18n/index";
import { onBack } from "../ui/back-nav";
import "./first-use-hint.element";
import "./cover-editor.element";
import "./chapter-editor.element";

type SortMode = "added" | "title" | "progress";

const STORE_FAVORITES = "flower.library.favorites";
const STORE_SORT = "flower.library.sort";

const SORT_LABEL: Record<SortMode, string> = {
  added: "lib.sort.added",
  title: "lib.sort.title",
  progress: "lib.sort.progress",
};

// Covers downloaded from the reader, by book name (data URLs).
const coverCache = new Map<string, string>();

interface JustSent {
  name: string;
  title: string;
  epubCover: Blob | null;
}

@customElement("library-panel")
export class LibraryPanel extends LitElement {
  @state() private books: Book[] = [];
  @state() private loading = true;
  @state() private error = "";
  @state() private filter: "all" | "book" | "article" = "all";
  @state() private sort: SortMode = readSort();
  @state() private favorites: Set<string> = readFavorites();
  @state() private caps: DeviceCapabilities | null = null;
  @state() private uploadProgress: number | null = null;
  @state() private justSent: JustSent | null = null;
  @state() private coverFor: { book: Book; epubCover: Blob | null } | null = null;
  @state() private chaptersFor: Book | null = null;
  @state() private covers = new Map<string, string>();
  private unsubApi: (() => void) | null = null;
  private unsubLang: (() => void) | null = null;
  private unsubBack: (() => void) | null = null;

  connectedCallback(): void {
    super.connectedCallback();
    void this.refresh();
    this.unsubApi = onDeviceApiChange(() => void this.refresh());
    this.unsubLang = onLangChange(() => this.requestUpdate());
    this.unsubBack = onBack(() => {
      if (this.coverFor) {
        this.coverFor = null;
        return true;
      }
      if (this.chaptersFor) {
        this.chaptersFor = null;
        return true;
      }
      return false;
    });
  }

  disconnectedCallback(): void {
    super.disconnectedCallback();
    this.unsubApi?.();
    this.unsubLang?.();
    this.unsubBack?.();
  }

  private get onReader(): boolean {
    return deviceApi.current instanceof HttpDeviceApi;
  }

  // Unknown capabilities (the request timed out while the reader was busy)
  // count as supported: a newer reader must not be told it is too old. On a
  // truly old one the cover or chapter request itself reports the error.
  private get picturesSupported(): boolean {
    return !this.onReader || !this.caps || !!this.caps.bookPictures;
  }

  private get chaptersSupported(): boolean {
    return !this.onReader || !this.caps || !!this.caps.chapterEditor;
  }

  render() {
    if (this.loading) return html`<p class="muted">${tr("lib.loading")}</p>`;

    const list = this.filtered();
    return html`
      <first-use-hint screen-key="reading"></first-use-hint>
      ${this.error ? html`<p class="error">${this.error}</p>` : nothing}
      ${!this.onReader
        ? html`<p class="notice">${tr("lib.sample")}</p>`
        : nothing}
      ${this.onReader && (!this.picturesSupported || !this.chaptersSupported)
        ? html`<p class="notice">${tr("lib.oldFirmware")}</p>`
        : nothing}

      <div class="actions">
        <input id="upload" type="file" accept=".rsvp,.txt,.epub" hidden @change=${this.onUpload} />
        <label for="upload" class="btn ${this.uploadProgress !== null ? "busy" : ""}">
          ${this.uploadProgress !== null ? tr("lib.sending", { n: this.uploadProgress }) : tr("lib.send")}
        </label>
        <button class="btn ghost" @click=${this.refresh}>${tr("common.refresh")}</button>
      </div>
      ${this.uploadProgress !== null
        ? html`<div class="progress"><span style="width:${this.uploadProgress}%"></span></div>`
        : nothing}
      ${this.justSent ? this.renderJustSent(this.justSent) : nothing}

      <div class="tabs">
        ${this.tabButton("all", tr("lib.all"), this.books.length)}
        ${this.tabButton("book", tr("lib.books"), this.books.filter((b) => b.category !== "article").length)}
        ${this.tabButton("article", tr("lib.articles"), this.books.filter((b) => b.category === "article").length)}
      </div>

      <div class="sortbar">
        <span class="sortbar-label">${tr("lib.sortBy")}</span>
        ${(Object.keys(SORT_LABEL) as SortMode[]).map(
          (mode) => html`
            <button class=${this.sort === mode ? "sortbtn active" : "sortbtn"} @click=${() => this.setSort(mode)}>
              ${tr(SORT_LABEL[mode])}
            </button>
          `,
        )}
      </div>

      ${list.length === 0
        ? html`<p class="muted">${tr("lib.empty")}</p>`
        : html`<ul class="list">
            ${list.map((b) => this.row(b))}
          </ul>`}

      ${this.coverFor
        ? html`<cover-editor
            .bookName=${this.coverFor.book.name}
            .bookTitle=${bookTitle(this.coverFor.book)}
            .bookAuthor=${this.coverFor.book.author ?? ""}
            .hasCover=${!!this.coverFor.book.hasCover}
            .hasSpine=${!!this.coverFor.book.hasSpine}
            .epubCover=${this.coverFor.epubCover}
            @close=${() => (this.coverFor = null)}
            @saved=${this.onCoverSaved}
          ></cover-editor>`
        : nothing}
      ${this.chaptersFor
        ? html`<chapter-editor
            .bookName=${this.chaptersFor.name}
            .bookTitle=${bookTitle(this.chaptersFor)}
            @close=${() => (this.chaptersFor = null)}
            @saved=${() => void this.refresh(true)}
          ></chapter-editor>`
        : nothing}
    `;
  }

  private renderJustSent(sent: JustSent) {
    const book = this.books.find((b) => b.name === sent.name);
    return html`
      <div class="sent">
        <p>${tr("lib.sent", { title: sent.title })}</p>
        <div class="row">
          ${this.picturesSupported
            ? html`<button
                class="btn small"
                ?disabled=${!book}
                @click=${() => book && this.openCover(book, sent.epubCover)}
              >
                ${sent.epubCover ? tr("lib.coverFromEpub") : tr("lib.addCover")}
              </button>`
            : nothing}
          ${this.chaptersSupported
            ? html`<button class="btn small ghost" ?disabled=${!book} @click=${() => book && this.openChapters(book)}>
                ${tr("lib.chapters")}
              </button>`
            : nothing}
          <button class="btn small ghost" @click=${() => (this.justSent = null)}>${tr("lib.notNow")}</button>
        </div>
      </div>
    `;
  }

  private tabButton(key: typeof this.filter, label: string, count: number) {
    return html`
      <button class=${this.filter === key ? "tab active" : "tab"} @click=${() => (this.filter = key)}>
        ${label} <span>${count}</span>
      </button>
    `;
  }

  private row(b: Book) {
    const title = bookTitle(b);
    const isFav = this.favorites.has(b.name);
    const picture = this.covers.get(b.name);
    return html`
      <li>
        <div class="top">
          <button
            class="cover"
            style=${picture ? `background-image:url(${picture})` : `background:${readerCoverColor(b.name)}`}
            ?disabled=${!this.picturesSupported}
            @click=${() => this.openCover(b, null)}
            aria-label=${tr("lib.cover")}
          >
            ${picture ? nothing : html`<span>${readerInitials(title)}</span>`}
          </button>
          <div class="meta">
            <strong>${title}</strong>
            <span>
              ${b.author ? `${b.author} · ` : ""}${formatBytes(b.bytes)}${b.progressPercent != null
                ? ` · ${tr("lib.readPct", { n: b.progressPercent })}`
                : ""}
            </span>
            ${b.customChapters || b.hasSpine
              ? html`<span class="tags">
                  ${b.customChapters ? html`<em>${tr("lib.tag.chapters")}</em>` : nothing}
                  ${b.hasSpine ? html`<em>${tr("lib.tag.spine")}</em>` : nothing}
                </span>`
              : nothing}
          </div>
          <button
            class=${isFav ? "fav active" : "fav"}
            @click=${() => this.toggleFavorite(b.name)}
            aria-label=${isFav ? tr("lib.fav.remove") : tr("lib.fav.add")}
          >
            ${isFav ? "★" : "☆"}
          </button>
        </div>
        <div class="tools">
          <button class="tool" ?disabled=${!this.picturesSupported} @click=${() => this.openCover(b, null)}>
            ${tr("lib.cover")}
          </button>
          <button class="tool" ?disabled=${!this.chaptersSupported} @click=${() => this.openChapters(b)}>
            ${tr("lib.chapters")}
          </button>
          ${b.progressPercent
            ? html`<button class="tool" @click=${() => this.onResetProgress(b)}>${tr("lib.fromStart")}</button>`
            : nothing}
          <button class="tool danger" @click=${() => this.onDelete(b)}>${tr("lib.delete")}</button>
        </div>
      </li>
    `;
  }

  private openCover(book: Book, epubCover: Blob | null): void {
    this.justSent = null;
    this.coverFor = { book, epubCover };
  }

  private openChapters(book: Book): void {
    this.justSent = null;
    this.chaptersFor = book;
  }

  private onCoverSaved = async () => {
    const name = this.coverFor?.book.name;
    this.coverFor = null;
    if (name) coverCache.delete(name);
    await this.refresh(true);
  };

  private setSort(mode: SortMode): void {
    this.sort = mode;
    write(STORE_SORT, mode);
  }

  private toggleFavorite(name: string): void {
    const next = new Set(this.favorites);
    if (next.has(name)) next.delete(name);
    else next.add(name);
    this.favorites = next;
    write(STORE_FAVORITES, Array.from(next));
  }

  private filtered(): Book[] {
    const byCategory =
      this.filter === "all"
        ? this.books
        : this.books.filter((b) => (this.filter === "book" ? b.category !== "article" : b.category === "article"));
    const sorted = [...byCategory].sort((a, b) => {
      switch (this.sort) {
        case "title":
          return bookTitle(a).localeCompare(bookTitle(b), "pl");
        case "progress":
          return (b.progressPercent ?? 0) - (a.progressPercent ?? 0);
        case "added":
        default:
          return (b.addedAt ?? "").localeCompare(a.addedAt ?? "");
      }
    });
    sorted.sort((a, b) => Number(this.favorites.has(b.name)) - Number(this.favorites.has(a.name)));
    return sorted;
  }

  private refresh = async (quiet = false) => {
    if (quiet !== true) this.loading = true;
    this.error = "";
    try {
      this.books = await deviceApi.listBooks();
      this.caps =
        (await deviceApi.getCapabilities().catch(() => null)) ??
        (await deviceApi.getCapabilities().catch(() => null));
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
    } finally {
      this.loading = false;
    }
    void this.loadCovers();
  };

  // Pictures the reader has, one at a time (the reader serves one request
  // at a time anyway), cached for the session.
  private async loadCovers(): Promise<void> {
    const next = new Map<string, string>();
    for (const book of this.books) {
      if (!book.hasCover) continue;
      let url = coverCache.get(book.name);
      if (!url) {
        try {
          const blob = await deviceApi.getBookPicture(book.name, "cover");
          const canvas = blob ? await decodePicture(blob) : null;
          if (!canvas) continue;
          url = canvas.toDataURL();
          coverCache.set(book.name, url);
        } catch {
          continue;
        }
      }
      next.set(book.name, url);
      this.covers = new Map(next);
    }
    this.covers = next;
  }

  private onUpload = async (e: Event) => {
    const input = e.target as HTMLInputElement;
    const file = input.files?.[0];
    input.value = "";
    if (!file) return;
    this.error = "";
    this.justSent = null;
    this.uploadProgress = 0;
    try {
      const stored = await deviceApi.uploadBook(file, file.name, "book", (loaded, total) => {
        this.uploadProgress = total ? Math.round((loaded / total) * 100) : null;
      });
      const epubCover = /\.epub$/i.test(file.name) ? await extractEpubCover(file) : null;
      await this.refresh(true);
      const name = stored || `books/${file.name}`;
      const book = this.books.find((b) => b.name === name);
      this.justSent = { name, title: book ? bookTitle(book) : file.name, epubCover };
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
    } finally {
      this.uploadProgress = null;
    }
  };

  private onDelete = async (b: Book) => {
    const extras = this.onReader
      ? "\n\n" + tr("lib.confirmDelete.extras")
      : "";
    if (!confirm(tr("lib.confirmDelete", { title: bookTitle(b) }) + extras)) return;
    try {
      await deviceApi.deleteBook(b.name);
      coverCache.delete(b.name);
      await this.refresh(true);
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
    }
  };

  private onResetProgress = async (b: Book) => {
    if (!confirm(tr("lib.confirmRestart", { title: bookTitle(b) }))) return;
    try {
      await deviceApi.setBookPosition(b.name, { wordIndex: 0 });
      await this.refresh(true);
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
    }
  };

  static styles = css`
    :host {
      display: flex;
      flex-direction: column;
      gap: 12px;
    }
    .muted,
    .notice {
      color: var(--muted);
      font: 0.92rem/1.45 var(--ns);
      margin: 0;
    }
    .notice {
      padding: 10px 12px;
      border-radius: var(--radius-sm, 9px);
      background: rgba(227, 179, 85, 0.16);
      color: var(--ink-soft);
      font-size: 0.86rem;
    }
    .error {
      color: var(--err);
      font: 0.92rem var(--ns);
      margin: 0;
    }
    .actions,
    .row {
      display: flex;
      gap: 8px;
      flex-wrap: wrap;
    }
    .btn {
      flex: 1 1 auto;
      padding: 11px 16px;
      text-align: center;
      border: 1px solid var(--accent);
      border-radius: var(--radius-sm, 9px);
      color: #fff;
      background: var(--accent);
      font: 700 0.85rem var(--mn);
      letter-spacing: 0.02em;
      cursor: pointer;
    }
    .btn.busy {
      pointer-events: none;
      opacity: 0.8;
    }
    .btn.small {
      padding: 8px 12px;
      font-size: 0.76rem;
    }
    .btn.ghost {
      flex: 0 0 auto;
      background: transparent;
      color: var(--accent);
    }
    .btn:disabled {
      opacity: 0.5;
    }
    .progress {
      height: 4px;
      border-radius: 2px;
      background: var(--line);
      overflow: hidden;
    }
    .progress span {
      display: block;
      height: 100%;
      background: var(--accent);
      transition: width 0.2s ease;
    }
    .sent {
      display: flex;
      flex-direction: column;
      gap: 8px;
      padding: 12px;
      border: 1px solid var(--accent);
      border-radius: var(--radius, 13px);
      background: rgba(20, 136, 216, 0.06);
    }
    .sent p {
      margin: 0;
      font: 0.9rem/1.4 var(--ns);
    }
    .tabs {
      display: flex;
      gap: 6px;
    }
    .tab {
      flex: 1 1 auto;
      padding: 8px 10px;
      border: 1px solid var(--line);
      border-radius: var(--radius-sm, 9px);
      background: var(--paper-tint);
      color: var(--ink-soft);
      font: 600 0.72rem var(--mn);
      letter-spacing: 0.02em;
      text-transform: uppercase;
      cursor: pointer;
    }
    .tab.active {
      background: var(--accent);
      border-color: var(--accent);
      color: #fff;
    }
    .tab span {
      opacity: 0.7;
      font-weight: 500;
    }
    .sortbar {
      display: flex;
      align-items: center;
      gap: 6px;
      flex-wrap: wrap;
    }
    .sortbar-label {
      font: 0.72rem var(--mn);
      text-transform: uppercase;
      letter-spacing: 0.02em;
      color: var(--muted);
    }
    .sortbtn {
      padding: 5px 10px;
      border: 1px solid var(--line);
      border-radius: var(--radius-pill, 999px);
      background: transparent;
      color: var(--ink-soft);
      font: 600 0.72rem var(--mn);
      cursor: pointer;
    }
    .sortbtn.active {
      background: var(--paper-tint);
      border-color: var(--accent);
      color: var(--accent);
    }
    .list {
      list-style: none;
      margin: 0;
      padding: 0;
      display: flex;
      flex-direction: column;
      gap: 8px;
    }
    .list li {
      display: flex;
      flex-direction: column;
      gap: 8px;
      padding: 10px 12px;
      border: 1px solid var(--line);
      border-radius: var(--radius, 13px);
      background: var(--paper-tint);
    }
    .top {
      display: flex;
      align-items: center;
      gap: 12px;
    }
    /* Same shape as the reader's cover: 46:58, rounded, binding line. */
    .cover {
      position: relative;
      width: 40px;
      height: 50px;
      flex: 0 0 auto;
      border: 0;
      padding: 0 0 0 7px;
      border-radius: 5px;
      background-size: cover;
      background-position: center;
      color: #fff;
      font: 600 0.95rem Inter, system-ui, sans-serif;
      cursor: pointer;
      overflow: hidden;
    }
    .cover::after {
      content: "";
      position: absolute;
      top: 0;
      bottom: 0;
      left: 5px;
      width: 2px;
      background: rgba(8, 8, 10, 0.62);
    }
    .meta {
      flex: 1 1 auto;
      display: flex;
      flex-direction: column;
      gap: 2px;
      min-width: 0;
    }
    .meta strong {
      font-family: var(--fr);
      font-size: 0.98rem;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }
    .meta span {
      font: 0.78rem var(--ns);
      color: var(--muted);
    }
    .tags {
      display: flex;
      gap: 6px;
    }
    .tags em {
      font: 600 0.66rem var(--mn);
      font-style: normal;
      color: var(--accent);
      text-transform: uppercase;
    }
    .fav {
      width: 32px;
      height: 32px;
      border: 0;
      border-radius: 50%;
      background: transparent;
      color: var(--muted);
      font-size: 1.05rem;
      cursor: pointer;
      flex: 0 0 auto;
    }
    .fav.active {
      color: #e0a30d;
    }
    .tools {
      display: flex;
      gap: 6px;
      flex-wrap: wrap;
    }
    .tool {
      flex: 1 1 auto;
      padding: 7px 8px;
      border: 1px solid var(--line);
      border-radius: var(--radius-sm, 9px);
      background: transparent;
      color: var(--ink-soft);
      font: 600 0.72rem var(--mn);
      cursor: pointer;
    }
    .tool:disabled {
      opacity: 0.45;
    }
    .tool.danger {
      color: var(--err);
      border-color: rgba(184, 68, 58, 0.35);
    }
  `;
}

declare global {
  interface HTMLElementTagNameMap {
    "library-panel": LibraryPanel;
  }
}

function bookTitle(b: Book): string {
  return b.title || b.name.replace(/^.*\//, "").replace(/\.[^.]+$/, "");
}

function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} kB`;
  return `${(n / 1024 / 1024).toFixed(2)} MB`;
}

function readFavorites(): Set<string> {
  try {
    const raw = localStorage.getItem(STORE_FAVORITES);
    return new Set(raw ? (JSON.parse(raw) as string[]) : []);
  } catch {
    return new Set();
  }
}

function readSort(): SortMode {
  try {
    const raw = localStorage.getItem(STORE_SORT);
    return raw === "title" || raw === "progress" || raw === "added" ? raw : "added";
  } catch {
    return "added";
  }
}

function write<T>(key: string, value: T): void {
  try {
    localStorage.setItem(key, JSON.stringify(value));
  } catch {
    /* ignored */
  }
}
