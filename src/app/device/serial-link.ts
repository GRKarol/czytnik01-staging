/**
 * USB (Web Serial) — tryb advanced / diagnostyczny.
 * Główny tor klienta to WiFi (i ewentualnie BLE na Androidzie).
 * Tutaj zostawiamy USB dla serwisu / developera (Karol).
 */

import {
  type DeviceCommand,
  type DeviceEvent,
  encodeCommand,
  parseEvent,
} from "../../shared/device-protocol";
import type { DeviceLink, TransportInfo } from "./device-link";
import { tr } from "../i18n/index";

const BAUD_RATE = 115200;

export class SerialLink implements DeviceLink {
  private port: SerialPort | null = null;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
  private buffer = "";
  private handlers = new Set<(ev: DeviceEvent) => void>();
  readonly transport: TransportInfo = { kind: "serial", label: "USB (advanced)" };

  get connected(): boolean {
    return this.port !== null;
  }

  static isSupported(): boolean {
    return typeof navigator !== "undefined" && "serial" in navigator;
  }

  async connect(): Promise<void> {
    if (!SerialLink.isSupported()) {
      throw new Error(tr("err.webSerial"));
    }
    const port = await navigator.serial.requestPort();
    await port.open({ baudRate: BAUD_RATE });
    this.port = port;
    this.writer = port.writable!.getWriter();
    this.reader = port.readable!.getReader();
    void this.readLoop();
  }

  async disconnect(): Promise<void> {
    try {
      await this.reader?.cancel();
      this.reader?.releaseLock();
      await this.writer?.close();
      this.writer?.releaseLock();
      await this.port?.close();
    } finally {
      this.reader = null;
      this.writer = null;
      this.port = null;
      this.buffer = "";
    }
  }

  async send(cmd: DeviceCommand): Promise<void> {
    if (!this.writer) throw new Error(tr("err.notConnected"));
    await this.writer.write(encodeCommand(cmd));
  }

  onEvent(handler: (ev: DeviceEvent) => void): () => void {
    this.handlers.add(handler);
    return () => this.handlers.delete(handler);
  }

  private async readLoop(): Promise<void> {
    const decoder = new TextDecoder();
    while (this.reader) {
      try {
        const { value, done } = await this.reader.read();
        if (done) break;
        if (!value) continue;
        this.buffer += decoder.decode(value, { stream: true });
        let nl: number;
        while ((nl = this.buffer.indexOf("\n")) >= 0) {
          const line = this.buffer.slice(0, nl);
          this.buffer = this.buffer.slice(nl + 1);
          const ev = parseEvent(line);
          if (ev) for (const h of this.handlers) h(ev);
        }
      } catch (err) {
        console.warn("serial read loop error", err);
        break;
      }
    }
  }
}
