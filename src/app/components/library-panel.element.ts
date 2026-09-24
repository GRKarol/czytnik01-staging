import { LitElement, css, html } from "lit";
import { customElement, state } from "lit/decorators.js";
import { deviceApi, onDeviceApiChange, type Book } from "../device/api";
import "./first-use-hint.element";

type SortMode = "added" | "title" | "progress";

const STORE_FAVORITES = "flower.library.favorites";
const STORE_SORT = "flower.library.sort";

const SORT_LABEL: Record<SortMode, string> = {
  added: "Ostatnio dodane",
  title: "Tytuł",
  progress: "Postęp",
};

@customElement("library-panel")
export class LibraryPanel extends LitElement {
  @state() private books: Book[] = [];
  @state() private loading = true;
  @state() private error = "";
  @state() private filter: "all" | "book" | "article" = "all";
  @state() private sort: SortMode = readSort();
  @state() private favorites: Set<string> = readFavorites();
  private unsubApi: (() => void) | null = null;

  connectedCallback(): void {
    super.connectedCallback();
    void this.refresh();
    this.unsubApi = onDeviceApiChange(() => void this.refresh());
  }

  disconnectedCallback(): void {
    super.disconnectedCallback();
    this.unsubApi?.();
  }

  render() {
    if (this.loading) return html`<p class="muted">Wczytuję bibliotekę…</p>`;
    if (this.error) return html`<p class="error">${this.error}</p>`;

    const list = this.filtered();
    return html`
      <first-use-hint screen-key="reading"></first-use-hint>
      <div class="actions">
        <input id="upload" type="file" accept=".rsvp,.txt,.epub" hidden @change=${this.onUpload} />
        <label for="upload" class="btn">Wyślij plik na urządzenie</label>
        <button class="btn ghost" @click=${this.refresh}>Odśwież</button>
      </div>

      <div class="tabs">
        ${this.tabButton("all", "Wszystko", this.books.length)}
        ${this.tabButton(
          "book",
          "Książki",
          this.books.filter((b) => b.category !== "article").length,
        )}
        ${this.tabButton(
          "article",
          "Artykuły",
          this.books.filter((b) => b.category === "article").length,
        )}
      </div>

      <div class="sortbar">
        <span class="sortbar-label">Sortuj:</span>
        ${(Object.keys(SORT_LABEL) as SortMode[]).map(
          (mode) => html`
            <button
              class=${this.sort === mode ? "sortbtn active" : "sortbtn"}
              @click=${() => this.setSort(mode)}
            >
              ${SORT_LABEL[mode]}
            </button>
          `,
        )}
      </div>

      ${list.length === 0
        ? html`<p class="muted">
            Pusto. Wyślij coś z telefonu albo przekonwertuj plik w zakładce
            <strong>Konwerter</strong>.
          </p>`
        : html`<ul class="list">
            ${list.map((b) => this.row(b))}
          </ul>`}

      <p class="hint muted">
        Lista jest na razie symulowana w pamięci telefonu — kiedy firmware zacznie odpowiadać przez
        WiFi, ta sama logika pójdzie na realne API.
      </p>
    `;
  }

  private tabButton(key: typeof this.filter, label: string, count: number) {
    return html`
      <button
        class=${this.filter === key ? "tab active" : "tab"}
        @click=${() => (this.filter = key)}
      >
        ${label} <span>${count}</span>
      </button>
    `;
  }

