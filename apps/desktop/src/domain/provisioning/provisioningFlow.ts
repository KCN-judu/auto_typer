import type { ProvisioningDecision, WifiProvisionStatus } from "./types";

export function decideProvisioningNextStep(status: WifiProvisionStatus): ProvisioningDecision {
  if (status.state === "CONNECTED" && status.ip) {
    return {
      kind: "finish",
      ip: status.ip,
      nextState: "finishing_ap",
    };
  }
  if (status.state === "FAILED") {
    return {
      kind: "failed",
      reason: status.reason ?? "UNKNOWN",
      nextState: "sta_failed",
    };
  }
  return {
    kind: "wait",
    nextState: status.state === "CONNECTING" ? "waiting_sta" : "idle",
  };
}

export function formatProvisioningStatusLog(status: WifiProvisionStatus): string {
  const apSummary = status.ap ? ` ${status.ap.ssid}@${status.ap.ip}` : "";
  return `AP 状态 ${status.state}${apSummary}`;
}

export function formatProvisioningSuccessLog(ip: string): string {
  return `配网成功，设备地址 ${ip}，AP 已结束，3 秒后自动连接 TCP`;
}
