import type {
  KeyBinding,
  KeymapDocument,
  MachinePointMm,
  RemoteMotionBlock,
} from "../../../../shared/protocol/auto-typer-protocol";
import { displayKey, normalizeKey } from "./keymap";

export type PlannedRemoteMotionBlock = {
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
      blocks: PlannedRemoteMotionBlock[];
    }
  | {
      ok: false;
      code: "key_not_found" | "plan_full";
      message: string;
      failedKey?: string;
      blocks: PlannedRemoteMotionBlock[];
    };

const maxBlocks = 256;
const defaultMoveProfile = { rpm: 1600, accelRaw: 128 } as const;

export function planTextToRemoteMotionBlocks(
  text: string,
  keymap: KeymapDocument,
  jobId: string,
  initialPoint: MachinePointMm = { xMm: 0, yMm: 0 },
): BlockStreamPlan {
  const bindings = new Map<string, KeyBinding>();
  keymap.bindings.forEach((binding) => {
    bindings.set(normalizeKey(binding.key), binding);
  });

  const blocks: PlannedRemoteMotionBlock[] = [];
  let blockIndex = 0;
  let current = initialPoint;

  function nextBlockId(kind: RemoteMotionBlock["kind"]) {
    return `${jobId}-${String(blockIndex).padStart(4, "0")}-${kind}`;
  }

  function append(block: RemoteMotionBlock, meta: Omit<PlannedRemoteMotionBlock, "blockId" | "block" | "kind"> = {}): boolean {
    if (blocks.length >= maxBlocks) {
      return false;
    }
    blocks.push({
      blockId: nextBlockId(block.kind),
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
      current = { ...current, xMm: initialPoint.xMm };
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
      !append(
        {
          kind: "move_xy",
          dxMm: target.xMm - current.xMm,
          dyMm: target.yMm - current.yMm,
          profile: defaultMoveProfile,
        },
        characterMeta,
      ) ||
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

function planFull(blocks: PlannedRemoteMotionBlock[]): BlockStreamPlan {
  return {
    ok: false,
    code: "plan_full",
    message: "任务块数量超过固件单次队列上限",
    blocks,
  };
}
