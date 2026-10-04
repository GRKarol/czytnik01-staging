import { CapacitorHttp } from "@capacitor/core";
import { OTA_RELEASES_API } from "../../shared/config";
import { isNativeApp, withInternet } from "../device/network-pin";
import { tr } from "../i18n/index";

export interface ReleaseAsset {
  name: string;
  size: number;
  downloadUrl: string;
  contentType: string;
}

export interface ReleaseInfo {
  tag: string;
  name: string;
  publishedAt: string;
  body: string;
  htmlUrl: string;
  isPrerelease: boolean;
  assets: ReleaseAsset[];
}

interface GhAsset {
  name: string;
  size: number;
  browser_download_url: string;
  content_type: string;
}

interface GhRelease {
  tag_name: string;
  name: string | null;
  published_at: string;
  body: string | null;
  html_url: string;
  prerelease: boolean;
  draft: boolean;
  assets: GhAsset[];
}

/**
 * Pobiera ostatni release z GitHub (publiczny endpoint, bez tokena).
 * Nie wszystkie repo mają release'y — wtedy 404 i zwracamy `null`.
 */
export async function fetchLatestRelease(): Promise<ReleaseInfo | null> {
  // The app may be pinned to the reader's network (no internet there).
  const res = await withInternet(() =>
    fetch(OTA_RELEASES_API, {
      headers: { accept: "application/vnd.github+json" },
    }),
  );
  if (res.status === 404) return null;
  if (!res.ok) {
    throw new Error(tr("err.github", { status: res.status }));
  }
  const data = (await res.json()) as GhRelease;
  return toReleaseInfo(data);
}

function toReleaseInfo(r: GhRelease): ReleaseInfo {
  return {
    tag: r.tag_name,
    name: r.name ?? r.tag_name,
    publishedAt: r.published_at,
    body: r.body ?? "",
    htmlUrl: r.html_url,
    isPrerelease: r.prerelease,
    assets: r.assets.map((a) => ({
      name: a.name,
      size: a.size,
      downloadUrl: a.browser_download_url,
      contentType: a.content_type,
    })),
  };
}

/**
 * Heurystyka: który asset to firmware .bin do OTA?
 *
 * Preferujemy `flower-firmware.bin` (sama aplikacja, pasuje do Update.h
 * w firmware). `czytnik01.bin` to scalona binarka — działa tylko dla
 * web flashera, dla OTA przez WiFi by uszkodziła flash.
 */
export function pickFirmwareAsset(release: ReleaseInfo): ReleaseAsset | null {
  return (
    release.assets.find((a) => /flower-firmware.*\.bin$/i.test(a.name)) ??
    release.assets.find((a) => /\.bin$/i.test(a.name) && !/czytnik01/i.test(a.name)) ??
    release.assets.find((a) => /\.bin$/i.test(a.name)) ??
    release.assets.find((a) => a.contentType === "application/octet-stream") ??
    null
  );
}

/** Pobiera .bin z release'a i zwraca jako Blob (do dalszego POST-a na urządzenie). */
export async function downloadAsset(
  asset: ReleaseAsset,
  onProgress?: (loaded: number, total: number) => void,
): Promise<Blob> {
  return withInternet(() => readAsset(asset, onProgress));
}

async function readAsset(
  asset: ReleaseAsset,
  onProgress?: (loaded: number, total: number) => void,
): Promise<Blob> {
  // github.com/.../releases/download answers without CORS headers (and so
  // does the storage host it redirects to), so fetch() from the app's
  // WebView fails with "Failed to fetch". The native HTTP stack has no
  // CORS; it just gives no progress, so the bar jumps to 100% at the end.
  if (isNativeApp()) {
    onProgress?.(0, asset.size);
    const res = await CapacitorHttp.get({
      url: asset.downloadUrl,
      responseType: "blob",
      connectTimeout: 20_000,
      readTimeout: 60_000,
    });
    if (res.status < 200 || res.status >= 300) {
      throw new Error(tr("err.assetDownload", { name: asset.name, status: res.status }));
    }
    const bytes = base64ToBytes(String(res.data));
    onProgress?.(bytes.byteLength, bytes.byteLength);
    return new Blob([bytes as BlobPart], { type: asset.contentType });
  }

  const res = await fetch(asset.downloadUrl);
  if (!res.ok || !res.body) {
    throw new Error(tr("err.assetDownload", { name: asset.name, status: res.status }));
  }
  if (!onProgress) return res.blob();

  const total = Number(res.headers.get("content-length") ?? asset.size);
  const reader = res.body.getReader();
  const chunks: Uint8Array[] = [];
  let loaded = 0;
  while (true) {
    const { value, done } = await reader.read();
    if (done) break;
    if (value) {
      chunks.push(value);
      loaded += value.byteLength;
      onProgress(loaded, total);
    }
  }
  return new Blob(chunks as BlobPart[], { type: asset.contentType });
}

function base64ToBytes(data: string): Uint8Array {
  const binary = atob(data.replace(/^data:[^,]*,/, ""));
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

/** Porównanie semver lite: zwraca true gdy `latest` > `current`. */
export function isNewer(latest: string, current: string): boolean {
  const norm = (v: string) =>
    v
      .replace(/^v/, "")
      .split(/[-+.]/)
      .map((p) => (/^\d+$/.test(p) ? Number(p) : p));
  const a = norm(latest);
  const b = norm(current);
  for (let i = 0; i < Math.max(a.length, b.length); i++) {
    const x = a[i] ?? 0;
    const y = b[i] ?? 0;
    if (x === y) continue;
    if (typeof x === "number" && typeof y === "number") return x > y;
    return String(x) > String(y);
  }
  return false;
}
