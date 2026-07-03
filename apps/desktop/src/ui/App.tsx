import {
  Activity,
  AlertTriangle,
  ArrowDown,
  ArrowLeft,
  ArrowRight,
  ArrowUp,
  Crosshair,
  Gauge,
  Grid3X3,
  Keyboard,
  LayoutDashboard,
  PlugZap,
  Radio,
  RefreshCw,
  RotateCcw,
  Send,
  Settings,
  Square,
  Wifi,
  Wrench,
} from "lucide-react";
import { useEffect, useMemo, useRef, useState } from "react";
import type { ReactNode } from "react";
import type {
  DeviceStatus,
  GroupFaultMessage,
  GroupFinalMessage,
  GroupStreamEventMessage,
  KeymapDocument,
  MotorEventMessage,
  MotorRole,
  MotorState,
  MotorStateUpdateMessage,
  TaskGroup,
  TelemetryMessage,
  WifiNetwork,
  WifiStatus,
} from "../../../../shared/protocol/auto-typer-protocol";
import {
  formatExecutionTimeout,
  GroupExecutionWatchdog,
} from "../domain/groupExecutionWatchdog";
import type { GroupExecutionOutcome } from "../domain/groupExecutionWatchdog";
import { GroupStreamClient } from "../domain/groupStreamClient";
import type { PlannedRemoteMotionGroup } from "../domain/groupStreamPlanner";
import { planTextToRemoteMotionGroups } from "../domain/groupStreamPlanner";
import { currentFeiyu200Keymap, validateKeymap } from "../domain/keymap";
import { mockKeymap, mockStatus } from "../domain/mockDevice";
import { SerialWifiClient } from "../domain/serialWifiClient";
import type { SerialWifiConnectionState } from "../domain/serialWifiClient";
import { DashboardPage } from "./dashboard/DashboardPage";
import { KeymapPage } from "./keymap/KeymapPage";

type View = "dashboard" | "job" | "debug" | "settings" | "keymap";
type ConnectionState = "disconnected" | "connecting" | "connected" | "desync" | "transport_fault";
type StreamState = "disconnected" | "connecting" | "connected" | "running" | "fault";

type PrintTaskState = {
  stream: StreamState;
  running: boolean;
  currentIndex: number;
  totalGroups: number;
  completedGroups: number;
  currentLabel: string;
  fault?: string;
};

const defaultPort = 7777;
const returnZeroMotion = { rpm: 1600, accelRaw: 128, timeoutMs: 10000 } as const;
const lineFeedHomeMotion = { rpm: 1600, accelRaw: 128, timeoutMs: 10000 } as const;

