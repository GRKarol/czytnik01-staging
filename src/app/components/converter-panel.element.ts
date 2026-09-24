import { LitElement, css, html } from "lit";
import { customElement, state } from "lit/decorators.js";
import {
  detectFormat,
  parseFile,
  writeRsvp,
  type ParsedBook,
  type SupportedFormat,
} from "../converter";
import "./first-use-hint.element";

type Stage = "idle" | "parsing" | "ready" | "error";

@customElement("converter-panel")
export class ConverterPanel extends LitElement {
  @state() private stage: Stage = "idle";
  @state() private fileName = "";
  @state() private book: ParsedBook | null = null;
  @state() private rsvp = "";
  @state() private error = "";
  @state() private dragOver = false;
  @state() private bookTitle = "";
  @state() private bookAuthor = "";

  render() {
    return html`
      <first-use-hint screen-key="converter"></first-use-hint>
      <div
        class=${`drop ${this.dragOver ? "over" : ""}`}
        @dragover=${this.onDragOver}
        @dragleave=${this.onDragLeave}
        @drop=${this.onDrop}
      >
        <input
          id="picker"
          type="file"
          accept=".txt,.md,.markdown,.html,.htm,.xhtml,.epub,.pdf,.mobi,.azw,.azw3,.docx"
          @change=${this.onPick}
          hidden
        />
        <label for="picker" class="picker">
          <strong>Wybierz plik</strong>
          <span>lub upuść go tutaj</span>
          <span class="formats">EPUB · PDF · MOBI · AZW3 · DOCX · TXT · MD · HTML</span>
        </label>
      </div>

      ${this.stage === "parsing"
        ? html`<p class="status">Parsuję <strong>${this.fileName}</strong>…</p>`
        : ""}
      ${this.stage === "error" ? html`<p class="error">${this.error}</p>` : ""}
      ${this.stage === "ready" && this.book ? this.renderReady() : ""}
    `;
  }

  private renderReady() {
    const ev = this.book!.events;
    const chapters = ev.filter((e) => e.kind === "chapter").length;
    const paragraphs = ev.filter((e) => e.kind === "paragraph").length;
    const wordCount = ev.reduce(
      (n, e) => (e.kind === "paragraph" ? n + e.text.split(/\s+/).length : n),
      0,
    );

    return html`
      <section class="result">
        <h4>Gotowe</h4>
        <div class="meta">
          <label>
            <span>Tytuł</span>
            <input
              type="text"
              .value=${this.bookTitle}
              @input=${(e: InputEvent) => {
                this.bookTitle = (e.target as HTMLInputElement).value;
              }}
            />
          </label>
          <label>
            <span>Autor</span>
            <input
              type="text"
              .value=${this.bookAuthor}
              @input=${(e: InputEvent) => {
                this.bookAuthor = (e.target as HTMLInputElement).value;
              }}
            />
          </label>
        </div>
        <ul class="stats">
          <li><strong>${formatNumber(wordCount)}</strong>słów</li>
          <li><strong>${chapters}</strong>rozdziałów</li>
          <li><strong>${formatNumber(paragraphs)}</strong>paragrafów</li>
          <li><strong>${formatBytes(new Blob([this.rsvp]).size)}</strong>.rsvp</li>
        </ul>
        <div class="row">
          <button class="cta" @click=${this.download}>Pobierz .rsvp</button>
          <button class="cta ghost" disabled title="Wymaga połączenia z urządzeniem">
            Wyślij na urządzenie (wkrótce)
          </button>
        </div>
        <details class="preview">
          <summary>Podgląd pierwszych linii</summary>
          <pre>${this.rsvp.split("\n").slice(0, 20).join("\n")}</pre>
        </details>
      </section>
    `;
  }

  // ─── events ───────────────────────────────────────────────────────────────

  private onPick = (e: Event) => {
    const input = e.target as HTMLInputElement;
    const file = input.files?.[0];
    if (file) void this.handleFile(file);
    input.value = "";
  };

  private onDragOver = (e: DragEvent) => {
    e.preventDefault();
    this.dragOver = true;
  };

  private onDragLeave = () => {
    this.dragOver = false;
  };

  private onDrop = (e: DragEvent) => {
    e.preventDefault();
    this.dragOver = false;
    const file = e.dataTransfer?.files?.[0];
    if (file) void this.handleFile(file);
  };

