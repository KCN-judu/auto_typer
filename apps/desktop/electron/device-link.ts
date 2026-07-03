import { EventEmitter } from "node:events";
import net from "node:net";

export type RemoteMotionProfile = {
  rpm?: number;
  accelRaw?: number;
  timeoutMs?: number;
};

export type RemoteMotionStep =
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
  timeoutMs?: number;
};

export type GroupStreamCommandMessage =
  | HelloMessage
  | { v: 1; id: string; type: "exec_group"; groupId: string; steps: RemoteMotionStep[]; timeoutMs?: number }
  | { v: 1; id: string; type: "task_end"; taskId: string; totalGroups: number; completedGroups: number; warnCount: number; timeoutMs?: number }
  | { v: 1; id: string; type: "cancel"; timeoutMs?: number }
  | { v: 1; id: string; type: "reset_fault"; timeoutMs?: number }
  | { v: 1; id: string; type: "probe"; timeoutMs?: number }
  | { v: 1; id: string; type: "ping"; timeoutMs?: number };

export type AckMessage = {
  v: 1;
  type: "ack";
  id: string;
  ok: boolean;
  accepted: boolean;
  code?: string;
  message?: string;
};

export type GroupStreamEventMessage =
  | AckMessage
  | {
      v: 1;
      type: "group_started" | "group_done" | "group_warn" | "fault" | "telemetry" | "pong" | "snapshot";
      [key: string]: unknown;
    };

const maxLineBytes = 4096;
const ackTimeoutMs = 1000;
const helloTimeoutMs = 5000;
const execGroupAckTimeoutMs = 5000;
const heartbeatIntervalMs = 1000;

type PendingAck = {
  id: string;
  type: GroupStreamCommandMessage["type"];
  resolve: (message: AckMessage) => void;
  reject: (error: Error) => void;
  timer: NodeJS.Timeout;
  timeoutMs: number;
};

type DeviceLinkEvents = {
  message: [GroupStreamEventMessage];
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
              finish(new Error(ack.message ?? ack.code ?? "Group stream hello rejected"));
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
          const reason = error instanceof Error ? error : new Error("Invalid group stream line");
          finish(reason);
          this.handleDisconnect(reason);
        }
      });
      socket.on("error", (error) => {
        finish(error);
        this.handleDisconnect(error);
      });
      socket.on("close", () => {
        this.handleDisconnect(new Error("Group stream disconnected"));
      });
    });
  }

  async sendCommand(message: GroupStreamCommandMessage, timeoutMs = timeoutForMessage(message)): Promise<AckMessage> {
    if (!this.socket || !this.socket.writable) {
      throw new Error("Group stream is not connected");
    }
    if (message.type === "ping") {
      this.writeLine(message);
      return { v: 1, type: "ack", id: message.id, ok: true, accepted: true };
    }
    if (this.pending) {
      throw new Error("Another group stream command is in flight");
    }
    return new Promise<AckMessage>((resolve, reject) => {
      const timer = setTimeout(() => {
        if (this.pending?.id === message.id) {
          this.pending = undefined;
        }
        this.close();
        reject(new Error(this.timeoutMessage(message.type, timeoutMs)));
      }, timeoutMs);
      this.pending = {
        id: message.id,
        type: message.type,
        resolve,
        reject,
        timer,
        timeoutMs,
      };
      try {
        this.writeLine(message);
      } catch (error) {
        clearTimeout(timer);
        if (this.pending?.id === message.id) {
          this.pending = undefined;
        }
        reject(error instanceof Error ? error : new Error("Group stream write failed"));
      }
    });
  }

  close(): void {
    this.stopHeartbeat();
    this.buffer = "";
    if (this.pending) {
      clearTimeout(this.pending.timer);
      this.pending.reject(new Error("Group stream disconnected"));
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
          throw new Error("Group stream inbound line is too large");
        }
        return;
      }
      const line = this.buffer.slice(0, newline);
      this.buffer = this.buffer.slice(newline + 1);
      if (Buffer.byteLength(line, "utf8") > maxLineBytes) {
        throw new Error("Group stream inbound line is too large");
      }
      if (line.trim().length === 0) {
        continue;
      }
      const message = JSON.parse(line) as GroupStreamEventMessage;
      this.handleMessage(message);
    }
  }

  private handleMessage(message: GroupStreamEventMessage): void {
    if (message.type === "ack") {
      this.handleAck(message);
    } else if (message.type === "fault") {
      this.handleHandshakeFault(message);
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

  private handleHandshakeFault(message: GroupStreamEventMessage): void {
    const pending = this.pending;
    if (!pending || pending.type !== "hello") {
      return;
    }
    clearTimeout(pending.timer);
    this.pending = undefined;
    pending.reject(new Error(this.faultMessage(message)));
  }

  private handleDisconnect(error: Error): void {
    if (this.pending) {
      const pending = this.pending;
      clearTimeout(this.pending.timer);
      this.pending = undefined;
      pending.reject(this.disconnectError(pending, error));
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

  private writeLine(message: GroupStreamCommandMessage): void {
    if (!this.socket || !this.socket.writable) {
      throw new Error("Group stream is not connected");
    }
    this.socket.write(`${JSON.stringify(message)}\n`, "utf8");
  }

  private nextId(prefix: string): string {
    this.sequence += 1;
    return `${prefix}-${Date.now().toString(36)}-${this.sequence.toString(36)}`;
  }

  private timeoutMessage(type: GroupStreamCommandMessage["type"], timeoutMs: number): string {
    if (type === "hello") {
      return `Group stream hello ack timed out after ${timeoutMs}ms`;
    }
    return `Group stream ${type} ack timed out after ${timeoutMs}ms`;
  }

  private disconnectError(pending: PendingAck, error: Error): Error {
    if (pending.type === "hello") {
      return new Error(`Group stream disconnected before hello ack: ${error.message || "unknown reason"}`);
    }
    if (pending.type === "exec_group") {
      return new Error(`Group stream disconnected before ${pending.type} ack: ${error.message || "unknown reason"}`);
    }
    return error;
  }

  private faultMessage(message: GroupStreamEventMessage): string {
    const code = typeof message.code === "string" && message.code.length > 0 ? message.code : "fault";
    const text = typeof message.message === "string" && message.message.length > 0 ? message.message : "Group stream fault";
    return `Group stream hello rejected: ${code}: ${text}`;
  }
}

function timeoutForMessage(message: GroupStreamCommandMessage): number {
  if (message.type === "hello") {
    return helloTimeoutMs;
  }
  if (message.type === "exec_group") {
    return execGroupAckTimeoutMs;
  }
  return ackTimeoutMs;
}
