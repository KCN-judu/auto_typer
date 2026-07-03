import type {
  CancelResultMessage,
  DeviceStatus,
  GroupAcceptedMessage,
  GroupRejectedMessage,
  GroupStreamCommandMessage,
  GroupStreamEventMessage,
  KeymapDocument,
  KeymapMessage,
  PressDiagM5ResultMessage,
  ProbeResultMessage,
  ResetFaultResultMessage,
  StatusMessage,
  TaskGroup,
  WifiConfigResultMessage,
  WifiNetworksMessage,
  WifiSetupFinishedMessage,
  WifiStatus,
  WifiStatusMessage,
} from "../../../../shared/protocol/auto-typer-protocol";
import { encodeExecGroup } from "./groupStreamPlanner";

export type GroupStreamConnection = {
  host: string;
  port?: number;
};

export type GroupStreamListener = (message: GroupStreamEventMessage) => void;

const defaultPort = 7777;

export class GroupStreamClient {
  private sequence = 0;
  private unsubscribe?: () => void;
  private listeners = new Set<GroupStreamListener>();

  async connect({ host, port = defaultPort }: GroupStreamConnection): Promise<void> {
    if (!window.autoTyper) {
      throw new Error("TCP IPC is unavailable");
    }
    await window.autoTyper.groupStreamConnect({ host, port });
    if (!this.unsubscribe) {
      this.unsubscribe = window.autoTyper.groupStreamOnMessage((message) => {
        this.listeners.forEach((listener) => listener(message));
      });
    }
  }

  async disconnect(): Promise<void> {
    if (!window.autoTyper) {
      throw new Error("TCP IPC is unavailable");
    }
    await window.autoTyper.groupStreamDisconnect();
  }

  onMessage(listener: GroupStreamListener): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  async getStatus(): Promise<DeviceStatus> {
    const message = await this.sendCommand({ v: 1, requestId: this.nextId("status"), type: "get_status" });
    if (message.type !== "status") {
      throw responseError(message, "get_status");
    }
    return (message as StatusMessage).status;
  }

  async subscribeTelemetry(intervalMs: number): Promise<void> {
    const message = await this.sendCommand({
      v: 1,
      requestId: this.nextId("telemetry"),
      type: "subscribe_telemetry",
      intervalMs,
    });
    if (message.type !== "telemetry_subscribed") {
      throw responseError(message, "subscribe_telemetry");
    }
  }

  async getKeymap(): Promise<KeymapDocument> {
    const message = await this.sendCommand({ v: 1, requestId: this.nextId("keymap"), type: "get_keymap" });
    if (message.type !== "keymap") {
      throw responseError(message, "get_keymap");
    }
    return keymapFromMessage(message as KeymapMessage);
  }

  async getWifiStatus(): Promise<WifiStatus> {
    const message = await this.sendCommand({ v: 1, requestId: this.nextId("wifi"), type: "get_wifi_status" });
    if (message.type !== "wifi_status") {
      throw responseError(message, "get_wifi_status");
    }
    return (message as WifiStatusMessage).wifi;
  }

  async scanWifi(): Promise<WifiNetworksMessage> {
    const message = await this.sendCommand({ v: 1, requestId: this.nextId("scan"), type: "scan_wifi" });
    if (message.type !== "wifi_networks") {
      throw responseError(message, "scan_wifi");
    }
    const networks = message as WifiNetworksMessage;
    if (!networks.ok) {
      throw new Error(`${networks.code ?? "wifi_scan_failed"}: ${networks.message ?? "WiFi scan failed"}`);
    }
    return networks;
  }

  async configureWifi(ssid: string, password: string): Promise<WifiConfigResultMessage> {
    const message = await this.sendCommand({
      v: 1,
      requestId: this.nextId("wifi-config"),
      type: "configure_wifi",
      ssid,
      password,
    });
    if (message.type !== "wifi_config_result") {
      throw responseError(message, "configure_wifi");
    }
    return message as WifiConfigResultMessage;
  }

  async finishWifiSetup(): Promise<WifiSetupFinishedMessage> {
    const message = await this.sendCommand({
      v: 1,
      requestId: this.nextId("wifi-finish"),
      type: "finish_wifi_setup",
    });
    if (message.type !== "wifi_setup_finished") {
      throw responseError(message, "finish_wifi_setup");
    }
    return message as WifiSetupFinishedMessage;
  }

  async sendExecGroup(group: TaskGroup): Promise<GroupAcceptedMessage> {
    const message = await this.sendCommand(encodeExecGroup(group, this.nextId("group")));
    if (message.type === "group_rejected") {
      const rejected = message as GroupRejectedMessage;
      throw new Error(`${rejected.reason}: ${rejected.message}`);
    }
    if (message.type !== "group_accepted") {
      throw responseError(message, "exec_group");
    }
    return message as GroupAcceptedMessage;
  }

  async cancel(): Promise<CancelResultMessage> {
    const message = await this.sendCommand({ v: 1, requestId: this.nextId("cancel"), type: "cancel" });
    if (message.type !== "cancel_result") {
      throw responseError(message, "cancel");
    }
    return message as CancelResultMessage;
  }

  async resetFault(): Promise<ResetFaultResultMessage> {
    const message = await this.sendCommand({ v: 1, requestId: this.nextId("reset"), type: "reset_fault" });
    if (message.type !== "reset_fault_result") {
      throw responseError(message, "reset_fault");
    }
    return message as ResetFaultResultMessage;
  }

  async probe(): Promise<ProbeResultMessage> {
    const message = await this.sendCommand({ v: 1, requestId: this.nextId("probe"), type: "probe" });
    if (message.type !== "probe_result") {
      throw responseError(message, "probe");
    }
    return message as ProbeResultMessage;
  }

  async pressDiagM5(): Promise<PressDiagM5ResultMessage> {
    const message = await this.sendCommand({
      v: 1,
      requestId: this.nextId("press-diag-m5"),
      type: "press_diag_m5",
    });
    if (message.type !== "press_diag_m5_result") {
      throw responseError(message, "press_diag_m5");
    }
    return message as PressDiagM5ResultMessage;
  }

  private async sendCommand(message: GroupStreamCommandMessage): Promise<GroupStreamEventMessage> {
    if (!window.autoTyper) {
      throw new Error("TCP IPC is unavailable");
    }
    return window.autoTyper.groupStreamSend(message);
  }

  private nextId(prefix: string): string {
    this.sequence += 1;
    return `${prefix}-${Date.now().toString(36)}-${this.sequence.toString(36)}`;
  }
}

function keymapFromMessage(message: KeymapMessage): KeymapDocument {
  return {
    version: 1,
    machine: "feiyu200",
    updatedAt: new Date().toISOString(),
    bindings: message.keys.map((key) => ({
      key: key.label,
      point: { xMm: key.xMm, yMm: key.yMm },
    })),
  };
}

function responseError(message: GroupStreamEventMessage, command: string): Error {
  if (message.type === "protocol_error") {
    return new Error(`${message.code}: ${message.message}`);
  }
  if (message.type === "fault") {
    return new Error(`${message.code}: ${message.message}`);
  }
  return new Error(`Unexpected ${command} response: ${message.type}`);
}