export function App() {
  const [view, setView] = useState<View>("dashboard");
  const [tcpHost, setTcpHost] = useState("192.168.4.42");
  const [tcpPort, setTcpPort] = useState(defaultPort);
  const [connection, setConnection] = useState<ConnectionState>("disconnected");
  const [status, setStatus] = useState<DeviceStatus>(mockStatus);
  const [keymap, setKeymap] = useState<KeymapDocument>(mockKeymap);
  const [wifiStatus, setWifiStatus] = useState<WifiStatus | undefined>();
  const [wifiNetworks, setWifiNetworks] = useState<WifiNetwork[]>([]);
  const [wifiSsid, setWifiSsid] = useState("");
  const [wifiPassword, setWifiPassword] = useState("");
  const [wifiBusy, setWifiBusy] = useState(false);
  const [wifiBusyLabel, setWifiBusyLabel] = useState("");
  const [serialWifiConnection, setSerialWifiConnection] = useState<SerialWifiConnectionState>("disconnected");
  const [serialPorts, setSerialPorts] = useState<Array<{ path: string; label: string }>>([]);
  const [selectedSerialPort, setSelectedSerialPort] = useState("");
  const [jobText, setJobText] = useState("asdf jkl");
  const [debugStepPulses, setDebugStepPulses] = useState(80);
  const [debugRpm, setDebugRpm] = useState(800);
  const [debugAccelRaw, setDebugAccelRaw] = useState(80);
  const [printTask, setPrintTask] = useState<PrintTaskState>({
    stream: "disconnected",
    running: false,
    currentIndex: 0,
    totalGroups: 0,
    completedGroups: 0,
    currentLabel: "",
  });
  const [logLines, setLogLines] = useState<string[]>(["等待 TCP 连接"]);
  const streamClient = useMemo(() => new GroupStreamClient(), []);
  const serialWifiClient = useMemo(() => new SerialWifiClient(), []);
  const keymapIssues = useMemo(() => validateKeymap(keymap), [keymap]);
  const printTaskRef = useRef(printTask);
  const activeGroupIdRef = useRef<string | undefined>();
  const activeGroupSeqRef = useRef<number | undefined>();
  const returnZeroAfterCancelRef = useRef(false);

  useEffect(() => {
    printTaskRef.current = printTask;
  }, [printTask]);

  useEffect(() => {
    void window.autoTyper?.readStore().then((store) => {
      if (store.lastTcpHost) {
        setTcpHost(store.lastTcpHost);
      }
      if (store.lastTcpPort) {
        setTcpPort(store.lastTcpPort);
      }
    });
  }, []);

  useEffect(() => streamClient.onMessage(handleTcpEvent), [streamClient]);

  useEffect(() => {
    return window.autoTyper?.serialWifiOnLog?.((event) => {
      if (!event.protocol) {
        appendLog(`[serial] ${event.line}`);
      }
    });
  }, []);

  async function connect(hostOverride?: string) {
    const host = hostOverride ?? tcpHost;
    setConnection("connecting");
    setPrintTask((task) => ({ ...task, stream: "connecting" }));
    try {
      await streamClient.connect({ host, port: tcpPort });
      const nextStatus = await streamClient.getStatus();
      await streamClient.subscribeTelemetry(100);
      setStatus(nextStatus);
      setKeymap(currentFeiyu200Keymap());
      setConnection("connected");
      setPrintTask((task) => ({ ...task, stream: "connected" }));
      await window.autoTyper?.writeStore({ lastTcpHost: host, lastTcpPort: tcpPort, recentJobs: [] });
      appendLog(`TCP 已连接 ${host}:${tcpPort}`);
    } catch (error) {
      await streamClient.disconnect().catch(() => undefined);
      const message = error instanceof Error ? error.message : "TCP 连接失败";
      setConnection("transport_fault");
      setPrintTask((task) => ({ ...task, stream: "fault", fault: message }));
      appendLog(message);
    }
  }

  async function connectSerialWifi() {
    setSerialWifiConnection("connecting");
    try {
      let portPath = selectedSerialPort;
      if (serialPorts.length === 0) {
        const ports = await refreshSerialPorts();
        portPath = selectedSerialPort || ports[0]?.path || "";
      }
      await serialWifiClient.connect(portPath || undefined);
      setSerialWifiConnection("connected");
      const next = await serialWifiClient.getWifiStatus();
      setWifiStatus(next);
      if (next.staSsid) {
        setWifiSsid(next.staSsid);
      }
      appendLog("USB 串口 WiFi 配置已连接");
      if (next.staConnected && next.ipAddress) {
        await connectTcpFromWifiStatus(next);
      }
    } catch (error) {
      setSerialWifiConnection(serialWifiClient.isSupported() ? "disconnected" : "unsupported");
      appendLog(error instanceof Error ? error.message : "USB 串口连接失败");
    }
  }

  async function refreshSerialPorts() {
    const ports = await serialWifiClient.listPorts();
    setSerialPorts(ports);
    if (!selectedSerialPort && ports[0]) {
      setSelectedSerialPort(ports[0].path);
    }
    appendLog(`发现 ${ports.length} 个串口`);
    return ports;
  }

  async function disconnectSerialWifi() {
    await serialWifiClient.disconnect().catch(() => undefined);
    setSerialWifiConnection(serialWifiClient.isSupported() ? "disconnected" : "unsupported");
    appendLog("USB 串口 WiFi 配置已断开");
  }

  async function connectTcpFromWifiStatus(nextWifi?: WifiStatus) {
    setWifiBusy(true);
    setWifiBusyLabel("读取 IP");
    try {
      const next = nextWifi?.staConnected && nextWifi.ipAddress
        ? nextWifi
        : await waitForSerialWifiIp(nextWifi);
      setWifiStatus(next);
      if (!next.staConnected || !next.ipAddress) {
        appendLog("固件尚未连接 WiFi，无法读取可连接 IP");
        return;
      }
      if (next.staSsid) {
        setWifiSsid(next.staSsid);
      }
      setTcpHost(next.ipAddress);
      appendLog(`串口读取到固件 IP ${next.ipAddress}，开始连接 TCP`);
      await connect(next.ipAddress);
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "读取 IP 并连接失败");
    } finally {
      setWifiBusy(false);
      setWifiBusyLabel("");
    }
  }

  async function waitForSerialWifiIp(initial?: WifiStatus): Promise<WifiStatus> {
    const startedAt = Date.now();
    let next = initial;
    while (Date.now() - startedAt < 30000) {
      if (next?.staConnected && next.ipAddress) {
        return next;
      }
      await sleep(800);
      next = await serialWifiClient.getWifiStatus();
      setWifiStatus(next);
    }
    return next ?? await serialWifiClient.getWifiStatus();
  }

  async function refreshStatus() {
    try {
      const next = await streamClient.getStatus();
      setStatus(next);
      appendLog("状态已刷新");
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "状态刷新失败");
    }
  }

  async function submitJob() {
    const jobId = `job-${Date.now().toString(36)}`;
    const plan = planTextToRemoteMotionGroups(jobText, keymap, jobId, { xMm: 0, yMm: 0 });
    const needsLineFeedPrime = status.lineFeedPrimeRequired;
    const totalGroups = plan.groups.length + (needsLineFeedPrime ? 1 : 0);
    setPrintTask((task) => ({
      ...task,
      currentIndex: 0,
      totalGroups,
      completedGroups: 0,
      currentLabel: "",
      fault: plan.ok ? undefined : plan.message,
    }));
    if (!plan.ok) {
      plan.warnings.forEach(appendLog);
      appendLog(`规划失败：${plan.message}`);
      return;
    }
    if (plan.groups.length === 0) {
      appendLog("任务为空");
      return;
    }
    try {
      plan.warnings.forEach(appendLog);
      setPrintTask((task) => ({ ...task, running: true, stream: "running", fault: undefined }));
      if (needsLineFeedPrime) {
        appendLog("任务前走纸到位开始");
        await runSingleGroup(makeLineFeedHomeGroup(`line-feed-prime-${Date.now().toString(36)}`), "任务前走纸到位");
        setPrintTask((task) => ({ ...task, completedGroups: 1 }));
      }
      appendLog(`开始发送 ${plan.groups.length} 个 bounded groups`);
      await runPlannedGroups(plan.groups, needsLineFeedPrime ? 1 : 0);
      const finish = await streamClient.finishTask();
      appendLog(finish.ok ? "任务结束包已确认" : "任务结束包被拒绝");
      setPrintTask((task) => ({ ...task, running: false, stream: "connected", currentLabel: "" }));
      appendLog("任务组流完成");
    } catch (error) {
      const message = error instanceof Error ? error.message : "任务组流失败";
      const watchdogCancelled = message.startsWith("execution_timeout:");
      const userCancelled = message.startsWith("group_cancelled:");
      if (!returnZeroAfterCancelRef.current) {
        setPrintTask((task) => ({
          ...task,
          running: false,
          stream: userCancelled ? "connected" : "fault",
          currentLabel: "",
          fault: watchdogCancelled ? `desktop execution watchdog cancelled group: ${message}` : message,
        }));
      }
      appendLog(message);
    } finally {
      if (!returnZeroAfterCancelRef.current) {
        activeGroupIdRef.current = undefined;
        activeGroupSeqRef.current = undefined;
      }
    }
  }

  async function runPlannedGroups(groups: PlannedRemoteMotionGroup[], indexOffset = 0) {
    for (let index = 0; index < groups.length; index += 1) {
      const group = groups[index];
      if (!printTaskRef.current.running && index > 0) {
        throw new Error("任务已取消");
      }
      activeGroupIdRef.current = group.groupId;
      activeGroupSeqRef.current = group.seq;
      setPrintTask((task) => ({
        ...task,
        currentIndex: indexOffset + index + 1,
        currentLabel: groupLabel(group),
      }));
      appendLog(
        `Send group ${group.groupId} blocks=${group.blocks.length} estimated=${group.estimatedRuntimeMs}ms policy=${group.policy.maxRuntimeMs}ms`,
      );
      await streamClient.sendExecGroup(group);
      const outcome = await waitForGroupDone(group);
      if (outcome.status !== "done") {
        const code = outcome.status === "cancelled" ? "group_cancelled" : outcome.code ?? `group_${outcome.status}`;
        throw new Error(`${code}: ${outcome.message ?? outcome.status}`);
      }
      setPrintTask((task) => ({ ...task, completedGroups: indexOffset + index + 1 }));
      activeGroupIdRef.current = undefined;
      activeGroupSeqRef.current = undefined;
    }
  }

  async function runSingleGroup(group: TaskGroup, label: string): Promise<GroupExecutionOutcome> {
    activeGroupIdRef.current = group.groupId;
    activeGroupSeqRef.current = group.seq;
    setPrintTask((task) => ({ ...task, stream: "running", currentLabel: label, fault: undefined }));
    appendLog(`${label}: ${group.blocks.map((block) => block.type).join("+")}`);
    try {
      await streamClient.sendExecGroup(group);
      const outcome = await waitForGroupDone(group);
      if (outcome.status !== "done") {
        throw new Error(`${outcome.code ?? `group_${outcome.status}`}: ${outcome.message ?? outcome.status}`);
      }
      return outcome;
    } finally {
      activeGroupIdRef.current = undefined;
      activeGroupSeqRef.current = undefined;
    }
  }

  function waitForGroupDone(group: TaskGroup | PlannedRemoteMotionGroup): Promise<GroupExecutionOutcome> {
    return new Promise((resolve, reject) => {
      let unsubscribe = () => {};
      const watchdog = new GroupExecutionWatchdog(group, Date.now());
      const timer = window.setInterval(() => {
        const timeout = watchdog.checkTimeout(Date.now());
        if (!timeout) {
          return;
        }
        cleanup();
        const message = formatExecutionTimeout(timeout);
        appendLog(message);
        void streamClient.cancel();
        reject(new Error(message));
      }, 250);
      const cleanup = () => {
        window.clearInterval(timer);
        unsubscribe();
      };
      unsubscribe = streamClient.onMessage((message) => {
        const observation = watchdog.observe(message, Date.now());
        if (observation.kind === "final") {
          cleanup();
          resolve(observation.outcome);
          return;
        }
        if (observation.kind === "progress" && observation.progress.event === "block_started") {
          const label = blockStartedLabel(group, observation.progress.blockIndex, observation.progress.blockType);
          appendLog(label);
          setPrintTask((task) => ({ ...task, currentLabel: label }));
        }
      });
    });
  }

  async function cancelJob() {
    const cancellingGroup = activeGroupIdRef.current && activeGroupSeqRef.current !== undefined
      ? { groupId: activeGroupIdRef.current, seq: activeGroupSeqRef.current }
      : undefined;
    try {
      returnZeroAfterCancelRef.current = true;
      printTaskRef.current = { ...printTaskRef.current, running: false };
      setPrintTask((task) => ({ ...task, running: false }));
      const result = await streamClient.cancel();
      appendLog(result.ok ? "已取消任务组流" : "取消被拒绝");
      if (result.ok) {
        if (cancellingGroup) {
          const outcome = await waitForGroupDone({ ...cancellingGroup, policy: { maxRuntimeMs: 12000, onDisconnect: "cancel" }, blocks: [] });
          appendLog(`取消完成：${outcome.status}`);
        }
        await runReturnZeroAfterCancel();
      }
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "取消失败");
    } finally {
      returnZeroAfterCancelRef.current = false;
    }
  }

  async function runReturnZeroAfterCancel() {
    setPrintTask((task) => ({ ...task, running: true, stream: "running", currentLabel: "return_zero", fault: undefined }));
    appendLog("取消后自动回零开始");
    try {
      await runSingleGroup(makeReturnZeroGroup(`return-zero-${Date.now().toString(36)}`, true), "取消后自动回零");
      appendLog("取消后自动回零完成");
      setPrintTask((task) => ({ ...task, running: false, stream: "connected", currentLabel: "" }));
    } catch (error) {
      const message = error instanceof Error ? error.message : "取消后自动回零失败";
      setPrintTask((task) => ({ ...task, running: false, stream: "fault", currentLabel: "", fault: message }));
      throw error;
    }
  }

  async function resetFault() {
    try {
      const result = await streamClient.resetFault();
      if (result.status) {
        setStatus(result.status);
      }
      appendLog(result.ok ? "故障已清除" : "故障清除被拒绝");
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "清故障失败");
    }
  }

  async function probeMotors() {
    try {
      const result = await streamClient.probe();
      setStatus((current) => ({ ...current, motors: result.motors }));
      appendLog(result.ok ? "已探测 M1-M5" : "探测被拒绝");
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "电机探测失败");
    }
  }

  async function runLineFeedHome() {
    setPrintTask((task) => ({
      ...task,
      running: true,
      stream: "running",
      currentIndex: 1,
      totalGroups: 1,
      completedGroups: 0,
      currentLabel: "line_feed_home",
      fault: undefined,
    }));
    try {
      await runSingleGroup(makeLineFeedHomeGroup(`line-feed-home-${Date.now().toString(36)}`), "走纸到位");
      appendLog("走纸到位完成");
      const next = await streamClient.getStatus();
      setStatus(next);
      setPrintTask((task) => ({ ...task, running: false, stream: "connected", completedGroups: 1, currentLabel: "" }));
    } catch (error) {
      const message = error instanceof Error ? error.message : "走纸到位失败";
      setPrintTask((task) => ({ ...task, running: false, stream: "fault", currentLabel: "", fault: message }));
      appendLog(message);
    }
  }

  async function runFullReturnZeroAndReleaseLineFeed() {
    setPrintTask((task) => ({
      ...task,
      running: true,
      stream: "running",
      currentIndex: 1,
      totalGroups: 1,
      completedGroups: 0,
      currentLabel: "return_zero",
      fault: undefined,
    }));
    try {
      await runSingleGroup(makeReturnZeroGroup(`full-return-zero-${Date.now().toString(36)}`, false), "全回零");
      const result = await streamClient.releaseLineFeedOrigin();
      if (result.status) {
        setStatus(result.status);
      }
      if (!result.ok) {
        throw new Error("release_line_feed_origin rejected");
      }
      appendLog("全回零完成，M4 已失能");
      setPrintTask((task) => ({ ...task, running: false, stream: "connected", completedGroups: 1, currentLabel: "" }));
    } catch (error) {
      const message = error instanceof Error ? error.message : "全回零失败";
      setPrintTask((task) => ({ ...task, running: false, stream: "fault", currentLabel: "", fault: message }));
      appendLog(message);
    }
  }

  async function refreshWifiStatus() {
    setWifiBusy(true);
    setWifiBusyLabel("刷新 WiFi");
    try {
      const next = await serialWifiClient.getWifiStatus();
      setWifiStatus(next);
      if (next.staSsid) {
        setWifiSsid(next.staSsid);
      }
      appendLog("串口 WiFi 状态已刷新");
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "WiFi 状态刷新失败");
    } finally {
      setWifiBusy(false);
      setWifiBusyLabel("");
    }
  }

  async function scanWifiNetworks() {
    setWifiBusy(true);
    setWifiBusyLabel("扫描网络");
    appendLog("开始扫描 WiFi 网络");
    try {
      const result = await serialWifiClient.scanWifi();
      const networks = [...result.networks].sort((a, b) => b.rssi - a.rssi || a.ssid.localeCompare(b.ssid));
      setWifiNetworks(networks);
      if (!wifiSsid && result.networks[0]) {
        setWifiSsid(networks[0].ssid);
      }
      appendLog(`发现 ${result.networks.length} 个 WiFi 网络`);
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "WiFi 扫描失败");
    } finally {
      setWifiBusy(false);
      setWifiBusyLabel("");
    }
  }

  async function configureWifi() {
    setWifiBusy(true);
    setWifiBusyLabel("连接 WiFi");
    try {
      const result = await serialWifiClient.configureWifi(wifiSsid.trim(), wifiPassword);
      setWifiStatus(result.wifi);
      appendLog(result.ok ? `WiFi 已连接 ${result.wifi.ipAddress}` : `${result.code ?? "wifi_failed"}: ${result.message}`);
      if (result.ok) {
        await connectTcpFromWifiStatus(result.wifi);
      }
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "WiFi 配置失败");
    } finally {
      setWifiBusy(false);
      setWifiBusyLabel("");
    }
  }

  async function jogXY(dxSign: -1 | 0 | 1, dySign: -1 | 0 | 1, label: string) {
    const stepPulses = clampInteger(debugStepPulses, 1, 20000);
    const rpm = clampInteger(debugRpm, 1, 3000);
    const accelRaw = clampInteger(debugAccelRaw, 1, 255);
    const group: TaskGroup = {
      groupId: `debug-xy-${Date.now().toString(36)}-${label.toLowerCase()}`,
      seq: 0,
      policy: { maxRuntimeMs: 12000, onDisconnect: "cancel" },
      blocks: [{
        type: "move_xy",
        dxSteps: dxSign * stepPulses,
        dySteps: dySign * stepPulses,
        rpm,
        accelRaw,
        timeoutMs: 10000,
      }],
    };
    activeGroupIdRef.current = group.groupId;
    activeGroupSeqRef.current = group.seq;
    setPrintTask((task) => ({
      ...task,
      running: true,
      stream: "running",
      currentIndex: 1,
      totalGroups: 1,
      completedGroups: 0,
      currentLabel: `XY ${label} ${stepPulses} pulses`,
      fault: undefined,
    }));
    try {
      appendLog(`XY jog ${label}: dx=${dxSign * stepPulses} dy=${dySign * stepPulses}`);
      await streamClient.sendExecGroup(group);
      const outcome = await waitForGroupDone(group);
      if (outcome.status !== "done") {
        throw new Error(`${outcome.code ?? `group_${outcome.status}`}: ${outcome.message ?? outcome.status}`);
      }
      setPrintTask((task) => ({ ...task, running: false, stream: "connected", completedGroups: 1, currentLabel: "" }));
      appendLog(`XY ${label} 点动完成`);
      await probeMotors();
    } catch (error) {
      const message = error instanceof Error ? error.message : "XY 点动失败";
      setPrintTask((task) => ({ ...task, running: false, stream: "fault", currentLabel: "", fault: message }));
      appendLog(message);
    } finally {
      activeGroupIdRef.current = undefined;
      activeGroupSeqRef.current = undefined;
    }
  }

  async function jogPress(type: "press_down" | "press_up", label: string) {
    const group: TaskGroup = {
      groupId: `debug-press-${Date.now().toString(36)}-${type}`,
      seq: 0,
      policy: { maxRuntimeMs: 12000, onDisconnect: "cancel" },
      blocks: [{ type, rpm: 3000, accelRaw: 255, timeoutMs: 10000 }],
    };
    activeGroupIdRef.current = group.groupId;
    activeGroupSeqRef.current = group.seq;
    setPrintTask((task) => ({
      ...task,
      running: true,
      stream: "running",
      currentIndex: 1,
      totalGroups: 1,
      completedGroups: 0,
      currentLabel: `M5 ${label}`,
      fault: undefined,
    }));
    try {
      appendLog(`M5 ${label} 点动`);
      await streamClient.sendExecGroup(group);
      const outcome = await waitForGroupDone(group);
      if (outcome.status !== "done") {
        throw new Error(`${outcome.code ?? `group_${outcome.status}`}: ${outcome.message ?? outcome.status}`);
      }
      setPrintTask((task) => ({ ...task, running: false, stream: "connected", completedGroups: 1, currentLabel: "" }));
      appendLog(`M5 ${label} 完成`);
      await probeMotors();
    } catch (error) {
      const message = error instanceof Error ? error.message : "M5 点动失败";
      setPrintTask((task) => ({ ...task, running: false, stream: "fault", currentLabel: "", fault: message }));
      appendLog(message);
    } finally {
      activeGroupIdRef.current = undefined;
      activeGroupSeqRef.current = undefined;
    }
  }

  function handleTcpEvent(message: GroupStreamEventMessage) {
    if (message.type === "telemetry") {
      setStatus((message as TelemetryMessage).status);
      return;
    }
    if (message.type === "status") {
      setStatus(message.status);
      return;
    }
    if (message.type === "motor_state_update") {
      setStatus((current) => applyMotorStateUpdate(current, message as MotorStateUpdateMessage));
      return;
    }
    if (message.type === "motor_event") {
      setStatus((current) => applyMotorEvent(current, message as MotorEventMessage));
      return;
    }
    if (message.type === "telemetry_overflow") {
      appendLog(`${message.code}: ${message.message}`);
      return;
    }
    if (message.type === "group_started") {
      appendLog(`Group started ${message.groupId}`);
      return;
    }
    if (message.type === "block_started") {
      if (!matchesActiveGroup(message.groupId, message.seq)) {
        appendLog(`Block ${message.blockIndex} ${message.blockType}`);
      }
      if (matchesActiveGroup(message.groupId, message.seq)) {
        setPrintTask((task) => ({ ...task, currentLabel: `Block ${message.blockIndex} ${message.blockType}` }));
      }
      return;
    }
    if (message.type === "group_done") {
      appendLog(`Group done ${message.groupId}`);
      if (matchesActiveGroup(message.groupId, message.seq)) {
        setPrintTask((task) => ({ ...task, currentLabel: "" }));
      }
      return;
    }
    if (message.type === "group_final") {
      const final = message as GroupFinalMessage;
      appendLog(`Group final ${final.groupId} ${final.status}${final.code ? ` ${final.code}` : ""}`);
      if (matchesActiveGroup(final.groupId, final.seq)) {
        setPrintTask((task) => ({
          ...task,
          running: final.status === "done" ? task.running : false,
          stream: final.status === "done" || final.status === "cancelled" ? "connected" : "fault",
          currentLabel: "",
          fault: final.status === "cancelled"
            ? `${final.code ?? "group_cancelled"}: ${final.message ?? "cancelled"}`
            : task.fault,
        }));
      }
      if (final.status !== "done") {
        const code = final.code ?? `group_${final.status}`;
        const text = final.status === "cancelled"
          ? `${code}: ${final.message ?? "cancelled"}`
          : code === "motion_feedback_timeout"
          ? "motion_feedback_timeout: Motion feedback timed out"
          : code === "motion_command_no_ack"
          ? "motion_command_no_ack: Motor did not ACK motion command"
          : code === "motion_no_movement"
          ? "motion_no_movement: Motor ACKed but did not move"
          : code === "motion_target_timeout"
          ? "motion_target_timeout: Motion target timed out"
          : `${code}: ${final.message ?? final.status}`;
        setPrintTask((task) => ({
          ...task,
          running: false,
          stream: final.status === "cancelled" ? "connected" : "fault",
          currentLabel: "",
          fault: text,
        }));
      }
      return;
    }
    if (message.type === "fault") {
      const fault = message as GroupFaultMessage;
      const text = `${fault.code}: ${fault.message}`;
      if (!fault.groupId || matchesActiveGroup(fault.groupId, fault.seq ?? activeGroupSeqRef.current ?? 0)) {
        setPrintTask((task) => ({ ...task, running: false, stream: "fault", currentLabel: "", fault: text }));
      }
      appendLog(text);
    }
  }

  function matchesActiveGroup(groupId: string, seq: number) {
    return activeGroupIdRef.current === groupId && activeGroupSeqRef.current === seq;
  }

  function appendLog(line: string) {
    setLogLines((lines) => [...lines, `${new Date().toLocaleTimeString()} ${line}`].slice(-12));
  }

  const jobState = status.currentJob?.state ?? "none";
  const isBusy = printTask.running || status.mode === "running" || jobState === "queued" || jobState === "running" || jobState === "cancelling";
  const canDebug = connection === "connected" && !isBusy && status.mode !== "faulted";

  return (
    <div className="appShell">
      <aside className="sidebar">
        <div className="brand">
          <div className="brandMark">AT</div>
          <div>
            <div className="brandTitle">Auto Typer</div>
            <div className="brandSub">TCP motion console</div>
          </div>
        </div>
        <nav className="navList">
          <NavButton active={view === "dashboard"} icon={<LayoutDashboard />} label="设备仪表盘" onClick={() => setView("dashboard")} />
          <NavButton active={view === "job"} icon={<Keyboard />} label="打印任务" onClick={() => setView("job")} />
          <NavButton active={view === "keymap"} icon={<Grid3X3 />} label="映射表" onClick={() => setView("keymap")} />
          <NavButton active={view === "debug"} icon={<Wrench />} label="调试入口" onClick={() => setView("debug")} />
          <NavButton active={view === "settings"} icon={<Settings />} label="TCP 设置" onClick={() => setView("settings")} />
        </nav>
        <div className="devicePlate">
          <div className={`statusDot ${connection}`} />
          <div>
            <div className="plateTitle">{status.deviceId}</div>
            <div className="plateSub">{tcpHost}:{tcpPort}</div>
          </div>
        </div>
      </aside>

      <main className="workspace">
        <header className="topbar">
          <div>
            <div className="eyebrow">TCP NDJSON</div>
            <h1>{view === "dashboard" ? "设备仪表盘" : view === "job" ? "打印任务" : view === "debug" ? "调试工作台" : view === "keymap" ? "映射表" : "TCP 设置"}</h1>
          </div>
          <div className="connectionBox">
            <input value={tcpHost} onChange={(event) => setTcpHost(event.target.value)} />
            <input className="portInput" type="number" value={tcpPort} onChange={(event) => setTcpPort(Number(event.target.value))} />
            <button className="secondary" onClick={() => void connect()}>
              <PlugZap size={16} />
              连接
            </button>
            <button className="secondary" onClick={refreshStatus} disabled={connection !== "connected"}>
              <RefreshCw size={16} />
              刷新
            </button>
            <button className="danger" onClick={cancelJob} disabled={connection !== "connected"}>
              <AlertTriangle size={16} />
              取消/停止
            </button>
            <button className="secondary" onClick={resetFault} disabled={connection !== "connected"}>
              清故障
            </button>
          </div>
        </header>

        <section className="statusRail">
          <Metric icon={<Radio />} label="连接" value={connectionText(connection)} tone={connection} />
          <Metric icon={<Activity />} label="模式" value={status.mode} tone={status.mode === "faulted" ? "fault" : "connected"} />
          <Metric icon={<Gauge />} label="运动" value={status.motionReady ? "READY" : "WAIT"} tone={status.motionReady ? "connected" : "fault"} />
          <Metric icon={<Crosshair />} label="M5" value={status.pressReady ? "READY" : "WAIT"} tone={status.pressReady ? "connected" : "fault"} />
          <Metric icon={<RotateCcw />} label="走纸" value={status.lineFeedPrimeRequired ? "PRIME" : "READY"} tone={status.lineFeedPrimeRequired ? "fault" : "connected"} />
        </section>

        {view === "dashboard" && <DashboardPage status={status} connectionState={connection} />}
        {view === "keymap" && <KeymapPage keymap={keymap} status={status} />}

        {view === "job" && (
          <section className="panelGrid">
            <div className="panel large">
              <div className="panelHeader">
                <h2>文本任务</h2>
                <span>{jobText.length} 字符 / {printTask.totalGroups} groups</span>
              </div>
              <textarea value={jobText} onChange={(event) => setJobText(event.target.value)} spellCheck={false} />
              <div className="actionRow">
                <button className="primary" onClick={submitJob} disabled={connection !== "connected" || isBusy || status.mode === "faulted"}>
                  <Send size={16} />
                  开始 bounded group 打印
                </button>
                <button className="secondary" onClick={cancelJob} disabled={connection !== "connected"}>
                  <Square size={16} />
                  取消任务
                </button>
              </div>
            </div>
            <TaskStatusPanel status={status} logLines={logLines} printTask={printTask} />
          </section>
        )}

        {view === "debug" && (
          <section className="panelGrid">
            <div className="panel xyDebugPanel">
              <div className="panelHeader">
                <h2>XY 平台点动</h2>
                <span>{canDebug ? "空闲可调试" : "运行中锁定"}</span>
              </div>
              <div className="pulseReadoutGrid">
                <PulseReadout label="X pulse" motor={motorByRole(status, "x")} />
                <PulseReadout label="Y-L pulse" motor={motorByRole(status, "y_left")} />
                <PulseReadout label="Y-R pulse" motor={motorByRole(status, "y_right")} />
                <PulseReadout label="M4 pulse" motor={motorByRole(status, "line_feed")} />
                <PulseReadout label="M5 pulse" motor={motorByRole(status, "press")} />
              </div>
              <div className="debugControlGrid">
                <label>
                  步长 pulse
                  <input
                    type="number"
                    min={1}
                    max={20000}
                    value={debugStepPulses}
                    onChange={(event) => setDebugStepPulses(Number(event.target.value))}
                  />
                </label>
                <label>
                  RPM
                  <input
                    type="number"
                    min={1}
                    max={3000}
                    value={debugRpm}
                    onChange={(event) => setDebugRpm(Number(event.target.value))}
                  />
                </label>
                <label>
                  Accel
                  <input
                    type="number"
                    min={1}
                    max={255}
                    value={debugAccelRaw}
                    onChange={(event) => setDebugAccelRaw(Number(event.target.value))}
                  />
                </label>
              </div>
              <div className="xyJogPad" aria-label="XY jog controls">
                <button className="secondary xyJogButton yPlus" disabled={!canDebug} onClick={() => jogXY(0, 1, "Y+")} title="Y+">
                  <ArrowUp size={20} />
                  <span>Y+</span>
                </button>
                <button className="secondary xyJogButton xMinus" disabled={!canDebug} onClick={() => jogXY(-1, 0, "X-")} title="X-">
                  <ArrowLeft size={20} />
                  <span>X-</span>
                </button>
                <div className="xyJogCenter">XY</div>
                <button className="secondary xyJogButton xPlus" disabled={!canDebug} onClick={() => jogXY(1, 0, "X+")} title="X+">
                  <ArrowRight size={20} />
                  <span>X+</span>
                </button>
                <button className="secondary xyJogButton yMinus" disabled={!canDebug} onClick={() => jogXY(0, -1, "Y-")} title="Y-">
                  <ArrowDown size={20} />
                  <span>Y-</span>
                </button>
              </div>
              <div className="pressJogSection">
                <div className="pressJogTitle">
                  <h3>M5 按压点动</h3>
                  <span>{motorByRole(status, "press")?.readiness ?? "unknown"}</span>
                </div>
                <div className="pressJogPad" aria-label="Press motor jog controls">
                  <button className="secondary pressJogButton" disabled={!canDebug} onClick={() => jogPress("press_up", "上")} title="M5 上">
                    <ArrowUp size={20} />
                    <span>上</span>
                  </button>
                  <button className="secondary pressJogButton" disabled={!canDebug} onClick={() => jogPress("press_down", "下")} title="M5 下">
                    <ArrowDown size={20} />
                    <span>下</span>
                  </button>
                </div>
              </div>
              <div className="actionRow wrap">
                <button className="primary" disabled={!canDebug} onClick={runLineFeedHome}>
                  <RotateCcw size={16} />
                  走纸到位
                </button>
                <button className="secondary" disabled={!canDebug} onClick={runFullReturnZeroAndReleaseLineFeed}>
                  <Crosshair size={16} />
                  全回零/释放走纸
                </button>
                <button className="secondary" disabled={connection !== "connected" || isBusy} onClick={probeMotors}>刷新 pulse</button>
              </div>
            </div>
            <TaskStatusPanel status={status} logLines={logLines} printTask={printTask} />
          </section>
        )}

        {view === "settings" && (
          <section className="panelGrid">
            <div className="settingsStack">
              <div className="panel">
                <div className="panelHeader">
                  <h2>TCP 设备</h2>
                  <span>raw NDJSON</span>
                </div>
                <div className="formGrid">
                  <label>Host<input value={tcpHost} onChange={(event) => setTcpHost(event.target.value)} /></label>
                  <label>Port<input type="number" value={tcpPort} onChange={(event) => setTcpPort(Number(event.target.value))} /></label>
                </div>
                <div className="actionRow">
                  <button className="primary" onClick={() => void connect()}>连接</button>
                  <button className="secondary" onClick={() => void streamClient.disconnect()}>断开</button>
                </div>
              </div>
              <div className="panel wifiPanel">
                <div className="panelHeader">
                  <h2>WiFi 配置</h2>
                  <span>{wifiBusy ? wifiBusyLabel : serialWifiConnection === "connected" ? wifiStatus?.phase ?? "unknown" : serialWifiConnection}</span>
                </div>
                <div className="wifiSetupCard">
                  <div className="wifiSetupMark"><Wifi size={20} /></div>
                  <div>
                    <div className="wifiSetupTitle">{serialWifiConnection === "connected" ? "USB 串口 WiFi 配置" : "选择 ESP32 USB 串口"}</div>
                    <div className="wifiSetupSub">
                      {wifiStatus?.staConnected ? `STA ${wifiStatus.ipAddress} / ${wifiStatus.staSsid}` : "串口扫描网络，保存后自动连接返回的 IP"}
                    </div>
                  </div>
                </div>
                <div className="formGrid wifiFormGrid">
                  <label>
                    USB 串口端口
                    <select value={selectedSerialPort} onChange={(event) => setSelectedSerialPort(event.target.value)}>
                      <option value="">自动选择</option>
                      {serialPorts.map((port) => (
                        <option key={port.path} value={port.path}>{port.label} / {port.path}</option>
                      ))}
                    </select>
                  </label>
                  <label>
                    扫描到的网络
                    <select
                      value={wifiNetworks.some((network) => network.ssid === wifiSsid) ? wifiSsid : ""}
                      onChange={(event) => {
                        if (event.target.value) {
                          setWifiSsid(event.target.value);
                        }
                      }}
                    >
                      <option value="">手动输入 SSID</option>
                      {wifiNetworks.map((network) => (
                        <option key={`${network.ssid}-${network.channel}-${network.rssi}`} value={network.ssid}>
                          {network.ssid} ({network.rssi} dBm / ch {network.channel} / {network.encryption})
                        </option>
                      ))}
                    </select>
                  </label>
                  <label>
                    SSID
                    <input value={wifiSsid} onChange={(event) => setWifiSsid(event.target.value)} />
                  </label>
                  <label>
                    Password
                    <input type="password" value={wifiPassword} onChange={(event) => setWifiPassword(event.target.value)} />
                  </label>
                </div>
                <div className="actionRow wrap">
                  <button className="secondary" disabled={wifiBusy || serialWifiConnection === "connecting"} onClick={() => void refreshSerialPorts()}>刷新串口</button>
                  <button className="secondary" disabled={wifiBusy || serialWifiConnection === "connecting"} onClick={connectSerialWifi}>连接串口</button>
                  <button className="secondary" disabled={wifiBusy || serialWifiConnection !== "connected"} onClick={disconnectSerialWifi}>断开串口</button>
                  <button className="secondary" disabled={serialWifiConnection !== "connected" || wifiBusy} onClick={refreshWifiStatus}>刷新 WiFi</button>
                  <button className="secondary" disabled={serialWifiConnection !== "connected" || wifiBusy} onClick={scanWifiNetworks}>
                    {wifiBusyLabel === "扫描网络" ? "扫描中..." : "扫描网络"}
                  </button>
                  <button className="primary" disabled={serialWifiConnection !== "connected" || wifiBusy || wifiSsid.trim().length === 0} onClick={configureWifi}>保存并连接 TCP</button>
                  <button className="secondary" disabled={serialWifiConnection !== "connected" || wifiBusy} onClick={() => void connectTcpFromWifiStatus()}>读取 IP 并连接</button>
                </div>
                <div className="stateRows wifiStateRows">
                  <StateRow label="串口" value={serialWifiConnection} />
                  <StateRow label="STA" value={wifiStatus?.staConnected ? `${wifiStatus.staSsid} ${wifiStatus.ipAddress}` : wifiStatus?.staConnecting ? "connecting" : "offline"} />
                  <StateRow label="RSSI" value={wifiStatus?.staConnected ? `${wifiStatus.wifiRssi} dBm` : "-"} />
                  <StateRow label="凭据" value={wifiStatus?.savedCredentials ? "saved" : "empty"} />
                  {wifiStatus?.lastError && <StateRow label="错误" value={wifiStatus.lastError} />}
                </div>
                <div className="networkList">
                  {wifiNetworks.map((network) => (
                    <button className="networkRow" key={`${network.ssid}-${network.channel}`} onClick={() => setWifiSsid(network.ssid)}>
                      <span>{network.ssid}</span>
                      <code>{network.rssi} dBm / ch {network.channel} / {network.encryption}</code>
                    </button>
                  ))}
                  {wifiNetworks.length === 0 && <div className="emptyState">尚未扫描网络</div>}
                </div>
              </div>
            </div>
            <TaskStatusPanel status={status} logLines={logLines} printTask={printTask} />
          </section>
        )}
      </main>
    </div>
  );
}

