import type { ParsedBook } from "./rsvp";
import { writeRsvp } from "./rsvp";
import { parseTxt, parseMarkdown, parseHtml } from "./text-formats";
import { parseEpub } from "./epub";
import { parsePdf } from "./pdf";
import { parseMobi } from "./mobi";
import { parseDocx } from "./docx";

export type SupportedFormat =
  | "txt"
  | "md"
  | "html"
  | "epub"
  | "pdf"
  | "mobi"
  | "azw"
  | "azw3"
  | "docx";

const SUPPORTED_EXT: Record<string, SupportedFormat> = {
  txt: "txt",
  text: "txt",
  md: "md",
  markdown: "md",
  mdown: "md",
  html: "html",
  htm: "html",
  xhtml: "html",
  epub: "epub",
  pdf: "pdf",
  mobi: "mobi",
  azw: "azw",
  azw3: "azw3",
  docx: "docx",
};

export interface DetectionResult {
  kind: "supported" | "unknown";
  format?: SupportedFormat;
}

export function detectFormat(file: File): DetectionResult {
  const ext = (file.name.split(".").pop() ?? "").toLowerCase();
  if (ext in SUPPORTED_EXT) return { kind: "supported", format: SUPPORTED_EXT[ext] };
  return { kind: "unknown" };
}

export async function parseFile(file: File, format: SupportedFormat): Promise<ParsedBook> {
  switch (format) {
    case "txt":
      return parseTxt(file);
    case "md":
      return parseMarkdown(file);
    case "html":
      return parseHtml(file);
    case "epub":
      return parseEpub(file);
    case "pdf":
      return parsePdf(file);
    case "mobi":
    case "azw":
    case "azw3":
      return parseMobi(file);
    case "docx":
      return parseDocx(file);
  }
}

export { writeRsvp };
export type { ParsedBook, BookEvent, BookMetadata } from "./rsvp";
