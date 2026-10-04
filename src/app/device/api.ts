import { tr } from "../i18n/index";
/**
 * Wyższego rzędu API urządzenia: biblioteka, ustawienia, plugins, dev mode.
 *
 * Pod spodem (faza 3) opakuje to WifiLink / BluetoothLink. Na razie
 * — żeby PWA się rozwijała równolegle z firmware — `DeviceApi.mock`
 * zwraca dane testowe i pamięta zmiany w `localStorage`, więc Karol
 * widzi pełen flow UI od razu.
 */

export type Theme = "light" | "dark" | "night";
export type Language = "pl" | "en" | "de" | "es" | "fr" | "ro";
export type ReaderHand = "right" | "left";
export type ReaderMode = "rsvp" | "scroll";
export type PauseBehaviour = "tap" | "long-press" | "auto";
export type Typeface = "standard" | "open_dyslexic" | "atkinson";

/** Book faces the reader has, in its index order (DisplayManager::ReaderTypeface). */
export const TYPEFACE_NAMES = [
  "Standard",
  "OpenDyslexic",
  "Atkinson Hyperlegible",
  "Literata",
  "Merriweather",
  "Lora",
  "Bitter",
  "EB Garamond",
  "Vollkorn",
  "Gelasio",
  "PT Serif",
  "IBM Plex Serif",
  "Cardo",
  "Zilla Slab",
  "Old Standard",
  "Domine",
  "Alegreya",
  "Newsreader",
  "Noto Serif",
  "Spectral",
] as const;
export type FooterMetric = "percentage" | "chapter_time" | "book_time";
export type BatteryLabel = "percent" | "time_remaining" | "voltage";

export interface DeviceSettings {
  theme: Theme;
  brightness: number; // 0-100
  language: Language;
  readerHand: ReaderHand;
  readerMode: ReaderMode;
  pauseBehaviour: PauseBehaviour;
  baseWpm: number; // 10-1000
  longWordDelayMs: number;
  complexWordDelayMs: number;
  punctuationDelayMs: number;
  showBatteryWhileReading: boolean;
  showChapterWhileReading: boolean;
  showPercentWhileReading: boolean;
  devMode: boolean;
  scrollFontSize: number; // 0–8 (numeric scale, 0=tiny, 8=maximum)
  scrollLineSpacing: number; // 0–2 (Compact → Relaxed)
  scrollMargin: number; // 0–2 (Narrow → Wide)
  // Typography (RSVP)
  fontSizeIndex: number; // 0–2 (small/medium/large)
  typeface: Typeface;
  /** 0..19 index into TYPEFACE_NAMES (firmware with the full font list). */
  typefaceIndex: number;
  /**
   * Indices the reader can draw now (built-in faces + fonts on its SD card).
   * Null on firmware that doesn't report it: then every face is offered.
   */
  typefacesAvailable: number[] | null;
  phantomWords: boolean;
  focusHighlight: boolean;
  tracking: number; // -2 to +3
  anchorPercent: number; // 30–40
  guideWidth: number; // 12–30
  guideGap: number; // 2–8
  // HUD metrics
  footerMetric: FooterMetric;
  batteryLabel: BatteryLabel;
  /** Menu look, screensaver, battery, radios (firmware 0.4.03+), null before. */
  device: ReaderDeviceSettings | null;
  /** What the reader offers for those (its own colors, fonts, minutes). */
  options: ReaderOptions | null;
}

/** Same names as the firmware's "device" section (CompanionSyncManager::settingsJson). */
export interface ReaderDeviceSettings {
  /** App::ScreensaverMode value: 7 book, 8 words, 0 life, 2 maze, 3 voronoi, 9 waves, 6 screen off. */
  screensaverMode: number;
  /** Indexes into options.screensaverTimeoutMin / screensaverAutoOffMin / sleepGuardMin. */
  screensaverTimeout: number;
  screensaverAutoOff: number;
  sleepGuard: number;
  /** 0 icon + %, 1 number in icon, 2 number only, 3 icon only. */
  batteryStyle: number;
  /** Index into options.focusColors, 254 = own color (focusRgb). */
  focusColor: number;
  focusRgb: number;
  /** Index into options.palettes; 0 Classic follows the reading theme. */
  menuPalette: number;
  menuOwnAccent: boolean;
  /** 0 tabs left, 1 right, 2 icons left, 3 icons right. */
  menuLayout: number;
  /** Index into options.menuFonts, -1 = same as the book. */
  menuFont: number;
  /** 0 recently read, 1 title, 2 author, 3 progress. */
  librarySort: number;
  autoUpdate: boolean;
  bluetooth: boolean;
  helpHints: boolean;
  savePointNames: boolean;
}

