import type {
  ExecGroupMessage,
  KeyBinding,
  KeymapDocument,
  MachinePointMm,
  MotionBlock,
  TaskGroup,
} from "../../../../shared/protocol/auto-typer-protocol";
import {
  MAX_BLOCK_TIMEOUT_MS,
  MAX_BLOCKS_PER_GROUP,
  MAX_GROUP_RUNTIME_MS,
} from "../../../../shared/protocol/auto-typer-protocol";
import { estimateGroupRuntimeMs } from "./groupRuntime";
import { capsLockKey, displayKey, leftShiftKey, normalizeKey } from "./keymap";

export type PlannedMotionBlock = {
  block: MotionBlock;
  sourceCharacter?: string;
  targetKeyLabel?: string;
  displayCoordinates?: MachinePointMm;
};

export type PlannedRemoteMotionGroup = TaskGroup & {
  sourceCharacter?: string;
  targetKeyLabel?: string;
  displayCoordinates?: MachinePointMm;
  plannedBlocks: PlannedMotionBlock[];
  kind: MotionBlock["type"];
  estimatedRuntimeMs: number;
  absoluteMaxRuntimeMs: number;
};

export type GroupStreamPlan =
  | {
      ok: true;
      blocks: PlannedMotionBlock[];
      groups: PlannedRemoteMotionGroup[];
      warnings: string[];
    }
  | {
      ok: false;
      code: "key_not_found" | "plan_full";
      message: string;
      failedKey?: string;
      blocks: PlannedMotionBlock[];
      groups: PlannedRemoteMotionGroup[];
      warnings: string[];
    };

const moveXYMotion = { rpm: 1600, accelRaw: 128 } as const;
const pressDownMotion = { rpm: 3000, accelRaw: 255, timeoutMs: 3000 } as const;
const pressUpMotion = { rpm: 3000, accelRaw: 255, timeoutMs: 3000 } as const;
const characterReleaseMotion = { rpm: 1600, accelRaw: 128, timeoutMs: 10000 } as const;
const lineFeedMotion = { rpm: 1600, accelRaw: 128, timeoutMs: 10000 } as const;
const lineFeedHomeMotion = { rpm: 1600, accelRaw: 128, timeoutMs: 10000 } as const;
const returnZeroMotion = { rpm: 1600, accelRaw: 128, timeoutMs: 10000 } as const;
const maxGroups = 1024;
const stepsPerMm = 80;
const moveXYMinimumTimeoutMs = 5000;
const moveXYBaseTimeoutMs = 3000;
const moveXYTimeoutMsPerStep = 2;
const maxEstimatedGroupRuntimeMs = Math.floor(MAX_GROUP_RUNTIME_MS / 2);
const shiftedSymbolKeys: Record<string, string> = {
  "!": ",",
  "·": ".",
  "?": "/",
  "¥": "1",
  "£": "3",
  "%": "5",
  "_": "6",
  "&": "7",
  "(": "9",
  ")": "0",
  "*": "-",
  "+": "=",
  ":": ";",
  "@": "#",
  "'": "8",
  "\"": "2",
};

export function parseText(text: string): string[] {
  const glyphs: string[] = [];
  for (let i = 0; i < text.length; i += 1) {
    if (text[i] === "\r" && text[i + 1] === "\n") {
      glyphs.push("\n");
      i += 1;
    } else {
      glyphs.push(text[i]);
    }
  }
  return glyphs;
}

export function planTextToBlocks(
  text: string,
  keymap: KeymapDocument,
  initialPoint: MachinePointMm = { xMm: 0, yMm: 0 },
): GroupStreamPlan {
  const bindings = new Map<string, KeyBinding>();
  keymap.bindings.forEach((binding) => {
    bindings.set(normalizeKey(binding.key), binding);
  });

  const blocks: PlannedMotionBlock[] = [];
  const warnings: string[] = [];
  let current = initialPoint;

  for (const rawChar of parseText(text)) {
    if (rawChar === "\n" || rawChar === "\r") {
      blocks.push({ block: { type: "line_feed", lines: 1, ...lineFeedMotion }, sourceCharacter: "\n" });
      blocks.push({ block: { type: "return_zero", ...returnZeroMotion }, sourceCharacter: "\n" });
      current = initialPoint;
      continue;
    }

    for (const plannedKey of keysForGlyph(rawChar)) {
      const binding = bindings.get(plannedKey);
      if (!binding) {
        return planFailed("key_not_found", `缺少 ${displayKey(plannedKey)} 的坐标`, rawChar, blocks, warnings);
      }
      appendKeyPressBlocks(blocks, binding, rawChar, current, !isModifierKey(plannedKey));
      current = binding.point;
    }
  }

  if (blocks.length > 0) {
    blocks.push({ block: { type: "return_zero", ...returnZeroMotion } });
    blocks.push({ block: { type: "line_feed_home", ...lineFeedHomeMotion } });
  }

  if (Math.ceil(blocks.length / MAX_BLOCKS_PER_GROUP) > maxGroups) {
    return planFailed("plan_full", "任务组数量超过上限", undefined, blocks, warnings);
  }
  return { ok: true, blocks, groups: [], warnings };
}

