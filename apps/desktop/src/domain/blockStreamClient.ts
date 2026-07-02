import type {
  AckMessage,
  BlockStreamEventMessage,
  PrimitiveCommand,
} from "../../../../shared/protocol/auto-typer-protocol";

export type BlockStreamConnection = {
  host: string;
  port?: number;
};

export type BlockStreamListener = (message: BlockStreamEventMessage) => void;

const defaultPort = 7777;

export class BlockStreamClient {
  private sequence = 0;
  private unsubscribe?: () => void;
  private listeners = new Set<BlockStreamListener>();

  async connect({ host, port = defaultPort }: BlockStreamConnection): Promise<void> {
    await window.autoTyper?.blockStreamConnect({ host, port });
    if (!this.unsubscribe) {
      this.unsubscribe = window.autoTyper?.blockStreamOnMessage((message) => {
        this.listeners.forEach((listener) => listener(message));
      });
    }
  }

  async disconnect(): Promise<void> {
    await window.autoTyper?.blockStreamDisconnect();
  }

  onMessage(listener: BlockStreamListener): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  async sendPrimitive(command: PrimitiveCommand): Promise<AckMessage> {
    return this.sendCommand(command);
  }

  async cancel(): Promise<AckMessage> {
    return this.sendCommand({ v: 1, id: this.nextId("cancel"), type: "command", op: "cancel" });
  }

  async resetFault(): Promise<AckMessage> {
    return this.sendCommand({ v: 1, id: this.nextId("reset"), type: "command", op: "reset_fault" });
  }

  private async sendCommand(message: PrimitiveCommand): Promise<AckMessage> {
    const ack = await window.autoTyper?.blockStreamSend(message);
    if (!ack) {
      throw new Error("Block stream IPC is unavailable");
    }
    return ack;
  }

  private nextId(prefix: string): string {
    this.sequence += 1;
    return `${prefix}-${Date.now().toString(36)}-${this.sequence.toString(36)}`;
  }
}
