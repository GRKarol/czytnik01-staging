/**
 * WiFi transport — telefon łączy się do AP urządzenia (np. "Flower-AB12CD")
 * a app gada z `http://192.168.4.1` po HTTP.
 *
 * Działa na iOS i Androidzie (zwykły fetch), nie wymaga Web Serial ani Web
 * Bluetooth. W natywnej appce na Androidzie proces jest przypinany do sieci
 * czytnika (network-pin.ts), inaczej system wysyła ruch przez dane
 * komórkowe, bo sieć czytnika nie ma internetu.
 *
 * Firmware nie ma kanału zdarzeń (WebSocket /api/events). Wcześniejsza
 * wersja wymagała go przy łączeniu, więc każde połączenie przez WiFi
 * kończyło się błędem "Nie udało się otworzyć kanału eventów", mimo że
 * czytnik odpowiadał. Teraz połączenie = czytnik odpowiada na /api/hello.
 */

import { DEVICE_AP_BASE_URL } from "../../shared/config";
import type { DeviceCommand, DeviceEvent } from "../../shared/device-protocol";
import type { DeviceLink, TransportInfo } from "./device-link";
import { isNativeApp, pinToReaderNetwork, releaseReaderNetwork } from "./network-pin";
import { tr } from "../i18n/index";

export interface WifiLinkOptions {
  /** Bazowy URL urządzenia. Domyślnie `http://192.168.4.1`. */
  baseUrl?: string;
  /** Ile razy pytać /api/hello, zanim uznamy, że czytnika nie ma. */
  attempts?: number;
}

export interface HelloInfo {
  firmwareVersion?: string;
  name?: string;
}

const HELLO_TIMEOUT_MS = 4000;
const RETRY_GAP_MS = 1200;

/** One /api/hello with a timeout; the reader's reply, or null. */
export async function helloDevice(baseUrl: string = DEVICE_AP_BASE_URL): Promise<HelloInfo | null> {
  try {
    const res = await fetch(`${baseUrl.replace(/\/+$/, "")}/api/hello`, {
      signal: AbortSignal.timeout(HELLO_TIMEOUT_MS),
      cache: "no-store",
    });
    if (!res.ok) return null;
    return ((await res.json().catch(() => ({}))) as HelloInfo) ?? {};
  } catch {
    return null;
  }
}

export class WifiLink implements DeviceLink {
  private handlers = new Set<(ev: DeviceEvent) => void>();
  private isConnected = false;
  hello: HelloInfo | null = null;
  readonly transport: TransportInfo = { kind: "wifi", label: "WiFi" };

  constructor(private opts: WifiLinkOptions = {}) {}

  private get base(): string {
    return this.opts.baseUrl ?? DEVICE_AP_BASE_URL;
  }

  get connected(): boolean {
    return this.isConnected;
  }

  async connect(): Promise<void> {
    // Native app: requests to 192.168.4.1 must go through the reader's WiFi.
    await pinToReaderNetwork();

    const attempts = Math.max(1, this.opts.attempts ?? 3);
    for (let attempt = 0; attempt < attempts; attempt++) {
      this.hello = await helloDevice(this.base);
      if (this.hello) {
        this.isConnected = true;
        return;
      }
      if (attempt + 1 < attempts) {
        await new Promise((r) => setTimeout(r, RETRY_GAP_MS));
      }
    }

    // Najczęstsza przyczyna, gdy aplikacja jest hostowana na HTTPS
    // (grkarol.github.io): mixed content block — przeglądarka odmawia
    // requestu HTTP ze strony HTTPS.
    const isHttps = typeof location !== "undefined" && location.protocol === "https:";
    throw new Error(
      isHttps
        ? tr("err.mixedContent", { url: `${this.base}/` })
        : isNativeApp()
          ? tr("err.noAnswerApp")
          : tr("err.noAnswer", { url: this.base }),
    );
  }

  async disconnect(): Promise<void> {
    this.isConnected = false;
    await releaseReaderNetwork();
  }

  async send(cmd: DeviceCommand): Promise<void> {
    const res = await fetch(`${this.base}/api/cmd`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(cmd),
      signal: AbortSignal.timeout(HELLO_TIMEOUT_MS),
    });
    if (!res.ok) {
      throw new Error(tr("err.commandRejected", { status: res.status }));
    }
  }

  onEvent(handler: (ev: DeviceEvent) => void): () => void {
    this.handlers.add(handler);
    return () => this.handlers.delete(handler);
  }

  static isSupported(): boolean {
    return typeof fetch === "function";
  }
}
