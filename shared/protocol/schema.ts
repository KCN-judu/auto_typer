import type { MachinePointMm } from "./auto-typer-protocol";

export function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null;
}

export function isMachinePointMm(value: unknown): value is MachinePointMm {
  return (
    isObject(value) &&
    typeof value.xMm === "number" &&
    Number.isFinite(value.xMm) &&
    typeof value.yMm === "number" &&
    Number.isFinite(value.yMm)
  );
}
