import { contextBridge, ipcRenderer } from "electron";
import type {
  GroupStreamCommandMessage,
  GroupStreamEventMessage,
  WifiConfigResultMessage,
  WifiNetworksMessage,
  WifiStatus,
} from "../../../shared/protocol/auto-typer-protocol.js";

type StoreShape = {
  lastTcpHost?: string;
  lastTcpPort?: number;
  recentJobs: Array<{ jobId: string; text: string; createdAt: string }>;
};

contextBridge.exposeInMainWorld("autoTyper", {
  readStore: () => ipcRenderer.invoke("store:read") as Promise<StoreShape>,
  writeStore: (store: StoreShape) => ipcRenderer.invoke("store:write", store) as Promise<StoreShape>,
  groupStreamConnect: (request: { host: string; port: number }) =>
    ipcRenderer.invoke("groupStream:connect", request) as Promise<{ connected: boolean }>,
  groupStreamDisconnect: () => ipcRenderer.invoke("groupStream:disconnect") as Promise<{ connected: boolean }>,
  groupStreamSend: (message: GroupStreamCommandMessage) =>
    ipcRenderer.invoke("groupStream:send", message) as Promise<GroupStreamEventMessage>,
  serialWifiListPorts: () => ipcRenderer.invoke("serialWifi:listPorts") as Promise<Array<{ path: string; label: string }>>,
  serialWifiConnect: (request?: { path?: string }) =>
    ipcRenderer.invoke("serialWifi:connect", request) as Promise<{ connected: boolean; port: { path: string; label: string } }>,
  serialWifiDisconnect: () => ipcRenderer.invoke("serialWifi:disconnect") as Promise<{ connected: boolean }>,
  serialWifiGetWifiStatus: () => ipcRenderer.invoke("serialWifi:getWifiStatus") as Promise<WifiStatus>,
  serialWifiScanWifi: () => ipcRenderer.invoke("serialWifi:scanWifi") as Promise<WifiNetworksMessage>,
  serialWifiConfigureWifi: (request: { ssid: string; password: string }) =>
    ipcRenderer.invoke("serialWifi:configureWifi", request) as Promise<WifiConfigResultMessage>,
  serialWifiOnLog: (listener: (event: { line: string; protocol: boolean }) => void) => {
    const wrapped = (_event: unknown, message: { line: string; protocol: boolean }) => listener(message);
    ipcRenderer.on("serialWifi:log", wrapped);
    return () => {
      ipcRenderer.off("serialWifi:log", wrapped);
    };
  },
  groupStreamOnMessage: (listener: (message: GroupStreamEventMessage) => void) => {
    const wrapped = (_event: unknown, message: GroupStreamEventMessage) => listener(message);
    ipcRenderer.on("groupStream:message", wrapped);
    return () => {
      ipcRenderer.off("groupStream:message", wrapped);
    };
  },
});
