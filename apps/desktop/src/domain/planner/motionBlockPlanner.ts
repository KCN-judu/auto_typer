import type {
  AtomicMotionBlock,
  ExecuteBlockMessage,
  KeyBinding,
  KeymapDocument,
  MachinePointMm,
  MotionBlockRequest,
} from "../../../../../shared/protocol/protocolTypes";
import {
  MAX_ACTION_TIMEOUT_MS,
  MAX_BLOCK_RUNTIME_MS,
} from "../../../../../shared/protocol/protocolTypes";
import { estimateBlockRuntimeMs } from "../runtime/blockRuntime";
import { capsLockKey, displayKey, leftShiftKey, normalizeKey } from "../keymap";
import {
  encodeLineFeedMove,
  encodeLineFeedHome,
  encodePressMove,
  encodeXyMove,
  initialMotionPosition,
  type EncodedMotion,
  type MotionPositionState,
} from "./absoluteMotionEncoder";

export type PlannedMotionBlock = {
  block: AtomicMotionBlock;
  positionAfter: MotionPositionState;
  sourceCharacter?: string;
  targetKeyLabel?: string;
  displayCoordinates?: MachinePointMm;
};

export type PlannedRemoteMotionBlock = MotionBlockRequest & {
  plannedBlock: PlannedMotionBlock;
  kind: string;
  estimatedRuntimeMs: number;
  absoluteMaxRuntimeMs: number;
};

export type MotionBlockPlan =
  | {
      ok: true;
      blocks: PlannedMotionBlock[];
      requests: PlannedRemoteMotionBlock[];
      finalPosition: MotionPositionState;
      warnings: string[];
    }
  | {
      ok: false;
      code: "key_not_found" | "plan_full";
      message: string;
      failedKey?: string;
      blocks: PlannedMotionBlock[];
      requests: PlannedRemoteMotionBlock[];
      finalPosition: MotionPositionState;
      warnings: string[];
    };

type ModifierToken = {
  capsLockOn: boolean;
  glyphs: string[];
};

const maxBlocks = 1024;
const stepsPerMm = 80;
const moveXYMinimumTimeoutMs = 5000;
const moveXYBaseTimeoutMs = 3000;
const moveXYTimeoutMsPerStep = 2;
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
  for (let index = 0; index < text.length; index += 1) {
    if (text[index] === "\r" && text[index + 1] === "\n") {
      glyphs.push("\n");
      index += 1;
    } else {
      glyphs.push(text[index]);
    }
  }
  return glyphs;
}

export function planTextToMotionBlocks(
  text: string,
  keymap: KeymapDocument,
  jobId: string,
  initialPoint: MachinePointMm = { xMm: 0, yMm: 0 },
  initialPosition: MotionPositionState = initialMotionPosition(),
): MotionBlockPlan {
  const bindings = new Map<string, KeyBinding>();
  keymap.bindings.forEach((binding) => bindings.set(normalizeKey(binding.key), binding));

  const blocks: PlannedMotionBlock[] = [];
  const warnings: string[] = [];
  let currentPoint = initialPoint;
  let position = { ...initialPosition };

  const append = (encoded: EncodedMotion | undefined, metadata: Omit<PlannedMotionBlock, "block" | "positionAfter"> = {}) => {
    if (!encoded) {
      return;
    }
    position = encoded.position;
    blocks.push({ block: encoded.block, positionAfter: position, ...metadata });
  };

  for (const token of tokenizeTextByModifierState(text)) {
    if (token.capsLockOn) {
      const binding = bindings.get(capsLockKey);
      if (!binding) {
        return planFailed("key_not_found", `缺少 ${displayKey(capsLockKey)} 的坐标`, token.glyphs[0], blocks, position, warnings);
      }
      ({ currentPoint, position } = appendKeyPressBlocks(blocks, binding, token.glyphs[0] ?? "", currentPoint, position, false));
    }

    for (const rawCharacter of token.glyphs) {
      if (rawCharacter === "\n") {
        for (const encoded of encodeLineFeedHome(position)) {
          append(encoded, { sourceCharacter: "\n" });
        }
        append(encodeXyMove(position, 0, 0, MAX_ACTION_TIMEOUT_MS), { sourceCharacter: "\n" });
        currentPoint = initialPoint;
        continue;
      }

      const plannedKey = keyForGlyph(rawCharacter);
      const binding = bindings.get(plannedKey);
      if (!binding) {
        return planFailed("key_not_found", `缺少 ${displayKey(plannedKey)} 的坐标`, rawCharacter, blocks, position, warnings);
      }
      ({ currentPoint, position } = appendKeyPressBlocks(blocks, binding, rawCharacter, currentPoint, position, !isModifierKey(plannedKey)));
    }

    if (token.capsLockOn) {
      const binding = bindings.get(leftShiftKey);
      if (!binding) {
        return planFailed("key_not_found", `缺少 ${displayKey(leftShiftKey)} 的坐标`, token.glyphs.at(-1), blocks, position, warnings);
      }
      ({ currentPoint, position } = appendKeyPressBlocks(blocks, binding, token.glyphs.at(-1) ?? "", currentPoint, position, false));
    }
  }

  if (blocks.length > 0) {
    append(encodeXyMove(position, 0, 0, MAX_ACTION_TIMEOUT_MS));
    for (const encoded of encodeLineFeedHome(position)) {
      append(encoded);
    }
  }

  if (blocks.length > maxBlocks) {
    return planFailed("plan_full", "原子动作数量超过上限", undefined, blocks, position, warnings);
  }
  return {
    ok: true,
    blocks,
    requests: blocks.map((block, seq) => toRequest(block, jobId, seq)),
    finalPosition: position,
    warnings,
  };
}

