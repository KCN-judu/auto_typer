import { app, BrowserWindow, ipcMain } from "electron";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const isDev = process.env.VITE_DEV_SERVER_URL !== undefined || !app.isPackaged;

type StoreShape = {
  lastDeviceUrl?: string;
  recentJobs: Array<{ jobId: string; text: string; createdAt: string }>;
};

const defaultStore: StoreShape = {
  recentJobs: [],
};

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

  if (isDev) {
    await mainWindow.loadURL("http://127.0.0.1:5173");
  } else {
    await mainWindow.loadFile(path.join(__dirname, "../dist/index.html"));
  }
}

ipcMain.handle("store:read", async () => readStore());
ipcMain.handle("store:write", async (_event, nextStore: StoreShape) => {
  await writeStore({ ...defaultStore, ...nextStore });
  return readStore();
});

ipcMain.handle("network:request", async (_event, request: { url: string; init?: RequestInit }) => {
  const response = await fetch(request.url, request.init);
  const text = await response.text();
  return {
    ok: response.ok,
    status: response.status,
    statusText: response.statusText,
    body: text,
    contentType: response.headers.get("content-type") ?? "",
  };
});

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