export interface ReaderPalette {
  /** Name as the reader calls it ("Classic", "Dracula"...). */
  n: string;
  /** Background, foreground, accent (RGB565); absent for Classic. */
  c?: [number, number, number];
}

export interface ReaderOptions {
  screensaverModes: number[];
  screensaverTimeoutMin: number[];
  screensaverAutoOffMin: number[];
  sleepGuardMin: number[];
  focusColors: number[];
  palettes: ReaderPalette[];
  menuFonts: string[];
}

export const FOCUS_COLOR_CUSTOM = 254;

export interface Book {
  name: string;
  title?: string;
  author?: string;
  bytes: number;
  progressPercent?: number;
  category?: "book" | "article";
  addedAt?: string;
  /** Picture from this app shown as the book's cover / shelf spine. */
  hasCover?: boolean;
  hasSpine?: boolean;
  /** Chapters set in the app's chapter editor replace the detected ones. */
  customChapters?: boolean;
  /** The book open on the reader now. */
  current?: boolean;
}

export type PictureKind = "cover" | "spine";

/** Pixel size the reader stores pictures at (storage/BookExtras.h). */
export const PICTURE_SIZE: Record<PictureKind, { width: number; height: number }> = {
  cover: { width: 92, height: 116 },
  spine: { width: 36, height: 72 },
};

/** A paragraph of the book as the reader split it: first word, length, opening words. */
export interface BookParagraph {
  w: number;
  n: number;
  t: string;
}

/** Chapter start in the reader's own word numbering. */
export interface ChapterMark {
  w: number;
  t: string;
}

export interface BookTextPage {
  wordCount: number;
  paragraphCount: number;
  from: number;
  /** Chapters come from the app's editor (true) or from the book text. */
  custom: boolean;
  chapters: ChapterMark[];
  paragraphs: BookParagraph[];
}

export interface WifiStationConfig {
  configured: boolean;
  ssid: string;
  passwordSet: boolean;
}

export interface PluginInfo {
  id: string;
  name: string;
  installed: boolean;
  active: boolean;
  builtin?: boolean;
  requiresOta?: boolean;
}

export interface DeviceLogTail {
  total: number;
  lines: string[];
}

export interface BookPosition {
  name: string;
  wordIndex: number;
  wordCount: number;
  percent: number;
}

export interface DeviceCapabilities {
  api: number;
  firmwareVersion: string;
  settings: boolean;
  books: boolean;
  ota: boolean;
  pluginsList: boolean;
  pluginsRemove: boolean;
  pluginsInstallPackage: boolean;
  bluetoothTransfer: boolean;
  rss: boolean;
  focusTimer: boolean;
  wifiTimeout: boolean;
  /** Covers/spines from the app (firmware 0.3.62+). */
  bookPictures?: boolean;
  /** Chapter editor endpoints (firmware 0.3.62+). */
  chapterEditor?: boolean;
}

export interface DeviceInfo {
  name: string;
  mode: "station" | "access_point";
  baseUrl: string;
  networkSsid: string;
  firmwareVersion: string;
  batteryPercent: number;
  sdFreeKb: number;
  sdTotalKb: number;
}

