import type { BookEvent, ParsedBook } from "./rsvp";

// Rozpoznaje krótkie linie-nagłówki w tekście bez znaczników (txt, surowe
// strony PDF): "Rozdział 3", "ROZDZIAŁ PIERWSZY", "Chapter One", rzymskie
// numery ("XII"), albo krótka linia pisana WERSALIKAMI bez kropki na końcu.
// Celowo restrykcyjne (krótkie, bez końcowej interpunkcji) — żeby zwykłe
// zdania pisane wielką literą nie zamieniały się w fałszywe rozdziały.
const CHAPTER_WORD = /^(rozdzia[łl]|cz[ęe][śs][ćc]|chapter|part|book)\b/i;
const ROMAN_NUMERAL = /^[IVXLCDM]{1,8}\.?$/;

export function looksLikeChapterHeading(line: string): boolean {
  const t = line.trim();
  if (!t || t.length > 70) return false;
  if (/[.!?,;:]$/.test(t)) return false;
  if (CHAPTER_WORD.test(t)) return true;
  if (ROMAN_NUMERAL.test(t)) return true;
  if (/^\d{1,4}\.?$/.test(t)) return true;
  // WERSALIKI: co najmniej 3 litery, żadnej małej litery wśród liter.
  const letters = t.replace(/[^\p{L}]/gu, "");
  if (letters.length >= 3 && letters === letters.toUpperCase() && letters !== letters.toLowerCase()) {
    return true;
  }
  return false;
}

export async function parseTxt(file: File): Promise<ParsedBook> {
  const text = await file.text();
  const events: BookEvent[] = [];
  for (const block of text.split(/\r?\n\r?\n+/)) {
    const t = block.replace(/\s+/g, " ").trim();
    if (!t) continue;
    events.push({ kind: looksLikeChapterHeading(t) ? "chapter" : "paragraph", text: t });
  }
  return {
    metadata: {
      title: stripExt(file.name),
      author: "",
      source: file.name,
    },
    events,
  };
}

export async function parseMarkdown(file: File): Promise<ParsedBook> {
  const md = await file.text();
  const events: BookEvent[] = [];
  let title = "";

  // Podziel po pustych liniach — każdy blok osobno.
  for (const raw of md.split(/\r?\n\r?\n+/)) {
    const block = raw.trim();
    if (!block) continue;

    // Nagłówek? # / ## / ### …
    const head = block.match(/^(#{1,6})\s+(.+?)\s*#*$/);
    if (head) {
      const text = stripMdInline(head[2]);
      events.push({ kind: "chapter", text });
      if (!title && head[1].length <= 2) title = text;
      continue;
    }

    // Setext H1/H2: linia + "=== / ---" pod spodem.
    const setext = block.match(/^(.+)\n(=+|-+)\s*$/);
    if (setext) {
      events.push({ kind: "chapter", text: stripMdInline(setext[1]) });
      if (!title && setext[2][0] === "=") title = stripMdInline(setext[1]);
      continue;
    }

    events.push({
      kind: "paragraph",
      text: stripMdInline(block.replace(/\n/g, " ")),
    });
  }

  return {
    metadata: {
      title: title || stripExt(file.name),
      author: "",
      source: file.name,
    },
    events,
  };
}

export async function parseHtml(file: File): Promise<ParsedBook> {
  const html = await file.text();
  const doc = new DOMParser().parseFromString(html, "text/html");
  const events = extractEventsFromElement(doc.body);
  const docTitle = doc.querySelector("title")?.textContent?.trim() ?? "";
  return {
    metadata: {
      title: docTitle || stripExt(file.name),
      author: "",
      source: file.name,
    },
    events,
  };
}

// ─── helpers ────────────────────────────────────────────────────────────────

function stripExt(name: string): string {
  return name.replace(/\.[^.]+$/, "");
}

function stripMdInline(s: string): string {
  return s
    .replace(/!\[[^\]]*\]\([^)]*\)/g, "") // images
    .replace(/\[([^\]]+)\]\([^)]*\)/g, "$1") // links → text
    .replace(/[*_]{1,3}([^*_]+)[*_]{1,3}/g, "$1") // bold/italic
    .replace(/`([^`]+)`/g, "$1") // inline code
    .replace(/^>\s?/gm, "") // blockquotes
    .replace(/^[-*+]\s+/gm, "") // list bullets
    .replace(/^\d+\.\s+/gm, "") // ordered lists
    .replace(/\s+/g, " ")
    .trim();
}

const BLOCK_TAGS = new Set([
  "ADDRESS", "ARTICLE", "ASIDE", "BLOCKQUOTE", "DD", "DIV", "DL", "DT",
  "FIGCAPTION", "FIGURE", "FOOTER", "HEADER", "HR", "LI", "MAIN", "OL",
  "P", "PRE", "SECTION", "TABLE", "TBODY", "TD", "TFOOT", "TH", "THEAD",
  "TR", "UL",
]);
const HEADING_TAGS = new Set(["H1", "H2", "H3", "H4", "H5", "H6"]);
const SKIP_TAGS = new Set(["HEAD", "SCRIPT", "STYLE", "SVG", "NAV", "MATH"]);

/**
 * Eksportowane, bo używa też parser EPUB (chodzi po `<body>` każdego
 * spine-document). Zwraca listę eventów: rozdziały (z `<hN>`) i paragrafy
 * (cała reszta tekstu, scalona w bloki przez znaczniki blokowe).
 */
export function extractEventsFromElement(root: Element | null): BookEvent[] {
  const events: BookEvent[] = [];
  if (!root) return events;

  let buffer = "";
  const flush = () => {
    const t = buffer.replace(/\s+/g, " ").trim();
    if (t) events.push({ kind: "paragraph", text: t });
    buffer = "";
  };

  const walk = (node: Node) => {
    if (node.nodeType === Node.TEXT_NODE) {
      buffer += node.nodeValue ?? "";
      return;
    }
    if (node.nodeType !== Node.ELEMENT_NODE) return;
    const el = node as Element;
    const tag = el.tagName.toUpperCase();
    if (SKIP_TAGS.has(tag)) return;

    if (HEADING_TAGS.has(tag)) {
      flush();
      const text = (el.textContent ?? "").replace(/\s+/g, " ").trim();
      if (text) events.push({ kind: "chapter", text });
      return;
    }
    if (tag === "BR") {
      flush();
      return;
    }

    const isBlock = BLOCK_TAGS.has(tag);
    if (isBlock) flush();
    for (const child of Array.from(el.childNodes)) walk(child);
    if (isBlock) flush();
  };

  walk(root);
  flush();
  return events;
}
