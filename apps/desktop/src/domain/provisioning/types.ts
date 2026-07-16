export type WifiProvisionStatus = {
  state: "IDLE" | "CONNECTING" | "CONNECTED" | "FAILED";
  reason?: string;
  targetSsid?: string;
  ip?: string;
  ap?: {
    ssid: string;
    ip: string;
  };
};

export type ProvisioningUiState =
  | "idle"
  | "probing_ap"
  | "sending_credentials"
  | "waiting_sta"
  | "finishing_ap"
  | "sta_connected"
  | "sta_failed";

export type ProvisioningDecision =
  | {
      kind: "finish";
      ip: string;
      nextState: Extract<ProvisioningUiState, "finishing_ap">;
    }
  | {
      kind: "failed";
      reason: string;
      nextState: Extract<ProvisioningUiState, "sta_failed">;
    }
  | {
      kind: "wait";
      nextState: Extract<ProvisioningUiState, "idle" | "waiting_sta">;
    };
