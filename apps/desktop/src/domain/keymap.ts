import type { KeyBinding, KeymapDocument } from "../../../../shared/protocol/auto-typer-protocol";

export type KeymapIssue = {
  level: "error" | "warning";
  message: string;
  key?: string;
};

export const poetryFeiyu200Keys = "1234567890-qwertyuiopasdfghjkl;'zxcvbnm,.- ".split("");
export const poetryFeiyu200KeyCount = poetryFeiyu200Keys.length;

const poetryKeyOrder = new Map(poetryFeiyu200Keys.map((key, index) => [key, index]));
const feiyu200KeyPitchX = 19;
const feiyu200RowOffsets = [0, 42.5, 25, 57.5, 137.5] as const;
const feiyu200RowY = [106, 87, 68, 49, 30] as const;
const feiyu200PhysicalRows = ["1234567890-=", "qwertyuiop[]", "asdfghjkl;'", "zxcvbnm,./"] as const;

export function emptyKeymap(): KeymapDocument {
  return {
    version: 1,
    machine: "feiyu200",
    updatedAt: new Date().toISOString(),
    bindings: [],
  };
}

export function validateKeymap(keymap: KeymapDocument): KeymapIssue[] {
  const issues: KeymapIssue[] = [];
  const seen = new Map<string, number>();

  keymap.bindings.forEach((binding) => {
    const key = normalizeKey(binding.key);
    seen.set(key, (seen.get(key) ?? 0) + 1);
    if (!isPoetryFeiyu200Key(key)) {
      issues.push({ level: "warning", key, message: `${displayKey(key)} 不在诗歌字符集内` });
    }
    if (!Number.isFinite(binding.point.xMm) || !Number.isFinite(binding.point.yMm)) {
      issues.push({ level: "error", key, message: "坐标必须是有效数字" });
    }
  });

  for (const [key, count] of seen) {
    if (count > 1) {
      issues.push({ level: "error", key, message: `按键 ${displayKey(key)} 重复绑定` });
    }
  }

  poetryFeiyu200Keys.forEach((key) => {
    if (!seen.has(key)) {
      issues.push({ level: "warning", key, message: `缺少 ${displayKey(key)} 的坐标` });
    }
  });

  return issues;
}

export function upsertBinding(keymap: KeymapDocument, binding: KeyBinding): KeymapDocument {
  const normalized = normalizeKey(binding.key);
  const nextBindings = keymap.bindings.filter((entry) => normalizeKey(entry.key) !== normalized);
  nextBindings.push({ key: normalized, point: binding.point });
  nextBindings.sort(compareBindings);
  return {
    ...keymap,
    version: keymap.version + 1,
    updatedAt: new Date().toISOString(),
    bindings: nextBindings,
  };
}

export function sanitizeKeymap(keymap: KeymapDocument): KeymapDocument {
  const byKey = new Map<string, KeyBinding>();
  keymap.bindings.forEach((binding) => {
    const normalized = normalizeKey(binding.key);
    if (isPoetryFeiyu200Key(normalized)) {
      byKey.set(normalized, { key: normalized, point: binding.point });
    }
  });

  return {
    ...keymap,
    bindings: Array.from(byKey.values()).sort(compareBindings),
  };
}

export function currentFeiyu200Keymap(base?: Partial<KeymapDocument>): KeymapDocument {
  if (base?.bindings && base.bindings.length > 0) {
    return sanitizeKeymap({
      version: base.version ?? 1,
      machine: "feiyu200",
      updatedAt: base.updatedAt ?? new Date().toISOString(),
      bindings: base.bindings,
    });
  }

  const bindings: KeyBinding[] = [];

  feiyu200PhysicalRows.forEach((rowKeys, row) => {
    for (let index = 0; index < rowKeys.length; index += 1) {
      const key = rowKeys[index];
      if (!isPoetryFeiyu200Key(key)) {
        continue;
      }
      bindings.push({
        key,
        point: {
          xMm: feiyu200RowOffsets[row] + index * feiyu200KeyPitchX,
          yMm: feiyu200RowY[row],
        },
      });
    }
  });

  bindings.push({
    key: " ",
    point: {
      xMm: feiyu200RowOffsets[4],
      yMm: feiyu200RowY[4],
    },
  });

  return sanitizeKeymap({
    version: base?.version ?? 1,
    machine: "feiyu200",
    updatedAt: base?.updatedAt ?? new Date().toISOString(),
    bindings,
  });
}

export function normalizeKey(key: string): string {
  if (key === "Space") {
    return " ";
  }
  return key.length === 1 ? key.toLowerCase() : key;
}

export function displayKey(key: string): string {
  return key === " " ? "Space" : key;
}

export function isPoetryFeiyu200Key(key: string): boolean {
  return poetryKeyOrder.has(normalizeKey(key));
}

function compareBindings(a: KeyBinding, b: KeyBinding): number {
  const aOrder = poetryKeyOrder.get(normalizeKey(a.key));
  const bOrder = poetryKeyOrder.get(normalizeKey(b.key));
  if (aOrder !== undefined && bOrder !== undefined) {
    return aOrder - bOrder;
  }
  if (aOrder !== undefined) {
    return -1;
  }
  if (bOrder !== undefined) {
    return 1;
  }
  return a.key.localeCompare(b.key);
}
