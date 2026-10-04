import { LitElement, css, html, nothing, svg } from "lit";
import { customElement, property, query, state } from "lit/decorators.js";
import { deviceApi, PICTURE_SIZE, type PictureKind } from "../device/api";
import { onLangChange, tr } from "../i18n/index";
import {
  INITIAL_CROP,
  clampCrop,
  decodePicture,
  encodePicture,
  loadImage,
  placeImage,
  readerCoverColor,
  readerInitials,
  releaseImage,
  renderCrop,
  type CropState,
} from "../books/pictures";

type Step = PictureKind | "preview";

/** What the user did with one picture in this session. */
interface Slot {
  /** Picture chosen here, to be cropped. */
  image: HTMLImageElement | null;
  crop: CropState;
  /** What the reader has now (downloaded), shown until something else is chosen. */
  current: HTMLCanvasElement | null;
  /** Back to the reader's default look. */
  reset: boolean;
}

const emptySlot = (): Slot => ({ image: null, crop: { ...INITIAL_CROP }, current: null, reset: false });

// Frame sizes on the phone screen, same aspect as on the reader.
const COVER_FRAME_W = 220;
const SPINE_FRAME_H = 280;

/**
 * Cover and spine for one book: pick a photo, crop it to the reader's frame
 * (with the binding line / spine rules drawn over it exactly where the
 * reader draws them), see it on a mock of the reader's screens, send it.
 *
 * Events: `close` (nothing changed or done), `saved` (reader updated).
 */
@customElement("cover-editor")
export class CoverEditor extends LitElement {
  /** Library name on the reader, e.g. "books/Quo Vadis.epub". */
  @property() bookName = "";
  @property() bookTitle = "";
  @property() bookAuthor = "";
  @property({ type: Boolean }) hasCover = false;
  @property({ type: Boolean }) hasSpine = false;
  /** Cover packed in the EPUB, offered as a starting point. */
  @property({ attribute: false }) epubCover: Blob | null = null;

  @state() private step: Step = "cover";
  @state() private slots: Record<PictureKind, Slot> = { cover: emptySlot(), spine: emptySlot() };
  @state() private busy = "";
  @state() private error = "";

  @query("#preview") private previewCanvas?: HTMLCanvasElement;

  private pointers = new Map<number, { x: number; y: number }>();
  private pinchStart: { distance: number; zoom: number } | null = null;

  private unsubLang: (() => void) | null = null;

  connectedCallback(): void {
    super.connectedCallback();
    this.unsubLang = onLangChange(() => this.requestUpdate());
    void this.loadCurrent();
    if (this.epubCover && !this.hasCover) void this.useBlob("cover", this.epubCover);
  }

  disconnectedCallback(): void {
    super.disconnectedCallback();
    this.unsubLang?.();
    const images = new Set([this.slots.cover.image, this.slots.spine.image]);
    for (const image of images) releaseImage(image);
  }

  private async loadCurrent(): Promise<void> {
    for (const kind of ["cover", "spine"] as PictureKind[]) {
      if (!(kind === "cover" ? this.hasCover : this.hasSpine)) continue;
      try {
        const blob = await deviceApi.getBookPicture(this.bookName, kind);
        const canvas = blob ? await decodePicture(blob) : null;
        this.patchSlot(kind, { current: canvas });
      } catch {
        /* the default look stays */
      }
    }
  }

  private patchSlot(kind: PictureKind, patch: Partial<Slot>): void {
    this.slots = { ...this.slots, [kind]: { ...this.slots[kind], ...patch } };
  }

  protected updated(): void {
    if (this.step === "preview") this.drawPreview();
  }

