import type {
  ApiErrorBody,
  CreateJobRequest,
  CreateJobResponse,
  DeviceStatus,
  KeymapDocument,
  MotorEnableRequest,
  MotorMoveRelativeRequest,
  MotorStopRequest,
  ProbeKeyRequest,
  ServoApplyRequest,
  WifiConfigRequest,
  WifiNetworksResponse,
  WifiSetupStatusResponse,
} from "../../../../shared/protocol/auto-typer-protocol";
import { protocolRoutes } from "../../../../shared/protocol/auto-typer-protocol";
import { isApiErrorBody } from "../../../../shared/protocol/schema";

export class DeviceClientError extends Error {
  constructor(
    message: string,
    readonly status?: number,
  ) {
    super(message);
  }
}

export class DeviceClient {
  constructor(readonly baseUrl: string) {}

  async status(): Promise<DeviceStatus> {
    return this.getJson<DeviceStatus>(protocolRoutes.status);
  }

  async createJob(request: CreateJobRequest): Promise<CreateJobResponse> {
    return this.postJson<CreateJobResponse>(protocolRoutes.jobs, request);
  }

  async cancelJob(): Promise<DeviceStatus> {
    return this.postJson<DeviceStatus>(protocolRoutes.cancelJob, {});
  }

  async stopMachine(): Promise<DeviceStatus> {
    return this.postJson<DeviceStatus>(protocolRoutes.machineStop, {});
  }

  async resetFault(): Promise<DeviceStatus> {
    return this.postJson<DeviceStatus>(protocolRoutes.resetFault, {});
  }

  async probeMotors(): Promise<DeviceStatus> {
    return this.postJson<DeviceStatus>(protocolRoutes.probeMotors, {});
  }

  async canDiagnostics() {
    return this.getJson(protocolRoutes.canDiagnostics);
  }

  async wifiStatus(): Promise<WifiSetupStatusResponse> {
    return this.getJson<WifiSetupStatusResponse>(protocolRoutes.wifiStatus);
  }

  async wifiNetworks(): Promise<WifiNetworksResponse> {
    return this.getJson<WifiNetworksResponse>(protocolRoutes.wifiNetworks);
  }

  async configureWifi(request: WifiConfigRequest): Promise<WifiSetupStatusResponse> {
    return this.postJson<WifiSetupStatusResponse>(protocolRoutes.wifiConfig, request);
  }

  async finishWifiSetup(): Promise<WifiSetupStatusResponse> {
    return this.postJson<WifiSetupStatusResponse>(protocolRoutes.wifiSetupFinish, {});
  }

  async getKeymap(): Promise<KeymapDocument> {
    return this.getJson<KeymapDocument>(protocolRoutes.keymap);
  }

  async putKeymap(keymap: KeymapDocument): Promise<KeymapDocument> {
    return this.putJson<KeymapDocument>(protocolRoutes.keymap, keymap);
  }

  async moveMotor(request: MotorMoveRelativeRequest): Promise<DeviceStatus> {
    return this.postJson<DeviceStatus>(protocolRoutes.debugMotorMoveRelative, request);
  }

  async enableMotor(request: MotorEnableRequest): Promise<DeviceStatus> {
    return this.postJson<DeviceStatus>(protocolRoutes.debugMotorEnable, request);
  }

  async stopMotor(request: MotorStopRequest): Promise<DeviceStatus> {
    return this.postJson<DeviceStatus>(protocolRoutes.debugMotorStop, request);
  }

  async applyServo(request: ServoApplyRequest): Promise<DeviceStatus> {
    return this.postJson<DeviceStatus>(protocolRoutes.debugServoApply, request);
  }

  async probeKey(request: ProbeKeyRequest): Promise<KeymapDocument> {
    return this.postJson<KeymapDocument>(protocolRoutes.debugProbeKey, request);
  }

  private async getJson<T>(route: string): Promise<T> {
    return this.requestJson<T>(route, { method: "GET" });
  }

  private async postJson<T>(route: string, body: unknown): Promise<T> {
    return this.requestJson<T>(route, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(body),
    });
  }

  private async putJson<T>(route: string, body: unknown): Promise<T> {
    return this.requestJson<T>(route, {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(body),
    });
  }

  private async requestJson<T>(route: string, init: RequestInit): Promise<T> {
    const url = `${this.baseUrl.replace(/\/$/, "")}${route}`;
    let result: Awaited<ReturnType<typeof directRequest>>;
    try {
      result = window.autoTyper ? await window.autoTyper.request(url, init) : await directRequest(url, init);
    } catch (error) {
      throw new DeviceClientError(networkErrorMessage(error), 0);
    }

    if (!result.ok) {
      throw new DeviceClientError(parseErrorMessage(result.body, result.statusText), result.status);
    }

    if (!result.body) {
      return {} as T;
    }

    try {
      return JSON.parse(result.body) as T;
    } catch (error) {
      const preview = result.body.length > 500 ? `${result.body.slice(0, 500)}...` : result.body;
      throw new DeviceClientError(
        `${error instanceof Error ? error.message : "Invalid JSON"}\nResponse preview: ${preview}`,
        result.status,
      );
    }
  }
}

function parseErrorMessage(body: string, fallback: string): string {
  if (!body) {
    return fallback;
  }
  try {
    const parsed = JSON.parse(body) as ApiErrorBody & {
      details?: { code?: string; syscall?: string };
    };
    if (parsed.code === "network_error") {
      const suffix = [parsed.details?.code, parsed.details?.syscall].filter(Boolean).join("/");
      return `${parsed.code}: ${parsed.message}${suffix ? ` (${suffix})` : ""}`;
    }
    if (isApiErrorBody(parsed)) {
      return `${parsed.code}: ${parsed.message}`;
    }
  } catch {
    // Fall through to plain body.
  }
  return body;
}

function errorField(error: unknown, field: "message" | "code" | "syscall"): string | undefined {
  if (error && typeof error === "object" && field in error) {
    const value = (error as Record<string, unknown>)[field];
    if (typeof value === "string") {
      return value;
    }
  }
  return undefined;
}

function networkErrorMessage(error: unknown): string {
  const cause = error && typeof error === "object" && "cause" in error ? (error as { cause?: unknown }).cause : undefined;
  const message = errorField(error, "message") ?? errorField(cause, "message") ?? "Network request failed";
  const suffix = [errorField(cause, "code") ?? errorField(error, "code"), errorField(cause, "syscall") ?? errorField(error, "syscall")]
    .filter(Boolean)
    .join("/");
  return `network_error: ${message}${suffix ? ` (${suffix})` : ""}`;
}

async function directRequest(url: string, init: RequestInit) {
  const response = await fetch(url, init);
  return {
    ok: response.ok,
    status: response.status,
    statusText: response.statusText,
    body: await response.text(),
    contentType: response.headers.get("content-type") ?? "",
  };
}
