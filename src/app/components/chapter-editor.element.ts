import { LitElement, css, html, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { deviceApi, type BookParagraph, type ChapterMark } from "../device/api";
import { onLangChange, tr } from "../i18n/index";

type Filter = "all" | "chapters" | "suggested";

const FIRST_PAGE = 200;
const PAGE = 400;
// Whole paragraphs, so the search sees every word (a PDF page is often one
// paragraph); the list shows the first SHOWN of them.
const WORDS = 6000;
const SHOWN = 24;
const MAX_TITLE = 60;
const ROWS_STEP = 200;

// Short paragraphs that read like headings: "Rozdział 3", "CZĘŚĆ DRUGA",
// "XII", "7.", "Prolog"...
const HEADING_WORDS =
  /^(rozdzia[łl]|cz[eę][sś][cć]|ksi[eę]ga|tom|prolog|epilog|wst[eę]p|pos[łl]owie|przedmowa|zako[nń]czenie|chapter|part|book|prologue|epilogue|introduction|kapitel|teil|cap[ií]tulo|parte|chapitre|partie)(?=$|[\s.:,)\-–—])/i;
// (not \b: "ł" is not a word character in JS regexes, so "Rozdział 1"
// would never match)
const NUMBER_ONLY = /^([ivxlcdm]+|\d+)[.)]?$/i;

function looksLikeHeading(p: BookParagraph): boolean {
  if (p.n === 0 || p.n > 12) return false;
  const text = p.t.trim();
  if (HEADING_WORDS.test(text) || NUMBER_ONLY.test(text)) return true;
  const letters = text.replace(/[^\p{L}]/gu, "");
  return p.n <= 8 && letters.length >= 3 && letters === letters.toLocaleUpperCase("pl");
}

function clip(text: string): string {
  const flat = text.replace(/\s+/g, " ").trim();
  return flat.length > MAX_TITLE ? `${flat.slice(0, MAX_TITLE - 1).trimEnd()}…` : flat;
}

function defaultTitle(p: BookParagraph): string {
  return clip(p.t.split(" ").slice(0, SHOWN).join(" "));
}

/** The paragraph's words as the reader numbers them (joined by single spaces). */
function wordsOf(p: BookParagraph): string[] {
  return p.t.split(" ");
}

interface Hit {
  /** Reader word number the match falls in. */
  w: number;
  before: string;
  match: string;
  after: string;
}

/** Every place `q` shows up in the paragraph, as the word it starts in. */
function hitsIn(p: BookParagraph, q: string): Hit[] {
  const lower = p.t.toLocaleLowerCase("pl");
  const hits: Hit[] = [];
  let at = lower.indexOf(q);
  while (at >= 0 && hits.length < 20) {
    const wordOffset = (p.t.slice(0, at).match(/ /g) ?? []).length;
    const words = wordsOf(p);
    hits.push({
      w: p.w + wordOffset,
      before: words.slice(Math.max(0, wordOffset - 6), wordOffset).join(" ") + (wordOffset > 0 ? " " : ""),
      match: p.t.slice(at, at + q.length),
      after: p.t.slice(at + q.length).split(" ").slice(0, 10).join(" "),
    });
    at = lower.indexOf(q, at + q.length);
  }
  return hits;
}

/**
 * Where chapters start in a book on the reader. The reader sends its own
 * paragraphs (with the word each starts at), the user marks the ones that
 * open a chapter, the list goes back as word numbers — nothing depends on
 * the app splitting the text the same way the reader does.
 *
 * Events: `close`, `saved`.
 */
@customElement("chapter-editor")
export class ChapterEditor extends LitElement {
  @property() bookName = "";
  @property() bookTitle = "";

  @state() private paragraphs: BookParagraph[] = [];
  @state() private paragraphCount = 0;
  @state() private wordCount = 0;
  @state() private chapters = new Map<number, string>();
  @state() private custom = false;
  @state() private loading = true;
  @state() private loadingMore = false;
  @state() private filter: Filter = "all";
  @state() private query = "";
  @state() private rows = 120;
  @state() private busy = "";
  @state() private error = "";
  @state() private notice = "";
  private original = "";
  private cancelled = false;
  private unsubLang: (() => void) | null = null;

  connectedCallback(): void {
    super.connectedCallback();
    this.cancelled = false;
    this.unsubLang = onLangChange(() => this.requestUpdate());
    void this.load();
  }

  disconnectedCallback(): void {
    super.disconnectedCallback();
    this.cancelled = true;
    this.unsubLang?.();
  }

