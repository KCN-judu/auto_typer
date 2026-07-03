import { execFile } from "node:child_process";
import { createReadStream, createWriteStream, type ReadStream, type WriteStream } from "node:fs";
import { readdir } from "node:fs/promises";
import { promisify } from "node:util";
import type {
  WifiConfigResultMessage,
  WifiNetwork,
  WifiNetworkMessage,
  WifiNetworksMessage,
  WifiStatus,
  WifiStatusMessage,
} from "../../../shared/protocol/auto-typer-protocol.js";

const execFileAsync = promisify(execFile);

type SerialWifiCommand =
  | { v: 1; type: "get_wifi_status"; requestId: string }
  | { v: 1; type: "scan_wifi"; requestId: string }
  | { v: 1; type: "configure_wifi"; requestId: string; ssid: string; password: string };

type SerialWifiResponse =
  | WifiStatusMessage
  | WifiNetworkMessage
  | WifiNetworksMessage
  | WifiConfigResultMessage
  | { v: 1; type: "protocol_error"; requestId: string; code: string; message: string };

export type SerialPortInfo = {
  path: string;
  label: string;
};

export type SerialWifiLogEvent = {
  line: string;
  protocol: boolean;
};

type PendingRequest = {
  requestId: string;
  resolve: (message: SerialWifiResponse) => void;
  reject: (error: Error) => void;
  timer?: NodeJS.Timeout;
  networks?: WifiNetwork[];
};

const rxPrefix = "ATWIFI<";
const txPrefix = "ATWIFI>";
const baudRate = 115200;
const statusTimeoutMs = 8000;
const scanTimeoutMs = 30000;
const configureTimeoutMs = 20000;
const maxSerialBufferBytes = 8192;

export async function listSerialPorts(): Promise<SerialPortInfo[]> {
  if (process.platform !== "darwin") {
    return [];
  }
  const names = await readdir("/dev");
  return names
    .filter((name) => name.startsWith("cu."))
    .map((name) => ({ path: `/dev/${name}`, label: name }))
    .sort((a, b) => scoreSerialPort(b) - scoreSerialPort(a) || a.label.localeCompare(b.label));
}

export class SerialWifiLink {
  private portPath = "";
  private readStream?: ReadStream;
  private writeStream?: WriteStream;
  private buffer = "";
  private sequence = 0;
  private pending = new Map<string, PendingRequest>();
  private commandQueue: Promise<void> = Promise.resolve();
  private connectionGeneration = 0;

  constructor(private readonly onLog?: (event: SerialWifiLogEvent) => void) {}

  async connect(requestedPath?: string): Promise<SerialPortInfo> {
    this.emitDiagnostic(`connect requested path=${requestedPath ?? "auto"}`);
    await this.close({ rejectPending: false });
    const ports = await listSerialPorts();
    this.emitDiagnostic(`ports found=${ports.map((port) => port.path).join(",") || "none"}`);
    const selected = requestedPath
      ? ports.find((port) => port.path === requestedPath) ?? { path: requestedPath, label: requestedPath }
      : ports[0];
    if (!selected) {
      throw new Error("serial_no_ports: 未找到 USB 串口设备");
    }
    await configureSerialPort(selected.path);
    this.emitDiagnostic(`opening ${selected.path} baud=${baudRate}`);
    this.portPath = selected.path;
    this.readStream = createReadStream(selected.path, { encoding: "utf8" });
    this.writeStream = createWriteStream(selected.path, { encoding: "utf8" });
    this.connectionGeneration += 1;
    this.readStream.on("data", (chunk) => this.pushData(chunk.toString()));
    this.readStream.on("error", (error) => {
      this.emitDiagnostic(`read error ${error.message}`);
      this.failPending(error);
    });
    this.writeStream.on("error", (error) => {
      this.emitDiagnostic(`write error ${error.message}`);
      this.failPending(error);
    });
    this.emitDiagnostic(`connected ${selected.path}`);
    return selected;
  }

