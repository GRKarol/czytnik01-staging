/**
 * Real implementation of `DeviceApi` over HTTP — used kiedy PWA jest
 * podłączona do urządzenia przez WiFi (telefon w sieci `Flower-XXXX`,
 * urządzenie pod `http://192.168.4.1`). Stosujemy ten sam interface
 * co `MockDeviceApi`, więc komponenty UI nie wiedzą o różnicy.
 *
 * Firmware używa zagnieżdżonego JSON-a (sekcje `reading`, `display`,
 * `typography`, `developer`) — tu mamy adapter, który mapuje to na
 * płaski `DeviceSettings`. Dzięki temu komponenty mają czysty model
 * bez wiedzy o wewnętrznym schemacie urządzenia.
 */

import { DEVICE_AP_BASE_URL } from "../../shared/config";
import {
  DEFAULT_SETTINGS,
  type Book,
  type DeviceApi,
  type DeviceSettings,
  type Language,
  type PauseBehaviour,
  type ReaderHand,
  type ReaderMode,
  type Theme,
  type Typeface,
  type FooterMetric,
  type BatteryLabel,
  type WifiStationConfig,
  type PluginInfo,
  type PluginFiles,
  type DeviceLogTail,
  type BookPosition,
  type DeviceCapabilities,
  type DeviceInfo,
  type BookTextPage,
  type ChapterMark,
  type PictureKind,
  type ReaderDeviceSettings,
  type ReaderOptions,
} from "./api";
import { tr } from "../i18n/index";

// Requests to the reader never hang: a phone that silently left the
// reader's Wi-Fi otherwise waits a minute or more before fetch gives up.
const TIMEOUT_MS = 12_000;
// Opening a book for the chapter editor can convert an EPUB on the reader.
const SLOW_TIMEOUT_MS = 120_000;

function timed(ms = TIMEOUT_MS): AbortSignal {
  return AbortSignal.timeout(ms);
}

// Order of the firmware's UiLanguage enum (Localization.h): the "language"
// setting is that index. The old order here (pl, en, de, ...) showed an
// English reader as Polish and switched a reader set to Polish in the app
// to English.
const LANG_INDEX: Language[] = ["en", "es", "fr", "de", "ro", "pl"];

interface FirmwareSettings {
  reading?: {
    wpm?: number;
    readerMode?: ReaderMode;
    pauseMode?: "sentence_end" | "instant";
    pacing?: { longWordMs?: number; complexWordMs?: number; punctuationMs?: number };
  };
  display?: {
    brightnessIndex?: number;
    darkMode?: boolean;
    nightMode?: boolean;
    handedness?: ReaderHand;
    readingBattery?: boolean;
    readingChapter?: boolean;
    readingProgress?: boolean;
    language?: number;
    phantomWords?: boolean;
    fontSizeIndex?: number;
    footerMetric?: FooterMetric;
    batteryLabel?: BatteryLabel;
    brightnessPercent?: number;
  };
  typography?: {
    typeface?: Typeface;
    typefaceIndex?: number;
    typefacesAvailable?: number[];
    focusHighlight?: boolean;
    tracking?: number;
    anchorPercent?: number;
    guideWidth?: number;
    guideGap?: number;
  };
  scroll?: {
    scrollFontSize?: number;
    scrollLineSpacing?: number;
    scrollMargin?: number;
  };
  developer?: { devMode?: boolean };
  device?: ReaderDeviceSettings;
  options?: ReaderOptions;
}