  render() {
    return html`
      <div class="sheet" role="dialog" aria-label=${tr("cv.title")}>
        <header>
          <div>
            <small>${tr("cv.title")}</small>
            <strong>${this.bookTitle || this.bookName}</strong>
          </div>
          <button class="icon" @click=${this.close} aria-label=${tr("common.close")}>✕</button>
        </header>

        <nav class="steps">
          ${this.stepButton("cover", "1", tr("lib.cover"))}
          ${this.stepButton("spine", "2", tr("cv.spine"))}
          ${this.stepButton("preview", "3", tr("cv.preview"))}
        </nav>

        <div class="body">
          ${this.step === "preview" ? this.renderPreview() : this.renderSlot(this.step)}
        </div>

        ${this.error ? html`<p class="error">${this.error}</p>` : nothing}

        <footer>
          ${this.step === "cover"
            ? html`<button class="cta" @click=${() => (this.step = "spine")}>${tr("cv.nextSpine")}</button>`
            : this.step === "spine"
              ? html`
                  <button class="cta ghost" @click=${() => (this.step = "cover")}>${tr("common.back")}</button>
                  <button class="cta" @click=${() => (this.step = "preview")}>
                    ${this.slots.spine.image || this.slots.spine.reset ? tr("cv.nextPreview") : tr("common.skip")}
                  </button>
                `
              : html`
                  <button class="cta ghost" @click=${() => (this.step = "spine")}>${tr("common.back")}</button>
                  <button class="cta" ?disabled=${!!this.busy || !this.hasChanges()} @click=${this.save}>
                    ${this.busy || tr("cv.send")}
                  </button>
                `}
        </footer>
      </div>
    `;
  }

  private stepButton(step: Step, number: string, label: string) {
    return html`<button class=${this.step === step ? "active" : ""} @click=${() => (this.step = step)}>
      <span>${number}</span>${label}
    </button>`;
  }

  // ─── Crop step ────────────────────────────────────────────────────────────

  private renderSlot(kind: PictureKind) {
    const slot = this.slots[kind];
    const { width, height } = PICTURE_SIZE[kind];
    const frameW = kind === "cover" ? COVER_FRAME_W : Math.round((SPINE_FRAME_H * width) / height);
    const frameH = kind === "cover" ? Math.round((COVER_FRAME_W * height) / width) : SPINE_FRAME_H;
    const hasOwn = !!slot.image || (!!slot.current && !slot.reset);
    return html`
      <p class="lead">
        ${kind === "cover"
          ? tr("cv.coverLead")
          : tr("cv.spineLead")}
      </p>

      <div class="stage">
        <div
          class=${`frame ${kind} ${slot.image ? "editing" : ""}`}
          style="width:${frameW}px;height:${frameH}px"
          @pointerdown=${this.onPointerDown}
          @pointermove=${(e: PointerEvent) => this.onPointerMove(e, kind, frameW, frameH)}
          @pointerup=${this.onPointerUp}
          @pointercancel=${this.onPointerUp}
          @wheel=${(e: WheelEvent) => this.onWheel(e, kind, frameW, frameH)}
        >
          ${slot.image
            ? this.renderPlacedImage(slot, frameW, frameH)
            : slot.current && !slot.reset
              ? html`<img class="fill" src=${slot.current.toDataURL()} alt="" />`
              : this.renderDefault(kind)}
          ${this.renderTemplate(kind, frameW, frameH)}
        </div>
      </div>

      ${slot.image
        ? html`
            <label class="zoom">
              <span>${tr("cv.zoom")}</span>
              <input
                type="range"
                min="1"
                max="4"
                step="0.01"
                .value=${String(slot.crop.zoom)}
                @input=${(e: Event) =>
                  this.setCrop(kind, frameW, frameH, {
                    ...slot.crop,
                    zoom: Number((e.target as HTMLInputElement).value),
                  })}
              />
            </label>
            <p class="hint">
              ${tr("cv.hint")} ${kind === "cover" ? tr("cv.hintCover") : tr("cv.hintSpine")}
            </p>
          `
        : nothing}

      <div class="actions">
        <label class="btn">
          <input type="file" accept="image/*" hidden @change=${(e: Event) => this.onPick(e, kind)} />
          ${slot.image ? tr("cv.otherPhoto") : tr("cv.pickPhoto")}
        </label>
        ${kind === "cover" && this.epubCover
          ? html`<button class="btn ghost" @click=${() => this.useBlob("cover", this.epubCover!)}>
              ${tr("lib.coverFromEpub")}
            </button>`
          : nothing}
        ${kind === "spine" && this.slots.cover.image
          ? html`<button class="btn ghost" @click=${this.spineFromCover}>${tr("cv.useCover")}</button>`
          : nothing}
        ${hasOwn
          ? html`<button class="btn ghost danger" @click=${() => this.resetSlot(kind)}>
              ${tr("cv.reset")}
            </button>`
          : nothing}
      </div>
    `;
  }