export const DEFAULT_SETTINGS: DeviceSettings = {
  theme: "dark",
  brightness: 70,
  language: "pl",
  readerHand: "right",
  readerMode: "rsvp",
  pauseBehaviour: "tap",
  baseWpm: 300,
  longWordDelayMs: 150,
  complexWordDelayMs: 100,
  punctuationDelayMs: 200,
  showBatteryWhileReading: true,
  showChapterWhileReading: true,
  showPercentWhileReading: true,
  devMode: false,
  scrollFontSize: 4, // Medium (level 4 of 0-8)
  scrollLineSpacing: 1, // Normal
  scrollMargin: 1, // Normal
  // Typography (RSVP)
  fontSizeIndex: 0,
  typeface: "standard",
  typefaceIndex: 0,
  typefacesAvailable: null,
  phantomWords: true,
  focusHighlight: true,
  tracking: 0,
  anchorPercent: 30,
  guideWidth: 30,
  guideGap: 5,
  // HUD metrics
  footerMetric: "percentage",
  batteryLabel: "percent",
  device: {
    screensaverMode: 7,
    screensaverTimeout: 2,
    screensaverAutoOff: 0,
    sleepGuard: 0,
    batteryStyle: 0,
    focusColor: 1,
    focusRgb: 0x001f,
    menuPalette: 0,
    menuOwnAccent: false,
    menuLayout: 0,
    menuFont: -1,
    librarySort: 0,
    autoUpdate: true,
    bluetooth: false,
    helpHints: true,
    savePointNames: true,
  },
  options: {
    screensaverModes: [7, 8, 0, 2, 3, 9, 6],
    screensaverTimeoutMin: [1, 2, 3, 5, 10, 15, 20, 30],
    screensaverAutoOffMin: [0, 5, 10, 15, 20, 30, 45, 60],
    sleepGuardMin: [0, 5, 10, 15, 20, 30, 45, 60],
    focusColors: [0xf800, 0x001f, 0x07e0, 0xffe0, 0xfd20, 0xa01f],
    palettes: [
      { n: "Classic" },
      { n: "Mocha", c: [0x18e5, 0xcebe, 0xf455] },
      { n: "Dracula", c: [0x2946, 0xffde, 0xfbd8] },
      { n: "Nord", c: [0x29a8, 0xef7e, 0x8e1a] },
      { n: "Latte", c: [0xef9e, 0x4a6d, 0xd067] },
      { n: "Sepia", c: [0xf77b, 0x3965, 0xb2a5] },
      { n: "Forest", c: [0x1924, 0xdf3b, 0x7e2f] },
    ],
    menuFonts: ["Inter", "Noto Sans", "Atkinson", "Nunito", "Literata", "OpenDyslexic"],
  },
};

export interface DeviceApi {
  listBooks(): Promise<Book[]>;
  /**
   * Sends a book; resolves with its library name on the reader ("books/x.epub"),
   * which can differ from `name` (the reader replaces unsafe characters).
   */
  uploadBook(
    file: Blob,
    name: string,
    category?: "book" | "article",
    onProgress?: (loaded: number, total: number) => void,
  ): Promise<string | void>;
  deleteBook(name: string): Promise<void>;
  getSettings(): Promise<DeviceSettings>;
  putSettings(patch: Partial<DeviceSettings>): Promise<DeviceSettings>;
  /**
   * Wysyła firmware (.bin) na urządzenie i instaluje przez OTA. Po sukcesie
   * urządzenie się restartuje, więc Promise resolve'uje TUŻ przed restartem
   * — connection zaraz potem padnie. Mock no-op (rzuca błąd).
   */
  installOta(blob: Blob, onProgress?: (loaded: number, total: number) => void): Promise<void>;

  /** Stacja WiFi — pozwala czytnikowi łączyć się z domowym WiFi zamiast tylko trybu AP. */
  getWifiStation(): Promise<WifiStationConfig>;
  setWifiStation(ssid: string, password: string): Promise<WifiStationConfig>;
  clearWifiStation(): Promise<WifiStationConfig>;

  getRssFeeds(): Promise<string[]>;
  setRssFeeds(feeds: string[]): Promise<string[]>;

  getPlugins(): Promise<PluginInfo[]>;

  /**
   * 0 = nigdy nie wyłączaj WiFi/AP automatycznie. Wywołane bez argumentu
   * tylko odczytuje aktualną wartość (firmware nie ma osobnego GET-a —
   * POST z pustym body zwraca stan bez zmiany).
   */
  setWifiTimeoutSeconds(seconds?: number): Promise<number>;