function NavButton({ active, icon, label, onClick }: { active: boolean; icon: ReactNode; label: string; onClick: () => void }) {
  return (
    <button className={`navButton ${active ? "active" : ""}`} onClick={onClick}>
      {icon}
      {label}
    </button>
  );
}

function Metric({ icon, label, value, tone }: { icon: ReactNode; label: string; value: string; tone: string }) {
  return (
    <div className={`metric ${tone}`}>
      <div className="metricIcon">{icon}</div>
      <div>
        <div className="metricLabel">{label}</div>
        <div className="metricValue">{value}</div>
      </div>
    </div>
  );
}

function TaskStatusPanel({ status, logLines, printTask }: { status: DeviceStatus; logLines: string[]; printTask: PrintTaskState }) {
  return (
    <div className="panel">
      <div className="panelHeader">
        <h2>任务状态</h2>
        <span>{printTask.stream}</span>
      </div>
      <div className="stateRows">
        <StateRow label="模式" value={status.mode} />
        <StateRow label="健康" value={status.health} />
        <StateRow label="运动" value={status.motionReady ? "READY" : "WAIT"} />
        <StateRow label="走纸到位" value={status.lineFeedPrimeRequired ? "需要" : "完成"} />
        <StateRow label="M5 Press" value={status.pressReady ? "READY" : "WAIT"} />
        <StateRow label="Group" value={`${printTask.currentIndex}/${printTask.totalGroups}`} />
        <StateRow label="已完成" value={`${printTask.completedGroups}/${printTask.totalGroups}`} />
        <StateRow label="当前" value={printTask.currentLabel || "-"} />
        {printTask.fault && <StateRow label="故障" value={printTask.fault} />}
        {status.fault && <StateRow label="设备故障" value={`${status.fault.code}: ${status.fault.message}`} />}
      </div>
      <div className="logBox">
        {logLines.map((line) => <div key={line}>{line}</div>)}
      </div>
    </div>
  );
}

