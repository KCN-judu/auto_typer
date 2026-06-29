import { contextBridge, ipcRenderer } from "electron";

type StoreShape = {
  lastDeviceUrl?: string;
  recentJobs: Array<{ jobId: string; text: string; createdAt: string }>;
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
});
