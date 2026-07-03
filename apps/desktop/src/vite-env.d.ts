/// <reference types="vite/client" />

import type {
  GroupStreamCommandMessage,
  GroupStreamEventMessage,
  WifiConfigResultMessage,
  WifiNetworksMessage,
  WifiStatus,
} from "../../../shared/protocol/auto-typer-protocol";

type DesktopStore = {
  lastTcpHost?: string;
  lastTcpPort?: number;
  recentJobs: Array<{ jobId: string; text: string; createdAt: string }>;
};

declare global {
  interface Window {
    autoTyper?: {
      readStore: () => Promise<DesktopStore>;
      writeStore: (store: DesktopStore) => Promise<DesktopStore>;
      groupStreamConnect: (request: { host: string; port: number }) => Promise<{ connected: boolean }>;
      groupStreamDisconnect: () => Promise<{ connected: boolean }>;
      groupStreamSend: (message: GroupStreamCommandMessage) => Promise<GroupStreamEventMessage>;
      groupStreamOnMessage: (listener: (message: GroupStreamEventMessage) => void) => () => void;
      serialWifiListPorts: () => Promise<Array<{ path: string; label: string }>>;
      serialWifiConnect: (request?: { path?: string }) => Promise<{ connected: boolean; port: { path: string; label: string } }>;
      serialWifiDisconnect: () => Promise<{ connected: boolean }>;
      serialWifiGetWifiStatus: () => Promise<WifiStatus>;
      serialWifiScanWifi: () => Promise<WifiNetworksMessage>;
      serialWifiConfigureWifi: (request: { ssid: string; password: string }) => Promise<WifiConfigResultMessage>;
      serialWifiOnLog: (listener: (event: { line: string; protocol: boolean }) => void) => () => void;
    };
  }
}

export {};