export function chunkBlocksIntoGroups(blocks: PlannedMotionBlock[], jobId: string): PlannedRemoteMotionGroup[] {
  const groups: PlannedRemoteMotionGroup[] = [];
  let chunk: PlannedMotionBlock[] = [];
  let chunkRuntimeMs = 0;

  const flush = () => {
    if (chunk.length === 0) {
      return;
    }
    const seq = groups.length;
    const first = chunk[0];
    const estimatedRuntimeMs = chunkRuntimeMs;
    const absoluteMaxRuntimeMs = Math.min(Math.max(Math.ceil(estimatedRuntimeMs * 2), 12000), MAX_GROUP_RUNTIME_MS);
    groups.push({
      groupId: `${jobId}-${String(seq).padStart(4, "0")}`,
      seq,
      policy: { maxRuntimeMs: absoluteMaxRuntimeMs, onDisconnect: "cancel" },
      blocks: chunk.map((entry) => entry.block),
      plannedBlocks: chunk,
      kind: first?.block.type ?? "wait",
      sourceCharacter: first?.sourceCharacter,
      targetKeyLabel: first?.targetKeyLabel,
      displayCoordinates: first?.displayCoordinates,
      estimatedRuntimeMs,
      absoluteMaxRuntimeMs,
    });
    chunk = [];
    chunkRuntimeMs = 0;
  };

  for (const plannedBlock of blocks) {
    const blockRuntimeMs = estimateGroupRuntimeMs([plannedBlock.block]);
    const wouldExceedCount = chunk.length >= MAX_BLOCKS_PER_GROUP;
    const wouldExceedRuntime =
      chunk.length > 0 && chunkRuntimeMs + blockRuntimeMs > maxEstimatedGroupRuntimeMs;
    if (wouldExceedCount || wouldExceedRuntime) {
      flush();
    }
    chunk.push(plannedBlock);
    chunkRuntimeMs += blockRuntimeMs;
  }
  flush();
  return groups;
}

export function encodeExecGroup(group: TaskGroup, requestId: string): ExecGroupMessage {
  return {
    v: 1,
    requestId,
    type: "exec_group",
    groupId: group.groupId,
    seq: group.seq,
    policy: group.policy,
    blocks: group.blocks,
  };
}

export function planTextToRemoteMotionGroups(
  text: string,
  keymap: KeymapDocument,
  jobId: string,
  initialPoint: MachinePointMm = { xMm: 0, yMm: 0 },
): GroupStreamPlan {
  const plan = planTextToBlocks(text, keymap, initialPoint);
  const groups = chunkBlocksIntoGroups(plan.blocks, jobId);
  if (!plan.ok) {
    return { ...plan, groups } as GroupStreamPlan;
  }
  if (groups.length > maxGroups) {
    return planFailed("plan_full", "任务组数量超过上限", undefined, plan.blocks, plan.warnings);
  }
  return { ...plan, groups } as GroupStreamPlan;
}

function mmToSteps(deltaMm: number): number {
  const steps = Math.abs(deltaMm) * stepsPerMm;
  return deltaMm >= 0 ? Math.round(steps) : -Math.round(steps);
}

function moveXYTimeoutMs(dxSteps: number, dySteps: number): number {
  const distanceSteps = Math.max(Math.abs(dxSteps), Math.abs(dySteps));
  const timeoutMs = moveXYBaseTimeoutMs + distanceSteps * moveXYTimeoutMsPerStep;
  return clamp(timeoutMs, moveXYMinimumTimeoutMs, MAX_BLOCK_TIMEOUT_MS);
}

function keysForGlyph(rawChar: string): string[] {
  const shiftedBaseKey = shiftedSymbolKeys[rawChar];
  if (shiftedBaseKey) {
    return [capsLockKey, shiftedBaseKey, leftShiftKey];
  }
  if (/^[A-Z]$/.test(rawChar)) {
    return [capsLockKey, rawChar.toLowerCase(), leftShiftKey];
  }
  return [normalizeKey(rawChar)];
}

function appendKeyPressBlocks(
  blocks: PlannedMotionBlock[],
  binding: KeyBinding,
  sourceCharacter: string,
  current: MachinePointMm,
  includeCharacterRelease: boolean,
): void {
  const target = binding.point;
  const meta = {
    sourceCharacter,
    targetKeyLabel: displayKey(binding.key),
    displayCoordinates: target,
  };
  const dxSteps = mmToSteps(target.xMm - current.xMm);
  const dySteps = mmToSteps(target.yMm - current.yMm);
  if (dxSteps !== 0 || dySteps !== 0) {
    blocks.push({
      block: { type: "move_xy", dxSteps, dySteps, ...moveXYMotion, timeoutMs: moveXYTimeoutMs(dxSteps, dySteps) },
      ...meta,
    });
  }
  blocks.push({ block: { type: "press_down", ...pressDownMotion }, ...meta });
  blocks.push({ block: { type: "press_up", ...pressUpMotion }, ...meta });
  if (includeCharacterRelease) {
    blocks.push({ block: { type: "character_release", ...characterReleaseMotion }, ...meta });
  }
}

function isModifierKey(key: string): boolean {
  return key === capsLockKey || key === leftShiftKey;
}

function clamp(value: number, minValue: number, maxValue: number): number {
  return Math.min(Math.max(value, minValue), maxValue);
}

function planFailed(
  code: "key_not_found" | "plan_full",
  message: string,
  failedKey: string | undefined,
  blocks: PlannedMotionBlock[],
  warnings: string[],
): GroupStreamPlan {
  return {
    ok: false,
    code,
    message,
    failedKey,
    blocks,
    groups: [],
    warnings,
  };
}
