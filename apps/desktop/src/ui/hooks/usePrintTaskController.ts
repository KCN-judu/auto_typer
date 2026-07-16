import { useEffect, useRef, useState } from "react";
import type {
  AtomicMotionBlock,
  DeviceStatus,
  KeymapDocument,
  MotionBlockRequest,
  MotionProtocolEventMessage,
} from "../../../../../shared/protocol/protocolTypes";
import type { BlockExecutionOutcome, MotionProtocolClient } from "../../domain/runtime";
import { awaitBlockExecution } from "../../domain/runtime";
import type { PlannedRemoteMotionBlock } from "../../domain/planner";
import {
  encodeLineFeedMove,
  encodeLineFeedHome,
  encodePressMove,
  encodeReturnToZero,
  encodeXyMove,
  initialMotionPosition,
  planTextToMotionBlocks,
  type MotionPositionState,
} from "../../domain/planner";
import { clampInteger } from "../appState";
import { defaultPrintTaskState, idleStreamState, type ConnectionState, type PrintTaskState } from "../appTypes";

type UsePrintTaskControllerArgs = {
  streamClient: MotionProtocolClient;
  connection: ConnectionState;
  status: DeviceStatus;
  setStatus: (value: DeviceStatus | ((current: DeviceStatus) => DeviceStatus)) => void;
  keymap: KeymapDocument;
  appendLog: (line: string) => void;
};

