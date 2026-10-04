/**
 * PDF → .rsvp przez pdfjs. Każda strona = paragraf; pdfjs zwraca text
 * items per page, sklejamy je z heurystyką: nowy item w nowej linii →
 * spacja, w nowej kolumnie → nowy paragraf.
 *
 * Pdfjs worker pochodzi z paczki (`pdfjs-dist/build/pdf.worker.min.mjs`).
 * Vite z `?url` zwraca URL do zbundlowanej kopii.
 */

import * as pdfjs from "pdfjs-dist";
import workerSrc from "pdfjs-dist/build/pdf.worker.min.mjs?url";
import type { BookEvent, ParsedBook } from "./rsvp";
import { looksLikeChapterHeading } from "./text-formats";
import { tr } from "../i18n/index";

pdfjs.GlobalWorkerOptions.workerSrc = workerSrc as string;

/**
 * PDF nie ma znaczników `<hN>` — rozdziały trzeba znaleźć inaczej.
 * Najpewniejsze źródło to prawdziwy spis treści (outline/zakładki), jeśli
 * autor go osadził w pliku: mapujemy go na numer strony, na której zaczyna
 * się każdy wpis. Gdy PDF nie ma outline'u, wracamy do heurystyki z
 * pierwszej linii strony (ten sam detektor co dla .txt).
 */
async function buildChapterPageMap(
  pdf: Awaited<ReturnType<typeof pdfjs.getDocument>["promise"]>,
): Promise<Map<number, string>> {
  const map = new Map<number, string>();
  try {
    const outline = await pdf.getOutline();
    if (!outline?.length) return map;
    const flat: { title: string; dest: unknown }[] = [];
    const collect = (items: typeof outline) => {
      for (const item of items) {
        if (item.title && item.dest) flat.push({ title: item.title, dest: item.dest });
        if (item.items?.length) collect(item.items);
      }
    };
    collect(outline);

    for (const entry of flat) {
      try {
        const dest =
          typeof entry.dest === "string" ? await pdf.getDestination(entry.dest) : entry.dest;
        const ref = Array.isArray(dest) ? dest[0] : null;
        if (!ref) continue;
        const pageIndex = await pdf.getPageIndex(ref);
        const title = entry.title.replace(/\s+/g, " ").trim();
        if (title && !map.has(pageIndex)) map.set(pageIndex, title);
      } catch {
        // Zepsuty pojedynczy wpis outline'u — pomiń go, reszta i tak się przyda.
      }
    }
  } catch {
    // Brak/zepsuty outline — zostaje pusta mapa, parsePdf spadnie na heurystykę.
  }
  return map;
}

