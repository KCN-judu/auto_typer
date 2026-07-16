import type {
  BlockResultMessage,
  DeviceFaultMessage,
  MotionBlockRequest,
  MotionProtocolEventMessage,
} from "../../../../../../shared/protocol/protocolTypes";

type MotionExecutionStream = {
  onMessage(listener: (message: MotionProtocolEventMessage) => void): () => void;
};

export type BlockExecutionOutcome = {
  status: "done" | "failed" | "cancelled";
  code?: string;
  message?: string;
  eventType: "block_result" | "fault";
};

export function awaitBlockExecution(options: {
  stream: MotionExecutionStream;
  block: Pick<MotionBlockRequest, "blockId" | "seq">;
  timeoutMs: number;
  signal?: AbortSignal;
}): Promise<BlockExecutionOutcome> {
  const { stream, block, timeoutMs, signal } = options;
  return new Promise((resolve, reject) => {
    let unsubscribe: () => void = () => undefined;
    let timer: number | undefined;
    const cleanup = () => {
      unsubscribe();
      if (timer !== undefined) window.clearTimeout(timer);
      signal?.removeEventListener("abort", handleAbort);
    };
    const finish = (outcome: BlockExecutionOutcome) => {
      cleanup();
      resolve(outcome);
    };
    const handleAbort = () => {
      cleanup();
      reject(signal?.reason instanceof Error ? signal.reason : new Error("block_wait_aborted: block wait aborted"));
    };
    unsubscribe = stream.onMessage((message) => {
      if (message.type === "block_result" && matchesBlock(message, block)) {
        finish({
          status: message.status,
          code: message.code,
          message: message.message,
          eventType: "block_result",
        });
      } else if (message.type === "fault") {
        const fault = message as DeviceFaultMessage;
        finish({ status: "failed", code: fault.code, message: fault.message, eventType: "fault" });
      }
    });
    timer = window.setTimeout(() => {
      cleanup();
      reject(new Error(`block_result_timeout: no terminal result after ${timeoutMs}ms`));
    }, timeoutMs);
    if (signal?.aborted) {
      handleAbort();
    } else {
      signal?.addEventListener("abort", handleAbort, { once: true });
    }
  });
}

function matchesBlock(
  message: Pick<BlockResultMessage, "blockId" | "seq">,
  block: Pick<MotionBlockRequest, "blockId" | "seq">,
): boolean {
  return message.blockId === block.blockId && message.seq === block.seq;
}
