import JSZip from "jszip";
import { extractEventsFromElement } from "./text-formats";
import type { BookEvent, ParsedBook } from "./rsvp";
import { tr } from "../i18n/index";

/**
 * Port `firmware/tools/epub_to_rsvp.py` na TypeScript:
 * 1. Otwórz EPUB jako ZIP.
 * 2. `META-INF/container.xml` → ścieżka do `.opf`.
 * 3. `.opf` → tytuł, autor, lista spine itemów (XHTML w kolejności czytania).
 * 4. Dla każdego XHTML wyciągnij rozdziały (`<hN>`) i paragrafy.
 */
export async function parseEpub(file: File): Promise<ParsedBook> {
  const zip = await JSZip.loadAsync(await file.arrayBuffer());

  const containerXml = await readZipText(zip, "META-INF/container.xml");
  const opfPath = findRootfilePath(containerXml);
  if (!opfPath) {
    throw new Error(tr("err.conv.epubOpf"));
  }

  const opfXml = await readZipText(zip, opfPath);
  const opfDoc = new DOMParser().parseFromString(opfXml, "application/xml");
  const title = firstLocalText(opfDoc, "title") || stripExt(file.name);
  const author = firstLocalText(opfDoc, "creator");

  const manifest = new Map<string, string>();
  const navPaths: string[] = [];
  for (const item of Array.from(opfDoc.getElementsByTagName("*"))) {
    if (localName(item) !== "item") continue;
    const id = item.getAttribute("id");
    const href = item.getAttribute("href");
    if (id && href) manifest.set(id, joinZipPath(opfPath, href));
    if (href && (item.getAttribute("properties") ?? "").split(/\s+/).includes("nav")) {
      navPaths.push(joinZipPath(opfPath, href));
    }
  }

  const spinePaths: string[] = [];
  let ncxPath = "";
  for (const ref of Array.from(opfDoc.getElementsByTagName("*"))) {
    const name = localName(ref);
    if (name === "spine") {
      const tocId = ref.getAttribute("toc");
      if (tocId && manifest.has(tocId)) ncxPath = manifest.get(tocId)!;
    }
    if (name !== "itemref") continue;
    const idref = ref.getAttribute("idref");
    if (!idref) continue;
    const path = manifest.get(idref);
    if (path && /\.(x?html?|htm)$/i.test(path)) spinePaths.push(path);
  }

  if (!spinePaths.length) {
    throw new Error(tr("err.conv.epubEmpty"));
  }

  // The book's own table of contents is the chapter list; headings inside
  // the text are only a fallback. (Files in the spine are NOT chapters:
  // converters split books into files by size or page break.)
  const toc = (await readNavToc(zip, navPaths[0])) ?? (await readNcxToc(zip, ncxPath));
  const useToc = !!toc && new Set(toc.filter((e) => spinePaths.includes(e.path)).map((e) => e.path + "#" + e.fragment)).size >= 2;

  const events: BookEvent[] = [];
  for (const path of spinePaths) {
    const xhtml = await readZipText(zip, path);
    const doc = new DOMParser().parseFromString(xhtml, "application/xhtml+xml");
    // Niektóre EPUB-y mają błędy parserowe — fallback na text/html.
    const body =
      doc.querySelector("parsererror")
        ? new DOMParser().parseFromString(xhtml, "text/html").body
        : doc.querySelector("body");

    let chunk: BookEvent[];
    if (useToc) {
      const entries = toc!.filter((e) => e.path === path);
      const anchors = new Map<string, { title: string; level: number }>();
      for (const e of entries) {
        if (e.fragment && !anchors.has(e.fragment)) anchors.set(e.fragment, { title: e.title, level: e.level });
      }
      chunk = extractEventsFromElement(body, { anchors });
      const atStart = entries.find((e) => !e.fragment);
      if (atStart) chunk.unshift({ kind: "chapter", text: atStart.title, level: atStart.level });
    } else {
      chunk = extractEventsFromElement(body);
    }
    if (!chunk.some((e) => e.kind === "paragraph")) {
      // Cover, title page: keep only its chapter mark so a TOC entry isn't lost.
      events.push(...chunk.filter((e) => e.kind === "chapter"));
      continue;
    }
    events.push(...chunk);
  }

  return {
    metadata: { title, author, source: file.name },
    events,
  };
}

interface TocEntry {
  path: string;
  fragment: string;
  title: string;
  level: number;
}

function hrefParts(base: string, href: string): { path: string; fragment: string } {
  const hash = href.indexOf("#");
  const fragment = hash >= 0 ? decodeURIComponent(href.slice(hash + 1)) : "";
  const file = hash >= 0 ? href.slice(0, hash) : href;
  return { path: file ? joinZipPath(base, file) : base, fragment };
}

