/// <reference types="vite/client" />

import type {
  AckMessage,
  BlockStreamCommandMessage,
  BlockStreamEventMessage,
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
      blockStreamConnect: (request: { host: string; port: number }) => Promise<{ connected: boolean }>;
      blockStreamDisconnect: () => Promise<{ connected: boolean }>;
      blockStreamSend: (message: BlockStreamCommandMessage) => Promise<AckMessage>;
      blockStreamOnMessage: (listener: (message: BlockStreamEventMessage) => void) => () => void;
    };
  }
}

export {};