  private renderPlacedImage(slot: Slot, frameW: number, frameH: number) {
    const image = slot.image!;
    const place = placeImage(image.naturalWidth, image.naturalHeight, frameW, frameH, slot.crop);
    return html`<img
      class="placed"
      src=${image.src}
      alt=""
      draggable="false"
      style="left:${place.x}px;top:${place.y}px;width:${place.w}px;height:${place.h}px"
    />`;
  }

  private renderDefault(kind: PictureKind) {
    const color = readerCoverColor(this.bookName);
    if (kind === "cover") {
      return html`<div class="default" style="background:${color}">
        <span>${readerInitials(this.bookTitle || this.bookName)}</span>
      </div>`;
    }
    const letters = (this.bookTitle || this.bookName).replace(/^the\s+/i, "").toUpperCase().replace(/[^A-Z0-9ĄĆĘŁŃÓŚŹŻ]/g, "").slice(0, 6);
    return html`<div class="default spine" style="background:${color}">
      ${letters.split("").map((c) => html`<span>${c}</span>`)}
    </div>`;
  }

  /** The reader's frame over the picture: binding line / spine rules and ribbon. */
  private renderTemplate(kind: PictureKind, frameW: number, frameH: number) {
    const { width, height } = PICTURE_SIZE[kind];
    const sx = frameW / width;
    const sy = frameH / height;
    if (kind === "cover") {
      return svg`<svg class="template" viewBox="0 0 ${frameW} ${frameH}" width=${frameW} height=${frameH}>
        <rect x=${12 * sx} y="0" width=${3 * sx} height=${frameH} fill="rgba(8,8,10,0.62)" />
      </svg>`;
    }
    return svg`<svg class="template" viewBox="0 0 ${frameW} ${frameH}" width=${frameW} height=${frameH}>
      <rect x=${3 * sx} y=${5 * sy} width=${(width - 6) * sx} height=${Math.max(1.5, sy)} fill="rgba(8,8,10,0.55)" />
      <rect x=${3 * sx} y=${(height - 6) * sy} width=${(width - 6) * sx} height=${Math.max(1.5, sy)} fill="rgba(8,8,10,0.55)" />
      <path d="M ${(width - 9) * sx} 0 h ${5 * sx} v ${height * 0.4 * sy} l ${-2.5 * sx} ${-3 * sy} l ${-2.5 * sx} ${3 * sy} z"
        fill="rgba(218,88,80,0.85)" />
    </svg>`;
  }

  private onPick = async (e: Event, kind: PictureKind) => {
    const input = e.target as HTMLInputElement;
    const file = input.files?.[0];
    input.value = "";
    if (file) await this.useBlob(kind, file);
  };

  private async useBlob(kind: PictureKind, blob: Blob): Promise<void> {
    this.error = "";
    try {
      const image = await loadImage(blob);
      this.patchSlot(kind, { image, crop: { ...INITIAL_CROP }, reset: false });
    } catch {
      this.error = tr("cv.badImage");
    }
  }

  private spineFromCover = () => {
    const image = this.slots.cover.image;
    if (image) this.patchSlot("spine", { image, crop: { ...INITIAL_CROP }, reset: false });
  };

