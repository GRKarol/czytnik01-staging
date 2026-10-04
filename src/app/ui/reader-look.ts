import { FOCUS_COLOR_CUSTOM, type DeviceSettings } from "../device/api";
import { rgb565 } from "./theme";

/**
 * The app wears the reader's menu colors: its palette (Classic follows the
 * reading theme) and its highlight color. Kept in localStorage, so the app
 * opens in the reader's colors before it reconnects.
 */
const STORE_LOOK = "flower.readerLook";

export type LookVars = Record<string, string>;

// DisplayManager.cpp: kTrueBlack, kLightBackgroundColor, word colors.
const CLASSIC = {
  dark: { bg: 0x0000, fg: 0xffff },
  light: { bg: 0xdeda, fg: 0x0000 },
  night: { bg: 0x0000, fg: 0xfce0 },
};

function mix(a: string, b: string, percentA: number): string {
  return `color-mix(in srgb, ${a} ${percentA}%, ${b})`;
}

/** CSS variables for the shell from the reader's settings; null = keep the default look. */
export function lookFromSettings(s: DeviceSettings): LookVars | null {
  const device = s.device;
  const options = s.options;
  if (!device || !options) return null;

  const focus =
    device.focusColor === FOCUS_COLOR_CUSTOM
      ? device.focusRgb
      : (options.focusColors[device.focusColor] ?? options.focusColors[1] ?? 0x001f);
  const palette = options.palettes[device.menuPalette];
  let bg: number;
  let fg: number;
  let accent = focus;
  if (palette?.c) {
    [bg, fg] = palette.c;
    if (!device.menuOwnAccent) accent = palette.c[2];
  } else {
    const classic = CLASSIC[s.theme] ?? CLASSIC.dark;
    bg = classic.bg;
    fg = classic.fg;
  }
  const bgCss = rgb565(bg);
  const fgCss = rgb565(fg);
  const accentCss = rgb565(accent);
  const light = luminance(bg) > 0.5;
  return {
    "--bg": bgCss,
    "--surface": mix(fgCss, bgCss, light ? 6 : 8),
    "--surface-2": mix(fgCss, bgCss, light ? 10 : 12),
    "--surface-3": mix(fgCss, bgCss, light ? 16 : 19),
    "--outline": mix(fgCss, bgCss, 24),
    "--text": fgCss,
    "--text-2": mix(fgCss, bgCss, 75),
    "--muted": mix(fgCss, bgCss, 55),
    "--line": mix(fgCss, "transparent", 10),
    "--accent": accentCss,
    // Accent as text: lifted toward the foreground so it reads on the page.
    "--accent-text": mix(accentCss, fgCss, light ? 80 : 62),
    "--on-accent": luminance(accent) > 0.6 ? "#000000" : "#ffffff",
    "color-scheme": light ? "light" : "dark",
  };
}

function luminance(value: number): number {
  const r = ((value >> 11) & 0x1f) / 31;
  const g = ((value >> 5) & 0x3f) / 63;
  const b = (value & 0x1f) / 31;
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

export function saveLook(look: LookVars | null): void {
  try {
    if (look) localStorage.setItem(STORE_LOOK, JSON.stringify(look));
  } catch {
    /* ignored */
  }
}

export function savedLook(): LookVars | null {
  try {
    const raw = localStorage.getItem(STORE_LOOK);
    return raw ? (JSON.parse(raw) as LookVars) : null;
  } catch {
    return null;
  }
}

/** Puts the look on an element (the shell's host) and the browser chrome. */
export function applyLook(host: HTMLElement, look: LookVars | null): void {
  if (!look) return;
  for (const [name, value] of Object.entries(look)) {
    host.style.setProperty(name, value);
  }
  document.documentElement.style.background = look["--bg"] ?? "#000";
  document.querySelector('meta[name="theme-color"]')?.setAttribute("content", toHex(look["--bg"]));
}

function toHex(css: string | undefined): string {
  const m = css?.match(/rgb\((\d+), (\d+), (\d+)\)/);
  if (!m) return "#000000";
  return `#${[m[1], m[2], m[3]].map((v) => Number(v).toString(16).padStart(2, "0")).join("")}`;
}
