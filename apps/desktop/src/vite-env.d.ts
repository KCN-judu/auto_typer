/// <reference types="vite/client" />

type DesktopStore = {
  lastDeviceUrl?: string;
  recentJobs: Array<{ jobId: string; text: string; createdAt: string }>;
};

interface Window {
  autoTyper?: {
    readStore: () => Promise<DesktopStore>;
    writeStore: (store: DesktopStore) => Promise<DesktopStore>;
    request: (
      url: string,
      init?: RequestInit,
    ) => Promise<{ ok: boolean; status: number; statusText: string; body: string; contentType: string }>;
  };
}
