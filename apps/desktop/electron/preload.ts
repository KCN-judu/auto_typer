import { contextBridge, ipcRenderer } from "electron";
import type {
  GroupStreamCommandMessage,
  GroupStreamEventMessage,
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
  groupStreamOnMessage: (listener: (message: GroupStreamEventMessage) => void) => {
    const wrapped = (_event: unknown, message: GroupStreamEventMessage) => listener(message);
    ipcRenderer.on("groupStream:message", wrapped);
    return () => {
      ipcRenderer.off("groupStream:message", wrapped);
    };
  },
});
