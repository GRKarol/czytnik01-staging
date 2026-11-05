/**
 * Card set-up over the USB cable, right after the firmware install: format
 * the reader's microSD card and copy the font pack onto it, so the wizard
 * on a new reader skips the long font download.
 *
 * Talks to the firmware console (firmware/src/app/AppProvision.inl). Every
 * answer is a line starting with "@prov"; the log lines around it are
 * ignored. Files go in 8 KB chunks, each acknowledged before the next, so
 * the reader's USB receive queue never overflows while it writes the card.
 */

const CHUNK = 8192;
const LINE_TIMEOUT_MS = 15000;

export interface FontFile {
  name: string;
  size: number;
}

export interface CardSetupProgress {
  phase: "connect" | "wake" | "format" | "fonts" | "serial" | "done";
  filesDone: number;
  filesTotal: number;
  bytesDone: number;
  bytesTotal: number;
  file?: string;
  // Set once the "serial" phase resolves (ok or not) -- best-effort, never
  // fails the whole card set-up on its own (fonts/card are what matters for
  // the wizard; the serial number just rides along at the end of the same
  // USB session so the batch-flashing session doesn't need a second step).
  serialAssigned?: string;
  serialError?: string;
}

// Numer seryjny do przypisania przy flashowaniu wielu czytników pod rząd
// (format RRRR-NNN). Licznik żyje w localStorage tej przeglądarki -- każdy
// kolejny "Przygotuj kartę" na tym komputerze proponuje następny numer, a
// nowy rok zeruje licznik. Karol może go nadpisać w polu przed wysłaniem
// (np. żeby pominąć numer uszkodzonej sztuki).
const SERIAL_COUNTER_KEY = "flowerSerialCounter";

interface SerialCounterState {
  year: number;
  seq: number;
}

function readSerialCounter(): SerialCounterState {
  try {
    const raw = window.localStorage.getItem(SERIAL_COUNTER_KEY);
    if (raw) {
      const parsed = JSON.parse(raw) as SerialCounterState;
      if (typeof parsed.year === "number" && typeof parsed.seq === "number") return parsed;
    }
  } catch {
    /* localStorage unavailable or garbage -- start fresh */
  }
  return { year: new Date().getFullYear(), seq: 0 };
}

export function suggestNextSerialNumber(): string {
  const stored = readSerialCounter();
  const year = new Date().getFullYear();
  const seq = stored.year === year ? stored.seq + 1 : 1;
  return `${year}-${String(seq).padStart(3, "0")}`;
}

// Called only after the firmware confirms the serial it was actually sent
// (which may be a manually edited value, not the suggestion) -- the next
// suggestion continues from there.
export function rememberAssignedSerial(serial: string): void {
  const match = /^(\d{4})-(\d{3})$/.exec(serial);
  if (!match) return;
  const state: SerialCounterState = { year: Number(match[1]), seq: Number(match[2]) };
  try {
    window.localStorage.setItem(SERIAL_COUNTER_KEY, JSON.stringify(state));
  } catch {
    /* localStorage unavailable -- next suggestion just repeats this one */
  }
}

type Waiter = {
  match: (message: string) => boolean;
  resolve: (message: string) => void;
  reject: (error: Error) => void;
  timer: number;
};

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();