export function fromFirmware(fw: FirmwareSettings): DeviceSettings {
  const d = fw.display ?? {};
  const r = fw.reading ?? {};
  const p = r.pacing ?? {};
  const t = fw.typography ?? {};
  const sc = fw.scroll ?? {};
  const theme: Theme = d.nightMode ? "night" : d.darkMode ? "dark" : "light";
  const pause: PauseBehaviour = r.pauseMode === "instant" ? "auto" : "tap";
  const lang: Language = LANG_INDEX[d.language ?? 5] ?? "pl";
  // brightnessIndex w firmware to 0..N gdzie N to kMaxBrightness — skalujemy
  // przybliżenie do 0..100 dla UI.
  // Newer firmware keeps a smooth percent (20-100); older only the 5-step
  // index, scaled to 0-100 as before.
  const brightness =
    typeof d.brightnessPercent === "number"
      ? d.brightnessPercent
      : typeof d.brightnessIndex === "number"
        ? Math.min(100, Math.round((d.brightnessIndex / 4) * 100))
        : DEFAULT_SETTINGS.brightness;
  return {
    ...DEFAULT_SETTINGS,
    theme,
    brightness,
    language: lang,
    readerHand: d.handedness ?? DEFAULT_SETTINGS.readerHand,
    readerMode: r.readerMode ?? DEFAULT_SETTINGS.readerMode,
    pauseBehaviour: pause,
    baseWpm: r.wpm ?? DEFAULT_SETTINGS.baseWpm,
    longWordDelayMs: p.longWordMs ?? DEFAULT_SETTINGS.longWordDelayMs,
    complexWordDelayMs: p.complexWordMs ?? DEFAULT_SETTINGS.complexWordDelayMs,
    punctuationDelayMs: p.punctuationMs ?? DEFAULT_SETTINGS.punctuationDelayMs,
    showBatteryWhileReading: d.readingBattery ?? DEFAULT_SETTINGS.showBatteryWhileReading,
    showChapterWhileReading: d.readingChapter ?? DEFAULT_SETTINGS.showChapterWhileReading,
    showPercentWhileReading: d.readingProgress ?? DEFAULT_SETTINGS.showPercentWhileReading,
    devMode: fw.developer?.devMode ?? false,
    // Scroll settings
    scrollFontSize: sc.scrollFontSize ?? DEFAULT_SETTINGS.scrollFontSize,
    scrollLineSpacing: sc.scrollLineSpacing ?? DEFAULT_SETTINGS.scrollLineSpacing,
    scrollMargin: sc.scrollMargin ?? DEFAULT_SETTINGS.scrollMargin,
    // Typography (RSVP)
    // The reader counts 0 = large, 2 = small; the app shows S/M/L as 0/1/2.
    fontSizeIndex: typeof d.fontSizeIndex === "number" ? 2 - d.fontSizeIndex : DEFAULT_SETTINGS.fontSizeIndex,
    typeface: t.typeface ?? DEFAULT_SETTINGS.typeface,
    typefaceIndex:
      t.typefaceIndex ??
      Math.max(0, ["standard", "open_dyslexic", "atkinson"].indexOf(t.typeface ?? "standard")),
    typefacesAvailable: Array.isArray(t.typefacesAvailable) ? t.typefacesAvailable : null,
    phantomWords: d.phantomWords ?? DEFAULT_SETTINGS.phantomWords,
    focusHighlight: t.focusHighlight ?? DEFAULT_SETTINGS.focusHighlight,
    tracking: t.tracking ?? DEFAULT_SETTINGS.tracking,
    anchorPercent: t.anchorPercent ?? DEFAULT_SETTINGS.anchorPercent,
    guideWidth: t.guideWidth ?? DEFAULT_SETTINGS.guideWidth,
    guideGap: t.guideGap ?? DEFAULT_SETTINGS.guideGap,
    // HUD metrics
    footerMetric: d.footerMetric ?? DEFAULT_SETTINGS.footerMetric,
    batteryLabel: d.batteryLabel ?? DEFAULT_SETTINGS.batteryLabel,
    // Older firmware has neither: the app hides what it can't change.
    device: fw.device ?? null,
    options: fw.options ?? null,
  };
}

