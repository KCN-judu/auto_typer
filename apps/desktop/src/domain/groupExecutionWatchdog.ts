import type {
  DeviceStatus,
  GroupFaultMessage,
  GroupFinalMessage,
  GroupStreamEventMessage,
  MotionBlock,
  TaskGroup,
} from "../../../../shared/protocol/auto-typer-protocol";

export type ExecutionTimeoutKind =
  | "admission_timeout"
  | "progress_timeout"
  | "final_timeout"
  | "absolute_runtime_timeout";

export type GroupExecutionTimeout = {
  activeGroupId: string;
  seq: number;
  elapsedMs: number;
  lastProgressAt: number;
  lastProgressEvent: string;
  blockIndex: number;
  expectedBlockCount: number;
  timeoutKind: ExecutionTimeoutKind;
};

export type GroupExecutionOutcome = {
  status: "done" | "failed" | "cancelled";
  code?: string;
  message?: string;
  eventType: "group_final" | "group_done" | "fault";
};

export type GroupProgressUpdate = {
  event: string;
  blockIndex: number;
  blockType?: MotionBlock["type"];
};

export type WatchdogObservation =
  | { kind: "ignored" }
  | { kind: "progress"; progress: GroupProgressUpdate }
  | { kind: "final"; outcome: GroupExecutionOutcome };

export type GroupExecutionWatchdogOptions = {
  progressTimeoutMs?: number;
  finalAfterLastBlockTimeoutMs?: number;
  absoluteMaxRuntimeMs?: number;
};

const defaultProgressTimeoutMs = 12000;
const defaultFinalAfterLastBlockTimeoutMs = 4000;
const defaultAbsoluteMaxRuntimeMs = 30000;

export class GroupExecutionWatchdog {
  readonly groupId: string;
  readonly seq: number;
  readonly expectedBlockCount: number;
  readonly startedAtMs: number;
  readonly progressTimeoutMs: number;
  readonly finalAfterLastBlockTimeoutMs: number;
  readonly absoluteMaxRuntimeMs: number;

  private started = false;
  private finalObserved = false;
  private lastProgressAtMs: number;
  private lastProgressEvent = "group_accepted";
  private currentBlockIndex = -1;
  private lastBlockDoneAtMs?: number;

  constructor(group: TaskGroup & { absoluteMaxRuntimeMs?: number }, startedAtMs: number, options: GroupExecutionWatchdogOptions = {}) {
    this.groupId = group.groupId;
    this.seq = group.seq;
    this.expectedBlockCount = group.blocks.length;
    this.startedAtMs = startedAtMs;
    this.lastProgressAtMs = startedAtMs;
    this.progressTimeoutMs = options.progressTimeoutMs ?? defaultProgressTimeoutMs;
    this.finalAfterLastBlockTimeoutMs = options.finalAfterLastBlockTimeoutMs ?? defaultFinalAfterLastBlockTimeoutMs;
    this.absoluteMaxRuntimeMs = options.absoluteMaxRuntimeMs ?? group.absoluteMaxRuntimeMs ?? defaultAbsoluteMaxRuntimeMs;
  }

  observe(message: GroupStreamEventMessage, nowMs: number): WatchdogObservation {
    if (this.finalObserved) {
      return { kind: "ignored" };
    }

    const outcome = this.outcomeFrom(message);
    if (outcome) {
      this.finalObserved = true;
      return { kind: "final", outcome };
    }

    const progress = this.progressFrom(message);
    if (!progress) {
      return { kind: "ignored" };
    }

    this.started = true;
    this.lastProgressAtMs = nowMs;
    this.lastProgressEvent = progress.event;
    this.currentBlockIndex = progress.blockIndex;
    if (progress.event === "block_done" && progress.blockIndex >= this.expectedBlockCount - 1) {
      this.lastBlockDoneAtMs = nowMs;
    }
    return { kind: "progress", progress };
  }

