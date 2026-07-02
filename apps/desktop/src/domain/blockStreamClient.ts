import type {
  AckMessage,
  BlockStreamCommandMessage,
  BlockStreamEventMessage,
  ExecBlockMessage,
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
    await window.autoTyper?.blockStreamConnect({ host, port });
    if (!this.unsubscribe) {
      this.unsubscribe = window.autoTyper?.blockStreamOnMessage((message) => {
        this.listeners.forEach((listener) => listener(message));
      });
    }
    const ack = await this.sendCommand({ v: 1, id: this.nextId("hello"), type: "hello", client: "desktop" });
    if (!ack.accepted) {
      throw new Error(ack.message ?? ack.code ?? "Block stream hello rejected");
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

  async execBlock(blockId: string, block: RemoteMotionBlock): Promise<AckMessage> {
    const message: ExecBlockMessage = {
      v: 1,
      id: this.nextId("block"),
      type: "exec_block",
      blockId,
      block,
    };
    return this.sendCommand(message);
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
