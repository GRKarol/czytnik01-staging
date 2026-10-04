import { TYPEFACE_NAMES, type DeviceSettings } from "../device/api";
import atkinsonUrl from "../assets/fonts/AtkinsonHyperlegible.woff2?url";
import interUrl from "../assets/fonts/Inter.woff2?url";
import literataUrl from "../assets/fonts/Literata.woff2?url";
import notoUrl from "../assets/fonts/NotoSans.woff2?url";
import nunitoUrl from "../assets/fonts/Nunito.woff2?url";
import openDyslexicUrl from "../assets/fonts/OpenDyslexic.woff2?url";

/**
 * The app writes in the reader's menu font, the way it wears the reader's
 * colors. The six menu faces ship with the app (the same Latin subsets the
 * firmware renders from, firmware/tools/ui_fonts); "same as the book" maps
 * the reading face onto them where one matches, and keeps Inter otherwise.
 * Remembered in localStorage, so the app opens in it before reconnecting.
 */
const STORE_FONT = "flower.readerFont";

const FILES: Record<string, string> = {
  Inter: interUrl,
  "Noto Sans": notoUrl,
  Atkinson: atkinsonUrl,
  Nunito: nunitoUrl,
  Literata: literataUrl,
  OpenDyslexic: openDyslexicUrl,
};

// Reading faces (TYPEFACE_NAMES) that are also menu faces.
const BOOK_TO_MENU: Record<string, string> = {
  "Atkinson Hyperlegible": "Atkinson",
  OpenDyslexic: "OpenDyslexic",
  Literata: "Literata",
};

const FALLBACK = 'system-ui, -apple-system, "Segoe UI", Roboto, sans-serif';
const loaded = new Map<string, Promise<void>>();

/** Menu face name for these settings; null = not known yet, keep what is shown. */
export function fontFromSettings(s: DeviceSettings): string | null {
  const device = s.device;
  const options = s.options;
  if (!device || !options) return null;
  if (device.menuFont >= 0) {
    const name = options.menuFonts[device.menuFont];
    return name && FILES[name] ? name : "Inter";
  }
  return BOOK_TO_MENU[TYPEFACE_NAMES[s.typefaceIndex] ?? ""] ?? "Inter";
}

export function saveFont(name: string | null): void {
  try {
    if (name) localStorage.setItem(STORE_FONT, name);
  } catch {
    /* ignored */
  }
}

export function savedFont(): string | null {
  try {
    return localStorage.getItem(STORE_FONT);
  } catch {
    return null;
  }
}

/** Loads the face once and points --font of the shell at it. */
export function applyFont(host: HTMLElement, name: string | null): void {
  if (!name || !FILES[name]) return;
  const family = `Reader ${name}`;
  let ready = loaded.get(name);
  if (!ready) {
    const face = new FontFace(family, `url(${FILES[name]}) format("woff2")`);
    ready = face.load().then((f) => {
      document.fonts.add(f);
    });
    loaded.set(name, ready);
  }
  ready
    .then(() => host.style.setProperty("--font", `"${family}", ${FALLBACK}`))
    .catch(() => {
      loaded.delete(name);
    });
}