export function encodeExecuteBlock(block: MotionBlockRequest, requestId: string): ExecuteBlockMessage {
  return { v: 1, requestId, type: "execute_block", ...block };
}

export function tokenizeTextByModifierState(text: string): ModifierToken[] {
  const tokens: ModifierToken[] = [];
  let currentToken: ModifierToken | undefined;
  for (const rawCharacter of parseText(text)) {
    const capsLockOn = requiresModifierMode(rawCharacter);
    if (!currentToken || (rawCharacter !== "\n" && currentToken.capsLockOn !== capsLockOn)) {
      currentToken = { capsLockOn, glyphs: [] };
      tokens.push(currentToken);
    }
    currentToken.glyphs.push(rawCharacter);
  }
  return tokens.filter((token) => token.glyphs.length > 0);
}

function appendKeyPressBlocks(
  blocks: PlannedMotionBlock[],
  binding: KeyBinding,
  sourceCharacter: string,
  currentPoint: MachinePointMm,
  initialPosition: MotionPositionState,
  includeCharacterAdvance: boolean,
): { currentPoint: MachinePointMm; position: MotionPositionState } {
  const metadata = {
    sourceCharacter,
    targetKeyLabel: displayKey(binding.key),
    displayCoordinates: binding.point,
  };
  let position = initialPosition;
  const append = (encoded: EncodedMotion | undefined) => {
    if (encoded) {
      position = encoded.position;
      blocks.push({ block: encoded.block, positionAfter: position, ...metadata });
    }
  };
  const dxSteps = mmToSteps(binding.point.xMm - currentPoint.xMm);
  const dySteps = mmToSteps(binding.point.yMm - currentPoint.yMm);
  append(encodeXyMove(position, position.x + dxSteps, position.y + dySteps, moveXYTimeoutMs(dxSteps, dySteps)));
  append(encodePressMove(position, -2700));
  append(encodePressMove(position, 0));
  if (includeCharacterAdvance) {
    append(encodeLineFeedMove(position, position.l - 180, { rpm: 1600, accelRaw: 128, timeoutMs: 10000 }));
  }
  return { currentPoint: binding.point, position };
}

function toRequest(plannedBlock: PlannedMotionBlock, jobId: string, seq: number): PlannedRemoteMotionBlock {
  const estimatedRuntimeMs = estimateBlockRuntimeMs(plannedBlock.block);
  const absoluteMaxRuntimeMs = Math.min(Math.max(Math.ceil(estimatedRuntimeMs * 2), 12000), MAX_BLOCK_RUNTIME_MS);
  return {
    blockId: `${jobId}-${String(seq).padStart(4, "0")}`,
    seq,
    policy: { maxRuntimeMs: absoluteMaxRuntimeMs, onDisconnect: "cancel" },
    block: plannedBlock.block,
    plannedBlock,
    kind: describeMotionBlock(plannedBlock.block),
    estimatedRuntimeMs,
    absoluteMaxRuntimeMs,
  };
}

function mmToSteps(deltaMm: number): number {
  const steps = Math.abs(deltaMm) * stepsPerMm;
  return deltaMm >= 0 ? Math.round(steps) : -Math.round(steps);
}

function moveXYTimeoutMs(dxSteps: number, dySteps: number): number {
  const distanceSteps = Math.max(Math.abs(dxSteps), Math.abs(dySteps));
  return clamp(moveXYBaseTimeoutMs + distanceSteps * moveXYTimeoutMsPerStep, moveXYMinimumTimeoutMs, MAX_ACTION_TIMEOUT_MS);
}

function keyForGlyph(rawCharacter: string): string {
  return shiftedSymbolKeys[rawCharacter] ?? (/^[A-Z]$/.test(rawCharacter) ? rawCharacter.toLowerCase() : normalizeKey(rawCharacter));
}

function requiresModifierMode(rawCharacter: string): boolean {
  return Boolean(shiftedSymbolKeys[rawCharacter]) || /^[A-Z]$/.test(rawCharacter);
}

function describeMotionBlock(block: AtomicMotionBlock): string {
  return block.length === 1 ? block[0].type : "parallel";
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
  finalPosition: MotionPositionState,
  warnings: string[],
): MotionBlockPlan {
  return { ok: false, code, message, failedKey, blocks, requests: [], finalPosition, warnings };
}
