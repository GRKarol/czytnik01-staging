/**
 * Web Bluetooth transport — bonus dla Androida (Chrome).
 * NIE działa na iOS Safari (Apple nie wspiera Web Bluetooth od lat).
 *
 * UUID-y serwisu/charakterystyk są placeholderami — do uzgodnienia
 * z firmware. Komendy idą jako bajty na characteristic CMD, eventy
 * przychodzą jako notifications na characteristic EVENT.
 */

import { DEVICE_BLE_SERVICE_UUID } from "../../shared/config";
import type { DeviceCommand, DeviceEvent } from "../../shared/device-protocol";
import { parseEvent } from "../../shared/device-protocol";
import type { DeviceLink, TransportInfo } from "./device-link";
import { tr } from "../i18n/index";

// Placeholder UUID-y — dokończymy kiedy firmware będzie miał BLE.
const CMD_CHAR_UUID = "f10e7e11-f10e-7e10-f10e-7e10f10e7e10";
const EVT_CHAR_UUID = "f10e7e12-f10e-7e10-f10e-7e10f10e7e10";

// Chunked framing flags (matching firmware BleApi.cpp)
const FLAG_MORE = 0x01;
const FLAG_START = 0x02;

export class BluetoothLink implements DeviceLink {
  private device: BluetoothDevice | null = null;
  private cmdChar: BluetoothRemoteGATTCharacteristic | null = null;
  private evtChar: BluetoothRemoteGATTCharacteristic | null = null;
  private handlers = new Set<(ev: DeviceEvent) => void>();
  private reassemblyBuf = "";
  private negotiatedMtu = 23;
  readonly transport: TransportInfo = { kind: "bluetooth", label: "Bluetooth" };

  get connected(): boolean {
    return this.device?.gatt?.connected ?? false;
  }

  static isSupported(): boolean {
    return typeof navigator !== "undefined" && "bluetooth" in navigator;
  }

  async connect(): Promise<void> {
    if (!BluetoothLink.isSupported()) {
      throw new Error(tr("err.webBluetooth"));
    }
    const device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [DEVICE_BLE_SERVICE_UUID] }],
    });
    const server = await device.gatt!.connect();
    const service = await server.getPrimaryService(DEVICE_BLE_SERVICE_UUID);
    const cmdChar = await service.getCharacteristic(CMD_CHAR_UUID);
    const evtChar = await service.getCharacteristic(EVT_CHAR_UUID);

    await evtChar.startNotifications();
    evtChar.addEventListener("characteristicvaluechanged", this.onNotify);

    this.device = device;
    this.cmdChar = cmdChar;
    this.evtChar = evtChar;

    // Try to get MTU (Web Bluetooth doesn't expose it directly, assume 512 after negotiation)
    this.negotiatedMtu = 512;
  }

  async disconnect(): Promise<void> {
    try {
      this.evtChar?.removeEventListener("characteristicvaluechanged", this.onNotify);
      await this.evtChar?.stopNotifications();
      this.device?.gatt?.disconnect();
    } finally {
      this.device = null;
      this.cmdChar = null;
      this.evtChar = null;
      this.reassemblyBuf = "";
    }
  }

  async send(cmd: DeviceCommand): Promise<void> {
    if (!this.cmdChar) throw new Error(tr("err.notConnected"));
    const json = JSON.stringify(cmd) + "\n";
    await this.sendChunked(new TextEncoder().encode(json));
  }

  /** Send raw bytes with chunked framing protocol */
  async sendChunked(data: Uint8Array): Promise<void> {
    if (!this.cmdChar) throw new Error(tr("err.notConnected"));
    // Max payload per write = MTU - 3 (ATT header) - 1 (framing byte)
    const maxPayload = Math.max(20, this.negotiatedMtu - 4);
    let offset = 0;
    let first = true;

    while (offset < data.length) {
      const remaining = data.length - offset;
      const chunkSize = Math.min(remaining, maxPayload);
      const more = remaining - chunkSize > 0;

      let flags = 0;
      if (first) flags |= FLAG_START;
      if (more) flags |= FLAG_MORE;

      const packet = new Uint8Array(1 + chunkSize);
      packet[0] = flags;
      packet.set(data.subarray(offset, offset + chunkSize), 1);

      await this.cmdChar.writeValueWithoutResponse(packet.buffer);
      offset += chunkSize;
      first = false;

      // Small delay between chunks to avoid BLE congestion
      if (more) await sleep(5);
    }
  }

  onEvent(handler: (ev: DeviceEvent) => void): () => void {
    this.handlers.add(handler);
    return () => this.handlers.delete(handler);
  }

  private onNotify = (e: Event) => {
    const ch = e.target as BluetoothRemoteGATTCharacteristic;
    const value = ch.value;
    if (!value || value.byteLength < 1) return;

    // First byte is framing flags
    const flags = value.getUint8(0);
    const payload = new TextDecoder().decode(value.buffer.slice(1));

    const isStart = (flags & FLAG_START) !== 0;
    const isMore = (flags & FLAG_MORE) !== 0;

    if (isStart) {
      this.reassemblyBuf = "";
    }

    this.reassemblyBuf += payload;

    if (!isMore) {
      // Message complete
      const lines = this.reassemblyBuf.split("\n");
      this.reassemblyBuf = "";
      for (const line of lines) {
        const ev = parseEvent(line);
        if (ev) for (const h of this.handlers) h(ev);
      }
    }
  };
}

function sleep(ms: number): Promise<void> {
  return new Promise((r) => setTimeout(r, ms));
}
