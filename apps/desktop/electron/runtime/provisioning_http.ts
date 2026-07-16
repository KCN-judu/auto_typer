export const PROVISIONING_HTTP_ORIGIN = "http://192.168.4.1" as const;

export type WifiProvisionStatus = {
  state: "IDLE" | "CONNECTING" | "CONNECTED" | "FAILED";
  reason?: string;
  targetSsid?: string;
  ip?: string;
  ap?: {
    ssid: string;
    ip: string;
  };
};

export type ProvisioningCredentialsRequest = {
  ssid: string;
  password: string;
};

type ProvisioningEndpoint = "/api/status" | "/api/provision" | "/api/finish";

function provisioning_url(endpoint: ProvisioningEndpoint): URL {
  return new URL(endpoint, `${PROVISIONING_HTTP_ORIGIN}/`);
}

async function read_error_body(response: Response): Promise<string> {
  try {
    return await response.text();
  } catch {
    return "";
  }
}

async function request_provisioning_device(
  endpoint: ProvisioningEndpoint,
  init: RequestInit,
): Promise<WifiProvisionStatus> {
  const response = await fetch(provisioning_url(endpoint), init);
  if (!response.ok) {
    const body = await read_error_body(response);
    const detail = body || response.statusText;
    throw new Error(`Provisioning device request failed (${response.status}): ${detail}`);
  }
  return response.json() as Promise<WifiProvisionStatus>;
}

export function get_provisioning_status(): Promise<WifiProvisionStatus> {
  return request_provisioning_device("/api/status", { method: "GET" });
}

export function send_provisioning_credentials(
  request: ProvisioningCredentialsRequest,
): Promise<WifiProvisionStatus> {
  const ssid = request.ssid.trim();
  if (!ssid) {
    throw new Error("SSID is required.");
  }
  return request_provisioning_device("/api/provision", {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
    },
    body: JSON.stringify({ ssid, password: request.password }),
  });
}

export function finish_provisioning(): Promise<WifiProvisionStatus> {
  return request_provisioning_device("/api/finish", { method: "POST" });
}
