import { EventEmitter } from "node:events";
import net from "node:net";
import type {
  GroupStreamCommandMessage,
  GroupStreamEventMessage,
} from "../../../shared/protocol/auto-typer-protocol.js";
import {
  MAX_TCP_MESSAGE_BYTES,
  TCP_TERMINAL_RESPONSE_TYPES,
} from "../../../shared/protocol/auto-typer-protocol.js";

const requestTimeoutMs = 20000;
const heartbeatIntervalMs = 1000;
const terminalResponseTypes = new Set<string>(TCP_TERMINAL_RESPONSE_TYPES);

type PendingRequest = {
  requestId: string;
  resolve: (message: GroupStreamEventMessage) => void;
  reject: (error: Error) => void;
  timer: NodeJS.Timeout;
};

type DeviceLinkEvents = {
  message: [GroupStreamEventMessage];
  disconnect: [Error];
};

export class DeviceLink extends EventEmitter<DeviceLinkEvents> {
  private socket?: net.Socket;
  private buffer = "";
  private sequence = 0;
  private pending = new Map<string, PendingRequest>();
  private heartbeatTimer?: NodeJS.Timeout;

  async connect(host: string, port: number): Promise<void> {
    this.close();
    await new Promise<void>((resolve, reject) => {
      const socket = net.createConnection({ host, port });
      let settled = false;
      const finish = (error?: Error) => {
        if (settled) {
          return;
        }
        settled = true;
        if (error) {
          this.close();
          reject(error);
        } else {
          resolve();
        }
      };

      this.socket = socket;
      socket.setNoDelay(true);
      socket.setTimeout(0);
      socket.once("connect", () => {
        this.sendCommand({ v: 1, requestId: this.nextId("hello"), type: "hello" })
          .then((message) => {
            if (message.type !== "hello_ack") {
              finish(new Error("TCP hello did not receive hello_ack"));
              return;
            }
            this.startHeartbeat();
            finish();
          })
          .catch(finish);
      });
      socket.on("data", (chunk) => {
        try {
          this.pushLines(chunk);
        } catch (error) {
          const reason = error instanceof Error ? error : new Error("Invalid TCP line");
          finish(reason);
          this.handleDisconnect(reason);
        }
      });
      socket.on("error", (error) => {
        finish(error);
        this.handleDisconnect(error);
      });
      socket.on("close", () => {
        this.handleDisconnect(new Error("TCP device disconnected"));
      });
    });
  }

  async sendCommand(message: GroupStreamCommandMessage, timeoutMs = requestTimeoutMs): Promise<GroupStreamEventMessage> {
    if (!this.socket || !this.socket.writable) {
      throw new Error("TCP device is not connected");
    }
    if (typeof message.requestId !== "string" || message.requestId.length === 0) {
      throw new Error("TCP message requestId is required");
    }
    if (message.type === "ping") {
      this.writeLine(message);
      return { v: 1, type: "pong", requestId: message.requestId };
    }
    if (this.pending.has(message.requestId)) {
      throw new Error(`Duplicate TCP requestId ${message.requestId}`);
    }
    return new Promise<GroupStreamEventMessage>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(message.requestId);
        reject(new Error(`TCP ${message.type} timed out after ${timeoutMs}ms`));
      }, timeoutMs);
      this.pending.set(message.requestId, { requestId: message.requestId, resolve, reject, timer });
      try {
        this.writeLine(message);
      } catch (error) {
        clearTimeout(timer);
        this.pending.delete(message.requestId);
        reject(error instanceof Error ? error : new Error("TCP write failed"));
      }
    });
  }

  close(): void {
    this.stopHeartbeat();
    this.buffer = "";
    for (const pending of this.pending.values()) {
      clearTimeout(pending.timer);
      pending.reject(new Error("TCP device disconnected"));
    }
    this.pending.clear();
    if (this.socket) {
      this.socket.removeAllListeners();
      this.socket.destroy();
      this.socket = undefined;
    }
  }

  nextRequestId(prefix: string): string {
    return this.nextId(prefix);
  }

  private pushLines(chunk: Buffer): void {
    this.buffer += chunk.toString("utf8").replace(/\r/g, "");
    while (true) {
      const newline = this.buffer.indexOf("\n");
      if (newline < 0) {
        if (Buffer.byteLength(this.buffer, "utf8") > MAX_TCP_MESSAGE_BYTES) {
          throw new Error("TCP inbound line is too large");
        }
        return;
      }
      const line = this.buffer.slice(0, newline);
      this.buffer = this.buffer.slice(newline + 1);
      if (Buffer.byteLength(line, "utf8") > MAX_TCP_MESSAGE_BYTES) {
        throw new Error("TCP inbound line is too large");
      }
      if (line.trim().length === 0) {
        continue;
      }
      this.handleMessage(JSON.parse(line) as GroupStreamEventMessage);
    }
  }

  private handleMessage(message: GroupStreamEventMessage): void {
    const requestId = typeof (message as { requestId?: unknown }).requestId === "string"
      ? (message as { requestId: string }).requestId
      : undefined;
    if (requestId) {
      const pending = this.pending.get(requestId);
      if (pending && isTerminalResponse(message)) {
        clearTimeout(pending.timer);
        this.pending.delete(requestId);
        pending.resolve(message);
      }
    }
    this.emit("message", message);
  }

  private handleDisconnect(error: Error): void {
    for (const pending of this.pending.values()) {
      clearTimeout(pending.timer);
      pending.reject(error);
    }
    this.pending.clear();
    this.stopHeartbeat();
    if (this.socket) {
      this.socket.removeAllListeners();
      this.socket.destroy();
      this.socket = undefined;
    }
    this.emit("disconnect", error);
  }

  private startHeartbeat(): void {
    this.stopHeartbeat();
    this.heartbeatTimer = setInterval(() => {
      if (this.socket?.writable) {
        this.writeLine({ v: 1, requestId: this.nextId("ping"), type: "ping" });
      }
    }, heartbeatIntervalMs);
  }

  private stopHeartbeat(): void {
    if (this.heartbeatTimer) {
      clearInterval(this.heartbeatTimer);
      this.heartbeatTimer = undefined;
    }
  }

  private writeLine(message: GroupStreamCommandMessage): void {
    if (!this.socket || !this.socket.writable) {
      throw new Error("TCP device is not connected");
    }
    this.socket.write(`${JSON.stringify(message)}\n`, "utf8");
  }

  private nextId(prefix: string): string {
    this.sequence += 1;
    return `${prefix}-${Date.now().toString(36)}-${this.sequence.toString(36)}`;
  }
}

function isTerminalResponse(message: GroupStreamEventMessage): boolean {
  return terminalResponseTypes.has(message.type);
}
