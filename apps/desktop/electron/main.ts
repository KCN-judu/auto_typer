import { app, BrowserWindow, ipcMain } from "electron";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { DeviceLink } from "./deviceLink.js";
import {
  finish_provisioning,
  get_provisioning_status,
  send_provisioning_credentials,
  type ProvisioningCredentialsRequest,
} from "./runtime/provisioning_http.js";
import type {
  MotionProtocolCommandMessage,
  MotionProtocolEventMessage,
} from "../../../shared/protocol/protocolTypes.js";
import { MOTION_PROTOCOL_COMMAND_TYPES } from "../../../shared/protocol/protocolTypes.js";

const supportedTcpCommandTypes = new Set<string>(MOTION_PROTOCOL_COMMAND_TYPES.filter((type) => type !== "handshake"));

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(__dirname, "../../../..");
const isDev = process.env.VITE_DEV_SERVER_URL !== undefined || !app.isPackaged;

type StoreShape = {
  lastTcpHost?: string;
  lastTcpPort?: number;
  savedWifiSsid?: string;
  savedWifiPassword?: string;
  recentJobs: Array<{ jobId: string; text: string; createdAt: string }>;
};

const defaultStore: StoreShape = {
  recentJobs: [],
};

let mainWindowRef: BrowserWindow | undefined;
let deviceLink: DeviceLink | undefined;

function storePath() {
  return path.join(app.getPath("userData"), "controller-store.json");
}

async function readStore(): Promise<StoreShape> {
  try {
    const raw = await readFile(storePath(), "utf8");
    return { ...defaultStore, ...JSON.parse(raw) };
  } catch {
    return defaultStore;
  }
}

async function writeStore(store: StoreShape): Promise<void> {
  await mkdir(app.getPath("userData"), { recursive: true });
  await writeFile(storePath(), JSON.stringify(store, null, 2), "utf8");
}

async function createWindow() {
  const mainWindow = new BrowserWindow({
    width: 1280,
    height: 820,
    minWidth: 1040,
    minHeight: 720,
    title: "Auto Typer Control",
    backgroundColor: "#f5f2ea",
    webPreferences: {
      preload: path.join(__dirname, "preload.js"),
      nodeIntegration: false,
      contextIsolation: true,
      sandbox: false,
    },
  });
  mainWindowRef = mainWindow;
  mainWindow.on("closed", () => {
    if (mainWindowRef === mainWindow) {
      mainWindowRef = undefined;
    }
  });

  if (isDev) {
    await mainWindow.loadURL("http://127.0.0.1:5173");
  } else {
    await mainWindow.loadFile(path.join(packageRoot, "dist/index.html"));
  }
}

function emitMotionProtocolMessage(message: MotionProtocolEventMessage) {
  mainWindowRef?.webContents.send("motionProtocol:message", message);
}

function closeMotionProtocol() {
  deviceLink?.close();
  deviceLink = undefined;
}

async function connectMotionProtocol(host: string, port: number): Promise<void> {
  closeMotionProtocol();
  const link = new DeviceLink();
  link.on("message", emitMotionProtocolMessage);
  link.on("disconnect", (error) => {
    if (deviceLink === link) {
      deviceLink = undefined;
    }
    emitMotionProtocolMessage({
      v: 1,
      type: "fault",
      code: "transport_disconnect",
      message: error.message || "TCP device disconnected",
    });
  });
  deviceLink = link;
  try {
    await link.connect(host, port);
  } catch (error) {
    if (deviceLink === link) {
      deviceLink = undefined;
    }
    link.close();
    throw error;
  }
}

function sendMotionProtocolMessage(message: MotionProtocolCommandMessage): Promise<MotionProtocolEventMessage> {
  if (!deviceLink) {
    return Promise.reject(new Error("TCP device is not connected"));
  }
  if (!supportedTcpCommandTypes.has(message.type)) {
    return Promise.reject(new Error("Unsupported TCP message type"));
  }
  return deviceLink.sendCommand(message);
}

ipcMain.handle("store:read", async () => readStore());
ipcMain.handle("store:write", async (_event, nextStore: StoreShape) => {
  await writeStore({ ...defaultStore, ...nextStore });
  return readStore();
});

ipcMain.handle("wifiProvision:getStatus", async () => get_provisioning_status());

ipcMain.handle(
  "wifiProvision:provision",
  async (_event, request: ProvisioningCredentialsRequest) => send_provisioning_credentials(request),
);

ipcMain.handle("wifiProvision:finish", async () => finish_provisioning());

ipcMain.handle("motionProtocol:connect", async (_event, request: { host: string; port: number }) => {
  await connectMotionProtocol(request.host, request.port);
  return { connected: true };
});

ipcMain.handle("motionProtocol:disconnect", async () => {
  closeMotionProtocol();
  return { connected: false };
});

ipcMain.handle("motionProtocol:send", async (_event, message: MotionProtocolCommandMessage) => sendMotionProtocolMessage(message));

app.whenReady().then(createWindow);

app.on("window-all-closed", () => {
  if (process.platform !== "darwin") {
    app.quit();
  }
});

app.on("activate", () => {
  if (BrowserWindow.getAllWindows().length === 0) {
    void createWindow();
  }
});
