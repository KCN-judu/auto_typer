import type {
  CancelResultMessage,
  DeviceStatus,
  FinishTaskResultMessage,
  GroupAcceptedMessage,
  GroupRejectedMessage,
  GroupStreamCommandMessage,
  GroupStreamEventMessage,
  KeymapDocument,
  KeymapMessage,
  PressDiagM5ResultMessage,
  ProbeResultMessage,
  ReleaseLineFeedOriginResultMessage,
  ResetFaultResultMessage,
  StatusMessage,
  TaskGroup,
} from "../../../../shared/protocol/auto-typer-protocol";
import { encodeExecGroup } from "./groupStreamPlanner";

export type GroupStreamConnection = {
  host: string;
  port?: number;
};

export type GroupStreamListener = (message: GroupStreamEventMessage) => void;

const defaultPort = 7777;

export class GroupStreamClient {
  private sequence = 0;
  private unsubscribe?: () => void;
  private listeners = new Set<GroupStreamListener>();

  async connect({ host, port = defaultPort }: GroupStreamConnection): Promise<void> {
    if (!window.autoTyper) {
      throw new Error("TCP IPC is unavailable");
    }
    await window.autoTyper.groupStreamConnect({ host, port });
    if (!this.unsubscribe) {
      this.unsubscribe = window.autoTyper.groupStreamOnMessage((message) => {
        this.listeners.forEach((listener) => listener(message));
      });
    }
  }

  async disconnect(): Promise<void> {
    if (!window.autoTyper) {
      throw new Error("TCP IPC is unavailable");
    }
    await window.autoTyper.groupStreamDisconnect();
  }

  onMessage(listener: GroupStreamListener): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  async getStatus(): Promise<DeviceStatus> {
    const message = await this.sendCommand({ v: 1, requestId: this.nextId("status"), type: "get_status" });
    if (message.type !== "status") {
      throw responseError(message, "get_status");
    }
    return (message as StatusMessage).status;
  }

  async subscribeTelemetry(intervalMs: number): Promise<void> {
    const message = await this.sendCommand({
      v: 1,
      requestId: this.nextId("telemetry"),
      type: "subscribe_telemetry",
      intervalMs,
    });
    if (message.type !== "telemetry_subscribed") {
      throw responseError(message, "subscribe_telemetry");
    }
  }

  async getKeymap(): Promise<KeymapDocument> {
    const message = await this.sendCommand({ v: 1, requestId: this.nextId("keymap"), type: "get_keymap" });
    if (message.type !== "keymap") {
      throw responseError(message, "get_keymap");
    }
    return keymapFromMessage(message as KeymapMessage);
  }

  async sendExecGroup(group: TaskGroup): Promise<GroupAcceptedMessage> {
    const message = await this.sendCommand(encodeExecGroup(group, this.nextId("group")));
    if (message.type === "group_rejected") {
      const rejected = message as GroupRejectedMessage;
      throw new Error(`${rejected.reason}: ${rejected.message}`);
    }
    if (message.type !== "group_accepted") {
      throw responseError(message, "exec_group");
    }
    return message as GroupAcceptedMessage;
  }

  async cancel(): Promise<CancelResultMessage> {
    const message = await this.sendCommand({ v: 1, requestId: this.nextId("cancel"), type: "cancel" });
    if (message.type !== "cancel_result") {
      throw responseError(message, "cancel");
    }
    return message as CancelResultMessage;
  }

  async finishTask(): Promise<FinishTaskResultMessage> {
    const message = await this.sendCommand({ v: 1, requestId: this.nextId("finish-task"), type: "finish_task" });
    if (message.type !== "finish_task_result") {
      throw responseError(message, "finish_task");
    }
    return message as FinishTaskResultMessage;
  }

  async resetFault(): Promise<ResetFaultResultMessage> {
    const message = await this.sendCommand({ v: 1, requestId: this.nextId("reset"), type: "reset_fault" });
    if (message.type !== "reset_fault_result") {
      throw responseError(message, "reset_fault");
    }
    return message as ResetFaultResultMessage;
  }

  async releaseLineFeedOrigin(): Promise<ReleaseLineFeedOriginResultMessage> {
    const message = await this.sendCommand({
      v: 1,
      requestId: this.nextId("release-line-feed"),
      type: "release_line_feed_origin",
    });
    if (message.type !== "release_line_feed_origin_result") {
      throw responseError(message, "release_line_feed_origin");
    }
    return message as ReleaseLineFeedOriginResultMessage;
  }

  async probe(): Promise<ProbeResultMessage> {
    const message = await this.sendCommand({ v: 1, requestId: this.nextId("probe"), type: "probe" });
    if (message.type !== "probe_result") {
      throw responseError(message, "probe");
    }
    return message as ProbeResultMessage;
  }

  async pressDiagM5(): Promise<PressDiagM5ResultMessage> {
    const message = await this.sendCommand({
      v: 1,
      requestId: this.nextId("press-diag-m5"),
      type: "press_diag_m5",
    });
    if (message.type !== "press_diag_m5_result") {
      throw responseError(message, "press_diag_m5");
    }
    return message as PressDiagM5ResultMessage;
  }

  private async sendCommand(message: GroupStreamCommandMessage): Promise<GroupStreamEventMessage> {
    if (!window.autoTyper) {
      throw new Error("TCP IPC is unavailable");
    }
    return window.autoTyper.groupStreamSend(message);
  }

  private nextId(prefix: string): string {
    this.sequence += 1;
    return `${prefix}-${Date.now().toString(36)}-${this.sequence.toString(36)}`;
  }
}

function keymapFromMessage(message: KeymapMessage): KeymapDocument {
  return {
    version: 1,
    machine: "feiyu200",
    updatedAt: new Date().toISOString(),
    bindings: message.keys.map((key) => ({
      key: key.label,
      point: { xMm: key.xMm, yMm: key.yMm },
    })),
  };
}

function responseError(message: GroupStreamEventMessage, command: string): Error {
  if (message.type === "protocol_error") {
    return new Error(`${message.code}: ${message.message}`);
  }
  if (message.type === "fault") {
    return new Error(`${message.code}: ${message.message}`);
  }
  return new Error(`Unexpected ${command} response: ${message.type}`);
}
