import { contextBridge, ipcRenderer } from "electron";
import type {
  GroupStreamCommandMessage,
  GroupStreamEventMessage,
} from "../../../shared/protocol/auto-typer-protocol.js";

type StoreShape = {
  lastTcpHost?: string;
  lastTcpPort?: number;
  lastProvisionBaseUrl?: string;
  savedWifiSsid?: string;
  savedWifiPassword?: string;
  recentJobs: Array<{ jobId: string; text: string; createdAt: string }>;
};

contextBridge.exposeInMainWorld("autoTyper", {
  readStore: () => ipcRenderer.invoke("store:read") as Promise<StoreShape>,
  writeStore: (store: StoreShape) => ipcRenderer.invoke("store:write", store) as Promise<StoreShape>,
  wifiProvisionGetStatus: (request: { baseUrl?: string }) =>
    ipcRenderer.invoke("wifiProvision:getStatus", request) as Promise<unknown>,
  wifiProvisionSendCredentials: (request: { baseUrl?: string; ssid: string; password: string }) =>
    ipcRenderer.invoke("wifiProvision:provision", request) as Promise<unknown>,
  wifiProvisionFinish: (request: { baseUrl?: string }) =>
    ipcRenderer.invoke("wifiProvision:finish", request) as Promise<unknown>,
  groupStreamConnect: (request: { host: string; port: number }) =>
    ipcRenderer.invoke("groupStream:connect", request) as Promise<{ connected: boolean }>,
  groupStreamDisconnect: () => ipcRenderer.invoke("groupStream:disconnect") as Promise<{ connected: boolean }>,
  groupStreamSend: (message: GroupStreamCommandMessage) =>
    ipcRenderer.invoke("groupStream:send", message) as Promise<GroupStreamEventMessage>,
  groupStreamOnMessage: (listener: (message: GroupStreamEventMessage) => void) => {
    const wrapped = (_event: unknown, message: GroupStreamEventMessage) => listener(message);
    ipcRenderer.on("groupStream:message", wrapped);
    return () => {
      ipcRenderer.off("groupStream:message", wrapped);
    };
  },
});