  private async load(): Promise<void> {
    this.loading = true;
    this.error = "";
    this.paragraphs = [];
    try {
      const first = await deviceApi.getBookText(this.bookName, 0, FIRST_PAGE, WORDS);
      this.paragraphCount = first.paragraphCount;
      this.wordCount = first.wordCount;
      this.custom = first.custom;
      this.chapters = new Map(first.chapters.map((c) => [c.w, c.t]));
      this.original = this.serialize();
      this.paragraphs = first.paragraphs;
      this.loading = false;
      // The rest in the background, so search and suggestions see the
      // whole book.
      this.loadingMore = true;
      let from = first.paragraphs.length;
      while (!this.cancelled && from < this.paragraphCount) {
        const page = await deviceApi.getBookText(this.bookName, from, PAGE, WORDS);
        if (page.paragraphs.length === 0) break;
        this.paragraphs = [...this.paragraphs, ...page.paragraphs];
        from += page.paragraphs.length;
      }
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
    } finally {
      this.loading = false;
      this.loadingMore = false;
    }
  }

  private serialize(): string {
    return JSON.stringify([...this.chapters.entries()].sort((a, b) => a[0] - b[0]));
  }

  private get dirty(): boolean {
    return this.serialize() !== this.original;
  }

  private get suggestions(): BookParagraph[] {
    return this.paragraphs.filter((p) => looksLikeHeading(p));
  }

  render() {
    const suggestions = this.suggestions;
    const newSuggestions = suggestions.filter((p) => !this.chapters.has(p.w));
    return html`
      <div class="sheet" role="dialog" aria-label=${tr("lib.chapters")}>
        <header>
          <div>
            <small>${tr("lib.chapters")} · ${this.custom ? tr("ch.custom") : tr("ch.detected")}</small>
            <strong>${this.bookTitle || this.bookName}</strong>
          </div>
          <button class="icon" @click=${this.close} aria-label=${tr("common.close")}>✕</button>
        </header>

        ${this.loading
          ? html`<p class="lead">${tr("ch.preparing")}</p>`
          : this.renderEditor(suggestions, newSuggestions)}

        <footer>
          ${this.error ? html`<p class="error">${this.error}</p>` : nothing}
          ${this.notice ? html`<p class="ok">${this.notice}</p>` : nothing}
          <button class="cta ghost" ?disabled=${!!this.busy} @click=${this.resetToDetected}>
            ${tr("ch.reset")}
          </button>
          <button class="cta" ?disabled=${!!this.busy || !this.dirty || this.loading} @click=${this.save}>
            ${this.busy || tr("ch.save", { n: this.chapters.size })}
          </button>
        </footer>
      </div>
    `;
  }

  private renderEditor(suggestions: BookParagraph[], newSuggestions: BookParagraph[]) {
    const list = this.visibleParagraphs(suggestions);
    const loadedPercent = this.paragraphCount ? Math.round((this.paragraphs.length / this.paragraphCount) * 100) : 100;
    return html`
      <p class="lead">${tr("ch.lead")}</p>

      ${this.chapters.size > 0
        ? html`<div class="chips">
            ${[...this.chapters.entries()]
              .sort((a, b) => a[0] - b[0])
              .map(
                ([w, t], i) => html`<button class="chip" @click=${() => this.jumpTo(w)}>
                  <span>${i + 1}</span>${t || tr("ch.untitled")}
                </button>`,
              )}
          </div>`
        : html`<p class="hint">${tr("ch.none")}</p>`}

      ${newSuggestions.length > 0
        ? html`<div class="suggest">
            <span>${tr("ch.suggest", { n: newSuggestions.length })}</span>
            <button class="btn" @click=${() => this.addAll(newSuggestions)}>${tr("ch.addAll")}</button>
          </div>`
        : nothing}

      <div class="toolbar">
        <input
          type="search"
          placeholder=${tr("ch.search")}
          .value=${this.query}
          @input=${(e: Event) => {
            this.query = (e.target as HTMLInputElement).value;
            this.rows = 120;
          }}
        />
        <div class="filters">
          ${this.filterButton("all", tr("ch.filter.all"))}
          ${this.filterButton("chapters", tr("ch.filter.chapters", { n: this.chapters.size }))}
          ${this.filterButton("suggested", tr("ch.filter.headings", { n: suggestions.length }))}
        </div>
      </div>

      ${this.loadingMore
        ? html`<div class="progress"><span style="width:${loadedPercent}%"></span></div>
            <p class="hint">${tr("ch.loadingRest", { n: loadedPercent })}</p>`
        : nothing}

      <ol class="paragraphs">
        ${list.items.map((p) => {
          const q = this.query.trim().toLocaleLowerCase("pl");
          // A match past the paragraph's start: show where it is.
          return q && !p.t.slice(0, 120).toLocaleLowerCase("pl").startsWith(q) && this.filter === "all"
            ? this.renderHits(p, q)
            : this.renderParagraph(p);
        })}
      </ol>
      ${list.more
        ? html`<button class="btn ghost more" @click=${() => (this.rows += ROWS_STEP)}>${tr("ch.more")}</button>`
        : nothing}
      ${list.items.length === 0 && !this.loadingMore ? html`<p class="hint">${tr("ch.noMatch")}</p>` : nothing}
    `;
  }

