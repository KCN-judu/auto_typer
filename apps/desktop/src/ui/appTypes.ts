import type { DeviceStatus } from "../../../../shared/protocol/protocolTypes";
import type { ProvisioningUiState } from "../domain/provisioning";

export type View = "job" | "settings" | "keymap";
export type ConnectionState = "disconnected" | "connecting" | "connected" | "desync" | "transport_fault";
export type StreamState = "disconnected" | "connecting" | "connected" | "running" | "fault";
export type ProvisioningState = ProvisioningUiState;

export type WifiProvisionStatus = NonNullable<Window["autoTyper"]> extends {
  wifiProvisionGetStatus: (...args: never[]) => Promise<infer T>;
} ? T : never;

export type PrintTaskState = {
  stream: StreamState;
  running: boolean;
  requiresRecovery: boolean;
  currentIndex: number;
  totalBlocks: number;
  completedBlocks: number;
  currentLabel: string;
  fault?: string;
};

export type PersistedFieldPatch = Partial<{
  savedWifiSsid: string;
  savedWifiPassword: string;
  lastTcpHost: string;
  lastTcpPort: number;
}>;

export const defaultPort = 7777;

export const defaultPrintTaskState: PrintTaskState = {
  stream: "disconnected",
  running: false,
  requiresRecovery: false,
  currentIndex: 0,
  totalBlocks: 0,
  completedBlocks: 0,
  currentLabel: "",
};

export function idleStreamState(connection: ConnectionState): StreamState {
  switch (connection) {
    case "connecting":
      return "connecting";
    case "connected":
      return "connected";
    case "disconnected":
      return "disconnected";
    case "desync":
    case "transport_fault":
    default:
      return "fault";
  }
}

export function isDeviceBusy(status: DeviceStatus, printTask: PrintTaskState): boolean {
  const jobState = status.currentJob?.state;
  return printTask.running || status.mode === "running" || jobState === "queued" || jobState === "running" || jobState === "cancelling";
}
