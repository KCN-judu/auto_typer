import { useEffect, useRef, useState } from "react";
import type { ProvisioningState, WifiProvisionStatus } from "../appTypes";
import {
  decideProvisioningNextStep,
  formatProvisioningStatusLog,
  formatProvisioningSuccessLog,
} from "../../domain/provisioning";

type UseWifiProvisioningArgs = {
  wifiSsid: string;
  wifiPassword: string;
  savedWifiSsid: string;
  savedWifiPassword: string;
  setTcpHost: (value: string) => void;
  connectProvisionedDevice: (host: string) => Promise<void>;
  persistFields: (next: Partial<{
    savedWifiSsid: string;
    savedWifiPassword: string;
  }>) => Promise<void>;
  appendLog: (line: string) => void;
};

export function useWifiProvisioning({
  wifiSsid,
  wifiPassword,
  savedWifiSsid,
  savedWifiPassword,
  setTcpHost,
  connectProvisionedDevice,
  persistFields,
  appendLog,
}: UseWifiProvisioningArgs) {
  const [provisionState, setProvisionState] = useState<ProvisioningState>("idle");
  const [provisionStatus, setProvisionStatus] = useState<WifiProvisionStatus | null>(null);
  const provisionPollRef = useRef<number | undefined>();
  const pendingCredentialsRef = useRef<{ ssid: string; password: string } | null>(null);

  useEffect(() => () => {
    if (provisionPollRef.current !== undefined) {
      window.clearInterval(provisionPollRef.current);
    }
  }, []);

  async function finishConnectedProvisioning(ip: string) {
    setProvisionState("finishing_ap");
    setTcpHost(ip);
    const credentials = pendingCredentialsRef.current;
    await persistFields({
      ...(credentials ? {
        savedWifiSsid: credentials.ssid,
        savedWifiPassword: credentials.password,
      } : {}),
    });
    pendingCredentialsRef.current = null;
    if (!window.autoTyper) {
      throw new Error("Provisioning IPC is unavailable");
    }
    await window.autoTyper.wifiProvisionFinish();
    setProvisionState("sta_connected");
    appendLog(formatProvisioningSuccessLog(ip));
    await connectProvisionedDevice(ip).catch(() => undefined);
  }

  async function applyProvisioningStatus(payload: WifiProvisionStatus) {
    setProvisionStatus(payload);
    const decision = decideProvisioningNextStep(payload);
    if (decision.kind === "finish") {
      await finishConnectedProvisioning(decision.ip);
      return payload;
    }
    setProvisionState(decision.nextState);
    if (decision.kind === "failed") {
      pendingCredentialsRef.current = null;
      appendLog(`配网失败 ${decision.reason}`);
    }
    return payload;
  }

  async function readProvisionStatus() {
    if (!window.autoTyper) {
      throw new Error("Provisioning IPC is unavailable");
    }
    setProvisionState("probing_ap");
    const payload = await window.autoTyper.wifiProvisionGetStatus();
    appendLog(formatProvisioningStatusLog(payload));
    await applyProvisioningStatus(payload);
    return payload;
  }

  function stopProvisionPolling() {
    if (provisionPollRef.current !== undefined) {
      window.clearInterval(provisionPollRef.current);
      provisionPollRef.current = undefined;
    }
  }

  function startProvisionPolling() {
    stopProvisionPolling();
    provisionPollRef.current = window.setInterval(() => {
      void readProvisionStatus()
        .then((payload) => {
          if (payload.state === "CONNECTED" || payload.state === "FAILED") {
            stopProvisionPolling();
          }
        })
        .catch((error) => {
          stopProvisionPolling();
          setProvisionState("sta_failed");
          appendLog(error instanceof Error ? error.message : "读取 AP 状态失败");
        });
    }, 2500);
  }

  async function provisionWifi(useSavedOnly = false) {
    if (!window.autoTyper) {
      throw new Error("Provisioning IPC is unavailable");
    }
    const ssid = (useSavedOnly ? savedWifiSsid : wifiSsid).trim();
    const password = useSavedOnly ? savedWifiPassword : wifiPassword;
    if (!ssid) {
      appendLog("缺少 SSID，无法配网");
      setProvisionState("sta_failed");
      return;
    }
    pendingCredentialsRef.current = { ssid, password };
    setProvisionState("sending_credentials");
    let payload: WifiProvisionStatus;
    try {
      payload = await window.autoTyper.wifiProvisionSendCredentials({
        ssid,
        password,
      });
    } catch (error) {
      pendingCredentialsRef.current = null;
      setProvisionState("sta_failed");
      appendLog(error instanceof Error ? error.message : "发送配网凭据失败");
      return;
    }
    appendLog(`已发送配网到 SoftAP 192.168.4.1，目标 SSID=${ssid}`);
    await applyProvisioningStatus(payload);
    if (payload.state === "CONNECTED" || payload.state === "FAILED") {
      return;
    }
    startProvisionPolling();
  }

  return {
    provisionState,
    provisionStatus,
    provisionWifi,
  };
}