  private visibleParagraphs(suggestions: BookParagraph[]): { items: BookParagraph[]; more: boolean } {
    let items =
      this.filter === "chapters"
        ? this.paragraphs.filter((p) => [...this.chapters.keys()].some((w) => p.w <= w && w < p.w + Math.max(1, p.n)))
        : this.filter === "suggested"
          ? suggestions
          : this.paragraphs;
    const q = this.query.trim().toLocaleLowerCase("pl");
    if (q) items = items.filter((p) => p.t.toLocaleLowerCase("pl").includes(q));
    return { items: items.slice(0, this.rows), more: items.length > this.rows };
  }

  /** Search results: the words around each match, a chapter can start right there. */
  private renderHits(p: BookParagraph, q: string) {
    const hits = hitsIn(p, q);
    return html`
      <li class="hits">
        <span class="pos">${this.wordCount ? Math.floor((p.w / this.wordCount) * 100) : 0}%</span>
        ${hits.map((h) => {
          const isChapter = this.chapters.has(h.w);
          return html`<button class=${isChapter ? "hit chapter" : "hit"} @click=${() => this.toggleAt(h)}>
            <span class="words">…${h.before}<mark>${h.match}</mark>${h.after}…</span>
            <span class="add">${isChapter ? tr("ch.isChapter") : tr("ch.addHere")}</span>
          </button>`;
        })}
      </li>
    `;
  }

  private filterButton(filter: Filter, label: string) {
    return html`<button
      class=${this.filter === filter ? "filter active" : "filter"}
      @click=${() => {
        this.filter = filter;
        this.rows = 120;
      }}
    >
      ${label}
    </button>`;
  }

  private renderParagraph(p: BookParagraph) {
    const title = this.chapters.get(p.w);
    const isChapter = title !== undefined;
    const position = this.wordCount ? Math.floor((p.w / this.wordCount) * 100) : 0;
    const heading = looksLikeHeading(p);
    return html`
      <li id=${`p${p.w}`} class=${isChapter ? "chapter" : heading ? "heading" : ""}>
        ${isChapter
          ? html`<div class="mark">
              <input
                type="text"
                maxlength=${MAX_TITLE}
                .value=${title}
                aria-label=${tr("ch.chapterTitle")}
                @input=${(e: Event) => this.rename(p.w, (e.target as HTMLInputElement).value)}
              />
              <button class="remove" @click=${() => this.toggle(p)} aria-label=${tr("ch.remove")}>✕</button>
            </div>`
          : nothing}
        <button class="text" @click=${() => (isChapter ? this.jumpTo(p.w) : this.toggle(p))}>
          <span class="pos">${position}%</span>
          <span class="words">${wordsOf(p).slice(0, SHOWN).join(" ")}${p.n > SHOWN ? "…" : ""}</span>
          ${isChapter ? nothing : html`<span class="add">${tr("ch.add")}</span>`}
        </button>
      </li>
    `;
  }

  private toggle(p: BookParagraph): void {
    const next = new Map(this.chapters);
    if (next.has(p.w)) next.delete(p.w);
    else next.set(p.w, defaultTitle(p));
    this.chapters = next;
    this.notice = "";
  }

  private toggleAt(hit: Hit): void {
    const next = new Map(this.chapters);
    if (next.has(hit.w)) next.delete(hit.w);
    else next.set(hit.w, clip(hit.match + hit.after));
    this.chapters = next;
    this.notice = "";
  }

  private rename(w: number, title: string): void {
    const next = new Map(this.chapters);
    next.set(w, title.slice(0, MAX_TITLE));
    this.chapters = next;
  }

  private addAll(paragraphs: BookParagraph[]): void {
    const next = new Map(this.chapters);
    for (const p of paragraphs) if (!next.has(p.w)) next.set(p.w, defaultTitle(p));
    this.chapters = next;
    this.notice = "";
  }

