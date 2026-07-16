import type {
  BlockAckMessage,
  CancelResultMessage,
  DeviceStatus,
  EmergencyStopResultMessage,
  FinishTaskResultMessage,
  MotionBlockRequest,
  MotionProtocolCommandMessage,
  MotionProtocolEventMessage,
  ResetFaultResultMessage,
  SnapshotMessage,
} from "../../../../../../shared/protocol/protocolTypes";
import { encodeExecuteBlock } from "../../planner";

export type MotionProtocolConnection = {
  host: string;
  port?: number;
};

export type MotionProtocolListener = (message: MotionProtocolEventMessage) => void;

const defaultPort = 7777;

export class MotionProtocolClient {
  private sequence = 0;
  private unsubscribe?: () => void;
  private listeners = new Set<MotionProtocolListener>();

  async connect({ host, port = defaultPort }: MotionProtocolConnection): Promise<void> {
    if (!window.autoTyper) {
      throw new Error("TCP IPC is unavailable");
    }
    await window.autoTyper.motionProtocolConnect({ host, port });
    this.unsubscribe?.();
    this.unsubscribe = window.autoTyper.motionProtocolOnMessage((message) => {
      this.listeners.forEach((listener) => listener(message));
    });
  }

  async disconnect(): Promise<void> {
    this.unsubscribe?.();
    this.unsubscribe = undefined;
    await window.autoTyper?.motionProtocolDisconnect();
  }

  onMessage(listener: MotionProtocolListener): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  async getSnapshot(): Promise<DeviceStatus> {
    const message = await this.sendCommand({ v: 1, requestId: this.nextId("snapshot"), type: "get_snapshot" });
    if (message.type !== "snapshot") {
      throw responseError(message, "get_snapshot");
    }
    return (message as SnapshotMessage).status;
  }

  async executeBlock(block: MotionBlockRequest): Promise<BlockAckMessage> {
    const message = await this.sendCommand(encodeExecuteBlock(block, this.nextId("block")));
    if (message.type !== "block_ack") {
      throw responseError(message, "execute_block");
    }
    return message;
  }

  async cancel(): Promise<CancelResultMessage> {
    const message = await this.sendCommand({ v: 1, requestId: this.nextId("cancel"), type: "cancel" });
    if (message.type !== "cancel_result") {
      throw responseError(message, "cancel");
    }
    return message;
  }

  async finishTask(): Promise<FinishTaskResultMessage> {
    const message = await this.sendCommand({ v: 1, requestId: this.nextId("finish-task"), type: "finish_task" });
    if (message.type !== "finish_task_result") {
      throw responseError(message, "finish_task");
    }
    return message;
  }

  async emergencyStop(): Promise<EmergencyStopResultMessage> {
    const message = await this.sendCommand({ v: 1, requestId: this.nextId("emergency-stop"), type: "emergency_stop" });
    if (message.type !== "emergency_stop_result") {
      throw responseError(message, "emergency_stop");
    }
    return message;
  }

  async resetFault(): Promise<ResetFaultResultMessage> {
    const message = await this.sendCommand({ v: 1, requestId: this.nextId("reset-fault"), type: "reset_fault" });
    if (message.type !== "reset_fault_result") {
      throw responseError(message, "reset_fault");
    }
    return message;
  }

  private async sendCommand(message: MotionProtocolCommandMessage): Promise<MotionProtocolEventMessage> {
    if (!window.autoTyper) {
      throw new Error("TCP IPC is unavailable");
    }
    return window.autoTyper.motionProtocolSend(message);
  }

  private nextId(prefix: string): string {
    this.sequence += 1;
    return `${prefix}-${Date.now().toString(36)}-${this.sequence.toString(36)}`;
  }
}

function responseError(message: MotionProtocolEventMessage, command: string): Error {
  if (message.type === "protocol_error" || message.type === "fault") {
    return new Error(`${message.code}: ${message.message}`);
  }
  return new Error(`${command} received unexpected ${message.type}`);
}
