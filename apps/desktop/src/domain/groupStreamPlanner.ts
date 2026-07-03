import type {
  KeyBinding,
  KeymapDocument,
  MachinePointMm,
  RemoteMotionStep,
} from "../../../../shared/protocol/auto-typer-protocol";
import { displayKey, normalizeKey } from "./keymap";

export type PlannedRemoteMotionGroup = {
  groupId: string;
  steps: RemoteMotionStep[];
  sourceCharacter?: string;
  targetKeyLabel?: string;
  displayCoordinates?: MachinePointMm;
  kind: "move_xy" | "press" | "release" | "character_release" | "line_feed";
};

export type GroupStreamPlan =
  | {
      ok: true;
      groups: PlannedRemoteMotionGroup[];
      warnings: string[];
    }
  | {
      ok: false;
      code: "key_not_found" | "plan_full";
      message: string;
      failedKey?: string;
      groups: PlannedRemoteMotionGroup[];
      warnings: string[];
    };

const maxGroups = 1024;
const defaultMoveProfile = { rpm: 1600, accelRaw: 128 } as const;

export type GroupStreamPlanningOptions = {
  disableLineFeed?: boolean;
};

export function planTextToRemoteMotionGroups(
  text: string,
  keymap: KeymapDocument,
  jobId: string,
  initialPoint: MachinePointMm = { xMm: 0, yMm: 0 },
  options: GroupStreamPlanningOptions = {},
): GroupStreamPlan {
  const bindings = new Map<string, KeyBinding>();
  keymap.bindings.forEach((binding) => {
    bindings.set(normalizeKey(binding.key), binding);
  });

  const groups: PlannedRemoteMotionGroup[] = [];
  const warnings: string[] = [];
  const disableLineFeed = options.disableLineFeed === true;
  if (disableLineFeed) {
    warnings.push("已启用跳过走纸模式，跳过字符释放和换行走纸");
  }
  let groupIndex = 0;
  let current = initialPoint;

  function nextGroupId(kind: PlannedRemoteMotionGroup["kind"]) {
    return `${jobId}-${String(groupIndex).padStart(4, "0")}-${kind}`;
  }

  function appendGroup(
    kind: PlannedRemoteMotionGroup["kind"],
    steps: RemoteMotionStep[],
    meta: Omit<PlannedRemoteMotionGroup, "groupId" | "steps" | "kind"> = {},
  ): boolean {
    if (groups.length >= maxGroups) {
      return false;
    }
    groups.push({
      groupId: nextGroupId(kind),
      steps,
      kind,
      ...meta,
    });
    groupIndex += 1;
    return true;
  }

  for (let i = 0; i < text.length; i += 1) {
    const rawChar = text[i];
    if (rawChar === "\r" || rawChar === "\n") {
      if (rawChar === "\r" && text[i + 1] === "\n") {
        i += 1;
      }
      if (!disableLineFeed && !appendGroup("line_feed", [{ kind: "line_feed" }], { sourceCharacter: "\n" })) {
        return planFull(groups, warnings);
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
        groups,
        warnings,
      };
    }

    const target = binding.point;
    const characterMeta = {
      sourceCharacter: rawChar,
      targetKeyLabel: displayKey(binding.key),
      displayCoordinates: target,
    };
    const hasMove = target.xMm !== current.xMm || target.yMm !== current.yMm;
    if (hasMove &&
        !appendGroup(
          "move_xy",
          [{
            kind: "move_xy",
            dxMm: target.xMm - current.xMm,
            dyMm: target.yMm - current.yMm,
            profile: defaultMoveProfile,
          }],
          characterMeta,
        )) {
      return planFull(groups, warnings);
    }
    if (!appendGroup("press", [{ kind: "servo_press" }], characterMeta)) {
      return planFull(groups, warnings);
    }
    if (!appendGroup("release", [{ kind: "servo_release" }], characterMeta)) {
      return planFull(groups, warnings);
    }
    if (!disableLineFeed && !appendGroup("character_release", [{ kind: "character_release" }], characterMeta)) {
      return planFull(groups, warnings);
    }
    current = target;
  }

  return { ok: true, groups, warnings };
}

function planFull(groups: PlannedRemoteMotionGroup[], warnings: string[]): GroupStreamPlan {
  return {
    ok: false,
    code: "plan_full",
    message: "任务组数量超过固件单次队列上限",
    groups,
    warnings,
  };
}
