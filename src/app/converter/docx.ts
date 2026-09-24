import mammoth from "mammoth";
import { extractEventsFromElement } from "./text-formats";
import type { ParsedBook } from "./rsvp";

export async function parseDocx(file: File): Promise<ParsedBook> {
  const arrayBuffer = await file.arrayBuffer();
  const { value: html } = await mammoth.convertToHtml({ arrayBuffer });

  const doc = new DOMParser().parseFromString(html, "text/html");
  const events = extractEventsFromElement(doc.body);

  if (!events.some((e) => e.kind === "paragraph")) {
    throw new Error("Nie udało się wyodrębnić tekstu z dokumentu.");
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

function stripExt(name: string): string {
  return name.replace(/\.[^.]+$/, "");
}