  private resetSlot(kind: PictureKind): void {
    this.patchSlot(kind, { image: null, reset: true, crop: { ...INITIAL_CROP } });
  }

  private setCrop(kind: PictureKind, frameW: number, frameH: number, crop: CropState): void {
    const image = this.slots[kind].image;
    if (!image) return;
    this.patchSlot(kind, { crop: clampCrop(image.naturalWidth, image.naturalHeight, frameW, frameH, crop) });
  }

  private onPointerDown = (e: PointerEvent) => {
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
    this.pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
    this.pinchStart = null;
  };

  private onPointerMove(e: PointerEvent, kind: PictureKind, frameW: number, frameH: number): void {
    const previous = this.pointers.get(e.pointerId);
    if (!previous || !this.slots[kind].image) return;
    const crop = this.slots[kind].crop;
    this.pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
    if (this.pointers.size >= 2) {
      const [a, b] = Array.from(this.pointers.values());
      const distance = Math.hypot(a.x - b.x, a.y - b.y);
      if (!this.pinchStart) this.pinchStart = { distance, zoom: crop.zoom };
      this.setCrop(kind, frameW, frameH, {
        ...crop,
        zoom: (this.pinchStart.zoom * distance) / Math.max(1, this.pinchStart.distance),
      });
      return;
    }
    this.setCrop(kind, frameW, frameH, {
      ...crop,
      panX: crop.panX + (e.clientX - previous.x) / frameW,
      panY: crop.panY + (e.clientY - previous.y) / frameH,
    });
  }

  private onPointerUp = (e: PointerEvent) => {
    this.pointers.delete(e.pointerId);
    if (this.pointers.size < 2) this.pinchStart = null;
  };

  private onWheel(e: WheelEvent, kind: PictureKind, frameW: number, frameH: number): void {
    const crop = this.slots[kind].crop;
    if (!this.slots[kind].image) return;
    e.preventDefault();
    this.setCrop(kind, frameW, frameH, { ...crop, zoom: crop.zoom * (e.deltaY < 0 ? 1.08 : 1 / 1.08) });
  }

  // ─── Preview ──────────────────────────────────────────────────────────────

  private renderPreview() {
    return html`
      <p class="lead">${tr("cv.previewLead")}</p>
      <canvas id="preview" width="1280" height="672"></canvas>
      ${!this.hasChanges() ? html`<p class="hint">${tr("cv.noChanges")}</p>` : nothing}
    `;
  }

  /** The picture the reader will show, at its own pixel size; null = default. */
  private finalPicture(kind: PictureKind): HTMLCanvasElement | null {
    const slot = this.slots[kind];
    if (slot.image) return renderCrop(slot.image, slot.image.naturalWidth, slot.image.naturalHeight, kind, slot.crop);
    if (slot.reset) return null;
    return slot.current;
  }

