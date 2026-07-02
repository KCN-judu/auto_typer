import { contextBridge, ipcRenderer } from "electron";

type StoreShape = {
  lastDeviceUrl?: string;
  recentJobs: Array<{ jobId: string; text: string; createdAt: string }>;
};

type BlockStreamCommandMessage = {
  v: 1;
  id: string;
  type: string;
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

type BlockStreamEventMessage = {
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
  blockStreamConnect: (request: { host: string; port: number }) =>
    ipcRenderer.invoke("blockStream:connect", request) as Promise<{ connected: boolean }>,
  blockStreamDisconnect: () => ipcRenderer.invoke("blockStream:disconnect") as Promise<{ connected: boolean }>,
  blockStreamSend: (message: BlockStreamCommandMessage) =>
    ipcRenderer.invoke("blockStream:send", message) as Promise<AckMessage>,
  blockStreamOnMessage: (listener: (message: BlockStreamEventMessage) => void) => {
    const wrapped = (_event: unknown, message: BlockStreamEventMessage) => listener(message);
    ipcRenderer.on("blockStream:message", wrapped);
    return () => {
      ipcRenderer.off("blockStream:message", wrapped);
    };
  },
});