function StateRow({ label, value }: { label: string; value: string }) {
  return (
    <div className="stateRow">
      <span>{label}</span>
      <code>{value}</code>
    </div>
  );
}

function PulseReadout({ label, motor }: { label: string; motor: MotorState | undefined }) {
  return (
    <div className={`pulseReadout ${motor?.hasRecentInputPulse ? "fresh" : ""}`}>
      <span>{label}</span>
      <strong>{motor?.hasInputPulse ? motor.inputPulseSteps : "-"}</strong>
      <code>{motor?.readiness ?? "unknown"}</code>
    </div>
  );
}

function groupLabel(group: PlannedRemoteMotionGroup): string {
  const suffix = group.targetKeyLabel ? ` ${group.targetKeyLabel}` : "";
  return `${group.kind}${suffix}`;
}

function blockStartedLabel(
  group: TaskGroup | PlannedRemoteMotionGroup,
  blockIndex: number,
  blockType?: PlannedRemoteMotionGroup["kind"],
): string {
  const planned = "plannedBlocks" in group ? group.plannedBlocks[blockIndex] : undefined;
  const block = planned?.block;
  const type = block?.type ?? blockType ?? "unknown";
  const target = planned?.targetKeyLabel ? ` target=${planned.targetKeyLabel}` : "";
  if (block?.type === "move_xy") {
    return `Block ${blockIndex} ${type}${target} dxSteps=${block.dxSteps} dySteps=${block.dySteps} timeoutMs=${block.timeoutMs}`;
  }
  if (block && "timeoutMs" in block) {
    return `Block ${blockIndex} ${type}${target} timeoutMs=${block.timeoutMs}`;
  }
  return `Block ${blockIndex} ${type}${target}`;
}

