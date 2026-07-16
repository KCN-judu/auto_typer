/// <reference types="vite/client" />

import type {
  MotionProtocolCommandMessage,
  MotionProtocolEventMessage,
} from "../../../shared/protocol/protocolTypes";

type DesktopStore = {
  lastTcpHost?: string;
  lastTcpPort?: number;
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
      wifiProvisionGetStatus: () => Promise<WifiProvisionStatus>;
      wifiProvisionSendCredentials: (request: { ssid: string; password: string }) => Promise<WifiProvisionStatus>;
      wifiProvisionFinish: () => Promise<WifiProvisionStatus>;
      motionProtocolConnect: (request: { host: string; port: number }) => Promise<{ connected: boolean }>;
      motionProtocolDisconnect: () => Promise<{ connected: boolean }>;
      motionProtocolSend: (message: MotionProtocolCommandMessage) => Promise<MotionProtocolEventMessage>;
      motionProtocolOnMessage: (listener: (message: MotionProtocolEventMessage) => void) => () => void;
    };
  }
}

export {};