  // A 640x172 mock of three reader screens at 2x, drawn the way
  // NanoScreens.cpp draws them (same sizes, frame and ribbon).
  private drawPreview(): void {
    const canvas = this.previewCanvas;
    if (!canvas) return;
    const ctx = canvas.getContext("2d")!;
    const cover = this.finalPicture("cover");
    const spine = this.finalPicture("spine");
    const color = readerCoverColor(this.bookName);
    const initials = readerInitials(this.bookTitle || this.bookName);
    ctx.save();
    ctx.scale(2, 2);
    ctx.fillStyle = "#000";
    ctx.fillRect(0, 0, 640, 336);
    ctx.imageSmoothingEnabled = true;

    const drawCover = (x: number, y: number, w: number, h: number, r: number, bindX: number, bindW: number, font: number) => {
      ctx.save();
      roundRect(ctx, x, y, w, h, r);
      ctx.clip();
      if (cover) {
        drawCropped(ctx, cover, x, y, w, h);
      } else {
        ctx.fillStyle = color;
        ctx.fillRect(x, y, w, h);
        ctx.fillStyle = "#fff";
        ctx.font = `600 ${font}px Inter, system-ui, sans-serif`;
        ctx.textAlign = "center";
        ctx.textBaseline = "middle";
        ctx.fillText(initials, x + bindX + bindW + (w - bindX - bindW) / 2, y + h / 2);
      }
      ctx.fillStyle = "rgba(8,8,10,0.62)";
      ctx.fillRect(x + bindX, y, bindW, h);
      ctx.restore();
    };

    // 1. Czytaj card (paintReadHome): 46x58 cover in a 78 px card.
    ctx.fillStyle = "#1b1d24";
    roundRect(ctx, 16, 8, 608, 78, 10);
    ctx.fill();
    drawCover(26, 18, 46, 58, 6, 6, 2, 16);
    ctx.fillStyle = "#f1f1f4";
    ctx.textAlign = "left";
    ctx.textBaseline = "alphabetic";
    ctx.font = "600 18px Inter, system-ui, sans-serif";
    ctx.fillText(ellipsize(ctx, this.bookTitle || this.bookName, 440), 84, 34);
    ctx.fillStyle = "#8a8d98";
    ctx.font = "13px Inter, system-ui, sans-serif";
    ctx.fillText(ellipsize(ctx, this.bookAuthor || tr("cv.unknownAuthor"), 440), 84, 54);
    ctx.fillStyle = "#3a3d48";
    roundRect(ctx, 84, 68, 440, 5, 2.5);
    ctx.fill();
    ctx.fillStyle = "#3d5afe";
    roundRect(ctx, 84, 68, 150, 5, 2.5);
    ctx.fill();

    // 2. Shelf (paintShelf): our spine in the middle of default ones.
    const shelfTop = 104;
    const base = shelfTop + 76;
    const colors = ["#9c3c18", "#1f5ca8", "#0b6a52", "#7b72c0", "#448080", "#b09868", "#984848"];
    let x = 150;
    for (let i = 0; i < 7; i++) {
      const w = 29 + 2 * (i % 4);
      const h = 50 + ((i * 7 + 5) % 19);
      const ours = i === 3;
      const y = base - h - (ours ? 6 : 0);
      ctx.save();
      roundRect(ctx, x, y, w, h, 3);
      ctx.clip();
      if (ours && spine) {
        drawCropped(ctx, spine, x, y, w, h);
      } else {
        ctx.fillStyle = ours ? color : colors[i];
        ctx.fillRect(x, y, w, h);
        if (ours) {
          ctx.fillStyle = "#fff";
          ctx.font = "600 10px Inter, system-ui, sans-serif";
          ctx.textAlign = "center";
          const letters = (this.bookTitle || this.bookName).toUpperCase().replace(/[^A-Z0-9]/g, "").slice(0, 5);
          letters.split("").forEach((c, k) => ctx.fillText(c, x + w / 2 - 3, y + 18 + k * 11));
        }
      }
      ctx.fillStyle = "rgba(0,0,0,0.35)";
      ctx.fillRect(x + 3, y + 5, w - 6, 1);
      ctx.fillRect(x + 3, y + h - 6, w - 6, 1);
      ctx.restore();
      if (ours) {
        ctx.fillStyle = "#3d5afe";
        roundRect(ctx, x, y - 5, w, 3, 1);
        ctx.fill();
        // progress ribbon
        const rx = x + w - 9;
        const rh = Math.max(8, (h - 8) * 0.35);
        ctx.fillStyle = "#da5850";
        ctx.beginPath();
        ctx.moveTo(rx, y);
        ctx.lineTo(rx + 5, y);
        ctx.lineTo(rx + 5, y + rh);
        ctx.lineTo(rx + 2.5, y + rh - 3);
        ctx.lineTo(rx, y + rh);
        ctx.closePath();
        ctx.fill();
      }
      x += w + 5;
    }
    ctx.fillStyle = "#3a3d48";
    ctx.fillRect(16, base, 608, 2);

    // 3. Book details (paintBookDetails): 92x116 cover.
    const top = 204;
    drawCover(16, top, 92, 116, 8, 12, 3, 30);
    ctx.fillStyle = "#f1f1f4";
    ctx.textAlign = "left";
    ctx.font = "600 18px Inter, system-ui, sans-serif";
    ctx.fillText(ellipsize(ctx, this.bookTitle || this.bookName, 480), 124, top + 20);
    ctx.fillStyle = "#8a8d98";
    ctx.font = "13px Inter, system-ui, sans-serif";
    ctx.fillText(ellipsize(ctx, this.bookAuthor || tr("cv.unknownAuthor"), 480), 124, top + 42);
    ctx.fillStyle = "#3d5afe";
    roundRect(ctx, 124, top + 62, 240, 36, 8);
    ctx.fill();
    ctx.fillStyle = "#fff";
    ctx.fillText(tr("cv.continue"), 150, top + 85);
    ctx.fillStyle = "#1b1d24";
    roundRect(ctx, 372, top + 62, 240, 36, 8);
    ctx.fill();
    ctx.fillStyle = "#d8d9de";
    ctx.fillText(tr("lib.chapters"), 398, top + 85);
    ctx.restore();
  }