  async close(options: { rejectPending?: boolean } = {}): Promise<void> {
    if (this.readStream || this.writeStream || this.portPath) {
      this.emitDiagnostic(`closing ${this.portPath || "serial"} rejectPending=${options.rejectPending ?? true}`);
    }
    this.connectionGeneration += 1;
    if (options.rejectPending ?? true) {
      this.failPending(new Error("serial_disconnected: USB 串口已断开"));
    } else {
      this.resolvePendingAsClosed();
    }
    await Promise.all([
      destroyStream(this.readStream),
      destroyStream(this.writeStream),
    ]);
    this.readStream = undefined;
    this.writeStream = undefined;
    this.portPath = "";
    this.buffer = "";
    this.commandQueue = Promise.resolve();
  }

  isConnected(): boolean {
    return this.writeStream !== undefined && !this.writeStream.destroyed;
  }

  async getWifiStatus(): Promise<WifiStatus> {
    const response = await this.sendCommand(
      { v: 1, type: "get_wifi_status", requestId: this.nextId("wifi") },
      statusTimeoutMs,
    );
    if (response.type !== "wifi_status") {
      throw responseError(response, "get_wifi_status");
    }
    return response.wifi;
  }

  async scanWifi(): Promise<WifiNetworksMessage> {
    const response = await this.sendCommand({ v: 1, type: "scan_wifi", requestId: this.nextId("scan") }, scanTimeoutMs);
    if (response.type !== "wifi_networks") {
      throw responseError(response, "scan_wifi");
    }
    if (!response.ok) {
      throw new Error(`${response.code ?? "wifi_scan_failed"}: ${response.message ?? "WiFi scan failed"}`);
    }
    return response;
  }

  async configureWifi(ssid: string, password: string): Promise<WifiConfigResultMessage> {
    const response = await this.sendCommand(
      { v: 1, type: "configure_wifi", requestId: this.nextId("wifi-config"), ssid, password },
      configureTimeoutMs,
    );
    if (response.type !== "wifi_config_result") {
      throw responseError(response, "configure_wifi");
    }
    return response;
  }

  private sendCommand(command: SerialWifiCommand, timeoutMs?: number): Promise<SerialWifiResponse> {
    const generation = this.connectionGeneration;
    const queued = this.commandQueue.then(
      () => this.writeCommand(command, timeoutMs, generation),
      () => this.writeCommand(command, timeoutMs, generation),
    );
    this.commandQueue = queued.then(
      () => undefined,
      () => undefined,
    );
    return queued;
  }

  private writeCommand(command: SerialWifiCommand, timeoutMs: number | undefined, generation: number): Promise<SerialWifiResponse> {
    if (generation !== this.connectionGeneration || !this.writeStream || this.writeStream.destroyed) {
      this.emitDiagnostic(`tx rejected type=${command.type} requestId=${command.requestId} connected=${this.isConnected() ? 1 : 0}`);
      return Promise.reject(new Error("serial_not_connected: USB 串口未连接"));
    }
    const line = `${txPrefix}${JSON.stringify(command)}\n`;
    this.emitDiagnostic(`tx type=${command.type} requestId=${command.requestId} bytes=${Buffer.byteLength(line, "utf8")}`);
    return new Promise<SerialWifiResponse>((resolve, reject) => {
      const timer = timeoutMs === undefined
        ? undefined
        : setTimeout(() => {
          this.pending.delete(command.requestId);
          this.emitDiagnostic(`timeout type=${command.type} requestId=${command.requestId} afterMs=${timeoutMs}`);
          reject(new Error(`serial_timeout: ${command.type} timed out after ${timeoutMs}ms`));
        }, timeoutMs);
      this.pending.set(command.requestId, {
        requestId: command.requestId,
        resolve,
        reject,
        timer,
        networks: command.type === "scan_wifi" ? [] : undefined,
      });
      this.writeStream?.write(line, "utf8", (error) => {
        if (!error) {
          this.emitDiagnostic(`tx flushed type=${command.type} requestId=${command.requestId}`);
          return;
        }
        if (timer) {
          clearTimeout(timer);
        }
        this.pending.delete(command.requestId);
        this.emitDiagnostic(`tx error type=${command.type} requestId=${command.requestId} ${error.message}`);
        reject(error);
      });
    });
  }