function makeLineFeedHomeGroup(groupId: string): TaskGroup {
  return {
    groupId,
    seq: 0,
    policy: { maxRuntimeMs: 20000, onDisconnect: "cancel" },
    blocks: [{ type: "line_feed_home", ...lineFeedHomeMotion }],
  };
}

function makeReturnZeroGroup(groupId: string, includeLineFeedHome: boolean): TaskGroup {
  return {
    groupId,
    seq: 0,
    policy: { maxRuntimeMs: includeLineFeedHome ? 30000 : 20000, onDisconnect: "cancel" },
    blocks: includeLineFeedHome
      ? [{ type: "return_zero", ...returnZeroMotion }, { type: "line_feed_home", ...lineFeedHomeMotion }]
      : [{ type: "return_zero", ...returnZeroMotion }],
  };
}

function connectionText(connection: ConnectionState) {
  switch (connection) {
    case "connected":
      return "ONLINE";
    case "connecting":
      return "CONNECT";
    case "desync":
      return "DESYNC";
    case "transport_fault":
      return "TRANSPORT";
    case "disconnected":
    default:
      return "OFFLINE";
  }
}

function applyMotorStateUpdate(status: DeviceStatus, message: MotorStateUpdateMessage): DeviceStatus {
  const motors = ensureMotorList(status.motors);
  for (const update of message.motors) {
    const motor = ensureMotor(motors, update.motorId, telemetryRoleToMotorRole(update.role));
    motor.role = telemetryRoleToMotorRole(update.role);
    motor.readiness = update.readiness;
    motor.lastAnyFrameMs = update.lastUpdatedAtMs;
    if (update.hasVelocity) {
      motor.hasVelocity = true;
      motor.hasRecentVelocity = true;
      motor.velocityRpm = update.velocityRpm ?? motor.velocityRpm;
      motor.lastVelocityMs = update.lastUpdatedAtMs;
    } else {
      motor.hasVelocity = false;
      motor.hasRecentVelocity = false;
    }
    if (update.hasInputPulse) {
      motor.hasInputPulse = true;
      motor.hasRecentInputPulse = true;
      motor.inputPulseSteps = update.inputPulseSteps ?? motor.inputPulseSteps;
      motor.lastInputPulseMs = update.lastUpdatedAtMs;
    } else {
      motor.hasInputPulse = false;
      motor.hasRecentInputPulse = false;
    }
    if (update.hasRealtimeAngle) {
      motor.hasRealtimeAngle = true;
      motor.realtimeAngleRaw65536 = update.angleRaw ?? motor.realtimeAngleRaw65536;
      motor.lastRealtimeAngleMs = update.lastUpdatedAtMs;
    } else {
      motor.hasRealtimeAngle = false;
    }
    if (update.hasStatusFlags) {
      motor.hasStatus = true;
      motor.hasRecentStatus = true;
      motor.statusFlags = update.statusFlags ?? motor.statusFlags;
      motor.lastStatusMs = update.lastUpdatedAtMs;
    } else {
      motor.hasStatus = false;
      motor.hasRecentStatus = false;
    }
  }
  return { ...status, motors };
}

