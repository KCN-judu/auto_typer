import { app, BrowserWindow, ipcMain } from "electron";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { DeviceLink } from "./device-link.js";
import type {
  GroupStreamCommandMessage,
  GroupStreamEventMessage,
} from "../../../shared/protocol/auto-typer-protocol.js";
import { TCP_COMMAND_TYPES } from "../../../shared/protocol/auto-typer-protocol.js";

const supportedTcpCommandTypes = new Set<string>(TCP_COMMAND_TYPES.filter((type) => type !== "hello"));

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(__dirname, "../../../..");
const isDev = process.env.VITE_DEV_SERVER_URL !== undefined || !app.isPackaged;

type StoreShape = {
  lastTcpHost?: string;
  lastTcpPort?: number;
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

function emitGroupStreamMessage(message: GroupStreamEventMessage) {
  mainWindowRef?.webContents.send("groupStream:message", message);
}

function closeGroupStream() {
  deviceLink?.close();
  deviceLink = undefined;
}

async function connectGroupStream(host: string, port: number): Promise<void> {
  closeGroupStream();
  const link = new DeviceLink();
  link.on("message", emitGroupStreamMessage);
  link.on("disconnect", (error) => {
    if (deviceLink === link) {
      deviceLink = undefined;
    }
    emitGroupStreamMessage({ v: 1, type: "fault", code: "disconnect", message: error.message || "TCP device disconnected" });
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

function sendGroupStreamMessage(message: GroupStreamCommandMessage): Promise<GroupStreamEventMessage> {
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

ipcMain.handle("groupStream:connect", async (_event, request: { host: string; port: number }) => {
  await connectGroupStream(request.host, request.port);
  return { connected: true };
});

ipcMain.handle("groupStream:disconnect", async () => {
  closeGroupStream();
  return { connected: false };
});

ipcMain.handle("groupStream:send", async (_event, message: GroupStreamCommandMessage) => sendGroupStreamMessage(message));

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
