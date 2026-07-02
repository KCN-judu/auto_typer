import { EventEmitter } from "node:events";
import net from "node:net";

export type PrimitiveCommand =
  | {
      v: 1;
      type: "command";
      id: string;
      op: "move_to";
      xMm: number;
      yMm: number;
      profile?: {
        rpm?: number;
        accelRaw?: number;
        timeoutMs?: number;
      };
    }
  | {
      v: 1;
      type: "command";
      id: string;
      op: "wait";
      durationMs: number;
    }
  | {
      v: 1;
      type: "command";
      id: string;
      op: "press" | "release";
      durationMs?: number;
    }
  | {
      v: 1;
      type: "command";
      id: string;
      op: "character_release" | "line_feed" | "cancel" | "reset_fault" | "emergency_stop";
    };

export type AckMessage = {
  v: 1;
  type: "ack";
  id: string;
  ok: boolean;
  accepted: boolean;
  code?: string;
  message?: string;
};

type DoneMessage = {
  v: 1;
  type: "done";
  id: string;
  op?: PrimitiveCommand["op"];
  durationMs?: number;
  currentPoint?: { xMm: number; yMm: number };
};

export type BlockStreamEventMessage =
  | AckMessage
  | DoneMessage
  | {
      v: 1;
      type: "hello_ack" | "snapshot" | "telemetry" | "fault" | "pong";
      [key: string]: unknown;
    };

export type BlockStreamCommandMessage = PrimitiveCommand | { v: 1; type: "hello"; client: "desktop" } | { v: 1; type: "ping" };

const magic0 = 0x41;
const magic1 = 0x54;
const protocolVersion = 1;
const headerLength = 16;
const maxPayloadBytes = 4096;
const ackTimeoutMs = 1000;
const doneTimeoutPaddingMs = 2000;
const helloTimeoutMs = 1000;
const heartbeatIntervalMs = 1000;

const frameTypes = {
  Hello: 1,
  HelloAck: 2,
  Command: 10,
  Ack: 11,
  Done: 12,
  Fault: 13,
  Telemetry: 20,
  Snapshot: 21,
  Ping: 30,
  Pong: 31,
} as const;

type PendingCommand = {
  command: PrimitiveCommand;
  resolve: (message: AckMessage) => void;
  reject: (error: Error) => void;
  ackTimer: NodeJS.Timeout;
  doneTimer?: NodeJS.Timeout;
};

type DeviceLinkEvents = {
  message: [BlockStreamEventMessage];
  disconnect: [Error];
};

export class DeviceLink extends EventEmitter<DeviceLinkEvents> {
  private socket?: net.Socket;
  private buffer = Buffer.alloc(0);
  private seq = 0;
  private pending?: PendingCommand;
  private heartbeatTimer?: NodeJS.Timeout;

