import { EventEmitter } from "node:events";
import net from "node:net";
import type {
  ExecuteBlockMessage,
  MotionBlockRequest,
  MotionProtocolCommandMessage,
  MotionProtocolEventMessage,
} from "../../../shared/protocol/protocolTypes.js";
import {
  MAX_TCP_MESSAGE_BYTES,
  MOTION_PROTOCOL_RESPONSE_TYPES,
} from "../../../shared/protocol/protocolTypes.js";

const requestTimeoutMs = 5000;
const connectTimeoutMs = 5000;
const handshakeTimeoutMs = 3000;
const blockAckTimeoutMs = 3000;
const writeTimeoutMs = 2000;
const heartbeatIntervalMs = 1000;
const responseTypes = new Set<string>(MOTION_PROTOCOL_RESPONSE_TYPES);
const mutatingCommands = new Set<MotionProtocolCommandMessage["type"]>([
  "execute_block",
  "finish_task",
  "cancel",
  "reset_fault",
]);

type PendingRequest = {
  commandType: MotionProtocolCommandMessage["type"];
  terminalTypes: Set<string>;
  resolve: (message: MotionProtocolEventMessage) => void;
  reject: (error: Error) => void;
  timer?: NodeJS.Timeout;
};

type ActiveBlock = Pick<MotionBlockRequest, "blockId" | "seq">;
type LinkState = "connected" | "uncertain";

type DeviceLinkEvents = {
  message: [MotionProtocolEventMessage];
  disconnect: [Error];
};

export class DeviceLink extends EventEmitter<DeviceLinkEvents> {
  private socket?: net.Socket;
  private buffer = "";
  private sequence = 0;
  private pending = new Map<string, PendingRequest>();
  private heartbeatTimer?: NodeJS.Timeout;
  private commandQueue: Promise<unknown> = Promise.resolve();
  private activeBlock?: ActiveBlock;
  private pendingBlock?: ActiveBlock;
  private linkState: LinkState = "connected";
  private heartbeatInFlight = false;