  private hasChanges(): boolean {
    return (["cover", "spine"] as PictureKind[]).some((k) => {
      const slot = this.slots[k];
      const existed = k === "cover" ? this.hasCover : this.hasSpine;
      return !!slot.image || (slot.reset && existed);
    });
  }

  private save = async () => {
    this.error = "";
    try {
      for (const kind of ["cover", "spine"] as PictureKind[]) {
        const slot = this.slots[kind];
        if (slot.image) {
          this.busy = tr(kind === "cover" ? "cv.sendingCover" : "cv.sendingSpine");
          const picture = renderCrop(slot.image, slot.image.naturalWidth, slot.image.naturalHeight, kind, slot.crop);
          await deviceApi.uploadBookPicture(this.bookName, kind, encodePicture(picture));
        } else if (slot.reset && (kind === "cover" ? this.hasCover : this.hasSpine)) {
          this.busy = tr(kind === "cover" ? "cv.removingCover" : "cv.removingSpine");
          await deviceApi.deleteBookPicture(this.bookName, kind);
        }
      }
      this.busy = "";
      this.dispatchEvent(new CustomEvent("saved", { bubbles: true, composed: true }));
    } catch (err) {
      this.busy = "";
      this.error = err instanceof Error ? err.message : String(err);
    }
  };