  async connect(host: string, port: number): Promise<void> {
    this.close();
    await new Promise<void>((resolve, reject) => {
      const socket = net.createConnection({ host, port });
      let settled = false;
      const helloTimer = setTimeout(() => {
        finish(new Error("Device link hello timed out"));
      }, helloTimeoutMs);

      const finish = (error?: Error) => {
        if (settled) {
          return;
        }
        settled = true;
        clearTimeout(helloTimer);
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
        this.writeFrame(frameTypes.Hello, { v: 1, type: "hello", client: "desktop" });
      });
      socket.on("data", (chunk) => {
        try {
          this.pushData(chunk, (type, payload) => {
            const message = payload as BlockStreamEventMessage;
            if (type === frameTypes.HelloAck) {
              this.emit("message", message);
              this.startHeartbeat();
              finish();
              return;
            }
            this.handleMessage(type, message);
          });
        } catch (error) {
          const reason = error instanceof Error ? error : new Error("Invalid device link frame");
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

  async sendCommand(command: PrimitiveCommand): Promise<AckMessage> {
    if (!this.socket || !this.socket.writable) {
      throw new Error("Block stream is not connected");
    }
    if (this.pending) {
      throw new Error("Another block stream command is in flight");
    }
    return new Promise<AckMessage>((resolve, reject) => {
      const ackTimer = setTimeout(() => {
        this.pending = undefined;
        this.close();
        reject(new Error("Block stream ack timed out"));
      }, ackTimeoutMs);
      this.pending = {
        command,
        resolve,
        reject,
        ackTimer,
      };
      this.writeFrame(frameTypes.Command, command, (error) => {
        if (!error) {
          return;
        }
        const pending = this.pending;
        if (pending && pending.command.id === command.id) {
          clearTimeout(pending.ackTimer);
          this.pending = undefined;
        }
        reject(error);
      });
    });
  }

  close(): void {
    this.stopHeartbeat();
    this.buffer = Buffer.alloc(0);
    if (this.pending) {
      clearTimeout(this.pending.ackTimer);
      if (this.pending.doneTimer) {
        clearTimeout(this.pending.doneTimer);
      }
      this.pending.reject(new Error("Block stream disconnected"));
      this.pending = undefined;
    }
    if (this.socket) {
      this.socket.removeAllListeners();
      this.socket.destroy();
      this.socket = undefined;
    }
  }

  private handleMessage(type: number, message: BlockStreamEventMessage): void {
    if (type === frameTypes.Ack && message.type === "ack") {
      this.handleAck(message);
    } else if (type === frameTypes.Done && message.type === "done") {
      this.handleDone(message);
    }
    this.emit("message", message);
  }

  private handleAck(message: AckMessage): void {
    const pending = this.pending;
    if (!pending || pending.command.id !== message.id) {
      return;
    }
    clearTimeout(pending.ackTimer);
    pending.resolve(message);
    if (!message.ok && !message.accepted) {
      this.pending = undefined;
      return;
    }
    const timeoutMs = commandTimeoutMs(pending.command) + doneTimeoutPaddingMs;
    pending.doneTimer = setTimeout(() => {
      this.pending = undefined;
      this.close();
      this.emit("message", {
        v: 1,
        type: "fault",
        id: pending.command.id,
        code: "done_timeout",
        message: "Block stream done timed out",
      });
    }, timeoutMs);
  }

  private handleDone(message: DoneMessage): void {
    const pending = this.pending;
    if (!pending || pending.command.id !== message.id) {
      return;
    }
    if (pending.doneTimer) {
      clearTimeout(pending.doneTimer);
    }
    this.pending = undefined;
  }

  private handleDisconnect(error: Error): void {
    if (this.pending) {
      clearTimeout(this.pending.ackTimer);
      if (this.pending.doneTimer) {
        clearTimeout(this.pending.doneTimer);
      }
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
        this.writeFrame(frameTypes.Ping, { v: 1, type: "ping" });
      }
    }, heartbeatIntervalMs);
  }

  private stopHeartbeat(): void {
    if (this.heartbeatTimer) {
      clearInterval(this.heartbeatTimer);
      this.heartbeatTimer = undefined;
    }
  }

  private writeFrame(type: number, value: unknown, callback?: (error?: Error | null) => void): void {
    if (!this.socket || !this.socket.writable) {
      callback?.(new Error("Block stream is not connected"));
      return;
    }
    this.seq = (this.seq + 1) >>> 0;
    const payload = Buffer.from(JSON.stringify(value), "utf8");
    if (payload.length > maxPayloadBytes) {
      callback?.(new Error("Device link payload is too large"));
      return;
    }
    const frame = Buffer.alloc(headerLength + payload.length);
    frame[0] = magic0;
    frame[1] = magic1;
    frame[2] = protocolVersion;
    frame[3] = headerLength;
    frame.writeUInt16LE(type, 4);
    frame.writeUInt16LE(0, 6);
    frame.writeUInt32LE(this.seq, 8);
    frame.writeUInt32LE(payload.length, 12);
    payload.copy(frame, headerLength);
    this.socket.write(frame, callback);
  }

  private pushData(chunk: Buffer, visit: (type: number, payload: unknown) => void): void {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    while (this.buffer.length >= headerLength) {
      if (this.buffer[0] !== magic0 || this.buffer[1] !== magic1) {
        throw new Error("Invalid device link magic");
      }
      if (this.buffer[2] !== protocolVersion || this.buffer[3] !== headerLength) {
        throw new Error("Unsupported device link frame header");
      }
      const type = this.buffer.readUInt16LE(4);
      const payloadLength = this.buffer.readUInt32LE(12);
      if (payloadLength > maxPayloadBytes) {
        throw new Error("Device link payload is too large");
      }
      const frameLength = headerLength + payloadLength;
      if (this.buffer.length < frameLength) {
        return;
      }
      const payload = JSON.parse(this.buffer.subarray(headerLength, frameLength).toString("utf8")) as unknown;
      this.buffer = this.buffer.subarray(frameLength);
      visit(type, payload);
    }
  }
}

function commandTimeoutMs(command: PrimitiveCommand): number {
  if (command.op === "move_to" && command.profile?.timeoutMs) {
    return command.profile.timeoutMs;
  }
  if (command.op === "wait") {
    return command.durationMs;
  }
  if ((command.op === "press" || command.op === "release") && command.durationMs) {
    return command.durationMs;
  }
  if (command.op === "cancel" || command.op === "reset_fault" || command.op === "emergency_stop") {
    return 3000;
  }
  return 30000;
}