export function usePrintTaskController({
  streamClient,
  connection,
  setStatus,
  keymap,
  appendLog,
}: UsePrintTaskControllerArgs) {
  const [jobText, setJobText] = useState("asdf jkl");
  const [debugStepPulses, setDebugStepPulses] = useState(80);
  const [debugRpm, setDebugRpm] = useState(800);
  const [debugAccelRaw, setDebugAccelRaw] = useState(80);
  const [printTask, setPrintTask] = useState<PrintTaskState>(defaultPrintTaskState);
  const runningRef = useRef(false);
  const cancelRequestedRef = useRef(false);
  const positionRef = useRef<MotionPositionState>(initialMotionPosition());
  const activeRunRef = useRef<AbortController | null>(null);
  const operationEpochRef = useRef(0);

  useEffect(() => {
    runningRef.current = printTask.running;
  }, [printTask.running]);

  useEffect(() => {
    if (!printTask.running && !printTask.requiresRecovery) {
      setPrintTask((task) => ({ ...task, stream: idleStreamState(connection) }));
    }
  }, [connection]);

  useEffect(() => streamClient.onMessage(handleProtocolEvent), [streamClient]);

  function beginOperation(): ActiveOperation {
    const operation = {
      epoch: operationEpochRef.current + 1,
      controller: new AbortController(),
    };
    operationEpochRef.current = operation.epoch;
    activeRunRef.current = operation.controller;
    runningRef.current = true;
    return operation;
  }

  function finishOperation(operation: ActiveOperation) {
    if (operation.epoch !== operationEpochRef.current) return;
    runningRef.current = false;
    cancelRequestedRef.current = false;
    activeRunRef.current = null;
  }

  async function submitJob() {
    if (runningRef.current) {
      appendLog("已有动作正在执行");
      return;
    }
    const plan = planTextToMotionBlocks(
      jobText,
      keymap,
      `job-${Date.now().toString(36)}`,
      { xMm: 0, yMm: 0 },
      positionRef.current,
    );
    let completedPosition = positionRef.current;
    cancelRequestedRef.current = false;
    setPrintTask((task) => ({
      ...task,
      currentIndex: 0,
      totalBlocks: plan.requests.length,
      completedBlocks: 0,
      currentLabel: "",
      requiresRecovery: false,
      fault: plan.ok ? undefined : plan.message,
    }));
    if (!plan.ok) {
      appendLog(`规划失败：${plan.message}`);
      return;
    }
    if (plan.requests.length === 0) {
      appendLog("任务为空");
      return;
    }
    const operation = beginOperation();
    try {
      setPrintTask((task) => ({ ...task, running: true, stream: "running", requiresRecovery: false, fault: undefined }));
      for (let index = 0; index < plan.requests.length; index += 1) {
        if (cancelRequestedRef.current) {
          await returnToZeroAfterCancel(completedPosition, operation.controller.signal);
          throw new Error("block_cancelled: task cancelled after return to zero");
        }
        const block = plan.requests[index];
        const label = blockLabel(block);
        setPrintTask((task) => ({ ...task, currentIndex: index + 1, currentLabel: label }));
        appendLog(`Send block ${block.blockId} ${describeBlock(block.block)} target=${describeTargets(block.block)}`);
        await requireDone(block, label, operation.controller.signal);
        completedPosition = block.plannedBlock.positionAfter;
        positionRef.current = completedPosition;
        setPrintTask((task) => ({ ...task, completedBlocks: index + 1 }));
        if (cancelRequestedRef.current) {
          await returnToZeroAfterCancel(completedPosition, operation.controller.signal);
          throw new Error("block_cancelled: task cancelled after return to zero");
        }
      }
      const finish = await streamClient.finishTask();
      if (!finish.ok) throw new Error("finish_task_failed: device did not finish task");
      positionRef.current = plan.finalPosition;
      if (operation.epoch === operationEpochRef.current) {
        setPrintTask((task) => ({ ...task, running: false, stream: "connected", requiresRecovery: false, currentLabel: "" }));
        appendLog("原子动作流完成");
      }
    } catch (error) {
      const message = error instanceof Error ? error.message : "原子动作流失败";
      const cancelled = message.startsWith("block_cancelled");
      if (operation.epoch === operationEpochRef.current) {
        setPrintTask((task) => ({
          ...task,
          running: false,
          stream: cancelled ? "connected" : "fault",
          requiresRecovery: !cancelled,
          currentLabel: "",
          fault: cancelled ? undefined : message,
        }));
        appendLog(message);
      }
    } finally {
      finishOperation(operation);
    }
  }

  async function cancelJob() {
    if (!runningRef.current) {
      appendLog("当前没有运行中的任务");
      return;
    }
    cancelRequestedRef.current = true;
    setPrintTask((task) => ({ ...task, currentLabel: "等待当前 block 完成后回零" }));
    appendLog("已请求取消：等待当前 block 完成，然后清空后续队列并绝对回零");
  }

  async function resetFault() {
    try {
      const result = await streamClient.resetFault();
      if (result.status) setStatus(result.status);
      if (result.ok) {
        setPrintTask((task) => ({ ...task, stream: "connected", requiresRecovery: false, fault: undefined }));
      }
      appendLog(result.ok ? "故障已清除" : "故障清除失败");
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "清故障失败");
    }
  }

  async function emergencyStop() {
    operationEpochRef.current += 1;
    activeRunRef.current?.abort(new Error("emergency_stop: Emergency stop requested"));
    activeRunRef.current = null;
    runningRef.current = false;
    cancelRequestedRef.current = false;
    setPrintTask((task) => ({
      ...task,
      running: false,
      stream: "fault",
      requiresRecovery: true,
      currentLabel: "",
      fault: "emergency_stop: Emergency stop requested",
    }));
    try {
      const result = await streamClient.emergencyStop();
      if (result.status) {
        setStatus(result.status);
      } else if (result.ok) {
        setStatus((current) => ({
          ...current,
          mode: "faulted",
          health: "fault",
          motionReady: false,
          pressReady: false,
          fault: { code: "emergency_stop", message: "Emergency stop requested", recoverable: true },
        }));
      }
      appendLog(result.ok ? "急停已执行：全部电机已停止、失能并解锁" : "急停执行失败");
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "急停请求失败");
    }
  }

  async function runLineFeedHome() {
    await runDebugBlocks("走纸到位", encodeLineFeedHome(positionRef.current));
  }

  async function jogXY(dxSign: -1 | 0 | 1, dySign: -1 | 0 | 1, label: string) {
    const steps = clampInteger(debugStepPulses, 1, 20000);
    const encoded = encodeXyMove(
      positionRef.current,
      positionRef.current.x + dxSign * steps,
      positionRef.current.y + dySign * steps,
      10000,
      { rpm: clampInteger(debugRpm, 1, 3000), accelRaw: clampInteger(debugAccelRaw, 1, 255) },
    );
    if (encoded) await runDebugBlocks(`XY ${label}`, [encoded]);
  }

  async function jogPress(type: "down" | "release", label: string) {
    const target = type === "down" ? -2700 : 0;
    await runDebugBlocks(`按压 ${label}`, [encodePressMove(positionRef.current, target)]);
  }

  async function runDebugBlocks(label: string, blocks: Array<{ block: AtomicMotionBlock; position: MotionPositionState }>) {
    if (runningRef.current) {
      appendLog("已有动作正在执行");
      return;
    }
    const operation = beginOperation();
    setPrintTask((task) => ({
      ...task,
      running: true,
      stream: "running",
      requiresRecovery: false,
      currentIndex: 0,
      totalBlocks: blocks.length,
      completedBlocks: 0,
      currentLabel: label,
      fault: undefined,
    }));
    try {
      for (let index = 0; index < blocks.length; index += 1) {
        const encoded = blocks[index];
        const request = makeRequest(`debug-${Date.now().toString(36)}-${index}`, index, encoded.block);
        await requireDone(request, label, operation.controller.signal);
        positionRef.current = encoded.position;
        setPrintTask((task) => ({ ...task, currentIndex: index + 1, completedBlocks: index + 1 }));
      }
      if (operation.epoch === operationEpochRef.current) {
        setPrintTask((task) => ({ ...task, running: false, stream: "connected", requiresRecovery: false, currentLabel: "" }));
        appendLog(`${label} 完成`);
      }
    } catch (error) {
      const message = error instanceof Error ? error.message : `${label} 失败`;
      if (operation.epoch === operationEpochRef.current) {
        setPrintTask((task) => ({ ...task, running: false, stream: "fault", requiresRecovery: true, currentLabel: "", fault: message }));
        appendLog(message);
      }
    } finally {
      finishOperation(operation);
    }
  }

  async function returnToZeroAfterCancel(position: MotionPositionState, signal: AbortSignal) {
    const blocks = encodeReturnToZero(position);
    setPrintTask((task) => ({ ...task, currentLabel: "取消后绝对回零" }));
    appendLog("当前 block 已完成，开始绝对回零：XY→0，M4→16400→10000，Z→0");
    for (let index = 0; index < blocks.length; index += 1) {
      const encoded = blocks[index];
      await requireDone(makeRequest(`cancel-return-${Date.now().toString(36)}-${index}`, index, encoded.block), "取消后回零", signal);
      positionRef.current = encoded.position;
    }
    appendLog("取消后绝对回零完成，剩余任务队列已丢弃");
  }

  async function requireDone(block: MotionBlockRequest, label: string, signal: AbortSignal): Promise<BlockExecutionOutcome> {
    const listenerAbort = new AbortController();
    const relayAbort = () => listenerAbort.abort(signal.reason);
    signal.addEventListener("abort", relayAbort, { once: true });
    const completion = awaitBlockExecution({
      stream: streamClient,
      block,
      timeoutMs: block.policy.maxRuntimeMs + 5000,
      signal: listenerAbort.signal,
    });
    try {
      try {
        await streamClient.executeBlock(block);
      } catch (error) {
        listenerAbort.abort(error);
        await completion.catch(() => undefined);
        throw error;
      }
      const outcome = await completion;
      if (outcome.status !== "done") {
        const code = outcome.status === "cancelled" ? "block_cancelled" : outcome.code ?? `block_${outcome.status}`;
        throw new Error(`${code}: ${outcome.message ?? outcome.status}`);
      }
      appendLog(`${label}: done`);
      return outcome;
    } finally {
      signal.removeEventListener("abort", relayAbort);
    }
  }

  function handleProtocolEvent(message: MotionProtocolEventMessage) {
    if (message.type === "block_result") {
      appendLog(`Block result ${message.blockId} ${message.status}`);
    } else if (message.type === "fault") {
      const text = `${message.code}: ${message.message}`;
      setPrintTask((task) => ({ ...task, running: false, stream: "fault", requiresRecovery: true, currentLabel: "", fault: text }));
      appendLog(text);
    }
  }

  return {
    jobText,
    setJobText,
    debugStepPulses,
    setDebugStepPulses,
    debugRpm,
    setDebugRpm,
    debugAccelRaw,
    setDebugAccelRaw,
    printTask,
    submitJob,
    cancelJob,
    resetFault,
    emergencyStop,
    runLineFeedHome,
    jogXY,
    jogPress,
  };
}

type ActiveOperation = {
  epoch: number;
  controller: AbortController;
};

function makeRequest(blockId: string, seq: number, block: AtomicMotionBlock): MotionBlockRequest {
  return { blockId, seq, policy: { maxRuntimeMs: 20000, onDisconnect: "cancel" }, block };
}

function blockLabel(block: PlannedRemoteMotionBlock): string {
  return `${block.kind}${block.plannedBlock.targetKeyLabel ? ` ${block.plannedBlock.targetKeyLabel}` : ""}`;
}

function describeBlock(block: AtomicMotionBlock): string {
  return block.length === 1 ? block[0].type : "parallel";
}

function describeTargets(block: AtomicMotionBlock): string {
  return block.map((action) => action.type === "motor_move" ? `M${action.motorId}:${action.target}` : `wait:${action.durationMs}`).join(",");
}
