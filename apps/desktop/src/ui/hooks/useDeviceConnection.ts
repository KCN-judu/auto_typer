import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type {
  DeviceStatus,
  KeymapDocument,
  MotionProtocolEventMessage,
} from "../../../../../shared/protocol/protocolTypes";
import { MotionProtocolClient } from "../../domain/runtime";
import { currentFeiyu200Keymap } from "../../domain/keymap";
import { mockStatus } from "../../domain/mockDevice";
import type { ConnectionState } from "../appTypes";

type UseDeviceConnectionArgs = {
  tcpHost: string;
  tcpPort: number;
  persistFields: (next: Partial<{ lastTcpHost: string; lastTcpPort: number }>) => Promise<void>;
  appendLog: (line: string) => void;
};

const provisionedConnectInitialDelayMs = 3000;
const provisionedConnectRetryDelayMs = 2000;
const provisionedConnectMaxAttempts = 10;

function wait(delayMs: number): Promise<void> {
  return new Promise((resolve) => window.setTimeout(resolve, delayMs));
}

export function useDeviceConnection({ tcpHost, tcpPort, persistFields, appendLog }: UseDeviceConnectionArgs) {
  const [connection, setConnection] = useState<ConnectionState>("disconnected");
  const [status, setStatus] = useState<DeviceStatus>(mockStatus);
  const [keymap] = useState<KeymapDocument>(currentFeiyu200Keymap());
  const streamClient = useMemo(() => new MotionProtocolClient(), []);
  const connectAttemptRef = useRef(0);
  const autoConnectFlowRef = useRef(0);

  useEffect(() => streamClient.onMessage(handleBaseMessage), [streamClient]);

  async function connectTo(host: string, persistEndpoint: boolean, logFailure: boolean) {
    const attempt = ++connectAttemptRef.current;
    setConnection("connecting");
    try {
      await streamClient.connect({ host, port: tcpPort });
      const snapshot = await streamClient.getSnapshot();
      if (attempt !== connectAttemptRef.current) return;
      setStatus(snapshot);
      setConnection("connected");
      if (persistEndpoint) {
        await persistFields({ lastTcpHost: host, lastTcpPort: tcpPort });
      }
      appendLog(`TCP 已连接 ${host}:${tcpPort}`);
    } catch (error) {
      if (attempt === connectAttemptRef.current) {
        await streamClient.disconnect().catch(() => undefined);
        setConnection("transport_fault");
        if (logFailure) {
          appendLog(error instanceof Error ? error.message : "TCP 连接失败");
        }
      }
      throw error;
    }
  }

  async function connect() {
    autoConnectFlowRef.current += 1;
    await connectTo(tcpHost, true, true);
  }

  async function connectProvisionedDevice(host: string) {
    const flow = ++autoConnectFlowRef.current;
    await wait(provisionedConnectInitialDelayMs);
    let lastError: unknown;
    for (let attempt = 1; attempt <= provisionedConnectMaxAttempts; attempt += 1) {
      if (flow !== autoConnectFlowRef.current) return;
      try {
        await connectTo(host, false, false);
        return;
      } catch (error) {
        if (flow !== autoConnectFlowRef.current) return;
        lastError = error;
        if (attempt < provisionedConnectMaxAttempts) {
          await wait(provisionedConnectRetryDelayMs);
        }
      }
    }
    appendLog(lastError instanceof Error ? lastError.message : "TCP 自动连接失败");
    throw lastError;
  }

  const disconnect = useCallback(async () => {
    autoConnectFlowRef.current += 1;
    connectAttemptRef.current += 1;
    await streamClient.disconnect();
    setConnection("disconnected");
    appendLog("TCP 已断开");
  }, [appendLog, streamClient]);

  const refreshStatus = useCallback(async () => {
    try {
      setStatus(await streamClient.getSnapshot());
      appendLog("状态已刷新");
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "状态刷新失败");
    }
  }, [appendLog, streamClient]);

  function handleBaseMessage(message: MotionProtocolEventMessage) {
    if (message.type === "snapshot") {
      setStatus(message.status);
    } else if (message.type === "fault" && message.code === "transport_disconnect") {
      setConnection("transport_fault");
    }
  }

  return {
    streamClient,
    connection,
    setConnection,
    status,
    setStatus,
    keymap,
    connect,
    connectProvisionedDevice,
    disconnect,
    refreshStatus,
  };
}