export function crc32(data: Uint8Array): number {
  let crc = 0xffffffff;
  for (let i = 0; i < data.length; i++) crc = CRC_TABLE[(crc ^ data[i]) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

class ProvisionLink {
  private port: SerialPort | null = null;
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private pending = "";
  private waiters: Waiter[] = [];
  // Answers nobody waited for yet (one can beat the waitFor() after it).
  private inbox: string[] = [];
  private readonly encoder = new TextEncoder();

  async open(port: SerialPort): Promise<void> {
    this.port = port;
    await port.open({ baudRate: 115200, bufferSize: 65536 });
    this.writer = port.writable!.getWriter();
    void this.readLoop();
  }

  async close(): Promise<void> {
    for (const w of this.waiters) {
      clearTimeout(w.timer);
      w.reject(new Error("closed"));
    }
    this.waiters = [];
    try {
      await this.reader?.cancel();
    } catch {
      /* already closed */
    }
    this.reader?.releaseLock();
    this.writer?.releaseLock();
    try {
      await this.port?.close();
    } catch {
      /* already closed */
    }
    this.port = null;
  }

  private async readLoop(): Promise<void> {
    const decoder = new TextDecoder();
    this.reader = this.port!.readable!.getReader();
    try {
      for (;;) {
        const { value, done } = await this.reader.read();
        if (done) break;
        this.pending += decoder.decode(value, { stream: true });
        let newline = this.pending.indexOf("\n");
        while (newline >= 0) {
          const line = this.pending.slice(0, newline);
          this.pending = this.pending.slice(newline + 1);
          const at = line.indexOf("@prov ");
          if (at >= 0) this.dispatch(line.slice(at + 6).trim());
          newline = this.pending.indexOf("\n");
        }
      }
    } catch {
      /* port closed or unplugged */
    }
  }

  private dispatch(message: string): void {
    const index = this.waiters.findIndex((w) => w.match(message));
    if (index < 0) {
      this.inbox.push(message);
      if (this.inbox.length > 64) this.inbox.shift();
      return;
    }
    const [waiter] = this.waiters.splice(index, 1);
    clearTimeout(waiter.timer);
    waiter.resolve(message);
  }

  clearInbox(): void {
    this.inbox = [];
  }

  waitFor(match: (message: string) => boolean, timeoutMs = LINE_TIMEOUT_MS): Promise<string> {
    const early = this.inbox.findIndex(match);
    if (early >= 0) {
      const [message] = this.inbox.splice(early, 1);
      return Promise.resolve(message);
    }
    return new Promise((resolve, reject) => {
      const waiter: Waiter = {
        match,
        resolve,
        reject,
        timer: window.setTimeout(() => {
          this.waiters = this.waiters.filter((w) => w !== waiter);
          reject(new Error("timeout"));
        }, timeoutMs),
      };
      this.waiters.push(waiter);
    });
  }

  async send(line: string): Promise<void> {
    await this.writer!.write(this.encoder.encode(line + "\n"));
  }

  async sendBytes(bytes: Uint8Array): Promise<void> {
    await this.writer!.write(bytes);
  }
}

export async function loadFontList(baseUrl: string): Promise<FontFile[]> {
  const response = await fetch(`${baseUrl}fonts/index.json`, { cache: "no-cache" });
  if (!response.ok) throw new Error("Brak listy czcionek na serwerze.");
  const index = (await response.json()) as { files: FontFile[] };
  return index.files;
}

async function putFile(
  link: ProvisionLink,
  path: string,
  data: Uint8Array,
  onBytes: (sent: number) => void,
): Promise<void> {
  link.clearInbox();
  const ready = link.waitFor((m) => m === "ready" || m.startsWith("put fail"));
  await link.send(`prov put ${path} ${data.length} ${crc32(data).toString(16)}`);
  const answer = await ready;
  if (answer !== "ready") throw new Error(answer);
  for (let offset = 0; offset < data.length; offset += CHUNK) {
    const end = Math.min(data.length, offset + CHUNK);
    const ack = link.waitFor((m) => m === `k ${end}` || m.startsWith("put fail"));
    await link.sendBytes(data.subarray(offset, end));
    const reply = await ack;
    if (reply !== `k ${end}`) throw new Error(reply);
    onBytes(end);
  }
  const result = await link.waitFor((m) => m.startsWith("put "));
  if (!result.startsWith("put ok")) throw new Error(result);
}

/**
 * Asks for the port, waits for the reader to answer, formats the card and
 * writes every font. `onProgress` drives the page's progress bar.
 */
export async function setUpCard(
  baseUrl: string,
  onProgress: (progress: CardSetupProgress) => void,
  serial?: string,
): Promise<void> {
  const files = await loadFontList(baseUrl);
  const bytesTotal = files.reduce((sum, f) => sum + f.size, 0);
  const progress: CardSetupProgress = {
    phase: "connect",
    filesDone: 0,
    filesTotal: files.length,
    bytesDone: 0,
    bytesTotal,
  };
  onProgress({ ...progress });

  const port = await navigator.serial.requestPort();
  const link = new ProvisionLink();
  await link.open(port);
  try {
    // The reader may still be starting (or asleep: then PWR wakes it).
    let hello = "";
    for (let attempt = 0; attempt < 30 && !hello; attempt++) {
      if (attempt === 4) onProgress({ ...progress, phase: "wake" });
      try {
        const answer = link.waitFor((m) => m.startsWith("hello"), 1500);
        await link.send("prov hello");
        hello = await answer;
      } catch {
        /* no answer yet */
      }
    }
    if (!hello) throw new Error("Czytnik nie odpowiada. Włącz go przyciskiem PWR i spróbuj jeszcze raz.");

    onProgress({ ...progress, phase: "format" });
    const formatted = link.waitFor((m) => m.startsWith("format"), 120000);
    await link.send("prov format");
    if ((await formatted) !== "format ok") {
      throw new Error("Nie udało się sformatować karty. Sprawdź, czy karta microSD jest w czytniku.");
    }

    progress.phase = "fonts";
    let bytesBefore = 0;
    for (const file of files) {
      const response = await fetch(`${baseUrl}fonts/${file.name}`);
      if (!response.ok) throw new Error(`Nie mogę pobrać ${file.name}.`);
      const data = new Uint8Array(await response.arrayBuffer());
      let lastError: Error | null = null;
      for (let attempt = 0; attempt < 3; attempt++) {
        try {
          await putFile(link, `/fonts/${file.name}`, data, (sent) =>
            onProgress({ ...progress, file: file.name, bytesDone: bytesBefore + sent }),
          );
          lastError = null;
          break;
        } catch (error) {
          lastError = error as Error;
        }
      }
      if (lastError) throw new Error(`Zapis ${file.name} nie wyszedł (${lastError.message}).`);
      bytesBefore += data.length;
      progress.filesDone += 1;
      progress.bytesDone = bytesBefore;
      onProgress({ ...progress, file: file.name });
    }

    const done = link.waitFor((m) => m.startsWith("done"));
    await link.send("prov done");
    const answer = await done;
    if (!answer.includes("fonts=1")) {
      throw new Error("Czytnik nie widzi wszystkich czcionek na karcie. Spróbuj jeszcze raz.");
    }

    const result: CardSetupProgress = { ...progress, phase: "done" };
    const trimmedSerial = serial?.trim();
    if (trimmedSerial) {
      onProgress({ ...progress, phase: "serial" });
      try {
        const serialReply = link.waitFor((m) => m.startsWith("serial "));
        await link.send(`prov serial ${trimmedSerial}`);
        const reply = await serialReply;
        if (reply === `serial ok ${trimmedSerial}`) {
          result.serialAssigned = trimmedSerial;
          rememberAssignedSerial(trimmedSerial);
        } else {
          result.serialError = reply;
        }
      } catch (error) {
        result.serialError = (error as Error).message || String(error);
      }
    }
    onProgress(result);
  } finally {
    await link.close();
  }
}
