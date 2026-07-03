/// <reference types="vite/client" />

import type {
  AckMessage,
  GroupStreamCommandMessage,
  GroupStreamEventMessage,
} from "../../../shared/protocol/auto-typer-protocol";

type DesktopStore = {
  lastDeviceUrl?: string;
  recentJobs: Array<{ jobId: string; text: string; createdAt: string }>;
};

declare global {
  interface Window {
    autoTyper?: {
      readStore: () => Promise<DesktopStore>;
      writeStore: (store: DesktopStore) => Promise<DesktopStore>;
      request: (
        url: string,
        init?: RequestInit,
      ) => Promise<{ ok: boolean; status: number; statusText: string; body: string; contentType: string }>;
      groupStreamConnect: (request: { host: string; port: number }) => Promise<{ connected: boolean }>;
      groupStreamDisconnect: () => Promise<{ connected: boolean }>;
      groupStreamSend: (message: GroupStreamCommandMessage) => Promise<AckMessage>;
      groupStreamOnMessage: (listener: (message: GroupStreamEventMessage) => void) => () => void;
    };
  }
}

export {};