function applyMotorEvent(status: DeviceStatus, message: MotorEventMessage): DeviceStatus {
  const motors = ensureMotorList(status.motors);
  const motor = ensureMotor(motors, message.motorId, telemetryRoleToMotorRole(message.role));
  motor.role = telemetryRoleToMotorRole(message.role);
  motor.lastAnyFrameMs = message.timestampMs;
  const command = parseCommandByte(message.data.command);
  if (message.eventKind === "ack") {
    motor.lastAckCommand = command;
    motor.lastAckMs = message.timestampMs;
    motor.lastErrorCode = "";
    motor.lastErrorMessage = "";
  } else if (message.eventKind === "motion_reached") {
    motor.motionReached = true;
    motor.lastMotionReachedMs = message.timestampMs;
    motor.lastErrorCode = "";
    motor.lastErrorMessage = "Motion reached";
  } else if (message.eventKind === "condition_not_met") {
    motor.conditionNotMet = true;
    motor.lastConditionNotMetCommand = command;
    motor.lastConditionNotMetMs = message.timestampMs;
    motor.lastErrorCode = "condition_not_met";
    motor.lastErrorMessage = "Motor reported command condition not met";
    motor.readiness = "condition_not_met";
  } else if (message.eventKind === "malformed") {
    motor.commandMalformed = true;
    motor.lastMalformedCommand = command;
    motor.lastMalformedMs = message.timestampMs;
    motor.lastErrorCode = "command_malformed";
    motor.lastErrorMessage = "Motor reported malformed command";
    motor.readiness = "faulted";
  }
  return { ...status, motors };
}

