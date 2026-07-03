import {
  Activity,
  AlertTriangle,
  Crosshair,
  Gauge,
  Grid3X3,
  Keyboard,
  LayoutDashboard,
  PlugZap,
  Radio,
  RefreshCw,
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
import { GroupStreamClient } from "../domain/groupStreamClient";
import type { PlannedRemoteMotionGroup } from "../domain/groupStreamPlanner";
import { planTextToRemoteMotionGroups } from "../domain/groupStreamPlanner";
import { currentFeiyu200Keymap, sanitizeKeymap, validateKeymap } from "../domain/keymap";
import { mockKeymap, mockStatus } from "../domain/mockDevice";
import { DashboardPage } from "./dashboard/DashboardPage";
import { KeymapPage } from "./keymap/KeymapPage";

type View = "dashboard" | "job" | "debug" | "settings" | "keymap";
type ConnectionState = "disconnected" | "connecting" | "connected" | "fault";
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
  const [jobText, setJobText] = useState("asdf jkl");
  const [skipLineFeed, setSkipLineFeed] = useState(true);
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
  const keymapIssues = useMemo(() => validateKeymap(keymap), [keymap]);
  const printTaskRef = useRef(printTask);
  const activeGroupIdRef = useRef<string | undefined>();

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

  async function connect() {
    setConnection("connecting");
    setPrintTask((task) => ({ ...task, stream: "connecting" }));
    try {
      await streamClient.connect({ host: tcpHost, port: tcpPort });
      const [nextStatus, nextKeymap, nextWifi] = await Promise.all([
        streamClient.getStatus(),
        streamClient.getKeymap(),
        streamClient.getWifiStatus(),
      ]);
      await streamClient.subscribeTelemetry(100);
      setStatus(nextStatus);
      setKeymap(currentFeiyu200Keymap(sanitizeKeymap(nextKeymap)));
      setWifiStatus(nextWifi);
      if (nextWifi.staSsid) {
        setWifiSsid(nextWifi.staSsid);
      }
      setConnection("connected");
      setPrintTask((task) => ({ ...task, stream: "connected" }));
      await window.autoTyper?.writeStore({ lastTcpHost: tcpHost, lastTcpPort: tcpPort, recentJobs: [] });
      appendLog(`TCP 已连接 ${tcpHost}:${tcpPort}`);
    } catch (error) {
      await streamClient.disconnect().catch(() => undefined);
      const message = error instanceof Error ? error.message : "TCP 连接失败";
      setConnection("fault");
      setPrintTask((task) => ({ ...task, stream: "fault", fault: message }));
      appendLog(message);
    }
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
    const plan = planTextToRemoteMotionGroups(jobText, keymap, jobId, { xMm: 0, yMm: 0 }, { disableLineFeed: skipLineFeed });
    setPrintTask((task) => ({
      ...task,
      currentIndex: 0,
      totalGroups: plan.groups.length,
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
      appendLog(`开始发送 ${plan.groups.length} 个 bounded groups`);
      await runPlannedGroups(plan.groups);
      setPrintTask((task) => ({ ...task, running: false, stream: "connected", currentLabel: "" }));
      appendLog("任务组流完成");
    } catch (error) {
      const message = error instanceof Error ? error.message : "任务组流失败";
      setPrintTask((task) => ({ ...task, running: false, stream: "fault", fault: message }));
      appendLog(message);
    } finally {
      activeGroupIdRef.current = undefined;
    }
  }

  async function runPlannedGroups(groups: PlannedRemoteMotionGroup[]) {
    for (let index = 0; index < groups.length; index += 1) {
      const group = groups[index];
      if (!printTaskRef.current.running && index > 0) {
        throw new Error("任务已取消");
      }
      activeGroupIdRef.current = group.groupId;
      setPrintTask((task) => ({
        ...task,
        currentIndex: index + 1,
        currentLabel: groupLabel(group),
      }));
      await streamClient.sendExecGroup(group);
      await waitForGroupDone(group);
      setPrintTask((task) => ({ ...task, completedGroups: index + 1 }));
      activeGroupIdRef.current = undefined;
    }
  }

  function waitForGroupDone(group: TaskGroup): Promise<void> {
    return new Promise((resolve, reject) => {
      let unsubscribe = () => {};
      const timeout = setTimeout(() => {
        cleanup();
        void streamClient.cancel();
        reject(new Error(`Group ${group.groupId} 等待 group_done 超时`));
      }, group.policy.maxRuntimeMs + 5000);
      const cleanup = () => {
        clearTimeout(timeout);
        unsubscribe();
      };
      unsubscribe = streamClient.onMessage((message) => {
        if (message.type === "group_done" && message.groupId === group.groupId) {
          cleanup();
          resolve();
          return;
        }
        if (message.type === "fault") {
          cleanup();
          reject(new Error(`${message.code}: ${message.message}`));
        }
      });
    });
  }

  async function cancelJob() {
    try {
      printTaskRef.current = { ...printTaskRef.current, running: false };
      setPrintTask((task) => ({ ...task, running: false }));
      const result = await streamClient.cancel();
      appendLog(result.ok ? "已取消任务组流" : "取消被拒绝");
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "取消失败");
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

  async function refreshWifiStatus() {
    try {
      const next = await streamClient.getWifiStatus();
      setWifiStatus(next);
      if (next.staSsid) {
        setWifiSsid(next.staSsid);
      }
      appendLog("WiFi 状态已刷新");
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "WiFi 状态刷新失败");
    }
  }

  async function scanWifiNetworks() {
    setWifiBusy(true);
    try {
      const result = await streamClient.scanWifi();
      setWifiNetworks(result.networks);
      if (!wifiSsid && result.networks[0]) {
        setWifiSsid(result.networks[0].ssid);
      }
      appendLog(`发现 ${result.networks.length} 个 WiFi 网络`);
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "WiFi 扫描失败");
    } finally {
      setWifiBusy(false);
    }
  }

  async function configureWifi() {
    setWifiBusy(true);
    try {
      const result = await streamClient.configureWifi(wifiSsid.trim(), wifiPassword);
      setWifiStatus(result.wifi);
      appendLog(result.ok ? `WiFi 已连接 ${result.wifi.ipAddress}` : `${result.code ?? "wifi_failed"}: ${result.message}`);
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "WiFi 配置失败");
    } finally {
      setWifiBusy(false);
    }
  }

  async function finishWifiSetup() {
    setWifiBusy(true);
    try {
      const result = await streamClient.finishWifiSetup();
      setWifiStatus(result.wifi);
      appendLog(result.ok ? "WiFi setup AP 已关闭" : `${result.code ?? "wifi_not_ready"}: ${result.message}`);
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "完成 WiFi setup 失败");
    } finally {
      setWifiBusy(false);
    }
  }

  async function pressMotorTest(type: "press_down" | "press_up") {
    const group: PlannedRemoteMotionGroup = {
      groupId: `debug-${Date.now().toString(36)}-${type}`,
      seq: 0,
      policy: { maxRuntimeMs: 5000, onDisconnect: "cancel" },
      blocks: [{ type, rpm: 1600, accelRaw: 128, timeoutMs: 3000 }],
      kind: type,
    };
    try {
      await streamClient.sendExecGroup(group);
      await waitForGroupDone(group);
      appendLog(type === "press_down" ? "M5 按下测试完成" : "M5 抬起测试完成");
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "M5 测试失败");
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
      appendLog(`Block ${message.blockIndex} ${message.blockType}`);
      return;
    }
    if (message.type === "group_done") {
      appendLog(`Group done ${message.groupId}`);
      return;
    }
    if (message.type === "fault") {
      const fault = message as GroupFaultMessage;
      const text = `${fault.code}: ${fault.message}`;
      setPrintTask((task) => ({ ...task, running: false, stream: "fault", fault: text }));
      appendLog(text);
    }
  }

  function appendLog(line: string) {
    setLogLines((lines) => [`${new Date().toLocaleTimeString()} ${line}`, ...lines].slice(0, 12));
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
            <button className="secondary" onClick={connect}>
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
              <label className="checkbox compact">
                <input type="checkbox" checked={skipLineFeed} onChange={(event) => setSkipLineFeed(event.target.checked)} />
                跳过走纸
              </label>
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
            <div className="panel">
              <div className="panelHeader">
                <h2>M5 按压电机</h2>
                <span>{canDebug ? "空闲可调试" : "运行中锁定"}</span>
              </div>
              <div className="actionRow wrap">
                <button className="primary" disabled={!canDebug} onClick={() => pressMotorTest("press_down")}>press_down</button>
                <button className="secondary" disabled={!canDebug} onClick={() => pressMotorTest("press_up")}>press_up</button>
                <button className="secondary" disabled={connection !== "connected" || isBusy} onClick={probeMotors}>探测 M1-M5</button>
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
                  <button className="primary" onClick={connect}>连接</button>
                  <button className="secondary" onClick={() => void streamClient.disconnect()}>断开</button>
                </div>
              </div>
              <div className="panel wifiPanel">
                <div className="panelHeader">
                  <h2>WiFi 配置</h2>
                  <span>{wifiStatus?.phase ?? "unknown"}</span>
                </div>
                <div className="wifiSetupCard">
                  <div className="wifiSetupMark"><Wifi size={20} /></div>
                  <div>
                    <div className="wifiSetupTitle">{wifiStatus?.setupSsid || "setup AP 未知"}</div>
                    <div className="wifiSetupSub">
                      {wifiStatus?.setupApActive ? `AP ${wifiStatus.setupIpAddress} / ${wifiStatus.setupPassword}` : "setup AP 已关闭"}
                    </div>
                  </div>
                </div>
                <div className="formGrid wifiFormGrid">
                  <label>
                    SSID
                    <input list="wifi-networks" value={wifiSsid} onChange={(event) => setWifiSsid(event.target.value)} />
                    <datalist id="wifi-networks">
                      {wifiNetworks.map((network) => <option key={`${network.ssid}-${network.channel}`} value={network.ssid} />)}
                    </datalist>
                  </label>
                  <label>
                    Password
                    <input type="password" value={wifiPassword} onChange={(event) => setWifiPassword(event.target.value)} />
                  </label>
                </div>
                <div className="actionRow wrap">
                  <button className="secondary" disabled={connection !== "connected" || wifiBusy} onClick={refreshWifiStatus}>刷新 WiFi</button>
                  <button className="secondary" disabled={connection !== "connected" || wifiBusy} onClick={scanWifiNetworks}>扫描网络</button>
                  <button className="primary" disabled={connection !== "connected" || wifiBusy || wifiSsid.trim().length === 0} onClick={configureWifi}>保存并连接</button>
                  <button className="secondary" disabled={connection !== "connected" || wifiBusy || !wifiStatus?.staConnected} onClick={finishWifiSetup}>关闭 setup AP</button>
                </div>
                <div className="stateRows wifiStateRows">
                  <StateRow label="STA" value={wifiStatus?.staConnected ? `${wifiStatus.staSsid} ${wifiStatus.ipAddress}` : wifiStatus?.staConnecting ? "connecting" : "offline"} />
                  <StateRow label="RSSI" value={wifiStatus?.staConnected ? `${wifiStatus.wifiRssi} dBm` : "-"} />
                  <StateRow label="凭据" value={wifiStatus?.savedCredentials ? "saved" : "empty"} />
                  {wifiStatus?.lastError && <StateRow label="错误" value={wifiStatus.lastError} />}
                </div>
                <div className="networkList">
                  {wifiNetworks.slice(0, 8).map((network) => (
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

function groupLabel(group: PlannedRemoteMotionGroup): string {
  const suffix = group.targetKeyLabel ? ` ${group.targetKeyLabel}` : "";
  return `${group.kind}${suffix}`;
}

function connectionText(connection: ConnectionState) {
  switch (connection) {
    case "connected":
      return "ONLINE";
    case "connecting":
      return "CONNECT";
    case "fault":
      return "FAULT";
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