  getCapabilities(): Promise<DeviceCapabilities>;
  getDeviceInfo(): Promise<DeviceInfo>;

  getLogTail(n?: number): Promise<DeviceLogTail>;
  clearLog(): Promise<void>;

  getBookPosition(name: string): Promise<BookPosition>;
  setBookPosition(
    name: string,
    patch: { wordIndex?: number; wordCount?: number },
  ): Promise<BookPosition>;

  /** `count` paragraphs from paragraph `from`, each with its first `words` words. */
  getBookText(name: string, from: number, count: number, words?: number): Promise<BookTextPage>;
  setBookChapters(name: string, chapters: ChapterMark[]): Promise<void>;
  /** Back to the chapters the reader finds in the text. */
  resetBookChapters(name: string): Promise<void>;
  /** Raw "FBI1" picture file, null when the book has none. */
  getBookPicture(name: string, kind: PictureKind): Promise<Blob | null>;
  uploadBookPicture(name: string, kind: PictureKind, data: Blob): Promise<void>;
  deleteBookPicture(name: string, kind: PictureKind): Promise<void>;
}

// ─── Mock implementation ────────────────────────────────────────────────────

const STORE_BOOKS = "flower.mock.books";
const STORE_SETTINGS = "flower.mock.settings";
const STORE_WIFI = "flower.mock.wifiStation";
const STORE_RSS = "flower.mock.rssFeeds";
const STORE_WIFI_TIMEOUT = "flower.mock.wifiTimeoutSeconds";
const STORE_POSITIONS = "flower.mock.bookPositions";
const STORE_EXTRAS = "flower.mock.bookExtras";

interface MockExtras {
  cover?: string; // base64 FBI1
  spine?: string;
  chapters?: ChapterMark[];
}

const EMPTY_WIFI: WifiStationConfig = { configured: false, ssid: "", passwordSet: false };

const MOCK_PLUGINS: PluginInfo[] = [
  { id: "dictaphone", name: "Dyktafon", installed: true, active: false, builtin: true },
  { id: "focus-timer", name: "Klepsydra", installed: true, active: true, builtin: true },
  { id: "rss", name: "RSS", installed: true, active: true, builtin: true },
  { id: "night-reading", name: "Tryb nocnego czytania", installed: true, active: false, builtin: true },
  { id: "page-counter", name: "Licznik stron", installed: true, active: false, builtin: true },
  { id: "quote-highlight", name: "Cytaty", installed: true, active: false, builtin: true },
  { id: "reading-stats", name: "Statystyki czytania", installed: true, active: false, builtin: true },
  { id: "notes-sync", name: "Notatki", installed: true, active: false, builtin: true },
];

const MOCK_CAPABILITIES: DeviceCapabilities = {
  api: 1,
  firmwareVersion: "mock",
  settings: true,
  books: true,
  ota: true,
  pluginsList: true,
  pluginsRemove: true,
  pluginsInstallPackage: false,
  bluetoothTransfer: false,
  rss: true,
  focusTimer: true,
  wifiTimeout: true,
  bookPictures: true,
  chapterEditor: true,
};

// Sample text for the chapter editor without a reader: headings between
// paragraphs, the way converted books look.
const MOCK_PARAGRAPHS: string[] = (() => {
  const out: string[] = [];
  const body =
    "Wiatr od rzeki niósł zapach mokrej trawy i dymu z odległych ognisk. Szli wolno, bo droga po deszczu zamieniła się w błoto, a nikt nie chciał zgubić butów przed zmrokiem.";
  for (let c = 1; c <= 12; c++) {
    out.push(`Rozdział ${c}`);
    for (let p = 0; p < 9; p++) out.push(`${body} (${c}.${p + 1})`);
  }
  return out;
})();

function mockText(): { paragraphs: BookParagraph[]; wordCount: number } {
  const paragraphs: BookParagraph[] = [];
  let w = 0;
  for (const text of MOCK_PARAGRAPHS) {
    const words = text.split(/\s+/);
    paragraphs.push({ w, n: words.length, t: words.slice(0, 24).join(" ") });
    w += words.length;
  }
  return { paragraphs, wordCount: w };
}