  private pushData(chunk: string): void {
    this.emitDiagnostic(`rx bytes=${Buffer.byteLength(chunk, "utf8")}`);
    this.buffer += chunk.replace(/\r/g, "");
    if (Buffer.byteLength(this.buffer, "utf8") > maxSerialBufferBytes) {
      console.warn("[serial-wifi] dropping oversized serial buffer");
      this.buffer = "";
      return;
    }
    while (true) {
      const prefixIndex = this.buffer.indexOf(rxPrefix);
      if (prefixIndex < 0) {
        this.emitCompleteLogLines();
        return;
      }
      if (prefixIndex > 0) {
        this.emitLogText(this.buffer.slice(0, prefixIndex));
        this.buffer = this.buffer.slice(prefixIndex);
      }
      const jsonStart = firstNonWhitespaceIndex(this.buffer, rxPrefix.length);
      if (jsonStart >= this.buffer.length) {
        return;
      }
      if (this.buffer[jsonStart] !== "{") {
        this.dropInvalidProtocolText(rxPrefix.length);
        continue;
      }
      const jsonEnd = findJsonObjectEnd(this.buffer, rxPrefix.length);
      if (jsonEnd < 0) {
        if (this.dropCompleteInvalidProtocolLine()) {
          continue;
        }
        return;
      }
      const frame = this.buffer.slice(0, jsonEnd);
      this.buffer = this.buffer.slice(jsonEnd);
      this.onLog?.({ line: frame, protocol: true });
      this.handleProtocolFrame(frame);
    }
  }

  private emitCompleteLogLines(): void {
    while (true) {
      const newline = this.buffer.indexOf("\n");
      if (newline < 0) {
        return;
      }
      this.emitLogText(this.buffer.slice(0, newline));
      this.buffer = this.buffer.slice(newline + 1);
    }
  }

  private emitLogText(text: string): void {
    for (const part of text.split("\n")) {
      const line = part.trim();
      if (line.length > 0) {
        this.onLog?.({ line, protocol: false });
      }
    }
  }

  private handleProtocolFrame(frame: string): void {
    let message: SerialWifiResponse;
    try {
      message = JSON.parse(frame.slice(rxPrefix.length)) as SerialWifiResponse;
    } catch (error) {
      console.warn("[serial-wifi] ignored invalid protocol line", error instanceof Error ? error.message : error);
      const requestId = extractRequestId(frame.slice(rxPrefix.length));
      if (requestId) {
        this.rejectPending(requestId, new Error("serial_invalid_frame: ESP32 returned invalid serial WiFi JSON"));
      }
      return;
    }
    if (typeof (message as { requestId?: unknown }).requestId !== "string") {
      console.warn("[serial-wifi] ignored protocol line without requestId");
      return;
    }
    const pending = this.pending.get(message.requestId);
    this.emitDiagnostic(`rx frame type=${message.type} requestId=${message.requestId} pending=${pending ? 1 : 0}`);
    if (!pending) {
      return;
    }
    if (message.type === "wifi_network") {
      pending.networks?.push(message.network);
      return;
    }
    if (pending.timer) {
      clearTimeout(pending.timer);
    }
    this.pending.delete(message.requestId);
    if (message.type === "wifi_networks" && pending.networks) {
      message = { ...message, networks: [...pending.networks, ...(message.networks ?? [])] };
    }
    pending.resolve(message);
  }

  private emitDiagnostic(line: string): void {
    this.onLog?.({ line: `[serial-wifi] ${line}`, protocol: false });
  }

  private dropCompleteInvalidProtocolLine(): boolean {
    const nextPrefix = this.buffer.indexOf(rxPrefix, rxPrefix.length);
    const newline = this.buffer.indexOf("\n", rxPrefix.length);
    if (newline >= 0 && (nextPrefix < 0 || newline < nextPrefix)) {
      const frame = this.buffer.slice(0, newline);
      this.buffer = this.buffer.slice(newline + 1);
      this.onLog?.({ line: frame, protocol: true });
      this.handleProtocolFrame(frame);
      return true;
    }
    if (nextPrefix > rxPrefix.length) {
      const frame = this.buffer.slice(0, nextPrefix);
      this.buffer = this.buffer.slice(nextPrefix);
      this.onLog?.({ line: frame, protocol: true });
      this.handleProtocolFrame(frame);
      return true;
    }
    return false;
  }

