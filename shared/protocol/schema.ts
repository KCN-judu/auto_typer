import type { ApiErrorBody, MachinePointMm, ProbeKeyRequest } from "./auto-typer-protocol";

export function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null;
}

export function isApiErrorBody(value: unknown): value is ApiErrorBody {
  return (
    isObject(value) &&
    typeof value.code === "string" &&
    typeof value.message === "string"
  );
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

export function isProbeKeyRequest(value: unknown): value is ProbeKeyRequest {
  return (
    isObject(value) &&
    typeof value.key === "string" &&
    value.key.length > 0 &&
    isMachinePointMm(value.point)
  );
}