function read<T>(key: string, fallback: T): T {
  try {
    const raw = localStorage.getItem(key);
    return raw ? (JSON.parse(raw) as T) : fallback;
  } catch {
    return fallback;
  }
}

function write<T>(key: string, value: T): void {
  try {
    localStorage.setItem(key, JSON.stringify(value));
  } catch {
    /* ignored */
  }
}

const MOCK_BOOKS_SEED: Book[] = [
  {
    name: "books/sample-rozdzialy.rsvp",
    title: "Mały próbnik",
    author: "Anonim",
    bytes: 12480,
    progressPercent: 42,
    category: "book",
    addedAt: "2026-05-22T10:14:00Z",
  },
  {
    name: "books/krotki-tekst.rsvp",
    title: "Krótki tekst",
    author: "",
    bytes: 3120,
    progressPercent: 100,
    category: "book",
    addedAt: "2026-05-18T19:02:00Z",
  },
  {
    name: "articles/poniedzialkowy-newsletter.rsvp",
    title: "Poniedziałkowy newsletter",
    author: "Redakcja",
    bytes: 18420,
    progressPercent: 0,
    category: "article",
    addedAt: "2026-05-24T07:30:00Z",
  },
];

export class MockDeviceApi implements DeviceApi {
  private async delay<T>(value: T, ms = 200): Promise<T> {
    await new Promise((r) => setTimeout(r, ms));
    return value;
  }

  async listBooks(): Promise<Book[]> {
    const books = read<Book[]>(STORE_BOOKS, MOCK_BOOKS_SEED);
    return this.delay(books.map((b, i) => ({ ...b, current: i === 0 })));
  }

