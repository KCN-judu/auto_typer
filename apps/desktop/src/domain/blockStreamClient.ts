import type {
  AckMessage,
  BlockStreamCommandMessage,
  BlockStreamEventMessage,
  RemoteMotionBlock,
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
    if (!window.autoTyper) {
      throw new Error("Block stream IPC is unavailable");
    }
    await window.autoTyper.blockStreamConnect({ host, port });
    if (!this.unsubscribe) {
      this.unsubscribe = window.autoTyper.blockStreamOnMessage((message) => {
        this.listeners.forEach((listener) => listener(message));
      });
    }
  }

  async disconnect(): Promise<void> {
    if (!window.autoTyper) {
      throw new Error("Block stream IPC is unavailable");
    }
    await window.autoTyper.blockStreamDisconnect();
  }

  onMessage(listener: BlockStreamListener): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  async sendExecBlock(blockId: string, block: RemoteMotionBlock): Promise<AckMessage> {
    return this.sendCommand({ v: 1, id: this.nextId("cmd"), type: "exec_block", blockId, block });
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

  private async sendCommand(message: BlockStreamCommandMessage): Promise<AckMessage> {
    if (!window.autoTyper) {
      throw new Error("Block stream IPC is unavailable");
    }
    return window.autoTyper.blockStreamSend(message);
  }

  private nextId(prefix: string): string {
    this.sequence += 1;
    return `${prefix}-${Date.now().toString(36)}-${this.sequence.toString(36)}`;
  }
}
