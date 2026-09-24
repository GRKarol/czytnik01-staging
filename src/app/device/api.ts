/**
 * Wyższego rzędu API urządzenia: biblioteka, ustawienia, plugins, dev mode.
 *
 * Pod spodem (faza 3) opakuje to WifiLink / BluetoothLink. Na razie
 * — żeby PWA się rozwijała równolegle z firmware — `DeviceApi.mock`
 * zwraca dane testowe i pamięta zmiany w `localStorage`, więc Karol
 * widzi pełen flow UI od razu.
 */

export type Theme = "light" | "dark" | "night";
export type Language = "pl" | "en" | "de" | "es" | "fr" | "it";
export type ReaderHand = "right" | "left";
export type ReaderMode = "rsvp" | "scroll";
export type PauseBehaviour = "tap" | "long-press" | "auto";
export type Typeface = "standard" | "open_dyslexic" | "atkinson";
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
  phantomWords: boolean;
  focusHighlight: boolean;
  tracking: number; // -2 to +3
  anchorPercent: number; // 30–40
  guideWidth: number; // 12–30
  guideGap: number; // 2–8
  // HUD metrics
  footerMetric: FooterMetric;
  batteryLabel: BatteryLabel;
}

export interface Book {
  name: string;
  title?: string;
  author?: string;
  bytes: number;
  progressPercent?: number;
  category?: "book" | "article";
  addedAt?: string;
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
  phantomWords: true,
  focusHighlight: true,
  tracking: 0,
  anchorPercent: 30,
  guideWidth: 30,
  guideGap: 5,
  // HUD metrics
  footerMetric: "percentage",
  batteryLabel: "percent",
};

export interface DeviceApi {
  listBooks(): Promise<Book[]>;
  uploadBook(file: Blob, name: string, category?: "book" | "article"): Promise<void>;
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
}

// ─── Mock implementation ────────────────────────────────────────────────────

const STORE_BOOKS = "flower.mock.books";
const STORE_SETTINGS = "flower.mock.settings";
const STORE_WIFI = "flower.mock.wifiStation";
const STORE_RSS = "flower.mock.rssFeeds";
const STORE_WIFI_TIMEOUT = "flower.mock.wifiTimeoutSeconds";
const STORE_POSITIONS = "flower.mock.bookPositions";

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
};

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
    return this.delay(read<Book[]>(STORE_BOOKS, MOCK_BOOKS_SEED));
  }

  async uploadBook(file: Blob, name: string, category: "book" | "article" = "book"): Promise<void> {
    const list = read<Book[]>(STORE_BOOKS, MOCK_BOOKS_SEED);
    const dir = category === "article" ? "articles" : "books";
    list.unshift({
      name: name.startsWith(`${dir}/`) ? name : `${dir}/${name}`,
      title: stripExt(name.replace(/^(books|articles)\//, "")),
      author: "",
      bytes: file.size,
      progressPercent: 0,
      category,
      addedAt: new Date().toISOString(),
    });
    write(STORE_BOOKS, list);
    await this.delay(undefined, 400);
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
      "Wgranie firmware'u wymaga połączenia z urządzeniem. Najpierw połącz się przez WiFi.",
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
  uploadBook: (f: Blob, n: string, c?: "book" | "article") => _api.uploadBook(f, n, c),
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
};

export function setDeviceApi(api: DeviceApi): void {
  _api = api;
  for (const l of _apiListeners) l(api);
}

export function onDeviceApiChange(handler: (api: DeviceApi) => void): () => void {
  _apiListeners.add(handler);
  return () => _apiListeners.delete(handler);
}