export function toFirmware(p: Partial<DeviceSettings>): Record<string, unknown> {
  // applySettingsJson w firmware czyta po nazwie klucza (nie po sekcji),
  // więc możemy spłaszczyć payload.
  const out: Record<string, unknown> = {};
  if (p.theme != null) {
    out.darkMode = p.theme === "dark" || p.theme === "night";
    out.nightMode = p.theme === "night";
  }
  if (p.brightness != null) {
    // Old firmware: 5-step index. New firmware reads the percent after the
    // index and keeps it (the reader's smooth slider value).
    out.brightnessIndex = Math.max(0, Math.min(4, Math.round((p.brightness / 100) * 4)));
    out.brightnessPercent = Math.max(10, Math.min(100, Math.round(p.brightness)));
  }
  if (p.language != null) {
    const idx = LANG_INDEX.indexOf(p.language);
    if (idx >= 0) out.language = idx;
  }
  if (p.readerHand != null) out.handedness = p.readerHand;
  if (p.readerMode != null) out.readerMode = p.readerMode;
  if (p.pauseBehaviour != null) {
    out.pauseMode = p.pauseBehaviour === "auto" ? "instant" : "sentence_end";
  }
  if (p.baseWpm != null) out.wpm = p.baseWpm;
  if (p.longWordDelayMs != null) out.longWordMs = p.longWordDelayMs;
  if (p.complexWordDelayMs != null) out.complexWordMs = p.complexWordDelayMs;
  if (p.punctuationDelayMs != null) out.punctuationMs = p.punctuationDelayMs;
  if (p.showBatteryWhileReading != null) out.readingBattery = p.showBatteryWhileReading;
  if (p.showChapterWhileReading != null) out.readingChapter = p.showChapterWhileReading;
  if (p.showPercentWhileReading != null) out.readingProgress = p.showPercentWhileReading;
  if (p.devMode != null) out.devMode = p.devMode;
  // Scroll settings
  if (p.scrollFontSize != null) out.scrollFontSize = p.scrollFontSize;
  if (p.scrollLineSpacing != null) out.scrollLineSpacing = p.scrollLineSpacing;
  if (p.scrollMargin != null) out.scrollMargin = p.scrollMargin;
  // Typography (RSVP)
  if (p.fontSizeIndex != null) out.fontSizeIndex = 2 - p.fontSizeIndex;
  if (p.typeface != null) out.typeface = p.typeface;
  if (p.typefaceIndex != null) out.typefaceIndex = p.typefaceIndex;
  if (p.phantomWords != null) out.phantomWords = p.phantomWords;
  if (p.focusHighlight != null) out.focusHighlight = p.focusHighlight;
  if (p.tracking != null) out.tracking = p.tracking;
  if (p.anchorPercent != null) out.anchorPercent = p.anchorPercent;
  if (p.guideWidth != null) out.guideWidth = p.guideWidth;
  if (p.guideGap != null) out.guideGap = p.guideGap;
  // HUD metrics
  if (p.footerMetric != null) out.footerMetric = p.footerMetric;
  if (p.batteryLabel != null) out.batteryLabel = p.batteryLabel;
  // Reader functions: same flat keys as the firmware's "device" section.
  if (p.device) Object.assign(out, p.device);
  return out;
}

export class HttpDeviceApi implements DeviceApi {
  constructor(private baseUrl: string = DEVICE_AP_BASE_URL) {}

  private url(path: string): string {
    return this.baseUrl.replace(/\/+$/, "") + path;
  }

  private async json<T>(res: Response): Promise<T> {
    if (!res.ok) {
      const text = await res.text().catch(() => "");
      throw new Error(tr("err.deviceRejected", { status: res.status, text }));
    }
    return (await res.json()) as T;
  }

  async listBooks(): Promise<Book[]> {
    const data = await this.json<{ books: Book[]; current?: string }>(
      await fetch(this.url("/api/books"), { signal: timed() }),
    );
    // "current" is the file the reader has open ("books/x.rsvp"); an EPUB's
    // entry carries the .epub name, so compare without the extension.
    const stem = (name: string) => name.replace(/\.[^./]+$/, "");
    const current = data.current ? stem(data.current) : "";
    return data.books.map((b) => (current && stem(b.name) === current ? { ...b, current: true } : b));
  }

