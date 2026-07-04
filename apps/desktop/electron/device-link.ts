import { EventEmitter } from "node:events";
import net from "node:net";
import type {
  ExecGroupMessage,
  GroupStreamCommandMessage,
  GroupStreamEventMessage,
  TaskGroup,
} from "../../../shared/protocol/auto-typer-protocol.js";
import {
  MAX_TCP_MESSAGE_BYTES,
  TCP_TERMINAL_RESPONSE_TYPES,
} from "../../../shared/protocol/auto-typer-protocol.js";

const requestTimeoutMs = 5000;
const tcpConnectTimeoutMs = 5000;
const helloTimeoutMs = 3000;
const groupAdmissionTimeoutMs = 3000;
const writeTimeoutMs = 2000;
const heartbeatIntervalMs = 1000;
const terminalResponseTypes = new Set<string>(TCP_TERMINAL_RESPONSE_TYPES);
const mutatingCommandTypes = new Set<string>([
  "exec_group",
  "finish_task",
  "cancel",
  "reset_fault",
  "release_line_feed_origin",
  "probe",
  "press_diag_m5",
]);

type PendingRequest = {
  requestId: string;
  commandType: GroupStreamCommandMessage["type"];
  terminalTypes: Set<string>;
  resolve: (message: GroupStreamEventMessage) => void;
  reject: (error: Error) => void;
  timer?: NodeJS.Timeout;
};

