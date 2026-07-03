import type {
  ExecGroupMessage,
  KeyBinding,
  KeymapDocument,
  MachinePointMm,
  MotionBlock,
  TaskGroup,
} from "../../../../shared/protocol/auto-typer-protocol";
import {
  MAX_BLOCKS_PER_GROUP,
  MAX_GROUP_RUNTIME_MS,
} from "../../../../shared/protocol/auto-typer-protocol";
import { displayKey, normalizeKey } from "./keymap";

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
  kind: MotionBlock["type"];
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

const defaultMotion = { rpm: 1600, accelRaw: 128, timeoutMs: 3000 } as const;
const lineFeedMotion = { rpm: 1600, accelRaw: 128, timeoutMs: 10000 } as const;
const maxGroups = 1024;
const maxRuntimeMs = 5000;
const stepsPerMm = 80;

export type GroupStreamPlanningOptions = {
  disableLineFeed?: boolean;
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
  options: GroupStreamPlanningOptions = {},
): GroupStreamPlan {
  const bindings = new Map<string, KeyBinding>();
  keymap.bindings.forEach((binding) => {
    bindings.set(normalizeKey(binding.key), binding);
  });

  const blocks: PlannedMotionBlock[] = [];
  const warnings: string[] = [];
  const disableLineFeed = options.disableLineFeed === true;
  if (disableLineFeed) {
    warnings.push("已启用跳过走纸模式，跳过字符释放和换行走纸");
  }
  let current = initialPoint;

  for (const rawChar of parseText(text)) {
    if (rawChar === "\n" || rawChar === "\r") {
      if (!disableLineFeed) {
        blocks.push({ block: { type: "line_feed", lines: 1, ...lineFeedMotion }, sourceCharacter: "\n" });
      }
      current = { ...current, xMm: initialPoint.xMm };
      continue;
    }

    const key = normalizeKey(rawChar);
    const binding = bindings.get(key);
    if (!binding) {
      return planFailed("key_not_found", `缺少 ${displayKey(rawChar)} 的坐标`, rawChar, blocks, warnings);
    }

    const target = binding.point;
    const meta = {
      sourceCharacter: rawChar,
      targetKeyLabel: displayKey(binding.key),
      displayCoordinates: target,
    };
    const dxSteps = mmToSteps(target.xMm - current.xMm);
    const dySteps = mmToSteps(target.yMm - current.yMm);
    if (dxSteps !== 0 || dySteps !== 0) {
      blocks.push({ block: { type: "move_xy", dxSteps, dySteps, ...defaultMotion }, ...meta });
    }
    blocks.push({ block: { type: "press_down", ...defaultMotion }, ...meta });
    blocks.push({ block: { type: "press_up", ...defaultMotion }, ...meta });
    if (!disableLineFeed) {
      blocks.push({ block: { type: "character_release", ...defaultMotion }, ...meta });
    }
    current = target;
  }

  if (Math.ceil(blocks.length / MAX_BLOCKS_PER_GROUP) > maxGroups) {
    return planFailed("plan_full", "任务组数量超过上限", undefined, blocks, warnings);
  }
  return { ok: true, blocks, groups: [], warnings };
}

export function chunkBlocksIntoGroups(blocks: PlannedMotionBlock[], jobId: string): PlannedRemoteMotionGroup[] {
  const groups: PlannedRemoteMotionGroup[] = [];
  for (let index = 0; index < blocks.length; index += MAX_BLOCKS_PER_GROUP) {
    const chunk = blocks.slice(index, index + MAX_BLOCKS_PER_GROUP);
    const seq = groups.length;
    const first = chunk[0];
    groups.push({
      groupId: `${jobId}-${String(seq).padStart(4, "0")}`,
      seq,
      policy: { maxRuntimeMs: Math.min(maxRuntimeMs, MAX_GROUP_RUNTIME_MS), onDisconnect: "cancel" },
      blocks: chunk.map((entry) => entry.block),
      kind: first?.block.type ?? "wait",
      sourceCharacter: first?.sourceCharacter,
      targetKeyLabel: first?.targetKeyLabel,
      displayCoordinates: first?.displayCoordinates,
    });
  }
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
  options: GroupStreamPlanningOptions = {},
): GroupStreamPlan {
  const plan = planTextToBlocks(text, keymap, initialPoint, options);
  const groups = chunkBlocksIntoGroups(plan.blocks, jobId);
  return { ...plan, groups } as GroupStreamPlan;
}

function mmToSteps(deltaMm: number): number {
  const steps = Math.abs(deltaMm) * stepsPerMm;
  return deltaMm >= 0 ? Math.round(steps) : -Math.round(steps);
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