  async uploadBook(
    file: Blob,
    name: string,
    category: "book" | "article" = "book",
    onProgress?: (loaded: number, total: number) => void,
  ): Promise<string> {
    const fd = new FormData();
    fd.append("file", file, name);
    // name+category jako query params — firmware (CompanionSyncManager::handleBookUpload)
    // czyta je przez server_.arg(), nie z multipart body.
    const query = `name=${encodeURIComponent(name)}&category=${encodeURIComponent(category)}`;
    // XHR for upload progress (fetch has none). No overall timeout: a big
    // book over the reader's WiFi takes a while; a stalled upload is caught
    // by the 60 s without progress below.
    const responseText = await new Promise<string>((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open("POST", this.url(`/api/books?${query}`));
      let stallTimer = setTimeout(() => xhr.abort(), 60_000);
      xhr.upload.onprogress = (e) => {
        clearTimeout(stallTimer);
        stallTimer = setTimeout(() => xhr.abort(), 60_000);
        if (e.lengthComputable && onProgress) onProgress(e.loaded, e.total);
      };
      xhr.onload = () => {
        clearTimeout(stallTimer);
        if (xhr.status >= 200 && xhr.status < 300) resolve(xhr.responseText);
        else reject(new Error(tr("err.uploadFailed", { status: xhr.status, text: xhr.responseText })));
      };
      xhr.onerror = () => {
        clearTimeout(stallTimer);
        reject(new Error(tr("err.uploadLost")));
      };
      xhr.onabort = () => reject(new Error(tr("err.uploadStalled")));
      xhr.send(fd);
    });
    // {"ok":true,"path":"/books/books/x.epub"} -> "books/x.epub"
    try {
      const path = (JSON.parse(responseText) as { path?: string }).path ?? "";
      if (path.startsWith("/books/")) return path.slice("/books/".length);
    } catch {
      /* older firmware: fall back to the requested name */
    }
    return `${category === "article" ? "articles" : "books"}/${name}`;
  }

  async deleteBook(name: string): Promise<void> {
    // Firmware rejestruje tylko dokładny path "/api/books" (HTTP_DELETE) —
    // nazwa musi iść jako ?name= query param, nie jako segment ścieżki
    // (ten wcześniej zawsze dawał 404).
    const res = await fetch(this.url(`/api/books?name=${encodeURIComponent(name)}`), {
      method: "DELETE",
    });
    if (!res.ok && res.status !== 404) {
      throw new Error(tr("err.deleteFailed", { status: res.status }));
    }
  }

  async getSettings(): Promise<DeviceSettings> {
    const fw = await this.json<FirmwareSettings>(await fetch(this.url("/api/settings"), { signal: timed() }));
    return fromFirmware(fw);
  }

  async putSettings(patch: Partial<DeviceSettings>): Promise<DeviceSettings> {
    const res = await fetch(this.url("/api/settings"), {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(toFirmware(patch)),
    });
    const fw = await this.json<FirmwareSettings>(res);
    return fromFirmware(fw);
  }