  private row(b: Book) {
    const title = b.title || b.name.replace(/^.*\//, "");
    const isFav = this.favorites.has(b.name);
    return html`
      <li>
        <div class="cover" style="background:${coverColor(title)}">${coverInitial(title)}</div>
        <div class="meta">
          <strong>${title}</strong>
          <span>
            ${b.author ? `${b.author} · ` : ""}${formatBytes(b.bytes)}
            ${b.progressPercent != null ? html` · ${b.progressPercent}% przeczytane` : ""}
          </span>
        </div>
        <button
          class=${isFav ? "fav active" : "fav"}
          @click=${() => this.toggleFavorite(b.name)}
          aria-label=${isFav ? "Usuń z ulubionych" : "Dodaj do ulubionych"}
        >
          ${isFav ? "★" : "☆"}
        </button>
        ${b.progressPercent
          ? html`
              <button
                class="reset"
                @click=${() => this.onResetProgress(b)}
                aria-label="Zresetuj postęp czytania"
                title="Zresetuj postęp czytania"
              >
                ⟲
              </button>
            `
          : ""}
        <button class="del" @click=${() => this.onDelete(b)} aria-label="Usuń">✕</button>
      </li>
    `;
  }

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
        : this.books.filter((b) =>
            this.filter === "book" ? b.category !== "article" : b.category === "article",
          );
    const sorted = [...byCategory].sort((a, b) => {
      switch (this.sort) {
        case "title":
          return (a.title || a.name).localeCompare(b.title || b.name, "pl");
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

  private refresh = async () => {
    this.loading = true;
    this.error = "";
    try {
      this.books = await deviceApi.listBooks();
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
    } finally {
      this.loading = false;
    }
  };

  private onUpload = async (e: Event) => {
    const input = e.target as HTMLInputElement;
    const file = input.files?.[0];
    input.value = "";
    if (!file) return;
    try {
      await deviceApi.uploadBook(file, file.name);
      await this.refresh();
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
    }
  };

  private onDelete = async (b: Book) => {
    if (!confirm(`Usunąć „${b.title || b.name}"?`)) return;
    try {
      await deviceApi.deleteBook(b.name);
      await this.refresh();
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
    }
  };

  private onResetProgress = async (b: Book) => {
    if (!confirm(`Zresetować postęp czytania „${b.title || b.name}"?`)) return;
    try {
      await deviceApi.setBookPosition(b.name, { wordIndex: 0 });
      await this.refresh();
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
    }
  };

  static styles = css`
    :host {
      display: block;
      display: flex;
      flex-direction: column;
      gap: 12px;
    }
    .muted {
      color: var(--muted);
      font: 0.92rem/1.45 var(--ns);
      margin: 0;
    }
    .error {
      color: var(--err);
      font: 0.92rem var(--ns);
      margin: 0;
    }
    .actions {
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
      transition: background 0.15s ease;
    }
    .btn:active {
      background: var(--accent-deep);
    }
    .btn.ghost {
      flex: 0 0 auto;
      background: transparent;
      color: var(--accent);
      border: 1px solid var(--accent);
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
      transition: background 0.15s ease;
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
      letter-spacing: 0.02em;
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
      gap: 6px;
    }
    .list li {
      display: flex;
      align-items: center;
      gap: 10px;
      padding: 10px 12px;
      border: 1px solid var(--line);
      border-radius: var(--radius, 13px);
      background: var(--paper-tint);
    }
    .cover {
      width: 34px;
      height: 34px;
      flex: 0 0 auto;
      display: flex;
      align-items: center;
      justify-content: center;
      border-radius: var(--radius-sm, 9px);
      color: #fff;
      font: 700 0.95rem var(--fr);
    }
    .meta {
      flex: 1 1 auto;
      display: flex;
      flex-direction: column;
      gap: 1px;
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
      transition: transform 0.1s ease;
    }
    .fav:active,
    .del:active {
      transform: scale(0.88);
    }
    .fav.active {
      color: #e0a30d;
    }
    .reset {
      width: 32px;
      height: 32px;
      border: 0;
      border-radius: 50%;
      background: rgba(20, 136, 216, 0.1);
      color: var(--accent);
      font-size: 1rem;
      cursor: pointer;
      flex: 0 0 auto;
      transition: transform 0.1s ease;
    }
    .reset:active {
      transform: scale(0.88) rotate(-40deg);
    }
    .del {
      width: 32px;
      height: 32px;
      border: 0;
      border-radius: 50%;
      background: rgba(228, 77, 101, 0.1);
      color: var(--err);
      font-size: 0.85rem;
      cursor: pointer;
      flex: 0 0 auto;
      transition: transform 0.1s ease;
    }
    .hint {
      font-size: 0.78rem;
      font-style: italic;
    }
  `;
}

declare global {
  interface HTMLElementTagNameMap {
    "library-panel": LibraryPanel;
  }
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

// Deterministyczna "okładka": kolor + inicjał z tytułu, bez ekstrakcji
// obrazu z EPUB (RSVP na urządzeniu i tak nie renderuje grafik).
const COVER_HUES = [4, 24, 44, 96, 152, 190, 210, 252, 280, 320];

function coverColor(title: string): string {
  let hash = 0;
  for (let i = 0; i < title.length; i++) hash = (hash * 31 + title.charCodeAt(i)) | 0;
  const hue = COVER_HUES[Math.abs(hash) % COVER_HUES.length];
  return `hsl(${hue} 55% 42%)`;
}

function coverInitial(title: string): string {
  const trimmed = title.trim();
  return trimmed ? trimmed[0].toUpperCase() : "?";
}
