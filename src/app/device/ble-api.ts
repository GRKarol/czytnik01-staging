/**
 * BLE implementation of DeviceApi — sends commands via BluetoothLink
 * and handles file upload through the chunked BLE protocol.
 *
 * Protocol for upload:
 *   1. Send {"cmd":"upload-begin","name":"file.epub","size":12345}
 *   2. Send {"cmd":"upload-chunk","d":"<base64 ~8KB>"} repeatedly
 *   3. Send {"cmd":"upload-end"}
 *   4. Device responds with {"ev":"upload-complete",...}
 */

import type { DeviceEvent } from "../../shared/device-protocol";
import type {
  Book,
  DeviceApi,
  DeviceSettings,
  WifiStationConfig,
  PluginInfo,
  DeviceLogTail,
  BookPosition,
  BookTextPage,
  DeviceCapabilities,
  DeviceInfo,
} from "./api";
import { DEFAULT_SETTINGS } from "./api";
import { BluetoothLink } from "./bluetooth-link";
import { tr } from "../i18n/index";

const bleUnsupported = () => tr("err.bleUnsupported");

const UPLOAD_CHUNK_SIZE = 8192; // 8KB raw bytes per chunk (becomes ~11KB base64)

export class BleDeviceApi implements DeviceApi {
  private link: BluetoothLink;
  private token: string;

  constructor(link: BluetoothLink, token: string) {
    this.link = link;
    this.token = token;
  }

  private async sendCmd(cmd: Record<string, unknown>): Promise<DeviceEvent> {
    return new Promise<DeviceEvent>((resolve, reject) => {
      const timeout = setTimeout(() => {
        unsub();
        reject(new Error(tr("err.bleTimeout")));
      }, 10000);

      const unsub = this.link.onEvent((ev) => {
        clearTimeout(timeout);
        unsub();
        resolve(ev);
      });

      this.link.send(cmd as { cmd: string; [key: string]: unknown }).catch((err) => {
        clearTimeout(timeout);
        unsub();
        reject(err);
      });
    });
  }

  private async authenticate(): Promise<void> {
    const ev = await this.sendCmd({ cmd: "auth", token: this.token });
    if (ev.ev !== "auth-ok") {
      throw new Error(`Autentykacja BLE nieudana: ${ev.ev}`);
    }
  }

  async listBooks(): Promise<Book[]> {
    await this.authenticate();
    const ev = await this.sendCmd({ cmd: "get-books" });
    if (ev.ev === "books" && Array.isArray(ev.data)) {
      return (ev.data as Array<Record<string, unknown>>).map((b) => ({
        name: String(b.name ?? ""),
        title: b.title ? String(b.title) : undefined,
        author: b.author ? String(b.author) : undefined,
        bytes: 0,
        progressPercent: typeof b.progressPercent === "number" ? b.progressPercent : undefined,
        category: b.category === "article" ? "article" : "book",
      }));
    }
    throw new Error(tr("err.bleBooks"));
  }

  async uploadBook(file: Blob, name: string): Promise<void> {
    await this.authenticate();

    const arrayBuffer = await file.arrayBuffer();
    const bytes = new Uint8Array(arrayBuffer);

    // 1. Send upload-begin
    const beginEv = await this.sendCmd({
      cmd: "upload-begin",
      name,
      size: bytes.length,
    });
    if (beginEv.ev === "upload-error") {
      throw new Error(`Upload error: ${beginEv.reason ?? "unknown"}`);
    }

    // 2. Send chunks
    let offset = 0;
    while (offset < bytes.length) {
      const chunkEnd = Math.min(offset + UPLOAD_CHUNK_SIZE, bytes.length);
      const chunk = bytes.subarray(offset, chunkEnd);
      const b64 = uint8ToBase64(chunk);

      const chunkEv = await this.sendCmd({ cmd: "upload-chunk", d: b64 });
      if (chunkEv.ev === "upload-error") {
        throw new Error(`Upload chunk error: ${chunkEv.reason ?? "unknown"}`);
      }
      offset = chunkEnd;
    }

    // 3. Send upload-end
    const endEv = await this.sendCmd({ cmd: "upload-end" });
    if (endEv.ev === "upload-error") {
      throw new Error(`Upload end error: ${endEv.reason ?? "unknown"}`);
    }
  }

  async deleteBook(_name: string): Promise<void> {
    // Not implemented via BLE yet — would need firmware support
    throw new Error(tr("err.bleUnsupported"));
  }

  async getSettings(): Promise<DeviceSettings> {
    await this.authenticate();
    const ev = await this.sendCmd({ cmd: "get-settings" });
    if (ev.ev === "settings" && ev.data) {
      // For now return defaults — full mapping can be added later
      return DEFAULT_SETTINGS;
    }
    return DEFAULT_SETTINGS;
  }

  async putSettings(patch: Partial<DeviceSettings>): Promise<DeviceSettings> {
    await this.authenticate();
    await this.sendCmd({ cmd: "set-settings", settings: patch });
    return { ...DEFAULT_SETTINGS, ...patch };
  }

  async installOta(): Promise<void> {
    throw new Error(tr("err.bleUnsupported"));
  }

  async getWifiStation(): Promise<WifiStationConfig> {
    throw new Error(bleUnsupported());
  }

  async setWifiStation(): Promise<WifiStationConfig> {
    throw new Error(bleUnsupported());
  }

  async clearWifiStation(): Promise<WifiStationConfig> {
    throw new Error(bleUnsupported());
  }

  async getRssFeeds(): Promise<string[]> {
    throw new Error(bleUnsupported());
  }

  async setRssFeeds(): Promise<string[]> {
    throw new Error(bleUnsupported());
  }

  async getPlugins(): Promise<PluginInfo[]> {
    throw new Error(bleUnsupported());
  }

  async setWifiTimeoutSeconds(): Promise<number> {
    throw new Error(bleUnsupported());
  }

  async getCapabilities(): Promise<DeviceCapabilities> {
    throw new Error(bleUnsupported());
  }

  async getDeviceInfo(): Promise<DeviceInfo> {
    throw new Error(bleUnsupported());
  }

  async getLogTail(): Promise<DeviceLogTail> {
    throw new Error(bleUnsupported());
  }

  async clearLog(): Promise<void> {
    throw new Error(bleUnsupported());
  }

  async getBookPosition(): Promise<BookPosition> {
    throw new Error(bleUnsupported());
  }

  async setBookPosition(): Promise<BookPosition> {
    throw new Error(bleUnsupported());
  }

  // Covers and the chapter editor move too much data for BLE: WiFi only.
  async getBookText(): Promise<BookTextPage> {
    throw new Error(bleUnsupported());
  }

  async setBookChapters(): Promise<void> {
    throw new Error(bleUnsupported());
  }

  async resetBookChapters(): Promise<void> {
    throw new Error(bleUnsupported());
  }

  async getBookPicture(): Promise<Blob | null> {
    return null;
  }

  async uploadBookPicture(): Promise<void> {
    throw new Error(bleUnsupported());
  }

  async deleteBookPicture(): Promise<void> {
    throw new Error(bleUnsupported());
  }
}

/** Convert Uint8Array to base64 string */
function uint8ToBase64(bytes: Uint8Array): string {
  let binary = "";
  for (let i = 0; i < bytes.length; i++) {
    binary += String.fromCharCode(bytes[i]);
  }
  return btoa(binary);
}