  /**
   * Multipart upload do `/api/ota`. Używamy XMLHttpRequest zamiast fetch,
   * bo fetch nie wystawia natywnego progress callbacka dla uploadu.
   * Urządzenie po sukcesie odpowiada `{"ok":true,"reboot":true}` i robi
   * `ESP.restart()` — następne komendy do `192.168.4.1` będą padać aż
   * wstanie z powrotem (~5–10 s) i klient ponownie podłączy się do AP.
   */
  async installOta(
    blob: Blob,
    onProgress?: (loaded: number, total: number) => void,
  ): Promise<void> {
    const fd = new FormData();
    fd.append("firmware", blob, "flower-firmware.bin");
    await new Promise<void>((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open("POST", this.url("/api/ota"));
      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable && onProgress) onProgress(e.loaded, e.total);
      };
      xhr.onload = () => {
        if (xhr.status >= 200 && xhr.status < 300) {
          resolve();
        } else {
          reject(new Error(tr("err.otaRejected", { status: xhr.status, text: xhr.responseText })));
        }
      };
      xhr.onerror = () => reject(new Error(tr("err.otaLost")));
      xhr.send(fd);
    });
  }

  async getWifiStation(): Promise<WifiStationConfig> {
    return this.json<WifiStationConfig>(await fetch(this.url("/api/wifi"), { signal: timed() }));
  }

  async setWifiStation(ssid: string, password: string): Promise<WifiStationConfig> {
    const res = await fetch(this.url("/api/wifi"), {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ ssid, password }),
    });
    return this.json<WifiStationConfig>(res);
  }

  async clearWifiStation(): Promise<WifiStationConfig> {
    return this.json<WifiStationConfig>(await fetch(this.url("/api/wifi"), { method: "DELETE" }));
  }

  async getRssFeeds(): Promise<string[]> {
    const data = await this.json<{ feeds: string[] }>(await fetch(this.url("/api/rss-feeds"), { signal: timed() }));
    return data.feeds;
  }

  async setRssFeeds(feeds: string[]): Promise<string[]> {
    const res = await fetch(this.url("/api/rss-feeds"), {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ feeds }),
    });
    const data = await this.json<{ feeds: string[] }>(res);
    return data.feeds;
  }

  async getPlugins(): Promise<PluginInfo[]> {
    const data = await this.json<{ plugins: PluginInfo[] }>(await fetch(this.url("/api/plugins"), { signal: timed() }));
    return data.plugins;
  }

  private async putPlugins(body: Record<string, unknown>): Promise<PluginInfo[]> {
    const res = await fetch(this.url("/api/plugins"), {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(body),
      signal: timed(),
    });
    return (await this.json<{ plugins: PluginInfo[] }>(res)).plugins;
  }

  setPluginActive(id: string, active: boolean): Promise<PluginInfo[]> {
    return this.putPlugins({ id, active });
  }

  setPluginOrder(ids: string[]): Promise<PluginInfo[]> {
    return this.putPlugins({ order: ids.join(",") });
  }

  async listPluginFiles(id: string): Promise<PluginFiles> {
    return this.json<PluginFiles>(
      await fetch(this.url(`/api/plugins/files?id=${encodeURIComponent(id)}`), { signal: timed() }),
    );
  }

  async getPluginFile(id: string, name: string): Promise<Blob> {
    // No short timeout: a long recording takes a while over the reader's Wi-Fi.
    const res = await fetch(
      this.url(`/api/plugins/file?id=${encodeURIComponent(id)}&name=${encodeURIComponent(name)}`),
    );
    if (!res.ok) await this.json(res);
    return res.blob();
  }

  async deletePluginFile(id: string, name: string): Promise<void> {
    await this.json(
      await fetch(this.url(`/api/plugins/file?id=${encodeURIComponent(id)}&name=${encodeURIComponent(name)}`), {
        method: "DELETE",
        signal: timed(),
      }),
    );
  }

  async setWifiTimeoutSeconds(seconds?: number): Promise<number> {
    const res = await fetch(this.url("/api/power/wifi-timeout"), {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(seconds == null ? {} : { timeout: seconds }),
    });
    const data = await this.json<{ timeoutSeconds: number }>(res);
    return data.timeoutSeconds;
  }

  async getCapabilities(): Promise<DeviceCapabilities> {
    const data = await this.json<{
      api: number;
      firmwareVersion: string;
      features: Omit<DeviceCapabilities, "api" | "firmwareVersion">;
    }>(await fetch(this.url("/api/capabilities"), { signal: timed() }));
    return { api: data.api, firmwareVersion: data.firmwareVersion, ...data.features };
  }

  async getDeviceInfo(): Promise<DeviceInfo> {
    const data = await this.json<{ info: DeviceInfo }>(await fetch(this.url("/api/state"), { signal: timed() }));
    return data.info;
  }

  async getLogTail(n = 50): Promise<DeviceLogTail> {
    return this.json<DeviceLogTail>(
      await fetch(this.url(`/api/log/tail?n=${encodeURIComponent(String(n))}`)),
    );
  }

  async clearLog(): Promise<void> {
    await this.json(await fetch(this.url("/api/log"), { method: "DELETE" }));
  }

  async getBookPosition(name: string): Promise<BookPosition> {
    return this.json<BookPosition>(
      await fetch(this.url(`/api/books/position?name=${encodeURIComponent(name)}`)),
    );
  }

  async setBookPosition(
    name: string,
    patch: { wordIndex?: number; wordCount?: number },
  ): Promise<BookPosition> {
    const res = await fetch(this.url(`/api/books/position?name=${encodeURIComponent(name)}`), {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(patch),
    });
    return this.json<BookPosition>(res);
  }

  async getBookText(name: string, from: number, count: number, words = 24): Promise<BookTextPage> {
    const query = `name=${encodeURIComponent(name)}&from=${from}&count=${count}&words=${words}`;
    const res = await fetch(this.url(`/api/books/text?${query}`), { signal: timed(SLOW_TIMEOUT_MS) });
    if (res.status === 404) {
      throw new Error(tr("err.noChapterEditor"));
    }
    return this.json<BookTextPage>(res);
  }

  async setBookChapters(name: string, chapters: ChapterMark[]): Promise<void> {
    // One "word<TAB>title" line per chapter: the reader parses it without JSON.
    const body = chapters
      .map((c) => `${Math.max(0, Math.floor(c.w))}\t${c.t.replace(/[\t\r\n]+/g, " ").trim()}`)
      .join("\n");
    const res = await fetch(this.url(`/api/books/chapters?name=${encodeURIComponent(name)}`), {
      method: "PUT",
      headers: { "content-type": "text/plain; charset=utf-8" },
      body,
      signal: timed(),
    });
    await this.json(res);
  }

  async resetBookChapters(name: string): Promise<void> {
    const res = await fetch(this.url(`/api/books/chapters?name=${encodeURIComponent(name)}`), {
      method: "DELETE",
      signal: timed(),
    });
    await this.json(res);
  }

  async getBookPicture(name: string, kind: PictureKind): Promise<Blob | null> {
    const res = await fetch(
      this.url(`/api/books/picture?name=${encodeURIComponent(name)}&kind=${kind}`),
      { signal: timed() },
    );
    if (res.status === 404) return null;
    if (!res.ok) throw new Error(tr("err.pictureGet", { status: res.status }));
    return res.blob();
  }

  async uploadBookPicture(name: string, kind: PictureKind, data: Blob): Promise<void> {
    const fd = new FormData();
    fd.append("picture", data, `${kind}.img`);
    const res = await fetch(
      this.url(`/api/books/picture?name=${encodeURIComponent(name)}&kind=${kind}`),
      { method: "POST", body: fd, signal: timed() },
    );
    if (res.ok) return;
    const text = await res.text().catch(() => "");
    if (res.status === 404) {
      throw new Error(
        text.includes("Book not found")
          ? tr("err.bookNotFound")
          : tr("err.noPictures"),
      );
    }
    throw new Error(tr("err.pictureRejected", { status: res.status, text }));
  }

  async deleteBookPicture(name: string, kind: PictureKind): Promise<void> {
    const res = await fetch(
      this.url(`/api/books/picture?name=${encodeURIComponent(name)}&kind=${kind}`),
      { method: "DELETE", signal: timed() },
    );
    await this.json(res);
  }
}

/** Lekki "ping" — sprawdza czy urządzenie jest pod baseUrl. */
export async function pingDevice(baseUrl: string = DEVICE_AP_BASE_URL): Promise<boolean> {
  try {
    const res = await fetch(`${baseUrl.replace(/\/+$/, "")}/api/hello`, {
      signal: AbortSignal.timeout(3000),
    });
    return res.ok;
  } catch {
    return false;
  }
}