  private async jumpTo(w: number): Promise<void> {
    this.filter = "all";
    this.query = "";
    // A chapter set inside a paragraph scrolls to that paragraph.
    let index = this.paragraphs.findIndex((p) => p.w <= w && w < p.w + Math.max(1, p.n));
    if (index < 0) index = this.paragraphs.findIndex((p) => p.w === w);
    if (index >= 0) w = this.paragraphs[index].w;
    if (index >= this.rows) this.rows = index + 20;
    await this.updateComplete;
    this.renderRoot.querySelector(`#p${w}`)?.scrollIntoView({ block: "center", behavior: "smooth" });
  }

  private save = async () => {
    if (this.chapters.size === 0) {
      this.error = tr("ch.needOne");
      return;
    }
    this.error = "";
    this.busy = tr("common.saving");
    try {
      const list: ChapterMark[] = [...this.chapters.entries()]
        .sort((a, b) => a[0] - b[0])
        .map(([w, t]) => ({ w, t: t.trim() || tr("ch.defaultName") }));
      await deviceApi.setBookChapters(this.bookName, list);
      this.original = this.serialize();
      this.custom = true;
      this.notice = tr("ch.saved");
      this.dispatchEvent(new CustomEvent("saved", { bubbles: true, composed: true }));
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
    } finally {
      this.busy = "";
    }
  };

  private resetToDetected = async () => {
    if (this.custom && !confirm(tr("ch.confirmReset"))) return;
    this.error = "";
    this.busy = tr("ch.restoring");
    try {
      if (this.custom) await deviceApi.resetBookChapters(this.bookName);
      const first = await deviceApi.getBookText(this.bookName, 0, 1, 1);
      this.custom = first.custom;
      this.chapters = new Map(first.chapters.map((c) => [c.w, c.t]));
      this.original = this.serialize();
      this.notice = tr("ch.restored");
      this.dispatchEvent(new CustomEvent("saved", { bubbles: true, composed: true }));
    } catch (err) {
      this.error = err instanceof Error ? err.message : String(err);
    } finally {
      this.busy = "";
    }
  };

  private close = () => {
    if (this.dirty && !confirm(tr("ch.confirmClose"))) return;
    this.dispatchEvent(new CustomEvent("close", { bubbles: true, composed: true }));
  };

