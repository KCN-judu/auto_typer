import { EventEmitter } from "node:events";
import net from "node:net";

export type RemoteMotionProfile = {
  rpm?: number;
  accelRaw?: number;
  timeoutMs?: number;
};

export type RemoteMotionBlock =
  | { kind: "move_xy"; dxMm: number; dyMm: number; profile?: RemoteMotionProfile }
  | { kind: "servo_press" }
  | { kind: "servo_release" }
  | { kind: "character_release" }
  | { kind: "line_feed" }
  | { kind: "wait"; durationMs: number };

export type HelloMessage = {
  v: 1;
  id: string;
  type: "hello";
  client: "desktop";
};

export type BlockStreamCommandMessage =
  | HelloMessage
  | { v: 1; id: string; type: "exec_block"; blockId: string; block: RemoteMotionBlock }
  | { v: 1; id: string; type: "cancel" }
  | { v: 1; id: string; type: "reset_fault" }
  | { v: 1; id: string; type: "probe" }
  | { v: 1; id: string; type: "ping" };

export type AckMessage = {
  v: 1;
  type: "ack";
  id: string;
  ok: boolean;
  accepted: boolean;
  code?: string;
  message?: string;
};

export type BlockStreamEventMessage =
  | AckMessage
  | {
      v: 1;
      type: "block_started" | "block_done" | "fault" | "telemetry" | "pong" | "snapshot";
      [key: string]: unknown;
    };

const maxLineBytes = 4096;
const ackTimeoutMs = 1000;
const helloTimeoutMs = 1000;
const heartbeatIntervalMs = 1000;

type PendingAck = {
  id: string;
  resolve: (message: AckMessage) => void;
  reject: (error: Error) => void;
  timer: NodeJS.Timeout;
};

type DeviceLinkEvents = {
  message: [BlockStreamEventMessage];
  disconnect: [Error];
};

export class DeviceLink extends EventEmitter<DeviceLinkEvents> {
  private socket?: net.Socket;
  private buffer = "";
  private sequence = 0;
  private pending?: PendingAck;
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
        const hello: HelloMessage = { v: 1, id: this.nextId("hello"), type: "hello", client: "desktop" };
        this.sendCommand(hello, helloTimeoutMs)
          .then((ack) => {
            if (!ack.ok || !ack.accepted) {
              finish(new Error(ack.message ?? ack.code ?? "Block stream hello rejected"));
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
          const reason = error instanceof Error ? error : new Error("Invalid block stream line");
          finish(reason);
          this.handleDisconnect(reason);
        }
      });
      socket.on("error", (error) => {
        finish(error);
        this.handleDisconnect(error);
      });
      socket.on("close", () => {
        this.handleDisconnect(new Error("Block stream disconnected"));
      });
    });
  }

  async sendCommand(message: BlockStreamCommandMessage, timeoutMs = ackTimeoutMs): Promise<AckMessage> {
    if (!this.socket || !this.socket.writable) {
      throw new Error("Block stream is not connected");
    }
    if (message.type === "ping") {
      this.writeLine(message);
      return { v: 1, type: "ack", id: message.id, ok: true, accepted: true };
    }
    if (this.pending) {
      throw new Error("Another block stream command is in flight");
    }
    return new Promise<AckMessage>((resolve, reject) => {
      const timer = setTimeout(() => {
        if (this.pending?.id === message.id) {
          this.pending = undefined;
        }
        this.close();
        reject(new Error("Block stream ack timed out"));
      }, timeoutMs);
      this.pending = {
        id: message.id,
        resolve,
        reject,
        timer,
      };
      try {
        this.writeLine(message);
      } catch (error) {
        clearTimeout(timer);
        if (this.pending?.id === message.id) {
          this.pending = undefined;
        }
        reject(error instanceof Error ? error : new Error("Block stream write failed"));
      }
    });
  }

  close(): void {
    this.stopHeartbeat();
    this.buffer = "";
    if (this.pending) {
      clearTimeout(this.pending.timer);
      this.pending.reject(new Error("Block stream disconnected"));
      this.pending = undefined;
    }
    if (this.socket) {
      this.socket.removeAllListeners();
      this.socket.destroy();
      this.socket = undefined;
    }
  }

  private pushLines(chunk: Buffer): void {
    this.buffer += chunk.toString("utf8").replace(/\r/g, "");
    while (true) {
      const newline = this.buffer.indexOf("\n");
      if (newline < 0) {
        if (Buffer.byteLength(this.buffer, "utf8") > maxLineBytes) {
          throw new Error("Block stream inbound line is too large");
        }
        return;
      }
      const line = this.buffer.slice(0, newline);
      this.buffer = this.buffer.slice(newline + 1);
      if (Buffer.byteLength(line, "utf8") > maxLineBytes) {
        throw new Error("Block stream inbound line is too large");
      }
      if (line.trim().length === 0) {
        continue;
      }
      const message = JSON.parse(line) as BlockStreamEventMessage;
      this.handleMessage(message);
    }
  }

  private handleMessage(message: BlockStreamEventMessage): void {
    if (message.type === "ack") {
      this.handleAck(message);
    }
    this.emit("message", message);
  }

  private handleAck(message: AckMessage): void {
    const pending = this.pending;
    if (!pending || pending.id !== message.id) {
      return;
    }
    clearTimeout(pending.timer);
    this.pending = undefined;
    pending.resolve(message);
  }

  private handleDisconnect(error: Error): void {
    if (this.pending) {
      clearTimeout(this.pending.timer);
      this.pending.reject(error);
      this.pending = undefined;
    }
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
        this.writeLine({ v: 1, id: this.nextId("ping"), type: "ping" });
      }
    }, heartbeatIntervalMs);
  }

  private stopHeartbeat(): void {
    if (this.heartbeatTimer) {
      clearInterval(this.heartbeatTimer);
      this.heartbeatTimer = undefined;
    }
  }

  private writeLine(message: BlockStreamCommandMessage): void {
    if (!this.socket || !this.socket.writable) {
      throw new Error("Block stream is not connected");
    }
    this.socket.write(`${JSON.stringify(message)}\n`, "utf8");
  }

  private nextId(prefix: string): string {
    this.sequence += 1;
    return `${prefix}-${Date.now().toString(36)}-${this.sequence.toString(36)}`;
  }
}
