import { contextBridge, ipcRenderer } from "electron";

type StoreShape = {
  lastDeviceUrl?: string;
  recentJobs: Array<{ jobId: string; text: string; createdAt: string }>;
};

type GroupStreamCommandMessage = {
  v: 1;
  id: string;
  type: string;
  timeoutMs?: number;
  [key: string]: unknown;
};

type AckMessage = {
  v: 1;
  type: "ack";
  id: string;
  ok: boolean;
  accepted: boolean;
  code?: string;
  message?: string;
};

type GroupStreamEventMessage = {
  v: 1;
  type: string;
  [key: string]: unknown;
};

contextBridge.exposeInMainWorld("autoTyper", {
  readStore: () => ipcRenderer.invoke("store:read") as Promise<StoreShape>,
  writeStore: (store: StoreShape) => ipcRenderer.invoke("store:write", store) as Promise<StoreShape>,
  request: (url: string, init?: RequestInit) =>
    ipcRenderer.invoke("network:request", { url, init }) as Promise<{
      ok: boolean;
      status: number;
      statusText: string;
      body: string;
      contentType: string;
    }>,
  groupStreamConnect: (request: { host: string; port: number }) =>
    ipcRenderer.invoke("groupStream:connect", request) as Promise<{ connected: boolean }>,
  groupStreamDisconnect: () => ipcRenderer.invoke("groupStream:disconnect") as Promise<{ connected: boolean }>,
  groupStreamSend: (message: GroupStreamCommandMessage) =>
    ipcRenderer.invoke("groupStream:send", message) as Promise<AckMessage>,
  groupStreamOnMessage: (listener: (message: GroupStreamEventMessage) => void) => {
    const wrapped = (_event: unknown, message: GroupStreamEventMessage) => listener(message);
    ipcRenderer.on("groupStream:message", wrapped);
    return () => {
      ipcRenderer.off("groupStream:message", wrapped);
    };
  },
});