  static styles = css`
    :host {
      position: fixed;
      inset: 0;
      z-index: 50;
      display: flex;
      align-items: flex-end;
      justify-content: center;
      background: rgba(20, 18, 15, 0.45);
    }
    .sheet {
      width: min(640px, 100%);
      height: 100%;
      overflow-y: auto;
      background: var(--paper, #f8f4ec);
      padding: 0 16px;
      display: flex;
      flex-direction: column;
      gap: 10px;
      /* Paragraphs keep arriving at the bottom while the rest loads. */
      overflow-anchor: none;
    }
    header {
      position: sticky;
      top: 0;
      z-index: 2;
      padding: 14px 0 6px;
      background: var(--paper, #f8f4ec);
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 10px;
    }
    header div {
      display: flex;
      flex-direction: column;
      min-width: 0;
    }
    header small {
      font: 600 0.7rem var(--mn);
      text-transform: uppercase;
      letter-spacing: 0.04em;
      color: var(--muted);
    }
    header strong {
      font: 600 1.1rem var(--fr);
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }
    .icon {
      width: 36px;
      height: 36px;
      flex: 0 0 auto;
      border: 0;
      border-radius: 50%;
      background: var(--paper-tint);
      color: var(--ink-soft);
      font-size: 1rem;
      cursor: pointer;
    }
    .lead,
    .hint,
    .ok,
    .error {
      margin: 0;
      font: 0.88rem/1.45 var(--ns);
      color: var(--ink-soft);
    }
    .hint {
      color: var(--muted);
      font-size: 0.8rem;
    }
    .ok {
      color: var(--ok);
    }
    footer .ok,
    footer .error {
      flex: 1 1 100%;
    }
    .error {
      color: var(--err);
    }
    .chips {
      display: flex;
      gap: 6px;
      overflow-x: auto;
      padding-bottom: 4px;
    }
    .chip {
      flex: 0 0 auto;
      max-width: 220px;
      display: flex;
      align-items: center;
      gap: 6px;
      padding: 6px 10px;
      border: 1px solid var(--line);
      border-radius: var(--radius-pill, 999px);
      background: var(--paper-tint);
      color: var(--ink);
      font: 0.8rem var(--ns);
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
      cursor: pointer;
    }
    .chip span {
      font: 700 0.7rem var(--mn);
      color: var(--accent);
    }
    .suggest {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 10px;
      padding: 10px 12px;
      border-radius: var(--radius, 13px);
      background: rgba(20, 136, 216, 0.08);
      font: 0.86rem var(--ns);
    }
    .toolbar {
      position: sticky;
      top: 58px;
      z-index: 1;
      display: flex;
      flex-direction: column;
      gap: 8px;
      padding: 8px 0;
      background: var(--paper, #f8f4ec);
    }
    .toolbar input {
      padding: 10px 12px;
      border: 1px solid var(--line);
      border-radius: var(--radius-sm, 9px);
      background: #fff;
      font: 0.92rem var(--ns);
    }
    .filters {
      display: flex;
      gap: 6px;
    }
    .filter {
      flex: 1;
      padding: 7px 6px;
      border: 1px solid var(--line);
      border-radius: var(--radius-sm, 9px);
      background: var(--paper-tint);
      color: var(--ink-soft);
      font: 600 0.72rem var(--mn);
      cursor: pointer;
    }
    .filter.active {
      background: var(--accent);
      border-color: var(--accent);
      color: #fff;
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
      transition: width 0.3s ease;
    }
    .paragraphs {
      list-style: none;
      margin: 0;
      padding: 0;
      display: flex;
      flex-direction: column;
      gap: 4px;
    }
    .paragraphs li {
      border: 1px solid transparent;
      border-radius: var(--radius-sm, 9px);
    }
    .paragraphs li.heading {
      border-color: var(--line);
    }
    .paragraphs li.chapter {
      border-color: var(--accent);
      background: rgba(20, 136, 216, 0.06);
    }
    .mark {
      display: flex;
      gap: 6px;
      padding: 8px 8px 0;
    }
    .mark input {
      flex: 1;
      min-width: 0;
      padding: 8px 10px;
      border: 1px solid var(--accent);
      border-radius: var(--radius-sm, 9px);
      background: #fff;
      font: 600 0.92rem var(--fr);
    }
    .remove {
      width: 36px;
      border: 0;
      border-radius: var(--radius-sm, 9px);
      background: rgba(228, 77, 101, 0.1);
      color: var(--err);
      cursor: pointer;
    }
    .text {
      width: 100%;
      display: grid;
      grid-template-columns: 38px 1fr auto;
      align-items: start;
      gap: 8px;
      padding: 8px;
      border: 0;
      background: transparent;
      text-align: left;
      color: var(--ink);
      cursor: pointer;
    }
    .pos {
      font: 0.68rem var(--mn);
      color: var(--muted);
      padding-top: 3px;
    }
    .words {
      font: 0.88rem/1.4 var(--ns);
      display: -webkit-box;
      -webkit-line-clamp: 2;
      -webkit-box-orient: vertical;
      overflow: hidden;
    }
    li.heading .words {
      font-weight: 600;
    }
    .hits {
      display: grid;
      grid-template-columns: 38px 1fr;
      gap: 4px 8px;
      padding: 8px;
    }
    .hits .pos {
      grid-row: span 20;
    }
    .hit {
      display: flex;
      flex-direction: column;
      align-items: flex-start;
      gap: 2px;
      padding: 6px 8px;
      border: 1px solid var(--line);
      border-radius: var(--radius-sm, 9px);
      background: transparent;
      color: var(--ink);
      text-align: left;
      cursor: pointer;
    }
    .hit.chapter {
      border-color: var(--accent);
    }
    mark {
      background: rgba(20, 136, 216, 0.22);
      color: inherit;
      border-radius: 3px;
      padding: 0 1px;
    }
    .add {
      font: 700 0.68rem var(--mn);
      color: var(--accent);
      white-space: nowrap;
      padding-top: 3px;
    }
    .sheet > * {
      flex-shrink: 0;
    }
    footer {
      position: sticky;
      bottom: 0;
      display: flex;
      flex-wrap: wrap;
      gap: 8px;
      padding: 10px 0 calc(12px + env(safe-area-inset-bottom));
      background: var(--paper, #f8f4ec);
      border-top: 1px solid var(--line);
      margin-top: auto;
    }
    .btn,
    .cta {
      padding: 10px 14px;
      border: 1px solid var(--accent);
      border-radius: var(--radius-sm, 9px);
      background: var(--accent);
      color: #fff;
      font: 700 0.8rem var(--mn);
      cursor: pointer;
    }
    .cta {
      flex: 1;
    }
    .btn.ghost,
    .cta.ghost {
      background: transparent;
      color: var(--accent);
    }
    .more {
      align-self: center;
      margin: 6px 0 10px;
    }
    .cta:disabled {
      opacity: 0.5;
      cursor: default;
    }
  `;
}

declare global {
  interface HTMLElementTagNameMap {
    "chapter-editor": ChapterEditor;
  }
}
