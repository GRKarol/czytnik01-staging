import JSZip from "jszip";
import { Directory, Filesystem } from "@capacitor/filesystem";
import { Share } from "@capacitor/share";
import { registerPlugin, type PluginListenerHandle } from "@capacitor/core";
import { deviceApi, type Book, type ChapterMark } from "../device/api";
import { detectFormat, parseFile, writeRsvp, type SupportedFormat } from "../converter/index";
import { extractEpubCover } from "../converter/epub";
import { isNativeApp } from "../device/network-pin";
import { decodePicture, encodePicture, INITIAL_CROP, loadImage, releaseImage, renderCrop } from "./pictures";
import { tr } from "../i18n/index";

/**
 * Books going to the reader, and from the reader to other people.
 *
 * To the reader: every format is turned into the reader's own .rsvp on the
 * phone first (the reader's EPUB converter gave up on books like Sun Tzu's
 * "Art of War": "Cannot open book"), then the reader opens the new file at
 * once, so a book it can't read is reported and removed right away instead
 * of failing later on the reader's screen.
 *
 * To other people (no accounts, no server): a book from the reader leaves
 * as an EPUB, with or without its cover, through the phone's share sheet;
 * "what I'm reading" goes as a short text. A book that someone sends and
 * that is opened with Flower lands in the inbox below and goes to the
 * reader as soon as there is one.
 */

export const ACCEPTED_BOOK_FILES = ".rsvp,.epub,.txt,.md,.html,.htm,.pdf,.mobi,.azw,.azw3,.docx,.odt";

export interface SentBook {
  name: string;
  title: string;
  cover: Blob | null;
}

/** A file name the reader keeps as it is, Polish letters included. */
function readerFileName(title: string, fallback: string): string {
  const base = (title || fallback || "book").replace(/[^\p{L}\p{N}\- ]+/gu, "_").trim().slice(0, 80);
  return `${base || "book"}.rsvp`;
}

function stripExt(name: string): string {
  return name.replace(/\.[^.]+$/, "");
}

async function toReaderFormat(file: File): Promise<{ blob: Blob; fileName: string; title: string; cover: Blob | null }> {
  if (/\.rsvp$/i.test(file.name)) {
    const head = await file.slice(0, 2048).text();
    const title = /^@title\s+(.+)$/m.exec(head)?.[1]?.trim() ?? stripExt(file.name);
    return { blob: file, fileName: file.name, title, cover: null };
  }
  const detection = detectFormat(file);
  if (detection.kind !== "supported") {
    throw new Error(tr("err.conv.unsupported", { name: file.name }));
  }
  const book = await parseFile(file, detection.format as SupportedFormat);
  const rsvp = writeRsvp(book);
  const title = book.metadata.title || stripExt(file.name);
  const cover = detection.format === "epub" ? await extractEpubCover(file).catch(() => null) : null;
  return {
    blob: new Blob([rsvp], { type: "text/plain" }),
    fileName: readerFileName(title, stripExt(file.name)),
    title,
    cover,
  };
}

/**
 * Converts, sends, and has the reader open the book. A book the reader
 * can't open is deleted again and the reason comes back as the error.
 */
