import { contextBridge, ipcRenderer } from "electron";
import type {
  MotionProtocolCommandMessage,
  MotionProtocolEventMessage,
} from "../../../shared/protocol/protocolTypes.js";

type StoreShape = {
  lastTcpHost?: string;
  lastTcpPort?: number;
  savedWifiSsid?: string;
  savedWifiPassword?: string;
  recentJobs: Array<{ jobId: string; text: string; createdAt: string }>;
};

contextBridge.exposeInMainWorld("autoTyper", {
  readStore: () => ipcRenderer.invoke("store:read") as Promise<StoreShape>,
  writeStore: (store: StoreShape) => ipcRenderer.invoke("store:write", store) as Promise<StoreShape>,
  wifiProvisionGetStatus: () =>
    ipcRenderer.invoke("wifiProvision:getStatus") as Promise<unknown>,
  wifiProvisionSendCredentials: (request: { ssid: string; password: string }) =>
    ipcRenderer.invoke("wifiProvision:provision", request) as Promise<unknown>,
  wifiProvisionFinish: () =>
    ipcRenderer.invoke("wifiProvision:finish") as Promise<unknown>,
  motionProtocolConnect: (request: { host: string; port: number }) =>
    ipcRenderer.invoke("motionProtocol:connect", request) as Promise<{ connected: boolean }>,
  motionProtocolDisconnect: () => ipcRenderer.invoke("motionProtocol:disconnect") as Promise<{ connected: boolean }>,
  motionProtocolSend: (message: MotionProtocolCommandMessage) =>
    ipcRenderer.invoke("motionProtocol:send", message) as Promise<MotionProtocolEventMessage>,
  motionProtocolOnMessage: (listener: (message: MotionProtocolEventMessage) => void) => {
    const wrapped = (_event: unknown, message: MotionProtocolEventMessage) => listener(message);
    ipcRenderer.on("motionProtocol:message", wrapped);
    return () => {
      ipcRenderer.off("motionProtocol:message", wrapped);
    };
  },
});