  checkTimeout(nowMs: number): GroupExecutionTimeout | undefined {
    if (this.finalObserved) {
      return undefined;
    }

    const elapsedMs = nowMs - this.startedAtMs;
    if (elapsedMs > this.absoluteMaxRuntimeMs) {
      return this.timeout("absolute_runtime_timeout", nowMs);
    }

    if (this.lastBlockDoneAtMs !== undefined) {
      if (nowMs - this.lastBlockDoneAtMs > this.finalAfterLastBlockTimeoutMs) {
        return this.timeout("final_timeout", nowMs);
      }
      return undefined;
    }

    if (this.started && nowMs - this.lastProgressAtMs > this.progressTimeoutMs) {
      return this.timeout("progress_timeout", nowMs);
    }

    return undefined;
  }

  private progressFrom(message: GroupStreamEventMessage): GroupProgressUpdate | undefined {
    if (message.type === "group_started" && this.matches(message.groupId, message.seq)) {
      return { event: "group_started", blockIndex: this.currentBlockIndex };
    }
    if (message.type === "block_started" && this.matches(message.groupId, message.seq)) {
      return { event: "block_started", blockIndex: message.blockIndex, blockType: message.blockType };
    }
    if (message.type === "block_done" && this.matches(message.groupId, message.seq)) {
      return { event: "block_done", blockIndex: message.blockIndex, blockType: message.blockType };
    }
    if (message.type === "motor_event" && this.started) {
      return { event: "motor_event", blockIndex: this.currentBlockIndex };
    }
    if (message.type === "motor_state_update" && this.started) {
      return { event: "motor_state_update", blockIndex: this.currentBlockIndex };
    }
    if (message.type === "status" && this.started && statusIsRunning(message.status)) {
      return { event: "status_running", blockIndex: this.currentBlockIndex };
    }
    if (message.type === "telemetry" && this.started && statusIsRunning(message.status)) {
      return { event: "telemetry_running", blockIndex: this.currentBlockIndex };
    }
    return undefined;
  }

  private outcomeFrom(message: GroupStreamEventMessage): GroupExecutionOutcome | undefined {
    if (message.type === "group_final" && this.matches(message.groupId, message.seq)) {
      const final = message as GroupFinalMessage;
      return {
        status: final.status,
        code: final.code,
        message: final.message,
        eventType: "group_final",
      };
    }
    if (message.type === "group_done" && this.matches(message.groupId, message.seq)) {
      return { status: "done", eventType: "group_done" };
    }
    if (message.type === "fault") {
      const fault = message as GroupFaultMessage;
      if (!fault.groupId || this.matches(fault.groupId, fault.seq ?? this.seq)) {
        return {
          status: "failed",
          code: fault.code,
          message: fault.message,
          eventType: "fault",
        };
      }
    }
    return undefined;
  }

  private matches(groupId: string, seq: number): boolean {
    return groupId === this.groupId && seq === this.seq;
  }

  private timeout(timeoutKind: ExecutionTimeoutKind, nowMs: number): GroupExecutionTimeout {
    return {
      activeGroupId: this.groupId,
      seq: this.seq,
      elapsedMs: nowMs - this.startedAtMs,
      lastProgressAt: this.lastProgressAtMs,
      lastProgressEvent: this.lastProgressEvent,
      blockIndex: this.currentBlockIndex,
      expectedBlockCount: this.expectedBlockCount,
      timeoutKind,
    };
  }
}

export function formatExecutionTimeout(timeout: GroupExecutionTimeout): string {
  return [
    `execution_timeout: ${timeout.timeoutKind}`,
    `activeGroupId=${timeout.activeGroupId}`,
    `seq=${timeout.seq}`,
    `elapsedMs=${timeout.elapsedMs}`,
    `lastProgressAt=${timeout.lastProgressAt}`,
    `lastProgressEvent=${timeout.lastProgressEvent}`,
    `blockIndex=${timeout.blockIndex}`,
    `expectedBlockCount=${timeout.expectedBlockCount}`,
  ].join(" ");
}

function statusIsRunning(status: DeviceStatus): boolean {
  const jobState = status.currentJob?.state;
  return status.mode === "running" || jobState === "queued" || jobState === "running" || jobState === "cancelling";
}