export async function sendBookToReader(
  file: File,
  onProgress?: (percent: number) => void,
  category: "book" | "article" = "book",
): Promise<SentBook> {
  const prepared = await toReaderFormat(file);
  const stored =
    (await deviceApi.uploadBook(prepared.blob, prepared.fileName, category, (loaded, total) => {
      if (onProgress && total) onProgress(Math.round((loaded / total) * 100));
    })) || `${category === "article" ? "articles" : "books"}/${prepared.fileName}`;
  try {
    await deviceApi.getBookText(stored, 0, 1, 1);
  } catch (err) {
    // Only the reader's own "can't open it" removes the book; a timeout or
    // a reader too old for the text endpoint leaves it on the card.
    const message = err instanceof Error ? err.message : String(err);
    const refused = /Cannot open book(?::\s*([^"}]*))?/i.exec(message);
    if (refused) {
      await deviceApi.deleteBook(stored).catch(() => {});
      throw new Error(tr("lib.err.unreadable", { title: prepared.title, reason: refused[1]?.trim() || "Cannot open book" }));
    }
  }
  return { name: stored, title: prepared.title, cover: prepared.cover };
}

/** The EPUB's own cover as the reader's cover picture. */
export async function setCoverFromImage(bookName: string, image: Blob): Promise<void> {
  const img = await loadImage(image);
  try {
    const canvas = renderCrop(img, img.naturalWidth, img.naturalHeight, "cover", INITIAL_CROP);
    await deviceApi.uploadBookPicture(bookName, "cover", encodePicture(canvas));
  } finally {
    releaseImage(img);
  }
}

// ─── Out to other people ─────────────────────────────────────────────────────

function escapeXml(text: string): string {
  return text.replace(/[<>&"']/g, (c) => ({ "<": "&lt;", ">": "&gt;", "&": "&amp;", '"': "&quot;", "'": "&apos;" })[c]!);
}

function xhtml(title: string, body: string, lang: string): string {
  return `<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="${lang}" xml:lang="${lang}">
<head><meta charset="utf-8"/><title>${escapeXml(title)}</title></head>
<body>
${body}
</body>
</html>
`;
}

/**
 * The book as the reader has it (its text in paragraphs, its chapters) in
 * an EPUB any reading app opens. `withCover` adds the cover picture.
 */
export async function exportBookAsEpub(
  book: Book,
  withCover: boolean,
  onProgress?: (percent: number) => void,
): Promise<File> {
  const title = book.title || stripExt(book.name.replace(/^.*\//, ""));
  const author = book.author || "";
  const lang = document.documentElement.lang || "pl";

  // Whole paragraphs (the reader stops each reply at ~24 KB).
  const paragraphs: { w: number; t: string }[] = [];
  let chapters: ChapterMark[] = [];
  let total = 1;
  let from = 0;
  while (from < total) {
    const page = await deviceApi.getBookText(book.name, from, 300, 6000);
    total = page.paragraphCount;
    if (from === 0) chapters = page.chapters;
    if (page.paragraphs.length === 0) break;
    for (const p of page.paragraphs) paragraphs.push({ w: p.w, t: p.t });
    from += page.paragraphs.length;
    onProgress?.(Math.round((Math.min(from, total) / Math.max(1, total)) * 90));
  }

  // Paragraphs into chapter files; text before the first chapter is its own.
  const marks = [...chapters].sort((a, b) => a.w - b.w);
  const sections: { title: string; paras: string[] }[] = [];
  let next = 0;
  let current: { title: string; paras: string[] } | null = null;
  for (const p of paragraphs) {
    while (next < marks.length && marks[next].w <= p.w) {
      current = { title: marks[next].t, paras: [] };
      sections.push(current);
      next++;
    }
    if (!current) {
      current = { title, paras: [] };
      sections.push(current);
    }
    current.paras.push(p.t);
  }
  if (sections.length === 0) sections.push({ title, paras: [] });

  const zip = new JSZip();
  zip.file("mimetype", "application/epub+zip", { compression: "STORE" });
  zip.file(
    "META-INF/container.xml",
    `<?xml version="1.0" encoding="utf-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>
`,
  );

  let coverJpeg: Blob | null = null;
  if (withCover && book.hasCover) {
    const raw = await deviceApi.getBookPicture(book.name, "cover").catch(() => null);
    const canvas = raw ? await decodePicture(raw) : null;
    if (canvas) {
      // The reader keeps 92x116; a 3x copy keeps it from looking like a stamp.
      const big = document.createElement("canvas");
      big.width = canvas.width * 3;
      big.height = canvas.height * 3;
      const ctx = big.getContext("2d");
      if (ctx) {
        ctx.imageSmoothingQuality = "high";
        ctx.drawImage(canvas, 0, 0, big.width, big.height);
        coverJpeg = await new Promise<Blob | null>((resolve) => big.toBlob(resolve, "image/jpeg", 0.9));
      }
    }
  }

  const manifest: string[] = [];
  const spine: string[] = [];
  const navItems: string[] = [];
  if (coverJpeg) {
    zip.file("OEBPS/cover.jpg", coverJpeg);
    zip.file("OEBPS/cover.xhtml", xhtml(title, `<img src="cover.jpg" alt="${escapeXml(title)}" style="max-width:100%"/>`, lang));
    manifest.push(`<item id="cover-image" href="cover.jpg" media-type="image/jpeg" properties="cover-image"/>`);
    manifest.push(`<item id="cover" href="cover.xhtml" media-type="application/xhtml+xml"/>`);
    spine.push(`<itemref idref="cover"/>`);
  }
  sections.forEach((section, i) => {
    const id = `c${i + 1}`;
    const body =
      `<h2>${escapeXml(section.title)}</h2>\n` + section.paras.map((t) => `<p>${escapeXml(t)}</p>`).join("\n");
    zip.file(`OEBPS/${id}.xhtml`, xhtml(section.title, body, lang));
    manifest.push(`<item id="${id}" href="${id}.xhtml" media-type="application/xhtml+xml"/>`);
    spine.push(`<itemref idref="${id}"/>`);
    navItems.push(`<li><a href="${id}.xhtml">${escapeXml(section.title)}</a></li>`);
  });
  zip.file(
    "OEBPS/nav.xhtml",
    xhtml(title, `<nav epub:type="toc"><h1>${escapeXml(title)}</h1><ol>${navItems.join("")}</ol></nav>`, lang),
  );
  manifest.push(`<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>`);
  const uid = `flower-${Date.now()}`;
  zip.file(
    "OEBPS/content.opf",
    `<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid" xml:lang="${lang}">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:identifier id="uid">${uid}</dc:identifier>
<dc:title>${escapeXml(title)}</dc:title>
${author ? `<dc:creator>${escapeXml(author)}</dc:creator>` : ""}
<dc:language>${lang}</dc:language>
<meta property="dcterms:modified">${new Date().toISOString().replace(/\.\d+Z$/, "Z")}</meta>
${coverJpeg ? `<meta name="cover" content="cover-image"/>` : ""}
</metadata>
<manifest>
${manifest.join("\n")}
</manifest>
<spine>
${spine.join("\n")}
</spine>
</package>
`,
  );
  const blob = await zip.generateAsync({ type: "blob", mimeType: "application/epub+zip" });
  onProgress?.(100);
  const fileName = `${(author ? `${author} - ` : "") + title}`.replace(/[\\/:*?"<>|]+/g, "_").slice(0, 100) + ".epub";
  return new File([blob], fileName, { type: "application/epub+zip" });
}

async function blobToBase64(blob: Blob): Promise<string> {
  const bytes = new Uint8Array(await blob.arrayBuffer());
  let binary = "";
  for (let i = 0; i < bytes.length; i += 0x8000) binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(binary);
}

/** The phone's share sheet (Messenger, WhatsApp, mail, Quick Share...). */
export async function shareFile(file: File, title: string): Promise<void> {
  if (isNativeApp()) {
    const { uri } = await Filesystem.writeFile({
      path: file.name,
      data: await blobToBase64(file),
      directory: Directory.Cache,
    });
    await Share.share({ title, dialogTitle: title, files: [uri] });
    return;
  }
  if (navigator.canShare?.({ files: [file] })) {
    await navigator.share({ title, files: [file] });
    return;
  }
  const url = URL.createObjectURL(file);
  const a = document.createElement("a");
  a.href = url;
  a.download = file.name;
  a.click();
  URL.revokeObjectURL(url);
}

export async function shareText(text: string, title: string): Promise<void> {
  if (isNativeApp()) {
    await Share.share({ title, dialogTitle: title, text });
    return;
  }
  if (navigator.share) {
    await navigator.share({ title, text });
    return;
  }
  await navigator.clipboard?.writeText(text);
}

/** "Czytam teraz: Tytuł - Autor (34%)" plus the app's address. */
export function readingStatusText(book: Book): string {
  const title = book.title || stripExt(book.name.replace(/^.*\//, ""));
  return tr("share.reading.text", {
    title,
    author: book.author ? ` - ${book.author}` : "",
    n: book.progressPercent ?? 0,
  });
}

// ─── Inbox: books other people sent ──────────────────────────────────────────

const DB = "flower-share";
const STORE = "pending-files";

export interface PendingBook {
  id: string;
  file: File;
  name: string;
}

function openDb(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(DB, 1);
    request.onupgradeneeded = () => {
      if (!request.result.objectStoreNames.contains(STORE)) request.result.createObjectStore(STORE);
    };
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
}

export async function addPendingBook(file: File): Promise<void> {
  const db = await openDb();
  await new Promise<void>((resolve, reject) => {
    const tx = db.transaction(STORE, "readwrite");
    tx.objectStore(STORE).put({ file, name: file.name }, `in-${Date.now()}-${Math.random().toString(36).slice(2, 7)}`);
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
  });
  db.close();
  document.dispatchEvent(new CustomEvent("flower-inbox-changed"));
}

export async function listPendingBooks(): Promise<PendingBook[]> {
  const db = await openDb();
  const result = await new Promise<PendingBook[]>((resolve, reject) => {
    const out: PendingBook[] = [];
    const cursor = db.transaction(STORE, "readonly").objectStore(STORE).openCursor();
    cursor.onsuccess = () => {
      const c = cursor.result;
      if (!c) {
        resolve(out);
        return;
      }
      const value = c.value as { file?: File; name?: string };
      if (value?.file) out.push({ id: String(c.key), file: value.file, name: value.name || value.file.name });
      c.continue();
    };
    cursor.onerror = () => reject(cursor.error);
  });
  db.close();
  return result;
}

export async function removePendingBook(id: string): Promise<void> {
  const db = await openDb();
  await new Promise<void>((resolve, reject) => {
    const tx = db.transaction(STORE, "readwrite");
    tx.objectStore(STORE).delete(id);
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
  });
  db.close();
  document.dispatchEvent(new CustomEvent("flower-inbox-changed"));
}

/** A file Android handed over ("Open with Flower" / share to Flower). */
export async function readNativeFile(path: string, name: string, mime: string): Promise<File> {
  const { data } = await Filesystem.readFile({ path });
  const bytes =
    typeof data === "string" ? Uint8Array.from(atob(data), (c) => c.charCodeAt(0)) : new Uint8Array(await data.arrayBuffer());
  await Filesystem.deleteFile({ path }).catch(() => {});
  return new File([bytes], name, { type: mime || "application/octet-stream" });
}

interface IncomingFile {
  path: string;
  name: string;
  mime: string;
}

interface ShareTargetPlugin {
  getPending(): Promise<{ text?: string | null; file?: IncomingFile }>;
  addListener(event: "fileReceived", cb: (file: IncomingFile) => void): Promise<PluginListenerHandle>;
}

const ShareTarget = registerPlugin<ShareTargetPlugin>("ShareTarget");

/**
 * Android hands over book files from MainActivity: the one that started
 * the app, then any that come while it runs. Each lands in the inbox and
 * `onBook` runs (the shell opens the library).
 */
export async function listenForIncomingBooks(onBook: () => void, onError: (err: unknown) => void): Promise<void> {
  if (!isNativeApp()) return;
  const take = async (incoming: IncomingFile) => {
    try {
      await addPendingBook(await readNativeFile(incoming.path, incoming.name, incoming.mime));
      onBook();
    } catch (err) {
      onError(err);
    }
  };
  try {
    await ShareTarget.addListener("fileReceived", (file) => void take(file));
    const pending = await ShareTarget.getPending();
    if (pending.file) await take(pending.file);
  } catch (err) {
    onError(err);
  }
}
