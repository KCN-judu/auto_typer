import type {
  WifiConfigResultMessage,
  WifiNetworksMessage,
  WifiStatus,
} from "../../../../shared/protocol/auto-typer-protocol";

export type SerialWifiConnectionState = "unsupported" | "disconnected" | "connecting" | "connected";

export type SerialPortInfo = {
  path: string;
  label: string;
};

export class SerialWifiClient {
  private connected = false;

  isSupported(): boolean {
    return window.autoTyper?.serialWifiConnect !== undefined;
  }

  isConnected(): boolean {
    return this.connected;
  }

  async listPorts(): Promise<SerialPortInfo[]> {
    if (!window.autoTyper?.serialWifiListPorts) {
      return [];
    }
    return window.autoTyper.serialWifiListPorts();
  }

  async connect(path?: string): Promise<void> {
    if (!window.autoTyper?.serialWifiConnect) {
      throw new Error("serial_unsupported: 当前环境不支持主进程串口");
    }
    await window.autoTyper.serialWifiConnect({ path });
    this.connected = true;
  }

  async disconnect(): Promise<void> {
    await window.autoTyper?.serialWifiDisconnect?.();
    this.connected = false;
  }

  async getWifiStatus(): Promise<WifiStatus> {
    if (!window.autoTyper?.serialWifiGetWifiStatus) {
      throw new Error("serial_unsupported: 当前环境不支持主进程串口");
    }
    return window.autoTyper.serialWifiGetWifiStatus();
  }

  async scanWifi(): Promise<WifiNetworksMessage> {
    if (!window.autoTyper?.serialWifiScanWifi) {
      throw new Error("serial_unsupported: 当前环境不支持主进程串口");
    }
    return window.autoTyper.serialWifiScanWifi();
  }

  async configureWifi(ssid: string, password: string): Promise<WifiConfigResultMessage> {
    if (!window.autoTyper?.serialWifiConfigureWifi) {
      throw new Error("serial_unsupported: 当前环境不支持主进程串口");
    }
    return window.autoTyper.serialWifiConfigureWifi({ ssid, password });
  }
}