  async connect(host: string, port: number): Promise<void> {
    this.close();
    await new Promise<void>((resolve, reject) => {
      const socket = net.createConnection({ host, port });
      let settled = false;
      const timer = setTimeout(() => finish(new Error(`connect_timeout: TCP connect timed out after ${connectTimeoutMs}ms`)), connectTimeoutMs);
      const finish = (error?: Error) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        error ? reject(error) : resolve();
      };
      this.socket = socket;
      socket.setNoDelay(true);
      socket.setTimeout(0);
      socket.once("connect", () => finish());
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
      socket.on("close", () => this.handleDisconnect(new Error("TCP device disconnected")));
    });
    try {
      const response = await this.sendRequest(
        { v: 1, requestId: this.nextId("handshake"), type: "handshake" },
        handshakeTimeoutMs,
      );
      if (response.type !== "handshake_ack") {
        throw new Error("handshake_unexpected_response: expected handshake_ack");
      }
      this.startHeartbeat();
    } catch (error) {
      this.close();
      throw error;
    }
  }

  sendCommand(message: MotionProtocolCommandMessage, timeoutMs = defaultTimeoutFor(message)): Promise<MotionProtocolEventMessage> {
    if (!this.socket?.writable) {
      return Promise.reject(new Error("TCP device is not connected"));
    }
    if (!message.requestId) {
      return Promise.reject(new Error("TCP message requestId is required"));
    }
    if (message.type === "emergency_stop") {
      return this.sendRequest(message, timeoutMs);
    }
    if (message.type === "finish_task" && this.activeBlock) {
      return Promise.reject(new Error(`device_busy: active block ${this.activeBlock.blockId}/${this.activeBlock.seq} is still running`));
    }
    const operation = message.type === "execute_block"
      ? () => this.submitBlock(message, timeoutMs)
      : () => this.sendRequest(this.attachActiveBlockToCancel(message), timeoutMs);
    return mutatingCommands.has(message.type) ? this.enqueue(operation) : operation();
  }

  state(): LinkState {
    return this.linkState;
  }

  currentActiveBlock(): ActiveBlock | undefined {
    return this.activeBlock ? { ...this.activeBlock } : undefined;
  }

  close(): void {
    this.stopHeartbeat();
    this.buffer = "";
    this.activeBlock = undefined;
    this.pendingBlock = undefined;
    this.linkState = "connected";
    this.heartbeatInFlight = false;
    for (const pending of this.pending.values()) {
      if (pending.timer) clearTimeout(pending.timer);
      pending.reject(new Error("TCP device disconnected"));
    }
    this.pending.clear();
    this.socket?.removeAllListeners();
    this.socket?.destroy();
    this.socket = undefined;
  }

  private async submitBlock(message: ExecuteBlockMessage, timeoutMs: number): Promise<MotionProtocolEventMessage> {
    if (this.activeBlock) {
      throw new Error(`device_busy: active block ${this.activeBlock.blockId}/${this.activeBlock.seq} is still running`);
    }
    const block = { blockId: message.blockId, seq: message.seq };
    this.pendingBlock = block;
    this.activeBlock = block;
    try {
      const response = await this.sendRequest(message, timeoutMs);
      if (response.type === "block_ack") {
        this.linkState = "connected";
      } else if (sameBlock(this.activeBlock, block)) {
        this.activeBlock = undefined;
      }
      return response;
    } catch (error) {
      this.linkState = "uncertain";
      throw error;
    } finally {
      if (sameBlock(this.pendingBlock, block)) this.pendingBlock = undefined;
    }
  }

  private sendRequest(message: MotionProtocolCommandMessage, timeoutMs: number): Promise<MotionProtocolEventMessage> {
    if (this.pending.has(message.requestId)) {
      return Promise.reject(new Error(`Duplicate TCP requestId ${message.requestId}`));
    }
    return new Promise((resolve, reject) => {
      this.pending.set(message.requestId, {
        commandType: message.type,
        terminalTypes: terminalTypesFor(message.type),
        resolve,
        reject,
      });
      this.writeLine(message).then(() => {
        const pending = this.pending.get(message.requestId);
        if (!pending) return;
        pending.timer = setTimeout(() => {
          this.pending.delete(message.requestId);
          reject(new Error(`${timeoutCode(message.type)}: TCP ${message.type} timed out after ${timeoutMs}ms`));
        }, timeoutMs);
      }).catch((error) => {
        this.pending.delete(message.requestId);
        reject(error instanceof Error ? error : new Error("TCP write failed"));
      });
    });
  }

  private pushLines(chunk: Buffer): void {
    this.buffer += chunk.toString("utf8").replace(/\r/g, "");
    while (true) {
      const newline = this.buffer.indexOf("\n");
      if (newline < 0) {
        if (Buffer.byteLength(this.buffer, "utf8") > MAX_TCP_MESSAGE_BYTES) throw new Error("TCP inbound line is too large");
        return;
      }
      const line = this.buffer.slice(0, newline);
      this.buffer = this.buffer.slice(newline + 1);
      if (Buffer.byteLength(line, "utf8") > MAX_TCP_MESSAGE_BYTES) throw new Error("TCP inbound line is too large");
      if (line.trim()) this.handleMessage(JSON.parse(line) as MotionProtocolEventMessage);
    }
  }

  private handleMessage(message: MotionProtocolEventMessage): void {
    const requestId = "requestId" in message ? message.requestId : undefined;
    if (requestId) {
      const pending = this.pending.get(requestId);
      if (pending && responseTypes.has(message.type) && pending.terminalTypes.has(message.type)) {
        if (pending.timer) clearTimeout(pending.timer);
        this.pending.delete(requestId);
        pending.resolve(message);
      }
    }
    if (message.type === "block_result" && sameBlock(this.activeBlock, message)) {
      this.activeBlock = undefined;
      this.linkState = "connected";
    } else if (message.type === "fault" || (message.type === "emergency_stop_result" && message.ok)) {
      this.activeBlock = undefined;
      this.pendingBlock = undefined;
      this.linkState = "uncertain";
    } else if (message.type === "reset_fault_result" && message.ok) {
      this.activeBlock = undefined;
      this.pendingBlock = undefined;
      this.linkState = "connected";
    }
    this.emit("message", message);
  }

  private handleDisconnect(error: Error): void {
    if (!this.socket) return;
    for (const pending of this.pending.values()) {
      if (pending.timer) clearTimeout(pending.timer);
      pending.reject(error);
    }
    this.pending.clear();
    this.activeBlock = undefined;
    this.pendingBlock = undefined;
    this.stopHeartbeat();
    this.socket.removeAllListeners();
    this.socket.destroy();
    this.socket = undefined;
    this.emit("disconnect", error);
  }

  private startHeartbeat(): void {
    this.stopHeartbeat();
    this.heartbeatTimer = setInterval(() => {
      if (!this.socket?.writable || this.heartbeatInFlight || this.pending.size > 0) return;
      this.heartbeatInFlight = true;
      this.sendCommand({ v: 1, requestId: this.nextId("heartbeat"), type: "heartbeat" }, 1500)
        .catch((error) => this.handleDisconnect(error instanceof Error ? error : new Error("TCP heartbeat failed")))
        .finally(() => { this.heartbeatInFlight = false; });
    }, heartbeatIntervalMs);
  }

  private stopHeartbeat(): void {
    if (this.heartbeatTimer) clearInterval(this.heartbeatTimer);
    this.heartbeatTimer = undefined;
  }

  private writeLine(message: MotionProtocolCommandMessage): Promise<void> {
    if (!this.socket?.writable) return Promise.reject(new Error("TCP device is not connected"));
    const socket = this.socket;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error(`transport_write_timeout: TCP write timed out after ${writeTimeoutMs}ms`)), writeTimeoutMs);
      socket.write(`${JSON.stringify(message)}\n`, "utf8", (error) => {
        clearTimeout(timer);
        error ? reject(error) : resolve();
      });
    });
  }

  private attachActiveBlockToCancel(message: MotionProtocolCommandMessage): MotionProtocolCommandMessage {
    if (message.type !== "cancel" || !this.activeBlock) return message;
    return { ...message, ...this.activeBlock };
  }

  private enqueue(operation: () => Promise<MotionProtocolEventMessage>): Promise<MotionProtocolEventMessage> {
    const run = this.commandQueue.then(operation, operation);
    this.commandQueue = run.catch(() => undefined);
    return run;
  }

  private nextId(prefix: string): string {
    this.sequence += 1;
    return `${prefix}-${Date.now().toString(36)}-${this.sequence.toString(36)}`;
  }
}

function terminalTypesFor(command: MotionProtocolCommandMessage["type"]): Set<string> {
  const expected: Record<MotionProtocolCommandMessage["type"], string> = {
    handshake: "handshake_ack",
    get_snapshot: "snapshot",
    execute_block: "block_ack",
    cancel: "cancel_result",
    finish_task: "finish_task_result",
    emergency_stop: "emergency_stop_result",
    reset_fault: "reset_fault_result",
    heartbeat: "heartbeat_ack",
  };
  return new Set([expected[command], "protocol_error"]);
}

function defaultTimeoutFor(message: MotionProtocolCommandMessage): number {
  return message.type === "execute_block" ? blockAckTimeoutMs : requestTimeoutMs;
}

function timeoutCode(command: MotionProtocolCommandMessage["type"]): string {
  return command === "execute_block" ? "block_ack_timeout" : "request_timeout";
}

function sameBlock(left: ActiveBlock | undefined, right: ActiveBlock): boolean {
  return left?.blockId === right.blockId && left.seq === right.seq;
}
