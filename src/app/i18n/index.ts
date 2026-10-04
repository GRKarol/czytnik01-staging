/**
 * i18n for the Flower app, six languages (the reader's own set).
 *
 * - `tr(key, params)` / `t(key)`: the string table in ./strings (one entry
 *   per key with every language side by side), then the older JSON locale
 *   files (tutorial, tooltips, help), then English, then the key itself.
 * - Language: the one picked in the app (Więcej → Język aplikacji) wins;
 *   without a pick the app follows the reader once connected, and the
 *   phone's language before that.
 * - Emits "lang-changed" on document; the shell re-renders on it.
 */

import enLocale from "./locales/en.json";
import esLocale from "./locales/es.json";
import frLocale from "./locales/fr.json";
import deLocale from "./locales/de.json";
import roLocale from "./locales/ro.json";
import plLocale from "./locales/pl.json";
import { STRINGS } from "./strings";

export type SupportedLang = "en" | "es" | "fr" | "de" | "ro" | "pl";

export const SUPPORTED_LANGS: readonly SupportedLang[] = ["pl", "en", "de", "es", "fr", "ro"];

export const LANG_NAMES: Record<SupportedLang, string> = {
  pl: "Polski",
  en: "English",
  de: "Deutsch",
  es: "Español",
  fr: "Français",
  ro: "Română",
};

/** BCP 47 tags for number and date formatting. */
export const LANG_LOCALE: Record<SupportedLang, string> = {
  pl: "pl-PL",
  en: "en-GB",
  de: "de-DE",
  es: "es-ES",
  fr: "fr-FR",
  ro: "ro-RO",
};

const STORE_LANG = "flower.lang";

type LocaleMap = Record<string, string>;

const locales: Record<SupportedLang, LocaleMap> = {
  en: enLocale as LocaleMap,
  es: esLocale as LocaleMap,
  fr: frLocale as LocaleMap,
  de: deLocale as LocaleMap,
  ro: roLocale as LocaleMap,
  pl: plLocale as LocaleMap,
};

function isSupportedLang(lang: string): lang is SupportedLang {
  return (SUPPORTED_LANGS as readonly string[]).includes(lang);
}

function phoneLang(): SupportedLang {
  const tags = typeof navigator !== "undefined" ? [...(navigator.languages ?? []), navigator.language] : [];
  for (const tag of tags) {
    const code = (tag ?? "").slice(0, 2).toLowerCase();
    if (isSupportedLang(code)) return code;
  }
  return "en";
}

/** The language picked in the app, null = automatic. */
export function chosenLang(): SupportedLang | null {
  try {
    const stored = localStorage.getItem(STORE_LANG);
    return stored && isSupportedLang(stored) ? stored : null;
  } catch {
    return null;
  }
}

let currentLang: SupportedLang = chosenLang() ?? phoneLang();
if (typeof document !== "undefined") document.documentElement.lang = currentLang;

/** Translate a key; `{name}` placeholders are filled from `params`. */
export function tr(key: string, params?: Record<string, string | number>): string {
  if (key === "") return "";
  const entry = STRINGS[key];
  let text: string | undefined = entry ? (entry[currentLang] ?? entry.en) : undefined;
  if (text === undefined) text = locales[currentLang]?.[key] ?? locales.en[key];
  if (text === undefined) return key;
  if (params) {
    text = text.replace(/\{(\w+)\}/g, (match, name: string) =>
      name in params ? String(params[name]) : match,
    );
  }
  return text;
}

/** Same as tr() without placeholders (older call sites). */
export function t(key: string): string {
  return tr(key);
}

/** Switch the language now (not remembered). */
export function setLang(lang: SupportedLang | string): void {
  const resolved: SupportedLang = isSupportedLang(lang) ? lang : "en";
  if (resolved === currentLang) return;
  currentLang = resolved;
  document.documentElement.lang = resolved;
  document.dispatchEvent(new CustomEvent("lang-changed", { detail: { lang: resolved } }));
}

/** The user's pick in the app; null goes back to automatic. */
export function chooseLang(lang: SupportedLang | null): void {
  try {
    if (lang) localStorage.setItem(STORE_LANG, lang);
    else localStorage.removeItem(STORE_LANG);
  } catch {
    /* ignored */
  }
  setLang(lang ?? phoneLang());
}

/** The reader's language: followed unless the user picked one in the app. */
export function followReaderLang(lang: SupportedLang | string): void {
  if (!chosenLang()) setLang(lang);
}

export function getLang(): SupportedLang {
  return currentLang;
}

export function getLocale(): string {
  return LANG_LOCALE[currentLang];
}

export function formatNumber(n: number): string {
  return n.toLocaleString(getLocale());
}

export function formatDate(value: string | number | Date): string {
  return new Date(value).toLocaleDateString(getLocale());
}

export function onLangChange(cb: () => void): () => void {
  document.addEventListener("lang-changed", cb);
  return () => {
    document.removeEventListener("lang-changed", cb);
  };
}