  private dropInvalidProtocolText(startIndex: number): void {
    const nextPrefix = this.buffer.indexOf(rxPrefix, startIndex);
    if (nextPrefix >= 0) {
      const frame = this.buffer.slice(0, nextPrefix);
      this.buffer = this.buffer.slice(nextPrefix);
      this.onLog?.({ line: frame, protocol: true });
      this.handleProtocolFrame(frame);
      return;
    }
    const newline = this.buffer.indexOf("\n", startIndex);
    if (newline >= 0) {
      const frame = this.buffer.slice(0, newline);
      this.buffer = this.buffer.slice(newline + 1);
      this.onLog?.({ line: frame, protocol: true });
      this.handleProtocolFrame(frame);
      return;
    }
    const frame = this.buffer;
    this.buffer = "";
    this.onLog?.({ line: frame, protocol: true });
    this.handleProtocolFrame(frame);
  }

  private rejectPending(requestId: string, error: Error): void {
    const pending = this.pending.get(requestId);
    if (!pending) {
      return;
    }
    if (pending.timer) {
      clearTimeout(pending.timer);
    }
    this.pending.delete(requestId);
    pending.reject(error);
  }

  private failPending(error: Error): void {
    for (const pending of this.pending.values()) {
      if (pending.timer) {
        clearTimeout(pending.timer);
      }
      pending.reject(error);
    }
    this.pending.clear();
  }

  private resolvePendingAsClosed(): void {
    const closed = { v: 1, type: "protocol_error", requestId: "", code: "serial_closed", message: "USB serial connection closed" } as const;
    for (const pending of this.pending.values()) {
      if (pending.timer) {
        clearTimeout(pending.timer);
      }
      pending.resolve({ ...closed, requestId: pending.requestId });
    }
    this.pending.clear();
  }

  private nextId(prefix: string): string {
    this.sequence += 1;
    return `${prefix}-${Date.now().toString(36)}-${this.sequence.toString(36)}`;
  }
}

async function configureSerialPort(portPath: string): Promise<void> {
  if (process.platform === "darwin") {
    await execFileAsync("stty", ["-f", portPath, String(baudRate), "cs8", "-cstopb", "-parenb", "raw", "-echo"]);
  }
}

function destroyStream(stream: ReadStream | WriteStream | undefined): Promise<void> {
  return new Promise((resolve) => {
    if (!stream || stream.destroyed) {
      resolve();
      return;
    }
    const closable = stream as ReadStream;
    closable.once("close", resolve);
    stream.destroy();
  });
}

function scoreSerialPort(port: SerialPortInfo): number {
  const haystack = `${port.path} ${port.label}`.toLowerCase();
  let score = 0;
  if (haystack.includes("usb")) score += 10;
  if (haystack.includes("esp32")) score += 8;
  if (haystack.includes("jtag")) score += 6;
  if (haystack.includes("slab")) score += 5;
  if (haystack.includes("wch")) score += 5;
  if (haystack.includes("bluetooth")) score -= 20;
  return score;
}

function firstNonWhitespaceIndex(text: string, startIndex: number): number {
  let index = startIndex;
  while (index < text.length && /\s/.test(text[index] ?? "")) {
    index += 1;
  }
  return index;
}

function findJsonObjectEnd(text: string, startIndex: number): number {
  let index = firstNonWhitespaceIndex(text, startIndex);
  if (text[index] !== "{") {
    return -1;
  }
  let depth = 0;
  let inString = false;
  let escaped = false;
  for (; index < text.length; index += 1) {
    const char = text[index];
    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (char === "\\") {
        escaped = true;
      } else if (char === "\"") {
        inString = false;
      }
      continue;
    }
    if (char === "\"") {
      inString = true;
      continue;
    }
    if (char === "{") {
      depth += 1;
      continue;
    }
    if (char === "}") {
      depth -= 1;
      if (depth === 0) {
        return index + 1;
      }
    }
  }
  return -1;
}

function extractRequestId(payload: string): string | undefined {
  const match = /"requestId"\s*:\s*"([^"\\]*(?:\\.[^"\\]*)*)"/.exec(payload);
  if (!match) {
    return undefined;
  }
  try {
    return JSON.parse(`"${match[1]}"`) as string;
  } catch {
    return undefined;
  }
}

function responseError(message: SerialWifiResponse, command: string): Error {
  if (message.type === "protocol_error") {
    return new Error(`${message.code}: ${message.message}`);
  }
  return new Error(`unexpected_response: ${command} received ${message.type}`);
}