export async function parsePdf(file: File): Promise<ParsedBook> {
  const buf = new Uint8Array(await file.arrayBuffer());
  const pdf = await pdfjs.getDocument({ data: buf }).promise;

  const meta = await pdf.getMetadata().catch(() => null);
  const info = (meta?.info ?? {}) as Record<string, unknown>;
  const title = typeof info.Title === "string" ? info.Title : "";
  const author = typeof info.Author === "string" ? info.Author : "";

  const chapterPageMap = await buildChapterPageMap(pdf);
  const hasOutline = chapterPageMap.size > 0;

  const pages: string[][] = [];
  for (let pageNum = 1; pageNum <= pdf.numPages; pageNum++) {
    const page = await pdf.getPage(pageNum);
    const content = await page.getTextContent();
    pages.push(pageLines(content.items as Array<TextItemLike>));
  }
  dropRunningHeaders(pages);

  const events: BookEvent[] = [];
  let guessedChapters = 0;
  let textPages = 0;
  for (let pageIndex = 0; pageIndex < pages.length; pageIndex++) {
    const text = pages[pageIndex].join("\n");
    if (!text.trim()) continue;
    textPages++;

    const outlineTitle = chapterPageMap.get(pageIndex);
    if (outlineTitle) events.push({ kind: "chapter", text: outlineTitle });

    // Rozbij stronę na akapity po pustych liniach.
    let first = true;
    for (const para of text.split(/\n\s*\n+/)) {
      // "przy-\nkład" złamane na końcu linii wraca do jednego słowa.
      const t = para
        .replace(/(\p{L})[-­]\n(\p{Ll})/gu, "$1$2")
        .replace(/\s+/g, " ")
        .trim();
      if (!t) continue;
      // Akapit przerwany końcem strony: dalszy ciąg od małej litery wraca
      // do poprzedniego akapitu.
      const last = events[events.length - 1];
      if (first && !outlineTitle && last?.kind === "paragraph" && !/[.!?…:"”»)]$/.test(last.text) && /^\p{Ll}/u.test(t)) {
        events[events.length - 1] = { kind: "paragraph", text: `${last.text} ${t}` };
        first = false;
        continue;
      }
      // Bez outline'u sprawdź, czy pierwszy akapit strony wygląda jak
      // nagłówek rozdziału (np. "Rozdział 3" na osobnej linii). Sama liczba
      // to w PDF-ie prawie zawsze numer strony, nie rozdział.
      if (!hasOutline && first && !outlineTitle && !/^\d{1,4}\.?$/.test(t) && looksLikeChapterHeading(t)) {
        events.push({ kind: "chapter", text: t });
        guessedChapters++;
      } else {
        events.push({ kind: "paragraph", text: t });
      }
      first = false;
    }
  }

  // A "chapter" on every few pages is a page header the filter missed,
  // not a chapter: keep the text, drop the guesses.
  if (guessedChapters > Math.max(4, textPages / 3)) {
    for (let i = 0; i < events.length; i++) {
      if (events[i].kind === "chapter") events[i] = { kind: "paragraph", text: events[i].text };
    }
  }

  if (!events.length) {
    throw new Error(tr("err.conv.pdfScan"));
  }

  return {
    metadata: {
      title: title || stripExt(file.name),
      author,
      source: file.name,
    },
    events,
  };
}

/**
 * Clears running headers and footers: page numbers and lines (book title,
 * author, chapter name) repeated at the top or bottom of many pages. Left
 * in, they came out as a "chapter" on nearly every page ("Dżuma": 186).
 */
function dropRunningHeaders(pages: string[][]): void {
  const edgeLines = (lines: string[]) => {
    const filled = lines.flatMap((l, i) => (l.trim() ? [i] : []));
    return [...new Set([...filled.slice(0, 2), ...filled.slice(-2)])];
  };
  const key = (line: string) => line.toLocaleLowerCase().replace(/[\d\s\-–—.·|]+/g, "");

  const seen = new Map<string, number>();
  for (const lines of pages) {
    const keys = new Set(edgeLines(lines).map((i) => key(lines[i])).filter(Boolean));
    for (const k of keys) seen.set(k, (seen.get(k) ?? 0) + 1);
  }
  const textPages = pages.filter((lines) => lines.some((l) => l.trim())).length;
  const repeatAt = Math.max(3, Math.ceil(textPages * 0.25));

  for (const lines of pages) {
    for (const i of edgeLines(lines)) {
      const line = lines[i].trim();
      const pageNumber = /^[-–—(]?\s*\d{1,4}\s*[-–—)]?$/.test(line);
      if (pageNumber || (seen.get(key(line)) ?? 0) >= repeatAt) lines[i] = "";
    }
  }
}

interface TextItemLike {
  str: string;
  hasEOL?: boolean;
  transform?: number[];
  width?: number;
  height?: number;
}

interface PdfLine {
  text: string;
  x: number;
  right: number;
  y: number;
  size: number;
}

/** Text items -> visual lines with their position (PDF y grows upwards). */
function collectLines(items: TextItemLike[]): PdfLine[] {
  const lines: PdfLine[] = [];
  let line: PdfLine | null = null;
  for (const it of items) {
    const t = it.transform;
    const x = t?.[4] ?? 0;
    const y = t?.[5] ?? 0;
    const size = Math.abs(t?.[3] ?? 0) || it.height || 10;
    if (line && Math.abs(y - line.y) > Math.max(2, size * 0.3)) {
      if (line.text.trim()) lines.push(line);
      line = null;
    }
    if (!line) line = { text: "", x, right: x, y, size };
    // Items on one line with a visible gap between them: a space the PDF
    // did not write out.
    if (line.text && x - line.right > size * 0.2 && !/\s$/.test(line.text) && !/^\s/.test(it.str)) {
      line.text += " ";
    }
    line.text += it.str;
    line.x = Math.min(line.x, x);
    line.right = Math.max(line.right, x + (it.width ?? 0));
    line.size = Math.max(line.size, size);
    if (it.hasEOL) {
      if (line.text.trim()) lines.push(line);
      line = null;
    }
  }
  if (line && line.text.trim()) lines.push(line);
  return lines;
}

function quantile(values: number[], q: number): number {
  if (!values.length) return 0;
  const sorted = [...values].sort((a, b) => a - b);
  return sorted[Math.min(sorted.length - 1, Math.floor(q * sorted.length))];
}

/**
 * One page as lines, with an empty line wherever a paragraph ends. Without
 * it a whole page came out as one paragraph: the chapter editor only marks
 * paragraph starts, so a chapter in the middle of a page could not be set,
 * and its search saw only the first words of every page.
 *
 * A paragraph ends at a wider gap than the usual line spacing, before an
 * indented first line, after a short line that closes a sentence, and
 * around lines in another size or centred (headings).
 */
function pageLines(items: TextItemLike[]): string[] {
  const lines = collectLines(items);
  if (lines.length < 2) return lines.map((l) => l.text);
  const gaps: number[] = [];
  for (let i = 1; i < lines.length; i++) {
    const d = lines[i - 1].y - lines[i].y;
    if (d > 0) gaps.push(d);
  }
  const lineGap = quantile(gaps, 0.5) || lines[0].size * 1.2;
  const left = quantile(lines.map((l) => l.x), 0.25);
  const right = quantile(lines.map((l) => l.right), 0.75);
  const width = Math.max(1, right - left);
  const centred = (l: PdfLine) => l.x - left > width * 0.12 && right - l.right > width * 0.12;

  const out: string[] = [lines[0].text];
  for (let i = 1; i < lines.length; i++) {
    const prev = lines[i - 1];
    const line = lines[i];
    const d = prev.y - line.y;
    const gap = d > lineGap * 1.45 || d < -lineGap; // blank line, or the next column
    const indent = line.x - left > line.size * 0.8 && line.x - left < width * 0.3;
    const shortEnd = prev.right < right - width * 0.12 && /[.!?…:"”»)]$/.test(prev.text.trim());
    const sizeChange = Math.abs(line.size - prev.size) > prev.size * 0.15;
    if (gap || indent || shortEnd || sizeChange || centred(prev) || centred(line)) out.push("");
    out.push(line.text);
  }
  return out;
}

function stripExt(name: string): string {
  return name.replace(/\.[^.]+$/, "");
}
