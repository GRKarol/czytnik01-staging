import { tr } from "../i18n/index";
/**
 * Wspólny model dla wszystkich parserów: każdy konwerter zwraca strumień
 * eventów `BookEvent` (rozdział lub paragraf tekstu), które writer
 * serializuje do pliku .rsvp.
 */

export interface BookMetadata {
  title: string;
  author: string;
  source: string;
}

export type BookEvent =
  /** `level`: 1 for a top heading (h1, a table-of-contents root), deeper = larger. */
  | { kind: "chapter"; text: string; level?: number }
  | { kind: "paragraph"; text: string };

export interface ParsedBook {
  metadata: BookMetadata;
  events: BookEvent[];
}

const RSVP_VERSION = "1";
const WRAP_WIDTH = 96;

function directiveSafe(s: string): string {
  return s.replace(/\s+/g, " ").trim();
}

/** Zawija tekst akapitu na linie ≤ WRAP_WIDTH, nie łamiąc słów. */
function wrap(text: string, width = WRAP_WIDTH): string[] {
  const words = text.split(/\s+/).filter(Boolean);
  const out: string[] = [];
  let line = "";
  for (const w of words) {
    if (!line) {
      line = w;
    } else if (line.length + 1 + w.length <= width) {
      line += " " + w;
    } else {
      out.push(line);
      line = w;
    }
  }
  if (line) out.push(line);
  return out;
}

const countWords = (text: string) => text.split(/\s+/).filter(Boolean).length;

/**
 * Chapters a reader can actually navigate by. Converters report every
 * heading they see; books exported from ODT/Word often have hundreds (styled
 * sub-heads, one per page or per split file), which made the reader's
 * chapter list useless ("Dżuma" came out with 186). This keeps the top two
 * heading levels in use, joins headings with no text between them
 * ("Część pierwsza" + "I" -> "Część pierwsza · I"), and gives up on chapters
 * whose average is under ~120 words: at that density they aren't chapters.
 */
export function tidyChapters(events: BookEvent[]): BookEvent[] {
  const levels = events.flatMap((e) => (e.kind === "chapter" ? [e.level ?? 1] : []));
  if (!levels.length) return events;
  const top = Math.min(...levels);
  const deepest = top + 1;

  // Deeper headings stay in the text as ordinary lines.
  let out: BookEvent[] = events.map((e) =>
    e.kind === "chapter" && (e.level ?? 1) > deepest ? { kind: "paragraph", text: e.text } : e,
  );

  // Join headings that have no words between them.
  const joined: BookEvent[] = [];
  for (const e of out) {
    const prev = joined[joined.length - 1];
    if (e.kind === "chapter" && prev?.kind === "chapter") {
      if (normalizeTitle(prev.text) !== normalizeTitle(e.text)) prev.text = `${prev.text} · ${e.text}`;
      continue;
    }
    joined.push(e.kind === "chapter" ? { ...e } : e);
  }
  out = joined;

  const totalWords = out.reduce((n, e) => (e.kind === "paragraph" ? n + countWords(e.text) : n), 0);
  const tooDense = (list: BookEvent[]) => {
    const count = list.filter((e) => e.kind === "chapter").length;
    return count > 1 && totalWords / count < 120;
  };
  if (tooDense(out) && deepest > top) {
    // Try the top level alone before giving up.
    const topOnly = out.map((e) =>
      e.kind === "chapter" && (e.level ?? 1) > top ? ({ kind: "paragraph", text: e.text } as BookEvent) : e,
    );
    out = tooDense(topOnly) ? out : topOnly;
  }
  if (tooDense(out)) {
    out = out.map((e) => (e.kind === "chapter" ? { kind: "paragraph", text: e.text } : e));
  }
  return out;
}

function normalizeTitle(text: string): string {
  return text.toLocaleLowerCase().replace(/\s+/g, " ").trim();
}

/** Zamienia ParsedBook na string .rsvp gotowy do zapisu na pliku/SD. */
export function writeRsvp(book: ParsedBook): string {
  const { title, author, source } = book.metadata;
  const lines: string[] = [
    `@rsvp ${RSVP_VERSION}`,
    `@title ${directiveSafe(title || tr("common.untitled"))}`,
  ];
  if (author) lines.push(`@author ${directiveSafe(author)}`);
  if (source) lines.push(`@source ${directiveSafe(source)}`);
  lines.push("");

  let chapterCount = 0;
  for (const ev of tidyChapters(book.events)) {
    if (ev.kind === "chapter") {
      chapterCount++;
      lines.push("");
      lines.push(`@chapter ${directiveSafe(ev.text)}`);
    } else {
      for (let chunk of wrap(directiveSafe(ev.text))) {
        if (chunk.startsWith("@")) chunk = "@" + chunk;
        lines.push(chunk);
      }
    }
  }

  if (chapterCount === 0) {
    // Wstaw pojedynczy rozdział po nagłówku, żeby firmware miał pierwsze
    // kotwiczne paragrafy do nawigacji.
    lines.splice(4, 0, `@chapter ${directiveSafe(title || tr("common.untitled"))}`);
  }

  return lines.join("\n").trim() + "\n";
}
