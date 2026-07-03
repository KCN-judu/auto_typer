import { app, BrowserWindow, ipcMain } from "electron";
import http from "node:http";
import https from "node:https";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath, URL } from "node:url";
import { DeviceLink } from "./device-link.js";
import type {
  AckMessage,
  GroupStreamCommandMessage,
  GroupStreamEventMessage,
} from "./device-link.js";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const isDev = process.env.VITE_DEV_SERVER_URL !== undefined || !app.isPackaged;
const networkRequestTimeoutMs = 10000;

type StoreShape = {
  lastDeviceUrl?: string;
  recentJobs: Array<{ jobId: string; text: string; createdAt: string }>;
};

type NetworkResponse = {
  ok: boolean;
  status: number;
  statusText: string;
  body: string;
  contentType: string;
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

function errorField(error: unknown, field: "message" | "code" | "errno" | "syscall"): string | number | undefined {
  if (error && typeof error === "object" && field in error) {
    const value = (error as Record<string, unknown>)[field];
    if (typeof value === "string" || typeof value === "number") {
      return value;
    }
  }
  return undefined;
}

function networkErrorResponse(error: unknown, url: string, method: string): NetworkResponse {
  const cause = error && typeof error === "object" && "cause" in error ? (error as { cause?: unknown }).cause : undefined;
  const code = errorField(cause, "code") ?? errorField(error, "code");
  const syscall = errorField(cause, "syscall") ?? errorField(error, "syscall");
  const message = String(errorField(error, "message") ?? errorField(cause, "message") ?? "Network request failed");
  const details = {
    url,
    method,
    cause: errorField(cause, "message"),
    code,
    errno: errorField(cause, "errno") ?? errorField(error, "errno"),
    syscall,
  };

  console.error(`[network:request] ${method} ${url} failed ${String(code ?? "unknown")} ${String(syscall ?? "")}`.trim());

  return {
    ok: false,
    status: 0,
    statusText: "network_error",
    body: JSON.stringify({
      code: "network_error",
      message,
      details,
    }),
    contentType: "application/json",
  };
}

function normalizeHeaders(init?: RequestInit): Record<string, string> {
  const headers: Record<string, string> = {
    Accept: "application/json",
    Connection: "close",
  };
  const source = init?.headers;
  if (!source) {
    return headers;
  }
  if (source instanceof Headers) {
    source.forEach((value, key) => {
      headers[key] = value;
    });
    return headers;
  }
  if (Array.isArray(source)) {
    for (const [key, value] of source) {
      headers[key] = value;
    }
    return headers;
  }
  for (const [key, value] of Object.entries(source)) {
    if (typeof value === "string") {
      headers[key] = value;
    }
  }
  return headers;
}

async function requestViaNodeHttp(url: string, init?: RequestInit): Promise<NetworkResponse> {
  return new Promise((resolve) => {
    const parsed = new URL(url);
    const transport = parsed.protocol === "https:" ? https : http;
    const method = init?.method ?? "GET";
    const body = typeof init?.body === "string" || Buffer.isBuffer(init?.body) ? init.body : undefined;
    const headers = normalizeHeaders(init);
    if (body !== undefined && !Object.keys(headers).some((key) => key.toLowerCase() === "content-length")) {
      headers["Content-Length"] = String(Buffer.byteLength(body));
    }

    const req = transport.request(
      parsed,
      {
        method,
        headers,
        agent: false,
        timeout: networkRequestTimeoutMs,
      },
      (response) => {
        response.setEncoding("utf8");
        let responseBody = "";
        response.on("data", (chunk: string) => {
          responseBody += chunk;
        });
        response.on("end", () => {
          const status = response.statusCode ?? 0;
          resolve({
            ok: status >= 200 && status < 300,
            status,
            statusText: response.statusMessage ?? "",
            body: responseBody,
            contentType: String(response.headers["content-type"] ?? ""),
          });
        });
      },
    );

    req.on("timeout", () => {
      req.destroy(new Error("Request timed out"));
    });
    req.on("error", (error) => {
      resolve(networkErrorResponse(error, url, method));
    });
    if (body !== undefined) {
      req.write(body);
    }
    req.end();
  });
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
    await mainWindow.loadFile(path.join(__dirname, "../dist/index.html"));
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
    emitGroupStreamMessage({ v: 1, type: "fault", code: "disconnect", message: error.message || "Group stream disconnected" });
  });
  deviceLink = link;
  await link.connect(host, port);
}

function sendGroupStreamMessage(message: GroupStreamCommandMessage): Promise<AckMessage> {
  if (!deviceLink) {
    return Promise.reject(new Error("Group stream is not connected"));
  }
  if (!["exec_group", "task_end", "cancel", "reset_fault", "probe", "ping"].includes(message.type)) {
    return Promise.reject(new Error("Unsupported group stream message type"));
  }
  if (typeof message.id !== "string" || message.id.length === 0) {
    return Promise.reject(new Error("Group stream message id is required"));
  }
  const timeoutMs = typeof message.timeoutMs === "number" && Number.isFinite(message.timeoutMs) ? message.timeoutMs : undefined;
  const outbound = { ...message };
  delete outbound.timeoutMs;
  return timeoutMs === undefined ? deviceLink.sendCommand(outbound) : deviceLink.sendCommand(outbound, timeoutMs);
}

ipcMain.handle("store:read", async () => readStore());
ipcMain.handle("store:write", async (_event, nextStore: StoreShape) => {
  await writeStore({ ...defaultStore, ...nextStore });
  return readStore();
});

ipcMain.handle("network:request", async (_event, request: { url: string; init?: RequestInit }) => {
  const method = request.init?.method ?? "GET";
  try {
    return await requestViaNodeHttp(request.url, request.init);
  } catch (error) {
    return networkErrorResponse(error, request.url, method);
  }
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
