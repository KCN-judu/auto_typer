/// <reference types="vite/client" />

import type {
  GroupStreamCommandMessage,
  GroupStreamEventMessage,
} from "../../../shared/protocol/auto-typer-protocol";

type DesktopStore = {
  lastTcpHost?: string;
  lastTcpPort?: number;
  lastProvisionBaseUrl?: string;
  savedWifiSsid?: string;
  savedWifiPassword?: string;
  recentJobs: Array<{ jobId: string; text: string; createdAt: string }>;
};

type WifiProvisionStatus = {
  state: "IDLE" | "CONNECTING" | "CONNECTED" | "FAILED";
  reason?: string;
  targetSsid?: string;
  ip?: string;
  ap?: {
    ssid: string;
    ip: string;
  };
};

declare global {
  interface Window {
    autoTyper?: {
      readStore: () => Promise<DesktopStore>;
      writeStore: (store: DesktopStore) => Promise<DesktopStore>;
      wifiProvisionGetStatus: (request: { baseUrl?: string }) => Promise<WifiProvisionStatus>;
      wifiProvisionSendCredentials: (request: { baseUrl?: string; ssid: string; password: string }) => Promise<WifiProvisionStatus>;
      wifiProvisionFinish: (request: { baseUrl?: string }) => Promise<WifiProvisionStatus>;
      groupStreamConnect: (request: { host: string; port: number }) => Promise<{ connected: boolean }>;
      groupStreamDisconnect: () => Promise<{ connected: boolean }>;
      groupStreamSend: (message: GroupStreamCommandMessage) => Promise<GroupStreamEventMessage>;
      groupStreamOnMessage: (listener: (message: GroupStreamEventMessage) => void) => () => void;
    };
  }
}

export {};