  private async handleFile(file: File) {
    this.error = "";
    this.fileName = file.name;

    const detection = detectFormat(file);
    if (detection.kind === "unknown") {
      this.stage = "error";
      this.error = `Nieobsługiwany format: ${file.name}.`;
      return;
    }

    this.stage = "parsing";
    try {
      const book = await parseFile(file, detection.format as SupportedFormat);
      this.book = book;
      this.bookTitle = book.metadata.title;
      this.bookAuthor = book.metadata.author;
      this.rsvp = writeRsvp(book);
      this.stage = "ready";
    } catch (err) {
      this.stage = "error";
      this.error = err instanceof Error ? err.message : String(err);
    }
  }

  private download = () => {
    if (!this.book) return;
    // Re-serialize z aktualnymi metadanymi (mogły być edytowane).
    const updated = writeRsvp({
      ...this.book,
      metadata: { ...this.book.metadata, title: this.bookTitle, author: this.bookAuthor },
    });
    const blob = new Blob([updated], { type: "text/plain" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    // \p{L}/\p{N} (Unicode letters/numbers) zamiast \w, żeby polskie znaki
    // (ą,ć,ę,ł,ń,ó,ś,ź,ż) w nazwie pliku nie zamieniały się na "_".
    a.download = `${(this.bookTitle || stripExt(this.fileName) || "book").replace(/[^\p{L}\p{N}\- ]+/gu, "_")}.rsvp`;
    a.click();
    URL.revokeObjectURL(url);
  };

  static styles = css`
    :host {
      display: block;
    }
    .drop {
      border: 2px dashed var(--line);
      padding: 28px 18px;
      text-align: center;
      transition:
        border-color 0.15s ease,
        background 0.15s ease;
      background: var(--paper-tint);
    }
    .drop:hover {
      border-color: var(--accent);
    }
    .drop.over {
      border-color: var(--accent);
      background: rgba(46, 142, 255, 0.08);
    }
    .picker {
      display: flex;
      flex-direction: column;
      gap: 4px;
      cursor: pointer;
    }
    .picker strong {
      font-family: var(--fr);
      color: var(--accent);
      font-size: 1.2rem;
    }
    .picker span {
      color: var(--muted);
      font: 0.92rem var(--ns);
    }
    .formats {
      margin-top: 6px;
      font-size: 0.78rem;
    }
    .status {
      margin: 0;
      color: var(--muted);
      font: 0.92rem var(--ns);
    }
    .error {
      margin: 0;
      color: var(--err);
      font: 0.92rem var(--ns);
    }
    .result {
      display: flex;
      flex-direction: column;
      gap: 12px;
    }
    .result h4 {
      margin: 4px 0 0;
      font-family: var(--fr);
      font-size: 1.1rem;
    }
    .meta {
      display: grid;
      gap: 8px;
    }
    .meta label {
      display: flex;
      flex-direction: column;
      gap: 4px;
      font: 600 0.72rem var(--mn);
      letter-spacing: 0.02em;
      color: var(--muted);
    }
    .meta input {
      padding: 10px 12px;
      border: 1px solid var(--line);
      background: #fff;
      font: 0.95rem var(--ns);
      color: var(--ink);
    }
    .meta input:focus {
      outline: none;
      border-color: var(--accent);
    }
    .stats {
      display: grid;
      grid-template-columns: repeat(2, 1fr);
      gap: 8px;
      margin: 4px 0 0;
      padding: 0;
      list-style: none;
    }
    .stats li {
      display: flex;
      gap: 4px;
      align-items: baseline;
      padding: 10px 12px;
      border: 1px solid var(--line);
      background: var(--paper-tint);
      font: 0.8rem var(--mn);
      color: var(--muted);
    }
    .stats strong {
      font-family: var(--fr);
      font-size: 1.15rem;
      color: var(--ink);
    }
    .row {
      display: flex;
      gap: 8px;
      flex-wrap: wrap;
    }
    .row .cta {
      flex: 1 1 auto;
    }
    .cta {
      padding: 12px 18px;
      border: 1px solid var(--accent);
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
    .preview {
      border: 1px solid var(--line);
      padding: 10px 12px;
      background: var(--paper-tint);
      font: 0.82rem var(--ns);
    }
    .preview summary {
      cursor: pointer;
      color: var(--muted);
    }
    .preview pre {
      margin: 8px 0 0;
      font: 0.78rem/1.5 var(--mn);
      color: var(--ink-soft);
      white-space: pre-wrap;
      max-height: 220px;
      overflow-y: auto;
    }
  `;
}

declare global {
  interface HTMLElementTagNameMap {
    "converter-panel": ConverterPanel;
  }
}

function stripExt(name: string): string {
  return name.replace(/\.[^.]+$/, "");
}

function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} kB`;
  return `${(n / 1024 / 1024).toFixed(2)} MB`;
}

function formatNumber(n: number): string {
  return n.toLocaleString("pl-PL");
}
