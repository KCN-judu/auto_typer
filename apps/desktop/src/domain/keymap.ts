import type { KeyBinding, KeymapDocument } from "../../../../shared/protocol/auto-typer-protocol";

export type KeymapIssue = {
  level: "error" | "warning";
  message: string;
  key?: string;
};

export const capsLockKey = "CAPSLOCK";
export const leftShiftKey = "LSHIFT";
export const poetryFeiyu200Keys = "1234567890-qwertyuiopasdfghjkl;zxcvbnm,.- ".split("");
export const feiyu200SymbolBaseKeys = ["/", "=", "#"] as const;
export const feiyu200ModifierKeys = [capsLockKey, leftShiftKey] as const;
export const feiyu200BaseKeys = [...poetryFeiyu200Keys, ...feiyu200SymbolBaseKeys];
export const feiyu200MappedKeys = [...feiyu200BaseKeys, ...feiyu200ModifierKeys];
export const poetryFeiyu200KeyCount = poetryFeiyu200Keys.length;

const keyOrder = new Map(feiyu200MappedKeys.map((key, index) => [key, index]));
const feiyu200KeyPitchX = 19.25;
const feiyu200OriginX = 28.775;
const feiyu200RowOffsets = [9, 19.5, 25, 37.5, 137.5] as const;
const feiyu200RowY = [108.925, 89.9625, 68, 52.4625, 30] as const;
const feiyu200PhysicalRows = ["1234567890-=", "qwertyuiop[]", "asdfghjkl;#", "zxcvbnm,./"] as const;
const modifierKeyPoints = {
  [capsLockKey]: {
    xMm: feiyu200OriginX + feiyu200RowOffsets[2] - feiyu200KeyPitchX,
    yMm: feiyu200RowY[2],
  },
  [leftShiftKey]: {
    xMm: feiyu200OriginX + feiyu200RowOffsets[3] - feiyu200KeyPitchX - 5,
    yMm: feiyu200RowY[3],
  },
} as const;

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
    if (!isFeiyu200MappedKey(key)) {
      issues.push({ level: "warning", key, message: `${displayKey(key)} 不在飞宇 200 映射集内` });
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

  feiyu200MappedKeys.forEach((key) => {
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
    if (isFeiyu200MappedKey(normalized)) {
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
      if (!isFeiyu200BaseKey(key)) {
        continue;
      }
      bindings.push({
        key,
        point: {
          xMm: feiyu200OriginX + feiyu200RowOffsets[row] + index * feiyu200KeyPitchX,
          yMm: feiyu200RowY[row],
        },
      });
    }
  });

  bindings.push({
    key: " ",
    point: {
      xMm: feiyu200OriginX + feiyu200RowOffsets[4],
      yMm: feiyu200RowY[4],
    },
  });
  feiyu200ModifierKeys.forEach((key) => {
    bindings.push({
      key,
      point: modifierKeyPoints[key],
    });
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
  const upper = key.toUpperCase();
  if (upper === capsLockKey || upper === "CAPS_LOCK") {
    return capsLockKey;
  }
  if (upper === leftShiftKey || upper === "LEFTSHIFT" || upper === "LEFT_SHIFT") {
    return leftShiftKey;
  }
  return key.length === 1 ? key.toLowerCase() : key;
}

export function displayKey(key: string): string {
  const normalized = normalizeKey(key);
  if (normalized === " ") {
    return "Space";
  }
  if (normalized === capsLockKey) {
    return "CapsLock";
  }
  if (normalized === leftShiftKey) {
    return "LShift";
  }
  return normalized;
}

export function isPoetryFeiyu200Key(key: string): boolean {
  return poetryFeiyu200Keys.includes(normalizeKey(key));
}

export function isFeiyu200BaseKey(key: string): boolean {
  return feiyu200BaseKeys.includes(normalizeKey(key));
}

export function isFeiyu200MappedKey(key: string): boolean {
  return keyOrder.has(normalizeKey(key));
}

function compareBindings(a: KeyBinding, b: KeyBinding): number {
  const aOrder = keyOrder.get(normalizeKey(a.key));
  const bOrder = keyOrder.get(normalizeKey(b.key));
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
