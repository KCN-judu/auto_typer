import type {
  KeyBinding,
  KeymapDocument,
  MachinePointMm,
  PrimitiveCommand,
  PrimitiveCommandProfile,
} from "../../../../shared/protocol/auto-typer-protocol";
import { displayKey, normalizeKey } from "./keymap";

export type PlannedPrimitiveCommand = {
  commandId: string;
  command: PrimitiveCommand;
  sourceCharacter?: string;
  targetKeyLabel?: string;
  displayCoordinates?: MachinePointMm;
  op: PrimitiveCommand["op"];
};

type PrimitiveCommandInput =
  | { op: "move_to"; xMm: number; yMm: number; profile?: PrimitiveCommandProfile }
  | { op: "wait"; durationMs: number }
  | { op: "press" | "release"; durationMs?: number }
  | { op: "character_release" | "line_feed" | "cancel" | "reset_fault" | "emergency_stop" };

export type PrimitivePlan =
  | {
      ok: true;
      commands: PlannedPrimitiveCommand[];
    }
  | {
      ok: false;
      code: "key_not_found" | "plan_full";
      message: string;
      failedKey?: string;
      commands: PlannedPrimitiveCommand[];
    };

const maxCommands = 256;

export function planTextToPrimitiveCommands(text: string, keymap: KeymapDocument, jobId: string): PrimitivePlan {
  const bindings = new Map<string, KeyBinding>();
  keymap.bindings.forEach((binding) => {
    bindings.set(normalizeKey(binding.key), binding);
  });

  const commands: PlannedPrimitiveCommand[] = [];
  let commandIndex = 0;

  function nextId(op: PrimitiveCommand["op"]) {
    return `${jobId}:${String(commandIndex).padStart(4, "0")}:${op}`;
  }

  function append(command: PrimitiveCommandInput, meta: Omit<PlannedPrimitiveCommand, "commandId" | "command" | "op"> = {}): boolean {
    if (commands.length >= maxCommands) {
      return false;
    }
    const op = command.op;
    const commandId = nextId(op);
    commands.push({
      commandId,
      command: {
        v: 1,
        type: "command",
        id: commandId,
        ...command,
      } as PrimitiveCommand,
      op,
      ...meta,
    });
    commandIndex += 1;
    return true;
  }

  for (let i = 0; i < text.length; i += 1) {
    const rawChar = text[i];
    if (rawChar === "\r" || rawChar === "\n") {
      if (rawChar === "\r" && text[i + 1] === "\n") {
        i += 1;
      }
      if (!append({ op: "line_feed" }, { sourceCharacter: "\n" })) {
        return planFull(commands);
      }
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
        commands,
      };
    }

    const target = binding.point;
    const characterMeta = {
      sourceCharacter: rawChar,
      targetKeyLabel: displayKey(binding.key),
      displayCoordinates: target,
    };
    if (
      !append({ op: "move_to", xMm: target.xMm, yMm: target.yMm }, characterMeta) ||
      !append({ op: "wait", durationMs: 80 }, characterMeta) ||
      !append({ op: "press" }, characterMeta) ||
      !append({ op: "release" }, characterMeta) ||
      !append({ op: "character_release" }, characterMeta)
    ) {
      return planFull(commands);
    }
  }

  return { ok: true, commands };
}

function planFull(commands: PlannedPrimitiveCommand[]): PrimitivePlan {
  return {
    ok: false,
    code: "plan_full",
    message: "任务命令数量超过固件单次队列上限",
    commands,
  };
}
