import type {
  AckMessage,
  GroupStreamCommandMessage,
  GroupStreamEventMessage,
  RemoteMotionStep,
} from "../../../../shared/protocol/auto-typer-protocol";

export type GroupStreamConnection = {
  host: string;
  port?: number;
};

export type GroupStreamListener = (message: GroupStreamEventMessage) => void;

const defaultPort = 7777;
const execGroupAckTimeoutMs = 5000;

export class GroupStreamClient {
  private sequence = 0;
  private unsubscribe?: () => void;
  private listeners = new Set<GroupStreamListener>();

  async connect({ host, port = defaultPort }: GroupStreamConnection): Promise<void> {
    if (!window.autoTyper) {
      throw new Error("Group stream IPC is unavailable");
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
      throw new Error("Group stream IPC is unavailable");
    }
    await window.autoTyper.groupStreamDisconnect();
  }

  onMessage(listener: GroupStreamListener): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  async sendExecGroup(groupId: string, steps: RemoteMotionStep[]): Promise<AckMessage> {
    try {
      return await this.sendCommand({ v: 1, id: this.nextId("cmd"), type: "exec_group", groupId, steps, timeoutMs: execGroupAckTimeoutMs });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Group stream exec_group failed";
      throw new Error(`Group ${groupId} exec_group failed: ${message}`);
    }
  }

  async cancel(): Promise<AckMessage> {
    return this.sendCommand({ v: 1, id: this.nextId("cancel"), type: "cancel" });
  }

  async resetFault(): Promise<AckMessage> {
    return this.sendCommand({ v: 1, id: this.nextId("reset"), type: "reset_fault" });
  }

  async probe(): Promise<AckMessage> {
    return this.sendCommand({ v: 1, id: this.nextId("probe"), type: "probe" });
  }

  private async sendCommand(message: GroupStreamCommandMessage): Promise<AckMessage> {
    if (!window.autoTyper) {
      throw new Error("Group stream IPC is unavailable");
    }
    return window.autoTyper.groupStreamSend(message);
  }

  private nextId(prefix: string): string {
    this.sequence += 1;
    return `${prefix}-${Date.now().toString(36)}-${this.sequence.toString(36)}`;
  }
}
