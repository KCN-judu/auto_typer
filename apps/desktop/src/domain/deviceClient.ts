import type {
  CreateJobRequest,
  CreateJobResponse,
  DeviceStatus,
  KeymapDocument,
  MotorEnableRequest,
  MotorMoveRelativeRequest,
  MotorStopRequest,
  ProbeKeyRequest,
  ServoApplyRequest,
} from "../../../../shared/protocol/auto-typer-protocol";
import { protocolRoutes } from "../../../../shared/protocol/auto-typer-protocol";

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
    const result = window.autoTyper
      ? await window.autoTyper.request(url, init)
      : await directRequest(url, init);

    if (!result.ok) {
      throw new DeviceClientError(result.body || result.statusText, result.status);
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