  async uploadBook(file: Blob, name: string, category: "book" | "article" = "book"): Promise<string> {
    const list = read<Book[]>(STORE_BOOKS, MOCK_BOOKS_SEED);
    const dir = category === "article" ? "articles" : "books";
    const stored = name.startsWith(`${dir}/`) ? name : `${dir}/${name}`;
    list.unshift({
      name: stored,
      title: stripExt(name.replace(/^(books|articles)\//, "")),
      author: "",
      bytes: file.size,
      progressPercent: 0,
      category,
      addedAt: new Date().toISOString(),
    });
    write(STORE_BOOKS, list);
    await this.delay(undefined, 400);
    return stored;
  }

  async deleteBook(name: string): Promise<void> {
    const list = read<Book[]>(STORE_BOOKS, MOCK_BOOKS_SEED);
    write(
      STORE_BOOKS,
      list.filter((b) => b.name !== name),
    );
    await this.delay(undefined, 200);
  }

  async getSettings(): Promise<DeviceSettings> {
    return this.delay(read<DeviceSettings>(STORE_SETTINGS, DEFAULT_SETTINGS));
  }

  async putSettings(patch: Partial<DeviceSettings>): Promise<DeviceSettings> {
    const current = read<DeviceSettings>(STORE_SETTINGS, DEFAULT_SETTINGS);
    const next = { ...current, ...patch };
    write(STORE_SETTINGS, next);
    return this.delay(next, 150);
  }

  async installOta(): Promise<void> {
    throw new Error(
      tr("err.otaNeedsDevice"),
    );
  }

  async getWifiStation(): Promise<WifiStationConfig> {
    return this.delay(read<WifiStationConfig>(STORE_WIFI, EMPTY_WIFI));
  }

  async setWifiStation(ssid: string, password: string): Promise<WifiStationConfig> {
    const next: WifiStationConfig = { configured: true, ssid, passwordSet: password.length > 0 };
    write(STORE_WIFI, next);
    return this.delay(next, 200);
  }

  async clearWifiStation(): Promise<WifiStationConfig> {
    write(STORE_WIFI, EMPTY_WIFI);
    return this.delay(EMPTY_WIFI, 150);
  }

  async getRssFeeds(): Promise<string[]> {
    return this.delay(read<string[]>(STORE_RSS, []));
  }

  async setRssFeeds(feeds: string[]): Promise<string[]> {
    write(STORE_RSS, feeds);
    return this.delay(feeds, 150);
  }

  async getPlugins(): Promise<PluginInfo[]> {
    return this.delay(MOCK_PLUGINS, 150);
  }

  async setWifiTimeoutSeconds(seconds?: number): Promise<number> {
    if (seconds == null) {
      return this.delay(read<number>(STORE_WIFI_TIMEOUT, 0), 100);
    }
    write(STORE_WIFI_TIMEOUT, seconds);
    return this.delay(seconds, 150);
  }

  async getCapabilities(): Promise<DeviceCapabilities> {
    return this.delay(MOCK_CAPABILITIES, 150);
  }

  async getDeviceInfo(): Promise<DeviceInfo> {
    return this.delay(
      {
        name: "Flower",
        mode: "access_point",
        baseUrl: "http://192.168.4.1",
        networkSsid: "Flower-MOCK",
        firmwareVersion: "mock",
        batteryPercent: 100,
        sdFreeKb: 1_000_000,
        sdTotalKb: 8_000_000,
      },
      150,
    );
  }

  async getLogTail(): Promise<DeviceLogTail> {
    return this.delay({ total: 0, lines: [] }, 150);
  }

  async clearLog(): Promise<void> {
    await this.delay(undefined, 100);
  }

  private extras(name: string): MockExtras {
    return read<Record<string, MockExtras>>(STORE_EXTRAS, {})[name] ?? {};
  }

  private setExtras(name: string, patch: Partial<MockExtras>): void {
    const all = read<Record<string, MockExtras>>(STORE_EXTRAS, {});
    all[name] = { ...(all[name] ?? {}), ...patch };
    write(STORE_EXTRAS, all);
  }

  async getBookText(name: string, from: number, count: number, words = 24): Promise<BookTextPage> {
    const { paragraphs, wordCount } = mockText();
    const own = this.extras(name).chapters;
    const detected = paragraphs
      .filter((p) => /^Rozdział \d+$/.test(p.t))
      .map((p) => ({ w: p.w, t: p.t }));
    return this.delay(
      {
        wordCount,
        paragraphCount: paragraphs.length,
        from,
        custom: own != null,
        chapters: own ?? detected,
        paragraphs: paragraphs
          .slice(from, from + count)
          .map((p) => ({ ...p, t: p.t.split(" ").slice(0, words).join(" ") })),
      },
      250,
    );
  }

  async setBookChapters(name: string, chapters: ChapterMark[]): Promise<void> {
    this.setExtras(name, { chapters });
    this.flagBook(name);
    await this.delay(undefined, 200);
  }

  async resetBookChapters(name: string): Promise<void> {
    this.setExtras(name, { chapters: undefined });
    this.flagBook(name);
    await this.delay(undefined, 150);
  }

  async getBookPicture(name: string, kind: PictureKind): Promise<Blob | null> {
    const data = this.extras(name)[kind];
    if (!data) return this.delay(null, 100);
    const bytes = Uint8Array.from(atob(data), (c) => c.charCodeAt(0));
    return this.delay(new Blob([bytes]), 100);
  }

  async uploadBookPicture(name: string, kind: PictureKind, data: Blob): Promise<void> {
    const bytes = new Uint8Array(await data.arrayBuffer());
    let binary = "";
    for (const b of bytes) binary += String.fromCharCode(b);
    this.setExtras(name, { [kind]: btoa(binary) });
    this.flagBook(name);
    await this.delay(undefined, 300);
  }

  async deleteBookPicture(name: string, kind: PictureKind): Promise<void> {
    this.setExtras(name, { [kind]: undefined });
    this.flagBook(name);
    await this.delay(undefined, 150);
  }

  // Keeps hasCover/hasSpine/customChapters on the mock list in step.
  private flagBook(name: string): void {
    const extras = this.extras(name);
    const list = read<Book[]>(STORE_BOOKS, MOCK_BOOKS_SEED).map((b) =>
      b.name === name
        ? {
            ...b,
            hasCover: !!extras.cover,
            hasSpine: !!extras.spine,
            customChapters: extras.chapters != null,
          }
        : b,
    );
    write(STORE_BOOKS, list);
  }

  async getBookPosition(name: string): Promise<BookPosition> {
    const store = read<Record<string, BookPosition>>(STORE_POSITIONS, {});
    return this.delay(
      store[name] ?? { name, wordIndex: 0, wordCount: 0, percent: 0 },
      120,
    );
  }

  async setBookPosition(
    name: string,
    patch: { wordIndex?: number; wordCount?: number },
  ): Promise<BookPosition> {
    const store = read<Record<string, BookPosition>>(STORE_POSITIONS, {});
    const current = store[name] ?? { name, wordIndex: 0, wordCount: 0, percent: 0 };
    const next: BookPosition = {
      name,
      wordIndex: patch.wordIndex ?? current.wordIndex,
      wordCount: patch.wordCount ?? current.wordCount,
      percent: 0,
    };
    next.percent = next.wordCount > 0 ? Math.min(100, Math.round((next.wordIndex / next.wordCount) * 100)) : 0;
    store[name] = next;
    write(STORE_POSITIONS, store);
    return this.delay(next, 120);
  }
}

function stripExt(name: string): string {
  return name.replace(/\.[^.]+$/, "");
}

// ─── Reactive API selection ─────────────────────────────────────────────────
//
// `deviceApi` to ruchomy wskaźnik. Domyślnie wskazuje na mocka (PWA
// działa nawet bez urządzenia). Kiedy klient połączy się przez WiFi,
// app.element.ts robi `setDeviceApi(new HttpDeviceApi(...))`.
//
// Komponenty subskrybują zmianę przez `onDeviceApiChange()` — kiedy
// API się przełączy, są informowane żeby odświeżyć dane.

let _api: DeviceApi = new MockDeviceApi();
const _apiListeners = new Set<(api: DeviceApi) => void>();

export const deviceApi = {
  get current(): DeviceApi {
    return _api;
  },
  listBooks: () => _api.listBooks(),
  uploadBook: (
    f: Blob,
    n: string,
    c?: "book" | "article",
    onProgress?: (loaded: number, total: number) => void,
  ) => _api.uploadBook(f, n, c, onProgress),
  deleteBook: (n: string) => _api.deleteBook(n),
  getSettings: () => _api.getSettings(),
  putSettings: (p: Partial<DeviceSettings>) => _api.putSettings(p),
  installOta: (b: Blob, onProgress?: (loaded: number, total: number) => void) =>
    _api.installOta(b, onProgress),
  getWifiStation: () => _api.getWifiStation(),
  setWifiStation: (ssid: string, password: string) => _api.setWifiStation(ssid, password),
  clearWifiStation: () => _api.clearWifiStation(),
  getRssFeeds: () => _api.getRssFeeds(),
  setRssFeeds: (feeds: string[]) => _api.setRssFeeds(feeds),
  getPlugins: () => _api.getPlugins(),
  setWifiTimeoutSeconds: (seconds?: number) => _api.setWifiTimeoutSeconds(seconds),
  getCapabilities: () => _api.getCapabilities(),
  getDeviceInfo: () => _api.getDeviceInfo(),
  getLogTail: (n?: number) => _api.getLogTail(n),
  clearLog: () => _api.clearLog(),
  getBookPosition: (name: string) => _api.getBookPosition(name),
  setBookPosition: (name: string, patch: { wordIndex?: number; wordCount?: number }) =>
    _api.setBookPosition(name, patch),
  getBookText: (name: string, from: number, count: number, words?: number) =>
    _api.getBookText(name, from, count, words),
  setBookChapters: (name: string, chapters: ChapterMark[]) => _api.setBookChapters(name, chapters),
  resetBookChapters: (name: string) => _api.resetBookChapters(name),
  getBookPicture: (name: string, kind: PictureKind) => _api.getBookPicture(name, kind),
  uploadBookPicture: (name: string, kind: PictureKind, data: Blob) =>
    _api.uploadBookPicture(name, kind, data),
  deleteBookPicture: (name: string, kind: PictureKind) => _api.deleteBookPicture(name, kind),
};

export function setDeviceApi(api: DeviceApi): void {
  _api = api;
  for (const l of _apiListeners) l(api);
}

export function onDeviceApiChange(handler: (api: DeviceApi) => void): () => void {
  _apiListeners.add(handler);
  return () => _apiListeners.delete(handler);
}
