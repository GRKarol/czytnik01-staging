import { LitElement, css, html, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { deviceApi, onDeviceApiChange, type Book, type DeviceCapabilities } from "../device/api";
import { HttpDeviceApi } from "../device/http-api";
import { hasSnapshot, snapshotTimeLabel } from "../device/snapshot";
import { decodePicture, readerCoverColor, readerInitials } from "../books/pictures";
import {
  ACCEPTED_BOOK_FILES,
  exportBookAsEpub,
  listPendingBooks,
  readingStatusText,
  removePendingBook,
  sendBookToReader,
  shareFile,
  shareText,
  type PendingBook,
} from "../books/book-transfer";
import { onLangChange, tr } from "../i18n/index";
import { onBack } from "../ui/back-nav";
import "./first-use-hint.element";
import "./cover-editor.element";
import "./chapter-editor.element";

type SortMode = "added" | "title" | "progress";

const STORE_FAVORITES = "flower.library.favorites";
const STORE_SORT = "flower.library.sort";
// When the app first saw each book: the reader keeps no date, so "Dodane"
// sorts by this (Karol 17.10: the sort buttons seemed to do nothing).
const STORE_SEEN = "flower.library.firstSeen";

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
  // Inline editors in a book's row: its name, or the ways to share it.
  @state() private renaming: { name: string; title: string; author: string } | null = null;
  @state() private sharing: string | null = null;
  @state() private shareBusy = "";
  @state() private shareBusyLabel = "";
  @state() private inbox: PendingBook[] = [];
  @state() private inboxBusy = "";
  private unsubApi: (() => void) | null = null;
  private unsubLang: (() => void) | null = null;
  private unsubBack: (() => void) | null = null;

  connectedCallback(): void {
    super.connectedCallback();
    void this.refresh();
    this.unsubApi = onDeviceApiChange(() => void this.refresh());
    this.unsubLang = onLangChange(() => this.requestUpdate());
    document.addEventListener("flower-inbox-changed", this.loadInbox);
    void this.loadInbox();
    this.unsubBack = onBack(() => {
      if (this.renaming || this.sharing) {
        this.renaming = null;
        this.sharing = null;
        return true;
      }
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
    document.removeEventListener("flower-inbox-changed", this.loadInbox);
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

  private get namesSupported(): boolean {
    return !this.onReader || !this.caps || !!this.caps.bookNames;
  }

  /** Without a reader the list is its last known state: look, don't touch. */
  private get readOnly(): boolean {
    return !this.onReader;
  }

  render() {
    if (this.loading) return html`<p class="muted">${tr("lib.loading")}</p>`;

    const list = this.filtered();
    return html`
      <first-use-hint screen-key="reading"></first-use-hint>
      ${this.error ? html`<p class="error">${this.error}</p>` : nothing}
      ${this.readOnly
        ? html`<p class="notice">
            ${hasSnapshot() ? tr("lib.offline", { time: snapshotTimeLabel() }) : tr("lib.offline.none")}
          </p>`
        : nothing}
      ${this.onReader && (!this.picturesSupported || !this.chaptersSupported)
        ? html`<p class="notice">${tr("lib.oldFirmware")}</p>`
        : nothing}

      <div class="actions">
        <input
          id="upload"
          type="file"
          accept=${ACCEPTED_BOOK_FILES}
          hidden
          ?disabled=${this.readOnly}
          @change=${this.onUpload}
        />
        <label for="upload" class="btn ${this.uploadProgress !== null ? "busy" : ""} ${this.readOnly ? "off" : ""}">
          ${this.uploadProgress !== null ? tr("lib.sending", { n: this.uploadProgress }) : tr("lib.send")}
        </label>
        <button class="btn ghost" @click=${this.refresh}>${tr("common.refresh")}</button>
      </div>
      ${this.uploadProgress !== null
        ? html`<div class="progress"><span style="width:${this.uploadProgress}%"></span></div>`
        : nothing}
      ${this.justSent ? this.renderJustSent(this.justSent) : nothing}
      ${this.inbox.length > 0 ? this.renderInbox() : nothing}

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

  /** Books friends sent (shared to Flower or opened with it). */
  private renderInbox() {
    return html`
      <div class="sent inbox">
        <p><strong>${tr("lib.inbox.title")}</strong></p>
        ${this.readOnly ? html`<p class="muted small">${tr("lib.inbox.offline")}</p>` : nothing}
        ${this.inbox.map(
          (item) => html`<div class="inbox-row">
            <span>${item.name}</span>
            <button
              class="btn small"
              ?disabled=${this.readOnly || !!this.inboxBusy || this.uploadProgress !== null}
              @click=${() => this.sendFromInbox(item)}
            >
              ${this.inboxBusy === item.id ? tr("lib.sending", { n: this.uploadProgress ?? 0 }) : tr("lib.inbox.send")}
            </button>
            <button class="btn small ghost" ?disabled=${!!this.inboxBusy} @click=${() => void removePendingBook(item.id)}>
              ${tr("lib.inbox.remove")}
            </button>
          </div>`,
        )}
      </div>
    `;
  }

  private loadInbox = async () => {
    try {
      this.inbox = await listPendingBooks();
    } catch {
      this.inbox = [];
    }
  };

  private async sendFromInbox(item: PendingBook): Promise<void> {
    this.inboxBusy = item.id;
    try {
      if (await this.send(item.file)) await removePendingBook(item.id);
    } finally {
      this.inboxBusy = "";
    }
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
            ?disabled=${!this.picturesSupported || this.readOnly}
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
            ${b.customChapters || b.hasSpine || b.renamed
              ? html`<span class="tags">
                  ${b.renamed ? html`<em>${tr("lib.tag.renamed")}</em>` : nothing}
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
          <button class="tool" ?disabled=${!this.picturesSupported || this.readOnly} @click=${() => this.openCover(b, null)}>
            ${tr("lib.cover")}
          </button>
          <button class="tool" ?disabled=${!this.chaptersSupported || this.readOnly} @click=${() => this.openChapters(b)}>
            ${tr("lib.chapters")}
          </button>
          <button class="tool" ?disabled=${!this.namesSupported || this.readOnly} @click=${() => this.startRename(b)}>
            ${tr("lib.rename")}
          </button>
          <button class=${this.sharing === b.name ? "tool on" : "tool"} @click=${() => this.toggleShare(b)}>
            ${tr("lib.share")}
          </button>
          ${b.progressPercent
            ? html`<button class="tool" ?disabled=${this.readOnly} @click=${() => this.onResetProgress(b)}>
                ${tr("lib.fromStart")}
              </button>`
            : nothing}
          <button class="tool danger" ?disabled=${this.readOnly} @click=${() => this.onDelete(b)}>${tr("lib.delete")}</button>
        </div>
        ${this.renaming?.name === b.name ? this.renderRename(b) : nothing}
        ${this.sharing === b.name ? this.renderShare(b) : nothing}
      </li>
    `;
  }

  private renderRename(b: Book) {
    const r = this.renaming!;
    return html`
      <div class="inline">
        <label>
          <span>${tr("lib.rename.title")}</span>
          <input
            .value=${r.title}
            maxlength="120"
            @input=${(e: Event) => (this.renaming = { ...this.renaming!, title: (e.target as HTMLInputElement).value })}
          />
        </label>
        <label>
          <span>${tr("lib.rename.author")}</span>
          <input
            .value=${r.author}
            maxlength="120"
            @input=${(e: Event) => (this.renaming = { ...this.renaming!, author: (e.target as HTMLInputElement).value })}
          />
        </label>
        <p class="muted small">${tr("lib.rename.note")}</p>
        <div class="row">
          <button class="btn small" ?disabled=${!r.title.trim() && !r.author.trim()} @click=${() => this.saveRename(b)}>
            ${tr("lib.rename.save")}
          </button>
          ${b.renamed
            ? html`<button class="btn small ghost" @click=${() => this.resetRename(b)}>${tr("lib.rename.reset")}</button>`
            : nothing}
          <button class="btn small ghost" @click=${() => (this.renaming = null)}>${tr("common.cancel")}</button>
        </div>
      </div>
    `;
  }

  /** To a friend: what I'm reading as text, or the book as an EPUB file. */
  private renderShare(b: Book) {
    return html`
      <div class="inline">
        <p class="muted small">${tr("lib.share.note")}</p>
        <div class="share-grid">
          <button class="tool" ?disabled=${!!this.shareBusy} @click=${() => this.shareReading(b)}>
            ${tr("lib.share.reading")}
          </button>
          <button class="tool" ?disabled=${!!this.shareBusy || this.readOnly} @click=${() => this.shareBook(b, true)}>
            ${this.shareBusy === "cover" ? this.shareBusyLabel : tr("lib.share.withCover")}
          </button>
          <button class="tool" ?disabled=${!!this.shareBusy || this.readOnly} @click=${() => this.shareBook(b, false)}>
            ${this.shareBusy === "plain" ? this.shareBusyLabel : tr("lib.share.noCover")}
          </button>
        </div>
        ${this.readOnly ? html`<p class="muted small">${tr("lib.share.offline")}</p>` : nothing}
      </div>
    `;
  }

  private startRename(b: Book): void {
    this.sharing = null;
    this.renaming = { name: b.name, title: bookTitle(b), author: b.author ?? "" };
  }

  private toggleShare(b: Book): void {
    this.renaming = null;
    this.sharing = this.sharing === b.name ? null : b.name;
  }

  private async saveRename(b: Book): Promise<void> {
    const r = this.renaming;
    if (!r) return;
    this.error = "";
    try {
      await deviceApi.setBookNames(b.name, r.title.trim(), r.author.trim());
      this.renaming = null;
      await this.refresh(true);
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
    }
  }

  private async resetRename(b: Book): Promise<void> {
    this.error = "";
    try {
      await deviceApi.resetBookNames(b.name);
      this.renaming = null;
      await this.refresh(true);
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
    }
  }

  private async shareReading(b: Book): Promise<void> {
    try {
      await shareText(readingStatusText(b), bookTitle(b));
    } catch (err) {
      if (!isCancel(err)) this.error = err instanceof Error ? err.message : String(err);
    }
  }

  private async shareBook(b: Book, withCover: boolean): Promise<void> {
    this.error = "";
    this.shareBusy = withCover ? "cover" : "plain";
    this.shareBusyLabel = tr("lib.share.preparing", { n: 0 });
    try {
      const file = await exportBookAsEpub(b, withCover, (n) => {
        this.shareBusyLabel = tr("lib.share.preparing", { n });
      });
      await shareFile(file, bookTitle(b));
    } catch (err) {
      if (!isCancel(err)) this.error = err instanceof Error ? err.message : String(err);
    } finally {
      this.shareBusy = "";
    }
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
          return addedTime(b) - addedTime(a) || this.books.indexOf(b) - this.books.indexOf(a);
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
      rememberFirstSeen(this.books);
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
    await this.send(file);
  };

  /**
   * Converted on the phone, sent, opened once by the reader; a book it
   * can't read comes back as the error (and is gone from the card).
   */
  private async send(file: File): Promise<boolean> {
    this.error = "";
    this.justSent = null;
    this.uploadProgress = 0;
    try {
      const sent = await sendBookToReader(file, (n) => (this.uploadProgress = n));
      markSeenNow(sent.name);
      await this.refresh(true);
      const book = this.books.find((b) => b.name === sent.name);
      this.justSent = { name: sent.name, title: book ? bookTitle(book) : sent.title, epubCover: sent.cover };
      return true;
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
      await this.refresh(true);
      return false;
    } finally {
      this.uploadProgress = null;
    }
  }

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
    .btn.off {
      pointer-events: none;
      opacity: 0.45;
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
    .tool.on {
      border-color: var(--accent);
      color: var(--accent);
    }
    .inline {
      display: flex;
      flex-direction: column;
      gap: 8px;
      padding-top: 8px;
      border-top: 1px solid var(--line);
    }
    .inline label {
      display: flex;
      flex-direction: column;
      gap: 4px;
      font: 600 0.72rem var(--mn);
      color: var(--muted);
    }
    .inline input {
      padding: 9px 10px;
      border: 1px solid var(--line);
      border-radius: var(--radius-sm, 9px);
      background: var(--bg);
      color: var(--text);
      font: 0.95rem var(--ns);
    }
    .muted.small {
      font-size: 0.8rem;
    }
    .share-grid {
      display: grid;
      grid-template-columns: 1fr;
      gap: 6px;
    }
    .inbox-row {
      display: flex;
      align-items: center;
      gap: 8px;
    }
    .inbox-row span {
      flex: 1 1 auto;
      min-width: 0;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
      font: 0.86rem var(--ns);
    }
    .inbox-row .btn {
      flex: 0 0 auto;
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

function isCancel(err: unknown): boolean {
  const text = err instanceof Error ? `${err.name} ${err.message}` : String(err);
  return /AbortError|cancel/i.test(text);
}

function readSeen(): Record<string, number> {
  try {
    return JSON.parse(localStorage.getItem(STORE_SEEN) ?? "{}") as Record<string, number>;
  } catch {
    return {};
  }
}

let seen: Record<string, number> = readSeen();

/** Books the app meets for the first time get the date it met them. */
function rememberFirstSeen(books: Book[]): void {
  let changed = false;
  const now = Date.now();
  for (const b of books) {
    if (seen[b.name] == null) {
      seen[b.name] = (b.addedAt && Date.parse(b.addedAt)) || now;
      changed = true;
    }
  }
  if (changed) write(STORE_SEEN, seen);
}

function markSeenNow(name: string): void {
  seen = { ...seen, [name]: Date.now() };
  write(STORE_SEEN, seen);
}

function addedTime(b: Book): number {
  return seen[b.name] ?? ((b.addedAt && Date.parse(b.addedAt)) || 0);
}

function write<T>(key: string, value: T): void {
  try {
    localStorage.setItem(key, JSON.stringify(value));
  } catch {
    /* ignored */
  }
}