  private close = () => {
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
      width: min(560px, 100%);
      max-height: 100%;
      overflow-y: auto;
      background: var(--paper, #f8f4ec);
      border-radius: 18px 18px 0 0;
      padding: 14px 16px calc(16px + env(safe-area-inset-bottom));
      display: flex;
      flex-direction: column;
      gap: 12px;
      box-shadow: 0 -12px 40px -20px rgba(0, 0, 0, 0.5);
    }
    header {
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
      border: 0;
      border-radius: 50%;
      background: var(--paper-tint);
      color: var(--ink-soft);
      font-size: 1rem;
      cursor: pointer;
    }
    .steps {
      display: flex;
      gap: 6px;
    }
    .steps button {
      flex: 1;
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 6px;
      padding: 8px 6px;
      border: 1px solid var(--line);
      border-radius: var(--radius-sm, 9px);
      background: var(--paper-tint);
      color: var(--ink-soft);
      font: 600 0.78rem var(--mn);
      cursor: pointer;
    }
    .steps button span {
      width: 18px;
      height: 18px;
      border-radius: 50%;
      background: var(--line);
      display: inline-flex;
      align-items: center;
      justify-content: center;
      font-size: 0.68rem;
    }
    .steps button.active {
      border-color: var(--accent);
      color: var(--accent);
    }
    .steps button.active span {
      background: var(--accent);
      color: #fff;
    }
    .lead,
    .hint {
      margin: 0;
      color: var(--ink-soft);
      font: 0.9rem/1.45 var(--ns);
    }
    .hint {
      color: var(--muted);
      font-size: 0.82rem;
    }
    .stage {
      display: flex;
      justify-content: center;
      padding: 14px 0;
      background: #111317;
      border-radius: var(--radius, 13px);
    }
    .frame {
      position: relative;
      overflow: hidden;
      touch-action: none;
      user-select: none;
      background: #26282f;
    }
    .frame.cover {
      border-radius: ${(8 / 92) * 100}% / ${(8 / 116) * 100}%;
    }
    .frame.spine {
      border-radius: ${(3 / 36) * 100}% / ${(3 / 72) * 100}%;
    }
    .frame.editing {
      cursor: grab;
      outline: 2px dashed rgba(255, 255, 255, 0.35);
      outline-offset: 4px;
    }
    .placed {
      position: absolute;
      max-width: none;
      pointer-events: none;
    }
    .fill {
      width: 100%;
      height: 100%;
      object-fit: cover;
      display: block;
    }
    .default {
      width: 100%;
      height: 100%;
      display: flex;
      align-items: center;
      justify-content: center;
      color: #fff;
      font: 600 3rem Inter, system-ui, sans-serif;
      padding-left: ${(15 / 92) * 100}%;
      box-sizing: border-box;
    }
    .default.spine {
      flex-direction: column;
      justify-content: flex-start;
      padding: 18% 0 0;
      font-size: 1.2rem;
      line-height: 1.25;
    }
    .template {
      position: absolute;
      inset: 0;
      pointer-events: none;
    }
    .zoom {
      display: flex;
      align-items: center;
      gap: 10px;
      font: 600 0.75rem var(--mn);
      color: var(--ink-soft);
    }
    .zoom input {
      flex: 1;
      accent-color: var(--accent);
    }
    .actions,
    footer {
      display: flex;
      flex-wrap: wrap;
      gap: 8px;
    }
    footer {
      padding-top: 4px;
    }
    .btn,
    .cta {
      flex: 1 1 auto;
      padding: 11px 14px;
      text-align: center;
      border: 1px solid var(--accent);
      border-radius: var(--radius-sm, 9px);
      background: var(--accent);
      color: #fff;
      font: 700 0.82rem var(--mn);
      cursor: pointer;
    }
    .btn.ghost,
    .cta.ghost {
      background: transparent;
      color: var(--accent);
    }
    .btn.danger {
      border-color: var(--err);
      color: var(--err);
    }
    .cta:disabled {
      opacity: 0.5;
      cursor: default;
    }
    #preview {
      width: 100%;
      height: auto;
      border-radius: var(--radius, 13px);
      background: #000;
    }
    .error {
      margin: 0;
      color: var(--err);
      font: 0.9rem var(--ns);
    }
  `;
}

function roundRect(ctx: CanvasRenderingContext2D, x: number, y: number, w: number, h: number, r: number): void {
  ctx.beginPath();
  ctx.roundRect(x, y, w, h, r);
}

// Centre crop to the rect's aspect, like DisplayManager::nanoImage.
function drawCropped(ctx: CanvasRenderingContext2D, source: HTMLCanvasElement, x: number, y: number, w: number, h: number): void {
  let sw = source.width;
  let sh = source.height;
  if (sw * h > sh * w) sw = (sh * w) / h;
  else sh = (sw * h) / w;
  ctx.drawImage(source, (source.width - sw) / 2, (source.height - sh) / 2, sw, sh, x, y, w, h);
}

function ellipsize(ctx: CanvasRenderingContext2D, text: string, maxWidth: number): string {
  if (ctx.measureText(text).width <= maxWidth) return text;
  let cut = text;
  while (cut.length > 1 && ctx.measureText(`${cut}…`).width > maxWidth) cut = cut.slice(0, -1);
  return `${cut.trimEnd()}…`;
}

declare global {
  interface HTMLElementTagNameMap {
    "cover-editor": CoverEditor;
  }
}