function ensureMotorList(current: MotorState[] | undefined): MotorState[] {
  const motors = current ? current.map((motor) => ({ ...motor })) : [];
  for (const entry of [
    [1, "x"],
    [2, "y_left"],
    [3, "y_right"],
    [4, "line_feed"],
    [5, "press"],
  ] as Array<[number, MotorRole]>) {
    ensureMotor(motors, entry[0], entry[1]);
  }
  return motors.sort((a, b) => a.id - b.id);
}

function ensureMotor(motors: MotorState[], id: number, role: MotorRole): MotorState {
  let motor = motors.find((candidate) => candidate.id === id);
  if (motor) {
    return motor;
  }
  motor = {
    id,
    role,
    readiness: "unknown",
    hasVelocity: false,
    hasRealtimeAngle: false,
    hasInputPulse: false,
    hasStatus: false,
    hasRecentStatus: false,
    hasRecentInputPulse: false,
    hasRecentVelocity: false,
    velocityRpm: 0,
    realtimeAngleRaw65536: 0,
    inputPulseSteps: 0,
    statusFlags: 0,
    driverFault: false,
    conditionNotMet: false,
    commandMalformed: false,
    lastAckCommand: 0,
    lastConditionNotMetCommand: 0,
    lastMalformedCommand: 0,
    lastAckMs: 0,
    lastConditionNotMetMs: 0,
    lastMalformedMs: 0,
    motionReached: false,
    lastMotionReachedMs: 0,
    lastVelocityMs: 0,
    lastRealtimeAngleMs: 0,
    lastInputPulseMs: 0,
    lastStatusMs: 0,
    lastAnyFrameMs: 0,
    lastProbeMs: 0,
    lastErrorCode: "",
    lastErrorMessage: "",
  };
  motors.push(motor);
  return motor;
}

function motorByRole(status: DeviceStatus, role: MotorRole): MotorState | undefined {
  return ensureMotorList(status.motors).find((motor) => motor.role === role);
}

function clampInteger(value: number, minValue: number, maxValue: number): number {
  if (!Number.isFinite(value)) {
    return minValue;
  }
  return Math.min(Math.max(Math.round(value), minValue), maxValue);
}

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => window.setTimeout(resolve, ms));
}

function telemetryRoleToMotorRole(role: string): MotorRole {
  switch (role) {
    case "X":
      return "x";
    case "YLeft":
      return "y_left";
    case "YRight":
      return "y_right";
    case "LineFeed":
      return "line_feed";
    case "Press":
      return "press";
    default:
      return "x";
  }
}

function parseCommandByte(command: string | undefined): number {
  if (!command) {
    return 0;
  }
  const parsed = Number.parseInt(command, 16);
  return Number.isFinite(parsed) ? parsed : 0;
}