/** EPUB 3 navigation document: `<nav epub:type="toc">` with nested lists. */
async function readNavToc(zip: JSZip, navPath: string | undefined): Promise<TocEntry[] | null> {
  if (!navPath || !zip.file(navPath)) return null;
  try {
    const doc = new DOMParser().parseFromString(await readZipText(zip, navPath), "application/xhtml+xml");
    const navs = Array.from(doc.getElementsByTagName("*")).filter((el) => localName(el) === "nav");
    const nav =
      navs.find((n) => /toc/.test(n.getAttribute("epub:type") ?? n.getAttribute("type") ?? "")) ?? navs[0];
    if (!nav) return null;
    const entries: TocEntry[] = [];
    const walk = (el: Element, depth: number) => {
      for (const child of Array.from(el.children)) {
        const name = localName(child);
        if (name === "li") {
          const link = Array.from(child.children).find((c) => localName(c) === "a");
          const href = link?.getAttribute("href");
          const title = (link?.textContent ?? "").replace(/\s+/g, " ").trim();
          if (href && title) entries.push({ ...hrefParts(navPath, href), title, level: depth });
          walk(child, depth + 1);
        } else if (name === "ol" || name === "ul") {
          walk(child, depth);
        }
      }
    };
    walk(nav, 1);
    return entries.length ? entries : null;
  } catch {
    return null;
  }
}

/** EPUB 2 NCX: nested `<navPoint>` with a label and `<content src>`. */
async function readNcxToc(zip: JSZip, ncxPath: string): Promise<TocEntry[] | null> {
  if (!ncxPath || !zip.file(ncxPath)) return null;
  try {
    const doc = new DOMParser().parseFromString(await readZipText(zip, ncxPath), "application/xml");
    const map = Array.from(doc.getElementsByTagName("*")).find((el) => localName(el) === "navmap");
    if (!map) return null;
    const entries: TocEntry[] = [];
    const walk = (el: Element, depth: number) => {
      for (const point of Array.from(el.children)) {
        if (localName(point) !== "navpoint") continue;
        const label = Array.from(point.getElementsByTagName("*")).find((c) => localName(c) === "text");
        const content = Array.from(point.children).find((c) => localName(c) === "content");
        const src = content?.getAttribute("src");
        const title = (label?.textContent ?? "").replace(/\s+/g, " ").trim();
        if (src && title) entries.push({ ...hrefParts(ncxPath, src), title, level: depth });
        walk(point, depth + 1);
      }
    };
    walk(map, 1);
    return entries.length ? entries : null;
  } catch {
    return null;
  }
}

// ─── helpers ────────────────────────────────────────────────────────────────

/**
 * The cover picture packed in an EPUB, if it has one: the manifest item
 * marked `properties="cover-image"` (EPUB 3), the one named by
 * `<meta name="cover">` (EPUB 2), or any image called "cover". Starting
 * point for the cover editor, so the reader shows the book's real cover.
 */
export async function extractEpubCover(file: Blob): Promise<Blob | null> {
  try {
    const zip = await JSZip.loadAsync(await file.arrayBuffer());
    const opfPath = findRootfilePath(await readZipText(zip, "META-INF/container.xml"));
    if (!opfPath) return null;
    const opfDoc = new DOMParser().parseFromString(await readZipText(zip, opfPath), "application/xml");

    const items: { id: string; href: string; type: string; props: string }[] = [];
    let coverId = "";
    for (const el of Array.from(opfDoc.getElementsByTagName("*"))) {
      const name = localName(el);
      if (name === "item") {
        items.push({
          id: el.getAttribute("id") ?? "",
          href: el.getAttribute("href") ?? "",
          type: el.getAttribute("media-type") ?? "",
          props: el.getAttribute("properties") ?? "",
        });
      } else if (name === "meta" && el.getAttribute("name") === "cover") {
        coverId = el.getAttribute("content") ?? "";
      }
    }
    const images = items.filter((i) => i.type.startsWith("image/") && i.href);
    const pick =
      images.find((i) => i.props.split(/\s+/).includes("cover-image")) ??
      images.find((i) => coverId && i.id === coverId) ??
      images.find((i) => /cover/i.test(i.id) || /cover/i.test(i.href));
    if (!pick) return null;
    const entry = zip.file(joinZipPath(opfPath, pick.href));
    if (!entry) return null;
    return new Blob([await entry.async("arraybuffer")], { type: pick.type });
  } catch {
    return null;
  }
}

async function readZipText(zip: JSZip, name: string): Promise<string> {
  const f = zip.file(name);
  if (!f) throw new Error(`Brak pliku w EPUB: ${name}`);
  return f.async("string");
}

function findRootfilePath(containerXml: string): string {
  const doc = new DOMParser().parseFromString(containerXml, "application/xml");
  for (const el of Array.from(doc.getElementsByTagName("*"))) {
    if (localName(el) === "rootfile") {
      return el.getAttribute("full-path") ?? "";
    }
  }
  return "";
}

function localName(el: Element): string {
  const t = el.tagName;
  const c = t.indexOf(":");
  return (c >= 0 ? t.slice(c + 1) : t).toLowerCase();
}

function firstLocalText(doc: Document, want: string): string {
  for (const el of Array.from(doc.getElementsByTagName("*"))) {
    if (localName(el) === want && el.textContent) {
      return el.textContent.replace(/\s+/g, " ").trim();
    }
  }
  return "";
}

function joinZipPath(base: string, href: string): string {
  const decoded = decodeURIComponent(href.split("#")[0]);
  const baseDir = base.replace(/[^/]+$/, "");
  const combined = baseDir + decoded;
  // Uprość ./ i ../
  const parts: string[] = [];
  for (const p of combined.split("/")) {
    if (p === "" || p === ".") continue;
    if (p === "..") parts.pop();
    else parts.push(p);
  }
  return parts.join("/");
}

function stripExt(name: string): string {
  return name.replace(/\.[^.]+$/, "");
}
