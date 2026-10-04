import JSZip from "jszip";
import type { BookEvent, ParsedBook } from "./rsvp";
import { tr } from "../i18n/index";

/**
 * OpenDocument text (.odt, LibreOffice / OpenOffice) → events.
 * content.xml holds the text: `<text:h text:outline-level="N">` headings,
 * `<text:p>` paragraphs, lists and tables around them. Footnotes, frames'
 * captions and the table of contents block are skipped. meta.xml gives the
 * title and author when the document has them.
 */
export async function parseOdt(file: File): Promise<ParsedBook> {
  const zip = await JSZip.loadAsync(await file.arrayBuffer());
  const contentFile = zip.file("content.xml");
  if (!contentFile) throw new Error(tr("err.conv.odtNot"));

  const doc = new DOMParser().parseFromString(await contentFile.async("string"), "application/xml");
  const body = Array.from(doc.getElementsByTagName("*")).find((el) => el.tagName === "office:text");
  const events: BookEvent[] = [];
  if (body) walkBlocks(body, events);

  if (!events.some((e) => e.kind === "paragraph")) {
    throw new Error(tr("err.conv.odt"));
  }

  let title = "";
  let author = "";
  const metaFile = zip.file("meta.xml");
  if (metaFile) {
    const meta = new DOMParser().parseFromString(await metaFile.async("string"), "application/xml");
    for (const el of Array.from(meta.getElementsByTagName("*"))) {
      const text = (el.textContent ?? "").replace(/\s+/g, " ").trim();
      if (!text) continue;
      if (el.tagName === "dc:title" && !title) title = text;
      if ((el.tagName === "dc:creator" || el.tagName === "meta:initial-creator") && !author) author = text;
    }
  }

  return {
    metadata: { title: title || file.name.replace(/\.[^.]+$/, ""), author, source: file.name },
    events,
  };
}

const SKIP = new Set([
  "text:note", // footnotes / endnotes
  "text:table-of-content",
  "text:alphabetical-index",
  "text:bibliography",
  "office:annotation",
  "text:tracked-changes",
  "draw:frame",
]);

function walkBlocks(el: Element, events: BookEvent[]): void {
  for (const child of Array.from(el.children)) {
    const tag = child.tagName;
    if (SKIP.has(tag)) continue;
    if (tag === "text:h") {
      const text = inlineText(child);
      if (text) {
        const level = Number(child.getAttribute("text:outline-level") ?? "1") || 1;
        events.push({ kind: "chapter", text, level });
      }
    } else if (tag === "text:p") {
      const text = inlineText(child);
      if (text) events.push({ kind: "paragraph", text });
    } else {
      // text:list, text:list-item, text:section, table:table, table:table-row…
      walkBlocks(child, events);
    }
  }
}

function inlineText(el: Element): string {
  let out = "";
  const walk = (node: Node) => {
    if (node.nodeType === Node.TEXT_NODE) {
      out += node.nodeValue ?? "";
      return;
    }
    if (node.nodeType !== Node.ELEMENT_NODE) return;
    const child = node as Element;
    const tag = child.tagName;
    if (SKIP.has(tag)) return;
    if (tag === "text:s") {
      out += " ".repeat(Number(child.getAttribute("text:c") ?? "1") || 1);
      return;
    }
    if (tag === "text:tab" || tag === "text:line-break") {
      out += " ";
      return;
    }
    for (const c of Array.from(child.childNodes)) walk(c);
  };
  walk(el);
  return out.replace(/\s+/g, " ").trim();
}
