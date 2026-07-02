import type {
  KeyBinding,
  KeymapDocument,
  MachinePointMm,
  RemoteMotionBlock,
} from "../../../../shared/protocol/auto-typer-protocol";
import { displayKey, normalizeKey } from "./keymap";

export type PlannedRemoteBlock = {
  blockId: string;
  block: RemoteMotionBlock;
  sourceCharacter?: string;
  targetKeyLabel?: string;
  displayCoordinates?: MachinePointMm;
  kind: RemoteMotionBlock["kind"];
};

export type BlockStreamPlan =
  | {
      ok: true;
      blocks: PlannedRemoteBlock[];
    }
  | {
      ok: false;
      code: "key_not_found" | "plan_full";
      message: string;
      failedKey?: string;
      blocks: PlannedRemoteBlock[];
    };

const maxBlocks = 256;

export function planTextToRemoteBlocks(text: string, keymap: KeymapDocument, jobId: string): BlockStreamPlan {
  const bindings = new Map<string, KeyBinding>();
  keymap.bindings.forEach((binding) => {
    bindings.set(normalizeKey(binding.key), binding);
  });

  const blocks: PlannedRemoteBlock[] = [];
  let current: MachinePointMm = { xMm: 0, yMm: 0 };
  let blockIndex = 0;

  function append(block: RemoteMotionBlock, meta: Omit<PlannedRemoteBlock, "blockId" | "block" | "kind"> = {}): boolean {
    if (blocks.length >= maxBlocks) {
      return false;
    }
    blocks.push({
      blockId: `${jobId}-${String(blockIndex).padStart(4, "0")}`,
      block,
      kind: block.kind,
      ...meta,
    });
    blockIndex += 1;
    return true;
  }

  for (let i = 0; i < text.length; i += 1) {
    const rawChar = text[i];
    if (rawChar === "\r" || rawChar === "\n") {
      if (rawChar === "\r" && text[i + 1] === "\n") {
        i += 1;
      }
      if (!append({ kind: "line_feed" }, { sourceCharacter: "\n" })) {
        return planFull(blocks);
      }
      current = { ...current, xMm: 0 };
      continue;
    }

    const key = normalizeKey(rawChar);
    const binding = bindings.get(key);
    if (!binding) {
      return {
        ok: false,
        code: "key_not_found",
        message: `缺少 ${displayKey(rawChar)} 的坐标`,
        failedKey: rawChar,
        blocks,
      };
    }

    const target = binding.point;
    const characterMeta = {
      sourceCharacter: rawChar,
      targetKeyLabel: displayKey(binding.key),
      displayCoordinates: target,
    };
    if (
      !append({ kind: "move_xy", dxMm: target.xMm - current.xMm, dyMm: target.yMm - current.yMm }, characterMeta) ||
      !append({ kind: "servo_press" }, characterMeta) ||
      !append({ kind: "servo_release" }, characterMeta) ||
      !append({ kind: "character_release" }, characterMeta)
    ) {
      return planFull(blocks);
    }
    current = target;
  }

  return { ok: true, blocks };
}

function planFull(blocks: PlannedRemoteBlock[]): BlockStreamPlan {
  return {
    ok: false,
    code: "plan_full",
    message: "任务块数量超过固件单次队列上限",
    blocks,
  };
}
