import { tr } from "../i18n/index";
import type {
  Book,
  BookPosition,
  BookTextPage,
  DeviceApi,
  DeviceCapabilities,
  DeviceInfo,
  DeviceLogTail,
  DeviceSettings,
  PictureKind,
  PluginFiles,
  PluginInfo,
  WifiStationConfig,
} from "./api";

/**
 * The reader as the app last saw it. HttpDeviceApi writes here whatever it
 * reads (books, settings, battery, covers); without a reader the app shows
 * this, read-only, instead of a sample library. Connecting again replaces
 * it with the reader's state before anything can be changed.
 */
const STORE = "flower.readerSnapshot.v1";
// Pictures are ~28 KB each in base64; past this many the oldest go.
const MAX_PICTURES = 80;

interface Snapshot {
  savedAt: string;
  books?: Book[];
  settings?: DeviceSettings;
  info?: DeviceInfo;
  caps?: DeviceCapabilities;
  plugins?: PluginInfo[];
  wifi?: WifiStationConfig;
  /** "cover:books/x.rsvp" -> base64 of the reader's FBI1 file. */
  pictures?: Record<string, string>;
}

let cache: Snapshot | null | undefined;

function load(): Snapshot | null {
  if (cache !== undefined) return cache;
  try {
    const raw = localStorage.getItem(STORE);
    cache = raw ? (JSON.parse(raw) as Snapshot) : null;
  } catch {
    cache = null;
  }
  return cache;
}

function update(patch: Partial<Snapshot>): void {
  const next: Snapshot = { ...(load() ?? {}), ...patch, savedAt: new Date().toISOString() };
  cache = next;
  try {
    localStorage.setItem(STORE, JSON.stringify(next));
  } catch {
    // Full storage: drop the pictures, keep the rest.
    try {
      next.pictures = {};
      localStorage.setItem(STORE, JSON.stringify(next));
    } catch {
      /* ignored */
    }
  }
}

export function hasSnapshot(): boolean {
  return !!load()?.books;
}

/** When the reader was last seen, for the "last state" notes. */
export function snapshotTime(): Date | null {
  const at = load()?.savedAt;
  return at ? new Date(at) : null;
}

export function snapshotTimeLabel(): string {
  const at = snapshotTime();
  if (!at) return "";
  return at.toLocaleString(undefined, { day: "numeric", month: "numeric", hour: "2-digit", minute: "2-digit" });
}

export function rememberBooks(books: Book[]): void {
  const names = new Set(books.map((b) => b.name));
  const pictures = Object.fromEntries(
    Object.entries(load()?.pictures ?? {}).filter(([key]) => names.has(key.slice(key.indexOf(":") + 1))),
  );
  update({ books, pictures });
}

export function rememberSettings(settings: DeviceSettings): void {
  update({ settings });
}

export function rememberInfo(info: DeviceInfo): void {
  update({ info });
}

export function rememberCaps(caps: DeviceCapabilities): void {
  update({ caps });
}

export function rememberPlugins(plugins: PluginInfo[]): void {
  update({ plugins });
}

export function rememberWifi(wifi: WifiStationConfig): void {
  update({ wifi });
}

export async function rememberPicture(name: string, kind: PictureKind, data: Blob | null): Promise<void> {
  const key = `${kind}:${name}`;
  const pictures = { ...(load()?.pictures ?? {}) };
  if (!data) {
    if (!(key in pictures)) return;
    delete pictures[key];
  } else {
    const bytes = new Uint8Array(await data.arrayBuffer());
    let binary = "";
    for (let i = 0; i < bytes.length; i += 0x8000) {
      binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
    }
    delete pictures[key];
    pictures[key] = btoa(binary);
    const keys = Object.keys(pictures);
    for (const old of keys.slice(0, Math.max(0, keys.length - MAX_PICTURES))) delete pictures[old];
  }
  update({ pictures });
}

/** Thrown by every change while the reader is away. */
export class ReaderOfflineError extends Error {
  constructor() {
    super(tr("offline.readOnly"));
    this.name = "ReaderOfflineError";
  }
}

function readOnly(): never {
  throw new ReaderOfflineError();
}

/** The last reader state, read-only. Empty until the app has met a reader. */
export class SnapshotDeviceApi implements DeviceApi {
  // Passed in by api.ts: importing its values here would make the two
  // modules load each other.
  constructor(private readonly defaults: DeviceSettings) {}

  async listBooks(): Promise<Book[]> {
    return load()?.books ?? [];
  }

  // Before the first connection: the reader's defaults, read-only as well.
  async getSettings(): Promise<DeviceSettings> {
    return load()?.settings ?? this.defaults;
  }

  async getDeviceInfo(): Promise<DeviceInfo> {
    const info = load()?.info;
    if (!info) throw new ReaderOfflineError();
    return info;
  }

  async getCapabilities(): Promise<DeviceCapabilities> {
    const caps = load()?.caps;
    if (!caps) throw new ReaderOfflineError();
    return caps;
  }

  async getPlugins(): Promise<PluginInfo[]> {
    return load()?.plugins ?? [];
  }

  async getWifiStation(): Promise<WifiStationConfig> {
    return load()?.wifi ?? { configured: false, ssid: "", passwordSet: false };
  }

  async getBookPicture(name: string, kind: PictureKind): Promise<Blob | null> {
    const data = load()?.pictures?.[`${kind}:${name}`];
    if (!data) return null;
    return new Blob([Uint8Array.from(atob(data), (c) => c.charCodeAt(0))]);
  }

  async getRssFeeds(): Promise<string[]> {
    return [];
  }

  async listPluginFiles(): Promise<PluginFiles> {
    return { mime: "", files: [] };
  }

  async getLogTail(): Promise<DeviceLogTail> {
    return { total: 0, lines: [] };
  }

  async setWifiTimeoutSeconds(seconds?: number): Promise<number> {
    if (seconds != null) readOnly();
    return 0;
  }

  async getBookPosition(): Promise<BookPosition> {
    return readOnly();
  }

  async getBookText(): Promise<BookTextPage> {
    return readOnly();
  }

  async getPluginFile(): Promise<Blob> {
    return readOnly();
  }

  async uploadBook(): Promise<string> {
    return readOnly();
  }

  async deleteBook(): Promise<void> {
    readOnly();
  }

  async putSettings(): Promise<DeviceSettings> {
    return readOnly();
  }

  async installOta(): Promise<void> {
    readOnly();
  }

  async setWifiStation(): Promise<WifiStationConfig> {
    return readOnly();
  }

  async clearWifiStation(): Promise<WifiStationConfig> {
    return readOnly();
  }

  async setRssFeeds(): Promise<string[]> {
    return readOnly();
  }

  async setPluginActive(): Promise<PluginInfo[]> {
    return readOnly();
  }

  async setPluginOrder(): Promise<PluginInfo[]> {
    return readOnly();
  }

  async deletePluginFile(): Promise<void> {
    readOnly();
  }

  async clearLog(): Promise<void> {
    readOnly();
  }

  async setBookPosition(): Promise<BookPosition> {
    return readOnly();
  }

  async setBookChapters(): Promise<void> {
    readOnly();
  }

  async resetBookChapters(): Promise<void> {
    readOnly();
  }

  async setBookNames(): Promise<void> {
    readOnly();
  }

  async resetBookNames(): Promise<void> {
    readOnly();
  }

  async uploadBookPicture(): Promise<void> {
    readOnly();
  }

  async deleteBookPicture(): Promise<void> {
    readOnly();
  }
}
