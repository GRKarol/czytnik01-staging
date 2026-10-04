import { FOCUS_COLOR_CUSTOM, type DeviceSettings } from "../device/api";
import { rgb565 } from "./theme";

/**
 * The app wears the reader's menu colors: its palette (Classic follows the
 * reading theme) and its highlight color. Kept in localStorage, so the app
 * opens in the reader's colors before it reconnects.
 */
const STORE_LOOK = "flower.readerLook";
const STORE_SOFT = "flower.lookSoft";

/**
 * Loud palettes (a saturated page, or text that barely stands off it) are
 * fine on the reader's small bar and tiring on a phone screen full of
 * forms. With softening on (the default) such a page becomes a neutral one
 * tinted with the reader's color, and the text a near-black or near-white
 * ink; the accent stays the reader's.
 */
export function softColors(): boolean {
  try {
    return localStorage.getItem(STORE_SOFT) !== "0";
  } catch {
    return true;
  }
}

export function setSoftColors(on: boolean): void {
  try {
    localStorage.setItem(STORE_SOFT, on ? "1" : "0");
  } catch {
    /* ignored */
  }
}

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
  let bgCss = rgb565(bg);
  let fgCss = rgb565(fg);
  const accentCss = rgb565(accent);
  const light = luminance(bg) > 0.5;
  if (softColors() && (saturation(bg) > 0.45 || contrast(bg, fg) < 7)) {
    bgCss = blend(bg, light ? [244, 242, 237] : [18, 19, 22], light ? 28 : 22);
    fgCss = blend(fg, light ? [22, 23, 26] : [241, 241, 243], 20);
  }
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

/** percent % of an RGB565 color over an RGB base, as rgb() (theme-color reads it). */
function blend(value: number, base: [number, number, number], percent: number): string {
  const rgb = [
    (((value >> 11) & 0x1f) * 255) / 31,
    (((value >> 5) & 0x3f) * 255) / 63,
    ((value & 0x1f) * 255) / 31,
  ];
  const out = rgb.map((c, i) => Math.round((c * percent + base[i] * (100 - percent)) / 100));
  return `rgb(${out[0]}, ${out[1]}, ${out[2]})`;
}

/** HSV saturation of an RGB565 color, 0..1. */
function saturation(value: number): number {
  const r = ((value >> 11) & 0x1f) / 31;
  const g = ((value >> 5) & 0x3f) / 63;
  const b = (value & 0x1f) / 31;
  const max = Math.max(r, g, b);
  return max === 0 ? 0 : (max - Math.min(r, g, b)) / max;
}

/** WCAG contrast ratio between two RGB565 colors (1..21). */
function contrast(a: number, b: number): number {
  const la = luminance(a);
  const lb = luminance(b);
  return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05);
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