type ActiveGroup = Pick<TaskGroup, "groupId" | "seq">;
type LinkState = "connected" | "desync_pending";

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
  private commandQueue: Promise<unknown> = Promise.resolve();
  private activeGroup?: ActiveGroup;
  private pendingAdmission?: ActiveGroup;
  private linkState: LinkState = "connected";
  private cancelInFlight = false;
  private cancelRequestedFor?: ActiveGroup;
  private pingInFlight = false;

  async connect(host: string, port: number): Promise<void> {
    this.close();
    this.logPhase("tcp_connect_start", `host=${host} port=${port}`);
    try {
      await new Promise<void>((resolve, reject) => {
        const socket = net.createConnection({ host, port });
        let settled = false;
        const connectTimer = setTimeout(
          () => finish(new Error(`connect_timeout: TCP connect timed out after ${tcpConnectTimeoutMs}ms`)),
          tcpConnectTimeoutMs,
        );
        const finish = (error?: Error) => {
          if (settled) {
            return;
          }
          settled = true;
          clearTimeout(connectTimer);
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
          this.logPhase("tcp_connect_success");
          finish();
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
      const message = await this.sendHello();
      if (message.type !== "hello_ack") {
        throw new Error("hello_unexpected_response: TCP hello did not receive hello_ack");
      }
      this.startHeartbeat();
      this.logPhase("connect_resolved");
    } catch (error) {
      const reason = error instanceof Error ? error : new Error("TCP connect failed");
      this.logPhase("connect_rejected", reason.message);
      this.close();
      throw reason;
    }
  }

  async sendCommand(message: GroupStreamCommandMessage, timeoutMs = defaultTimeoutFor(message)): Promise<GroupStreamEventMessage> {
    if (!this.socket || !this.socket.writable) {
      throw new Error("TCP device is not connected");
    }
    if (typeof message.requestId !== "string" || message.requestId.length === 0) {
      throw new Error("TCP message requestId is required");
    }
    if (message.type === "exec_group") {
      return this.enqueueMutatingCommand(() => this.submitGroup(message, timeoutMs));
    }
    if (message.type === "cancel") {
      return this.enqueueMutatingCommand(() => this.cancelGroup(message, timeoutMs));
    }
    if (mutatingCommandTypes.has(message.type)) {
      return this.enqueueMutatingCommand(() => this.sendRequest(message, timeoutMs));
    }
    return this.sendRequest(message, timeoutMs);
  }

  state(): LinkState {
    return this.linkState;
  }

  currentActiveGroup(): ActiveGroup | undefined {
    return this.activeGroup ? { ...this.activeGroup } : undefined;
  }

  private async sendRequest(message: GroupStreamCommandMessage, timeoutMs: number): Promise<GroupStreamEventMessage> {
    if (this.pending.has(message.requestId)) {
      throw new Error(`Duplicate TCP requestId ${message.requestId}`);
    }
    return new Promise<GroupStreamEventMessage>((resolve, reject) => {
      this.pending.set(message.requestId, {
        requestId: message.requestId,
        commandType: message.type,
        terminalTypes: terminalTypesFor(message.type),
        resolve,
        reject,
      });
      this.writeLine(message).then(() => {
        const pending = this.pending.get(message.requestId);
        if (!pending) {
          return;
        }
        pending.timer = setTimeout(() => {
          this.pending.delete(message.requestId);
          reject(new Error(`${timeoutCode(message.type)}: TCP ${message.type} timed out after ${timeoutMs}ms`));
        }, timeoutMs);
      }).catch((error) => {
        const pending = this.pending.get(message.requestId);
        if (pending?.timer) {
          clearTimeout(pending.timer);
        }
        this.pending.delete(message.requestId);
        reject(error instanceof Error ? error : new Error("TCP write failed"));
      });
    });
  }

  private async submitGroup(message: ExecGroupMessage, timeoutMs: number): Promise<GroupStreamEventMessage> {
    if (this.activeGroup) {
      throw new Error(`device_busy: active group ${this.activeGroup.groupId}/${this.activeGroup.seq} is still running`);
    }
    const group = { groupId: message.groupId, seq: message.seq };
    this.pendingAdmission = group;
    try {
      const response = await this.sendRequest(message, timeoutMs);
      if (response.type === "group_accepted") {
        this.activeGroup = group;
        this.linkState = "connected";
      }
      return response;
    } catch (error) {
      this.linkState = "desync_pending";
      throw error;
    } finally {
      if (this.pendingAdmission?.groupId === group.groupId && this.pendingAdmission.seq === group.seq) {
        this.pendingAdmission = undefined;
      }
    }
  }

  private async cancelGroup(message: GroupStreamCommandMessage, timeoutMs: number): Promise<GroupStreamEventMessage> {
    if (this.cancelInFlight) {
      return { v: 1, type: "cancel_result", requestId: message.requestId, ok: false };
    }
    const target = this.activeGroup ?? this.pendingAdmission;
    if (target && this.cancelRequestedFor?.groupId === target.groupId && this.cancelRequestedFor.seq === target.seq) {
      return { v: 1, type: "cancel_result", requestId: message.requestId, ok: false };
    }
    if (target) {
      (message as GroupStreamCommandMessage & { groupId?: string; seq?: number }).groupId = target.groupId;
      (message as GroupStreamCommandMessage & { groupId?: string; seq?: number }).seq = target.seq;
    }
    this.cancelRequestedFor = target;
    this.cancelInFlight = true;
    try {
      return await this.sendRequest(message, timeoutMs);
    } finally {
      this.cancelInFlight = false;
    }
  }

  close(): void {
    this.stopHeartbeat();
    this.buffer = "";
    this.activeGroup = undefined;
    this.pendingAdmission = undefined;
    this.linkState = "connected";
    this.cancelInFlight = false;
    this.cancelRequestedFor = undefined;
    this.pingInFlight = false;
    for (const pending of this.pending.values()) {
      if (pending.timer) {
        clearTimeout(pending.timer);
      }
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
      if (pending && isTerminalResponseForPending(message, pending)) {
        if (pending.timer) {
          clearTimeout(pending.timer);
        }
        this.pending.delete(requestId);
        pending.resolve(message);
      }
    }
    if (message.type === "group_started" && this.linkState === "desync_pending") {
      if (this.matchesPendingOrActiveGroup(message.groupId, message.seq)) {
        this.activeGroup = { groupId: message.groupId, seq: message.seq };
      }
    }
    const finalGroup = finalGroupIdentity(message);
    if (finalGroup) {
      if (this.activeGroup?.groupId === finalGroup.groupId && this.activeGroup.seq === finalGroup.seq) {
        this.activeGroup = undefined;
      }
      if (this.cancelRequestedFor?.groupId === finalGroup.groupId && this.cancelRequestedFor.seq === finalGroup.seq) {
        this.cancelRequestedFor = undefined;
      }
      if (this.pendingAdmission?.groupId === finalGroup.groupId && this.pendingAdmission.seq === finalGroup.seq) {
        this.pendingAdmission = undefined;
      }
      if (this.linkState === "desync_pending") {
        this.linkState = "connected";
      }
    }
    this.emit("message", message);
  }

  private handleDisconnect(error: Error): void {
    for (const pending of this.pending.values()) {
      if (pending.timer) {
        clearTimeout(pending.timer);
      }
      pending.reject(error);
    }
    this.pending.clear();
    this.activeGroup = undefined;
    this.pendingAdmission = undefined;
    this.cancelRequestedFor = undefined;
    this.linkState = "connected";
    this.stopHeartbeat();
    if (this.socket) {
      this.socket.removeAllListeners();
      this.socket.destroy();
      this.socket = undefined;
    }
    this.emit("disconnect", error);
  }

  private async sendHello(): Promise<GroupStreamEventMessage> {
    const message: GroupStreamCommandMessage = { v: 1, requestId: this.nextId("hello"), type: "hello" };
    this.logPhase("hello_write_start");
    return new Promise<GroupStreamEventMessage>((resolve, reject) => {
      this.pending.set(message.requestId, {
        requestId: message.requestId,
        commandType: message.type,
        terminalTypes: terminalTypesFor(message.type),
        resolve: (response) => {
          this.logPhase("hello_response_received", `type=${response.type}`);
          resolve(response);
        },
        reject,
      });
      this.writeLine(message).then(() => {
        this.logPhase("hello_write_flushed");
        const pending = this.pending.get(message.requestId);
        if (!pending) {
          return;
        }
        pending.timer = setTimeout(() => {
          this.pending.delete(message.requestId);
          this.logPhase("hello_timeout");
          reject(new Error(`hello_timeout: TCP hello timed out after ${helloTimeoutMs}ms`));
        }, helloTimeoutMs);
      }).catch((error) => {
        const pending = this.pending.get(message.requestId);
        if (pending?.timer) {
          clearTimeout(pending.timer);
        }
        this.pending.delete(message.requestId);
        reject(error instanceof Error ? error : new Error("TCP hello write failed"));
      });
    });
  }

  private startHeartbeat(): void {
    this.stopHeartbeat();
    this.heartbeatTimer = setInterval(() => {
      if (this.socket?.writable && !this.pingInFlight && this.pending.size === 0) {
        this.pingInFlight = true;
        this.sendCommand({ v: 1, requestId: this.nextId("ping"), type: "ping" }, 1500)
          .catch((error) => this.handleDisconnect(error instanceof Error ? error : new Error("TCP ping failed")))
          .finally(() => {
            this.pingInFlight = false;
          });
      }
    }, heartbeatIntervalMs);
  }

  private stopHeartbeat(): void {
    if (this.heartbeatTimer) {
      clearInterval(this.heartbeatTimer);
      this.heartbeatTimer = undefined;
    }
  }

  private writeLine(message: GroupStreamCommandMessage): Promise<void> {
    if (!this.socket || !this.socket.writable) {
      throw new Error("TCP device is not connected");
    }
    const socket = this.socket;
    const line = `${JSON.stringify(message)}\n`;
    return new Promise<void>((resolve, reject) => {
      let settled = false;
      let writeCallbackDone = false;
      let drainDone = false;
      const timer = setTimeout(() => finish(new Error(`transport_write_timeout: TCP write timed out after ${writeTimeoutMs}ms`)), writeTimeoutMs);
      const finish = (error?: Error) => {
        if (settled) {
          return;
        }
        settled = true;
        clearTimeout(timer);
        socket.off("error", onError);
        socket.off("close", onClose);
        socket.off("drain", onDrain);
        if (error) {
          reject(error);
        } else {
          resolve();
        }
      };
      const maybeFinish = () => {
        if (writeCallbackDone && drainDone) {
          finish();
        }
      };
      const onError = (error: Error) => finish(error);
      const onClose = () => finish(new Error("transport_disconnect: TCP device disconnected"));
      const onDrain = () => {
        drainDone = true;
        maybeFinish();
      };
      socket.once("error", onError);
      socket.once("close", onClose);
      const flushed = socket.write(line, "utf8", () => {
        writeCallbackDone = true;
        maybeFinish();
      });
      drainDone = flushed;
      if (!flushed) {
        socket.once("drain", onDrain);
      }
    });
  }

  private logPhase(phase: string, details = ""): void {
    console.info(details.length > 0 ? `[tcp-link] ${phase} ${details}` : `[tcp-link] ${phase}`);
  }

  private nextId(prefix: string): string {
    this.sequence += 1;
    return `${prefix}-${Date.now().toString(36)}-${this.sequence.toString(36)}`;
  }

  private enqueueMutatingCommand<T>(operation: () => Promise<T>): Promise<T> {
    const run = this.commandQueue.then(operation, operation);
    this.commandQueue = run.catch(() => undefined);
    return run;
  }

  private matchesPendingOrActiveGroup(groupId: string, seq: number): boolean {
    return (
      (this.activeGroup?.groupId === groupId && this.activeGroup.seq === seq) ||
      (this.pendingAdmission?.groupId === groupId && this.pendingAdmission.seq === seq)
    );
  }
}

function isTerminalResponseForPending(message: GroupStreamEventMessage, pending: PendingRequest): boolean {
  return terminalResponseTypes.has(message.type) && pending.terminalTypes.has(message.type);
}

function terminalTypesFor(commandType: GroupStreamCommandMessage["type"]): Set<string> {
  if (commandType === "exec_group") {
    return new Set(["group_accepted", "group_rejected", "protocol_error"]);
  }
  if (commandType === "ping") {
    return new Set(["pong", "protocol_error"]);
  }
  const expected: Record<GroupStreamCommandMessage["type"], string> = {
    hello: "hello_ack",
    get_status: "status",
    subscribe_telemetry: "telemetry_subscribed",
    get_keymap: "keymap",
    probe: "probe_result",
    press_diag_m5: "press_diag_m5_result",
    reset_fault: "reset_fault_result",
    release_line_feed_origin: "release_line_feed_origin_result",
    cancel: "cancel_result",
    finish_task: "finish_task_result",
    exec_group: "group_accepted",
    ping: "pong",
  };
  return new Set([expected[commandType], "protocol_error"]);
}

function defaultTimeoutFor(message: GroupStreamCommandMessage): number {
  if (message.type === "exec_group") {
    return groupAdmissionTimeoutMs;
  }
  return requestTimeoutMs;
}

function timeoutCode(commandType: GroupStreamCommandMessage["type"]): string {
  return commandType === "exec_group" ? "submit_timeout" : "request_timeout";
}

function finalGroupIdentity(message: GroupStreamEventMessage): ActiveGroup | undefined {
  if (message.type === "group_final" || message.type === "group_done") {
    return { groupId: message.groupId, seq: message.seq };
  }
  if (message.type === "fault" && message.groupId && message.seq !== undefined) {
    return { groupId: message.groupId, seq: message.seq };
  }
  return undefined;
}
