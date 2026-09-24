import { initKf8File, initMobiFile, type Kf8, type Mobi } from "@lingo-reader/mobi-parser";
import { extractEventsFromElement } from "./text-formats";
import type { BookEvent, ParsedBook } from "./rsvp";

/**
 * MOBI i AZW3 dzielą ten sam kontener PDB, różni je wewnętrzny układ treści
 * (stary MOBI6 kontra KF8). AZW3 zwykle niesie w sobie też uboższą kopię
 * MOBI6 dla kompatybilności wstecznej z parserami, które KF8 nie rozumieją —
 * stąd wybór parsera po rozszerzeniu, z fallbackiem na drugi gdyby plik
 * okazał się błędnie oznaczony.
 */
export async function parseMobi(file: File): Promise<ParsedBook> {
  const isAzw3 = /\.azw3$/i.test(file.name);
  const primary = isAzw3 ? initKf8File : initMobiFile;
  const fallback = isAzw3 ? initMobiFile : initKf8File;

  let book: Mobi | Kf8;
  try {
    book = await primary(file);
  } catch {
    book = await fallback(file);
  }

  const metadata = book.getMetadata();
  const spine = book.getSpine();
  const events: BookEvent[] = [];

  let i = 0;
  for (const entry of spine) {
    i++;
    const chapter = book.loadChapter(entry.id);
    if (!chapter?.html) continue;
    const doc = new DOMParser().parseFromString(chapter.html, "text/html");
    const chunk = extractEventsFromElement(doc.body);
    if (!chunk.length || !chunk.some((e) => e.kind === "paragraph")) continue;
    if (!chunk.some((e) => e.kind === "chapter")) {
      chunk.unshift({ kind: "chapter", text: `Rozdział ${i}` });
    }
    events.push(...chunk);
  }

  book.destroy();

  if (!events.length) {
    throw new Error("Nie udało się wyodrębnić tekstu z pliku (uszkodzony lub zabezpieczony DRM?).");
  }

  const author = Array.isArray(metadata.author) ? metadata.author.join(", ") : metadata.author || "";

  return {
    metadata: {
      title: metadata.title || stripExt(file.name),
      author,
      source: file.name,
    },
    events,
  };
}

function stripExt(name: string): string {
  return name.replace(/\.[^.]+$/, "");
}
