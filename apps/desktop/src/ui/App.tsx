import {
  Activity,
  AlertTriangle,
  ChevronLeft,
  ChevronRight,
  Crosshair,
  Gauge,
  Grid3X3,
  Keyboard,
  KeyRound,
  LayoutDashboard,
  PlugZap,
  Radio,
  RefreshCw,
  Save,
  Send,
  Settings,
  Square,
  Wifi,
  Wrench,
} from "lucide-react";
import { useEffect, useMemo, useRef, useState } from "react";
import type { ReactNode } from "react";
import type {
  CanBusDiagnostics,
  DeviceStatus,
  GroupFaultMessage,
  GroupStreamEventMessage,
  KeymapDocument,
  MotorDirection,
  MotorState,
  ServoCommand,
  TelemetryMessage,
  WifiNetwork,
  WifiSetupStatusResponse,
} from "../../../../shared/protocol/auto-typer-protocol";
import { GroupStreamClient } from "../domain/groupStreamClient";
import type { PlannedRemoteMotionGroup } from "../domain/groupStreamPlanner";
import { planTextToRemoteMotionGroups } from "../domain/groupStreamPlanner";
import { DeviceClient } from "../domain/deviceClient";
import {
  displayKey,
  emptyKeymap,
  currentFeiyu200Keymap,
  isPoetryFeiyu200Key,
  poetryFeiyu200KeyCount,
  sanitizeKeymap,
  upsertBinding,
  validateKeymap,
} from "../domain/keymap";
import { mockKeymap, mockStatus } from "../domain/mockDevice";
import { DashboardPage } from "./dashboard/DashboardPage";
import { KeymapPage } from "./keymap/KeymapPage";

type View = "dashboard" | "job" | "debug" | "settings" | "keymap";
type ConnectionState = "disconnected" | "connecting" | "connected" | "fault";
type StreamState = "disconnected" | "connecting" | "connected" | "running" | "unknown" | "fault";

type PrintTaskState = {
  stream: StreamState;
  running: boolean;
  currentIndex: number;
  totalGroups: number;
  currentLabel: string;
  fault?: string;
};

const motorTargets = [
  { id: 23, label: "Y 组 2+3" },
  { id: 1, label: "X 电机 1" },
  { id: 4, label: "走纸电机 4" },
  { id: 5, label: "按压电机 5" },
] as const;

export function App() {
  const [view, setView] = useState<View>("dashboard");
  const [deviceUrl, setDeviceUrl] = useState("http://192.168.4.42");
  const [connection, setConnection] = useState<ConnectionState>("disconnected");
  const [setupUrl, setSetupUrl] = useState("http://192.168.4.1");
  const [status, setStatus] = useState<DeviceStatus>(mockStatus);
  const [keymap, setKeymap] = useState<KeymapDocument>(mockKeymap);
  const [jobText, setJobText] = useState("asdf jkl");
  const [skipLineFeed, setSkipLineFeed] = useState(true);
  const [wifiStatus, setWifiStatus] = useState<WifiSetupStatusResponse | undefined>();
  const [wifiNetworks, setWifiNetworks] = useState<WifiNetwork[]>([]);
  const [selectedWifiSsid, setSelectedWifiSsid] = useState("");
  const [wifiPassword, setWifiPassword] = useState("");
  const [wifiSetupBusy, setWifiSetupBusy] = useState(false);
  const [printTask, setPrintTask] = useState<PrintTaskState>({
    stream: "disconnected",
    running: false,
    currentIndex: 0,
    totalGroups: 0,
    currentLabel: "",
  });
  const [logLines, setLogLines] = useState<string[]>(["等待连接 ESP32"]);
  const [selectedKey, setSelectedKey] = useState("a");
  const [probePoint, setProbePoint] = useState({ xMm: 10.5, yMm: 45 });
  const [servoDurationMs, setServoDurationMs] = useState(200);
  const [motor, setMotor] = useState({
    motorId: 23,
    direction: "cw" as MotorDirection,
    rpm: 800,
    acceleration: 10,
    steps: 1600,
    sync: false,
  });

  const client = useMemo(() => new DeviceClient(deviceUrl), [deviceUrl]);
  const setupClient = useMemo(() => new DeviceClient(setupUrl), [setupUrl]);
  const streamClient = useMemo(() => new GroupStreamClient(), []);
  const keymapIssues = useMemo(() => validateKeymap(keymap), [keymap]);
  const printTaskRef = useRef(printTask);
  const activeGroupIdRef = useRef<string | undefined>();

  useEffect(() => {
    printTaskRef.current = printTask;
  }, [printTask]);

  useEffect(() => {
    void window.autoTyper?.readStore().then((store) => {
      if (store.lastDeviceUrl) {
        setDeviceUrl(store.lastDeviceUrl);
      }
    });
  }, []);

  useEffect(() => {
    return streamClient.onMessage((message) => {
      handleGroupStreamEvent(message);
    });
  }, [streamClient]);

  async function connect() {
    await connectAt(deviceUrl);
  }

  async function connectAt(targetUrl: string) {
    setConnection("connecting");
    const targetClient = new DeviceClient(targetUrl);
    try {
      const nextStatus = await targetClient.status();
      const nextKeymap = await targetClient.getKeymap();
      setDeviceUrl(targetUrl);
      setStatus(nextStatus);
      setKeymap(currentFeiyu200Keymap(sanitizeKeymap(nextKeymap)));
      setConnection("connected");
      appendLog(`已连接 ${nextStatus.deviceId} ${nextStatus.ipAddress}`);
      await window.autoTyper?.writeStore({ lastDeviceUrl: targetUrl, recentJobs: [] });
      return nextStatus;
    } catch (error) {
      setConnection("fault");
      appendLog(error instanceof Error ? error.message : "连接失败");
      throw error;
    }
  }

  async function refreshWifiStatus() {
    setWifiSetupBusy(true);
    try {
      const next = await setupClient.wifiStatus();
      setWifiStatus(next);
      if (!selectedWifiSsid && next.staSsid) {
        setSelectedWifiSsid(next.staSsid);
      }
      appendLog(`配网状态：${wifiPhaseLabel(next.phase)}`);
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "读取配网状态失败");
    } finally {
      setWifiSetupBusy(false);
    }
  }

  async function scanWifiNetworks() {
    setWifiSetupBusy(true);
    try {
      const response = await setupClient.wifiNetworks();
      setWifiNetworks(response.networks);
      if (!selectedWifiSsid && response.networks[0]) {
        setSelectedWifiSsid(response.networks[0].ssid);
      }
      appendLog(`扫描到 ${response.networks.length} 个 Wi-Fi`);
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "Wi-Fi 扫描失败");
    } finally {
      setWifiSetupBusy(false);
    }
  }

  async function configureWifi() {
    if (!selectedWifiSsid) {
      appendLog("请选择 Wi-Fi SSID");
      return;
    }

    setWifiSetupBusy(true);
    try {
      const initial = await setupClient.configureWifi({ ssid: selectedWifiSsid, password: wifiPassword });
      setWifiStatus(initial);
      appendLog(`已提交 Wi-Fi 配置：${selectedWifiSsid}`);
      const connected = await pollWifiSetupConnected();
      if (!connected?.ipAddress) {
        appendLog("未等到设备 STA IP，可稍后刷新配网状态");
        return;
      }

      const nextUrl = `http://${connected.ipAddress}`;
      setDeviceUrl(nextUrl);
      appendLog(`设备已联网：${nextUrl}`);
      try {
        await connectAt(nextUrl);
        try {
          const finished = await new DeviceClient(nextUrl).finishWifiSetup();
          setWifiStatus(finished);
          appendLog("setup AP 已关闭");
        } catch (error) {
          appendLog(error instanceof Error ? `关闭 setup AP 失败：${error.message}` : "关闭 setup AP 失败");
        }
      } catch {
        appendLog(`请将电脑切回 ${selectedWifiSsid} 后连接 ${nextUrl}`);
      }
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "Wi-Fi 配网失败");
    } finally {
      setWifiSetupBusy(false);
    }
  }

  async function pollWifiSetupConnected(): Promise<WifiSetupStatusResponse | undefined> {
    for (let attempt = 0; attempt < 16; attempt += 1) {
      await delayMs(1500);
      try {
        const next = await setupClient.wifiStatus();
        setWifiStatus(next);
        if (next.staConnected && next.ipAddress) {
          return next;
        }
      } catch {
        // Keep polling while the setup link changes state.
      }
    }
    return undefined;
  }

  async function submitJob() {
    const jobId = `job-${Date.now().toString(36)}`;
    let latestStatus = status;
    try {
      latestStatus = await client.probeMotors();
      setStatus(latestStatus);
      appendLog("已刷新电机 readiness");
    } catch (error) {
      appendLog(error instanceof Error ? `电机探测失败，使用当前状态规划：${error.message}` : "电机探测失败，使用当前状态规划");
    }
    const lineFeedAvailable = isLineFeedMotorReady(latestStatus);
    const disableLineFeed = skipLineFeed || !lineFeedAvailable;
    const plan = planTextToRemoteMotionGroups(jobText, keymap, jobId, { xMm: 0, yMm: 0 }, { disableLineFeed });
    setPrintTask((task) => ({
      ...task,
      currentIndex: 0,
      totalGroups: plan.groups.length,
      currentLabel: "",
      fault: plan.ok ? undefined : plan.message,
    }));
    if (!plan.ok) {
      plan.warnings.forEach((warning) => appendLog(warning));
      appendLog(`规划失败：${plan.message}`);
      return;
    }
    if (plan.groups.length === 0) {
      appendLog("任务为空");
      return;
    }
    try {
      await ensureGroupStreamConnected();
      plan.warnings.forEach((warning) => appendLog(warning));
      const runningTask = {
        ...printTaskRef.current,
        running: true,
        stream: "running" as const,
        totalGroups: plan.groups.length,
        fault: undefined,
      };
      printTaskRef.current = runningTask;
      setPrintTask(runningTask);
      appendLog(`开始任务组流：${plan.groups.length} groups`);
      await runPlannedGroups(plan.groups);
      appendLog("任务组流完成");
      const nextStatus = await client.status();
      setStatus(nextStatus);
    } catch (error) {
      const message = error instanceof Error ? error.message : "提交任务失败";
      setPrintTask((task) => ({ ...task, running: false, stream: task.stream === "unknown" ? "unknown" : "fault", fault: message }));
      appendLog(message);
    } finally {
      activeGroupIdRef.current = undefined;
    }
  }

  async function ensureGroupStreamConnected() {
    if (printTaskRef.current.stream === "connected" || printTaskRef.current.stream === "running") {
      return;
    }
    setPrintTask((task) => ({ ...task, stream: "connecting" }));
    const host = deviceUrlToHost(deviceUrl);
    await streamClient.connect({ host, port: 7777 });
    setPrintTask((task) => ({ ...task, stream: "connected" }));
    appendLog(`任务组流已连接 ${host}:7777`);
  }

  async function probeGroupStreamMotors() {
    appendLog("任务组流电机探测中");
    const ack = await streamClient.probe();
    if (!ack.ok || !ack.accepted) {
      throw new Error(`Group probe rejected: ${ack.code ?? "rejected"} ${ack.message ?? ""}`.trim());
    }
    appendLog("任务组流电机探测完成");
  }

  async function runPlannedGroups(groups: PlannedRemoteMotionGroup[]) {
    await probeGroupStreamMotors();
    for (let index = 0; index < groups.length; index += 1) {
      const planned = groups[index];
      if (!printTaskRef.current.running) {
        throw new Error("任务已取消");
      }
      activeGroupIdRef.current = planned.groupId;
      setPrintTask((task) => ({
        ...task,
        currentIndex: index + 1,
        currentLabel: groupLabel(planned),
      }));
      const ack = await streamClient.sendExecGroup(planned.groupId, planned.steps);
      if (!ack.ok || !ack.accepted) {
        throw new Error(`Group rejected: ${ack.code ?? "rejected"} ${ack.message ?? ""}`.trim());
      }
      await waitForGroupDone(planned);
      activeGroupIdRef.current = undefined;
    }
    setPrintTask((task) => ({ ...task, running: false, stream: "connected", currentLabel: "" }));
  }

  function waitForGroupDone(planned: PlannedRemoteMotionGroup): Promise<void> {
    return new Promise((resolve, reject) => {
      let unsubscribe = () => {};
      let timeout: ReturnType<typeof setTimeout>;
      const cleanup = () => {
        clearTimeout(timeout);
        unsubscribe();
      };
      timeout = setTimeout(() => {
        cleanup();
        reject(new Error(`Group timed out: ${planned.groupId}`));
      }, groupDoneTimeoutMs(planned));
      unsubscribe = streamClient.onMessage((message) => {
        if (message.type === "group_done" && message.groupId === planned.groupId) {
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

  function handleGroupStreamEvent(message: GroupStreamEventMessage) {
    if (message.type === "group_done") {
      appendLog(`Group done ${message.groupId}${message.durationMs !== undefined ? ` ${message.durationMs}ms` : ""}`);
      if (message.currentPoint) {
        setStatus((current) => ({
          ...current,
          currentJob: current.currentJob
            ? { ...current.currentJob, currentPoint: message.currentPoint! }
            : {
                jobId: undefined,
                state: "completed",
                textLength: 0,
                currentIndex: 0,
                currentStep: 0,
                totalSteps: 0,
                currentPoint: message.currentPoint!,
              },
        }));
      }
      return;
    }
    if (message.type === "telemetry") {
      setStatus((current) => ({
        ...current,
        mode: message.executor === "faulted" ? "faulted" : message.executor === "running" ? "running" : current.mode,
        canDiagnostics: mergeCanDiagnostics(current.canDiagnostics, message.can),
        motors: message.motors ? mergeTelemetryMotors(current.motors, message.motors) : current.motors,
        currentJob: current.currentJob
          ? { ...current.currentJob, currentPoint: message.currentPoint }
          : {
              jobId: undefined,
              state: message.jobState,
              textLength: 0,
              currentIndex: 0,
              currentStep: 0,
              totalSteps: 0,
              currentPoint: message.currentPoint,
            },
      }));
      return;
    }
    if (message.type === "snapshot") {
      setStatus((current) => ({
        ...current,
        mode: message.snapshot.mode,
        currentJob: current.currentJob
          ? { ...current.currentJob, currentPoint: message.snapshot.currentPoint }
          : {
              jobId: undefined,
              state: message.snapshot.mode === "running" ? "running" : "none",
              textLength: 0,
              currentIndex: 0,
              currentStep: 0,
              totalSteps: 0,
              currentPoint: message.snapshot.currentPoint,
            },
      }));
      return;
    }
    if (message.type === "fault") {
      const fault = message as GroupFaultMessage;
      const text = `${fault.code}: ${fault.message}`;
      setPrintTask((task) => ({
        ...task,
        running: false,
        stream: fault.code === "disconnect" && activeGroupIdRef.current ? "unknown" : "fault",
        fault: text,
      }));
      appendLog(text);
    }
  }

  async function cancelJob() {
    try {
      printTaskRef.current = { ...printTaskRef.current, running: false };
      setPrintTask((task) => ({ ...task, running: false }));
      if (printTaskRef.current.stream === "connected" || printTaskRef.current.stream === "running") {
        const ack = await streamClient.cancel();
        appendLog(ack.accepted ? "已取消任务组流" : `取消拒绝：${ack.message ?? ack.code ?? "rejected"}`);
      } else {
        setStatus(await client.cancelJob());
        appendLog("已取消当前任务");
      }
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "提交任务失败");
    }
  }

  async function emergencyStop() {
    try {
      const next = await client.stopMachine();
      setStatus(next);
      appendLog("已发送急停命令");
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "急停命令失败");
    }
  }

  async function resetFault() {
    try {
      const next = await client.resetFault();
      setStatus(next);
      appendLog(next.mode === "faulted" ? `故障仍存在：${faultText(next)}` : "故障已清除");
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "清除故障失败");
    }
  }

  async function probeMotors() {
    try {
      const next = await client.probeMotors();
      setStatus(next);
      appendLog("已探测电机 readiness");
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "电机探测失败");
    }
  }

  async function runMotorMove() {
    try {
      setStatus(await client.moveMotor(motor));
      appendLog(`${motorTargetLabel(motor.motorId)} 相对移动 ${motor.steps} steps`);
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "电机移动失败");
    }
  }

  async function enableMotor(enabled: boolean) {
    try {
      setStatus(await client.enableMotor({ motorId: motor.motorId, enabled, sync: motor.sync }));
      appendLog(`${motorTargetLabel(motor.motorId)} ${enabled ? "使能" : "失能"}`);
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "电机使能命令失败");
    }
  }

  async function stopMotor() {
    try {
      setStatus(await client.stopMotor({ motorId: motor.motorId, sync: motor.sync }));
      appendLog(`${motorTargetLabel(motor.motorId)} 停止`);
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "电机停止失败");
    }
  }

  async function applyServo(command: ServoCommand) {
    try {
      setStatus(await client.applyServo({ command, durationMs: servoDurationMs }));
      appendLog(`舵机 ${command} ${servoDurationMs}ms`);
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "舵机命令失败");
    }
  }

  async function saveProbe() {
    if (!isPoetryFeiyu200Key(selectedKey)) {
      appendLog(`${displayKey(selectedKey)} 不在诗歌字符集内`);
      return;
    }
    const local = upsertBinding(keymap, { key: selectedKey, point: probePoint });
    setKeymap(local);
    try {
      const remote = await client.probeKey({ key: selectedKey, point: probePoint });
      setKeymap(sanitizeKeymap(remote));
      appendLog(`已保存 ${displayKey(selectedKey)} 坐标到 ESP32`);
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "映射保存失败，本地已暂存");
    }
  }

  async function syncKeymap() {
    try {
      const nextKeymap = sanitizeKeymap(keymap);
      setKeymap(sanitizeKeymap(await client.putKeymap(nextKeymap)));
      appendLog("映射表已同步到 ESP32");
    } catch (error) {
      appendLog(error instanceof Error ? error.message : "映射表同步失败");
    }
  }

  function appendLog(line: string) {
    setLogLines((lines) => [`${new Date().toLocaleTimeString()} ${line}`, ...lines].slice(0, 10));
  }

  const jobState = status.currentJob?.state ?? "none";
  const isBusy = printTask.running || status.mode === "running" || jobState === "queued" || jobState === "planning" || jobState === "running" || jobState === "cancelling";
  const canDebug = connection === "connected" && !isBusy && status.mode !== "faulted";

  return (
    <div className="appShell">
      <aside className="sidebar">
        <div className="brand">
          <div className="brandMark">AT</div>
          <div>
            <div className="brandTitle">Auto Typer</div>
            <div className="brandSub">ESP32 控制台</div>
          </div>
        </div>

        <nav className="navList">
          <NavButton active={view === "dashboard"} icon={<LayoutDashboard />} label="设备仪表盘" onClick={() => setView("dashboard")} />
          <NavButton active={view === "job"} icon={<Keyboard />} label="打印任务" onClick={() => setView("job")} />
          <NavButton active={view === "keymap"} icon={<Grid3X3 />} label="映射表" onClick={() => setView("keymap")} />
          <NavButton active={view === "debug"} icon={<Wrench />} label="调试入口" onClick={() => setView("debug")} />
          <NavButton active={view === "settings"} icon={<Settings />} label="设备设置" onClick={() => setView("settings")} />
        </nav>

        <div className="devicePlate">
          <div className={`statusDot ${connection}`} />
          <div>
            <div className="plateTitle">{status.deviceId}</div>
            <div className="plateSub">{status.ipAddress}</div>
          </div>
        </div>
      </aside>

      <main className="workspace">
        <header className="topbar">
          <div>
            <div className="eyebrow">LAN CONTROL</div>
            <h1>{view === "dashboard" ? "设备仪表盘" : view === "job" ? "打印任务" : view === "debug" ? "调试工作台" : view === "keymap" ? "映射表" : "设备设置"}</h1>
          </div>
          <div className="connectionBox">
            <input value={deviceUrl} onChange={(event) => setDeviceUrl(event.target.value)} />
            <button className="secondary" onClick={connect}>
              <PlugZap size={16} />
              连接
            </button>
            <button className="danger" onClick={emergencyStop}>
              <AlertTriangle size={16} />
              急停
            </button>
            <button className="secondary" onClick={resetFault} disabled={connection !== "connected" || status.mode !== "faulted"}>
              清故障
            </button>
            <button className="secondary" onClick={probeMotors} disabled={connection !== "connected" || isBusy}>
              探测电机
            </button>
          </div>
        </header>

        <section className="statusRail">
          <Metric icon={<Radio />} label="连接" value={connectionText(connection)} tone={connection} />
          <Metric icon={<Activity />} label="模式" value={status.mode} tone={status.mode === "faulted" ? "fault" : "connected"} />
          <Metric icon={<Gauge />} label="Wi-Fi" value={`${status.wifiRssi} dBm`} tone="connected" />
          <Metric icon={<Crosshair />} label="映射" value={`v${keymap.version}`} tone={keymapIssues.some((issue) => issue.level === "error") ? "fault" : "connected"} />
        </section>

        {view === "dashboard" && (
          <DashboardPage status={status} connectionState={connection} />
        )}

        {view === "keymap" && (
          <KeymapPage keymap={keymap} status={status} />
        )}

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
                  开始任务组流打印
                </button>
                {status.mode === "faulted" && <span className="inlineFault">{faultText(status)}</span>}
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
                <h2>电机控制</h2>
                <span>{canDebug ? "空闲可调试" : "运行中锁定"}</span>
              </div>
              <div className="formGrid">
                <label>
                  电机/组
                  <select value={motor.motorId} onChange={(e) => setMotor({ ...motor, motorId: Number(e.target.value) })}>
                    {motorTargets.map((target) => (
                      <option key={target.id} value={target.id}>
                        {target.label}
                      </option>
                    ))}
                  </select>
                </label>
                <label>方向<select value={motor.direction} onChange={(e) => setMotor({ ...motor, direction: e.target.value as MotorDirection })}><option value="cw">CW</option><option value="ccw">CCW</option></select></label>
                <label>RPM<input type="number" value={motor.rpm} onChange={(e) => setMotor({ ...motor, rpm: Number(e.target.value) })} /></label>
                <label>加速度<input type="number" value={motor.acceleration} onChange={(e) => setMotor({ ...motor, acceleration: Number(e.target.value) })} /></label>
                <label>步数<input type="number" value={motor.steps} onChange={(e) => setMotor({ ...motor, steps: Number(e.target.value) })} /></label>
              </div>
              <div className="actionRow wrap">
                <button className="secondary" disabled={!canDebug} onClick={() => enableMotor(true)}>使能</button>
                <button className="secondary" disabled={!canDebug} onClick={() => enableMotor(false)}>失能</button>
                <button className="primary" disabled={!canDebug} onClick={runMotorMove}>相对移动</button>
                <button className="danger" disabled={!canDebug} onClick={stopMotor}>停止电机</button>
              </div>
            </div>

            <div className="panel">
              <div className="panelHeader">
                <h2>舵机与映射</h2>
                <span>{keymap.bindings.length} 点</span>
              </div>
              <div className="servoPad">
                <button className="secondary" disabled={!canDebug} onClick={() => applyServo("release")}>释放</button>
                <button className="primary" disabled={!canDebug} onClick={() => applyServo("press")}>按压</button>
                <button className="secondary" disabled={!canDebug} onClick={() => applyServo("neutral")}>中位</button>
              </div>
              <div className="servoTune">
                <label>运动时长 ms<input type="number" min="1" max="65535" value={servoDurationMs} onChange={(e) => setServoDurationMs(Number(e.target.value))} /></label>
              </div>
              <div className="formGrid">
                <label>按键<input value={selectedKey} onChange={(e) => setSelectedKey(e.target.value.slice(0, 1))} /></label>
                <label>X mm<input type="number" value={probePoint.xMm} onChange={(e) => setProbePoint({ ...probePoint, xMm: Number(e.target.value) })} /></label>
                <label>Y mm<input type="number" value={probePoint.yMm} onChange={(e) => setProbePoint({ ...probePoint, yMm: Number(e.target.value) })} /></label>
              </div>
              <div className="actionRow">
                <button className="primary" disabled={!canDebug} onClick={saveProbe}>
                  <Save size={16} />
                  保存采样点
                </button>
                <button className="secondary" onClick={syncKeymap}>同步映射表</button>
              </div>
            </div>

            <KeymapPanel keymap={keymap} issues={keymapIssues} />
          </section>
        )}

        {view === "settings" && (
          <section className="panelGrid">
            <WifiSetupPanel
              setupUrl={setupUrl}
              setSetupUrl={setSetupUrl}
              wifiStatus={wifiStatus}
              networks={wifiNetworks}
              selectedSsid={selectedWifiSsid}
              setSelectedSsid={setSelectedWifiSsid}
              password={wifiPassword}
              setPassword={setWifiPassword}
              busy={wifiSetupBusy}
              onRefreshStatus={refreshWifiStatus}
              onScanNetworks={scanWifiNetworks}
              onConfigure={configureWifi}
            />
            <div className="settingsStack">
              <TaskStatusPanel status={status} logLines={logLines} printTask={printTask} />
              <div className="panel">
                <div className="panelHeader">
                  <h2>映射表</h2>
                  <span>{keymapIssues.length} 项提示</span>
                </div>
                <button className="secondary" onClick={() => setKeymap(emptyKeymap())}>新建空映射</button>
                <KeymapPanel keymap={keymap} issues={keymapIssues} />
              </div>
            </div>
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

function motorTargetLabel(motorId: number) {
  return motorTargets.find((target) => target.id === motorId)?.label ?? `电机 ${motorId}`;
}

function faultText(status: DeviceStatus) {
  if (status.fault) {
    return `${status.fault.code} ${status.fault.message}`.trim();
  }
  const diagnostics = status.canDiagnostics;
  if (diagnostics?.lastFaultCode || diagnostics?.lastFaultMessage) {
    return `${diagnostics.lastFaultCode} ${diagnostics.lastFaultMessage}`.trim();
  }
  return "未知故障";
}

function deviceUrlToHost(deviceUrl: string): string {
  try {
    return new URL(deviceUrl).hostname;
  } catch {
    return deviceUrl.replace(/^https?:\/\//, "").replace(/\/.*$/, "");
  }
}

function groupLabel(group: PlannedRemoteMotionGroup): string {
  const suffix = group.targetKeyLabel ? ` ${group.targetKeyLabel}` : "";
  return `${group.kind}${suffix}`;
}

function isLineFeedMotorReady(status: DeviceStatus): boolean {
  return status.motors?.some((motor) => motor.role === "line_feed" && motor.readiness === "ready") === true;
}

function delayMs(durationMs: number): Promise<void> {
  return new Promise((resolve) => {
    setTimeout(resolve, durationMs);
  });
}

function wifiPhaseLabel(phase: WifiSetupStatusResponse["phase"]): string {
  switch (phase) {
    case "connected":
      return "已联网";
    case "connecting":
      return "连接中";
    case "failed":
      return "连接失败";
    case "no_credentials":
      return "待配网";
    case "idle":
    default:
      return "空闲";
  }
}

function mergeCanDiagnostics(
  current: DeviceStatus["canDiagnostics"],
  telemetry: TelemetryMessage["can"],
): DeviceStatus["canDiagnostics"] {
  if (!telemetry || !current) {
    return current;
  }
  const merged: CanBusDiagnostics = { ...current, ...telemetry };
  return merged;
}

function mergeTelemetryMotors(
  current: MotorState[] | undefined,
  telemetry: NonNullable<TelemetryMessage["motors"]>,
): MotorState[] | undefined {
  if (!current || !telemetry) {
    return current;
  }
  return current.map((motor) => {
    const update = telemetry.find((candidate) => candidate.id === motor.id);
    if (!update) {
      return motor;
    }
    return {
      ...motor,
      role: update.role,
      readiness: update.readiness,
      velocityRpm: update.rpm,
      inputPulseSteps: update.inputPulse,
      realtimeAngleRaw65536: update.angle,
      hasRecentInputPulse: update.fresh,
      hasRecentVelocity: update.fresh,
    };
  });
}

function groupDoneTimeoutMs(planned: PlannedRemoteMotionGroup): number {
  const paddingMs = 3000;
  const moveStep = planned.steps.find((step) => step.kind === "move_xy");
  if (moveStep?.kind === "move_xy" && moveStep.profile?.timeoutMs) {
    return moveStep.profile.timeoutMs + paddingMs;
  }
  const waitStep = planned.steps.find((step) => step.kind === "wait");
  if (waitStep?.kind === "wait") {
    return waitStep.durationMs + paddingMs;
  }
  if (planned.steps.some((step) => step.kind === "servo_press" || step.kind === "servo_release")) {
    return 5000;
  }
  if (planned.steps.some((step) => step.kind === "line_feed")) {
    return 30000;
  }
  if (planned.steps.some((step) => step.kind === "character_release")) {
    return 10000;
  }
  return 30000;
}

function WifiSetupPanel({
  setupUrl,
  setSetupUrl,
  wifiStatus,
  networks,
  selectedSsid,
  setSelectedSsid,
  password,
  setPassword,
  busy,
  onRefreshStatus,
  onScanNetworks,
  onConfigure,
}: {
  setupUrl: string;
  setSetupUrl: (value: string) => void;
  wifiStatus?: WifiSetupStatusResponse;
  networks: WifiNetwork[];
  selectedSsid: string;
  setSelectedSsid: (value: string) => void;
  password: string;
  setPassword: (value: string) => void;
  busy: boolean;
  onRefreshStatus: () => void;
  onScanNetworks: () => void;
  onConfigure: () => void;
}) {
  return (
    <div className="panel wifiPanel">
      <div className="panelHeader">
        <h2>Wi-Fi 配网</h2>
        <span>{wifiStatus ? wifiPhaseLabel(wifiStatus.phase) : "setup AP"}</span>
      </div>
      <div className="wifiSetupCard">
        <div className="wifiSetupMark">
          <Wifi size={22} />
        </div>
        <div>
          <div className="wifiSetupTitle">{wifiStatus?.setupSsid || "auto-typer-setup"}</div>
          <div className="wifiSetupSub">{wifiStatus?.setupPassword ? `AP 密码 ${wifiStatus.setupPassword}` : "连接 setup AP 后扫描设备可用 Wi-Fi"}</div>
        </div>
      </div>
      <div className="formGrid wifiFormGrid">
        <label>
          Setup 地址
          <input value={setupUrl} onChange={(event) => setSetupUrl(event.target.value)} />
        </label>
        <label>
          目标 Wi-Fi
          <select value={selectedSsid} onChange={(event) => setSelectedSsid(event.target.value)}>
            <option value="">选择 SSID</option>
            {networks.map((network) => (
              <option key={`${network.ssid}-${network.channel}`} value={network.ssid}>
                {network.ssid} · {network.rssi} dBm · {network.secure ? network.encryption : "open"}
              </option>
            ))}
          </select>
        </label>
        <label>
          Wi-Fi 密码
          <input type="password" value={password} onChange={(event) => setPassword(event.target.value)} />
        </label>
      </div>
      <div className="actionRow wrap">
        <button className="secondary" disabled={busy} onClick={onRefreshStatus}>
          <RefreshCw size={16} />
          刷新状态
        </button>
        <button className="secondary" disabled={busy} onClick={onScanNetworks}>
          <Radio size={16} />
          扫描网络
        </button>
        <button className="primary" disabled={busy || !selectedSsid} onClick={onConfigure}>
          <KeyRound size={16} />
          发送配网
        </button>
      </div>
      <div className="stateRows wifiStateRows">
        <StateRow label="Setup AP" value={wifiStatus?.setupApActive ? "ACTIVE" : "OFF"} />
        <StateRow label="STA" value={wifiStatus?.staConnected ? "CONNECTED" : wifiStatus?.staConnecting ? "CONNECTING" : "OFF"} />
        <StateRow label="STA IP" value={wifiStatus?.ipAddress || "-"} />
        <StateRow label="SSID" value={wifiStatus?.staSsid || selectedSsid || "-"} />
        <StateRow label="RSSI" value={wifiStatus ? `${wifiStatus.wifiRssi} dBm` : "-"} />
        {wifiStatus?.lastError && <StateRow label="错误" value={wifiStatus.lastError} />}
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
        <StateRow label="任务组流" value={printTask.stream} />
        <StateRow label="舵机" value={status.servoReady ? "READY" : "WAIT"} />
        <StateRow label="运动" value={status.motionReady ? "READY" : "WAIT"} />
        <StateRow label="任务" value={status.currentJob?.state ?? "none"} />
        <StateRow label="Group" value={`${printTask.currentIndex}/${printTask.totalGroups}`} />
        <StateRow label="当前" value={printTask.currentLabel || "-"} />
        <StateRow label="坐标" value={`${status.currentJob?.currentPoint.xMm.toFixed(1) ?? "0.0"}, ${status.currentJob?.currentPoint.yMm.toFixed(1) ?? "0.0"}`} />
        {printTask.fault && <StateRow label="任务组流故障" value={printTask.fault} />}
        {status.fault && <StateRow label="故障" value={`${status.fault.code}: ${status.fault.message}`} />}
      </div>
      <div className="logBox">
        {logLines.map((line) => (
          <div key={line}>{line}</div>
        ))}
      </div>
    </div>
  );
}

function KeymapPanel({ keymap, issues }: { keymap: KeymapDocument; issues: Array<{ level: string; message: string; key?: string }> }) {
  const pageSize = 18;
  const [pageIndex, setPageIndex] = useState(0);
  const totalPages = Math.max(1, Math.ceil(keymap.bindings.length / pageSize));
  const currentPage = Math.min(pageIndex, totalPages - 1);
  const pageBindings = keymap.bindings.slice(currentPage * pageSize, currentPage * pageSize + pageSize);

  useEffect(() => {
    if (pageIndex > totalPages - 1) {
      setPageIndex(totalPages - 1);
    }
  }, [pageIndex, totalPages]);

  return (
    <div className="panel keymapPanel">
      <div className="panelHeader">
        <h2>映射表预览</h2>
        <span>{keymap.bindings.length} / {poetryFeiyu200KeyCount}</span>
      </div>
      <div className="bindingTable">
        {pageBindings.map((binding) => (
          <div className="bindingRow" key={`${binding.key}-${binding.point.xMm}-${binding.point.yMm}`}>
            <span>{displayKey(binding.key)}</span>
            <code>{binding.point.xMm.toFixed(1)}, {binding.point.yMm.toFixed(1)}</code>
          </div>
        ))}
        {pageBindings.length === 0 && <div className="emptyState">暂无映射点</div>}
      </div>
      <div className="pager">
        <button
          className="iconButton"
          aria-label="上一页"
          disabled={currentPage === 0}
          onClick={() => setPageIndex((value) => Math.max(0, value - 1))}
        >
          <ChevronLeft size={16} />
        </button>
        <span>{currentPage + 1} / {totalPages}</span>
        <button
          className="iconButton"
          aria-label="下一页"
          disabled={currentPage >= totalPages - 1}
          onClick={() => setPageIndex((value) => Math.min(totalPages - 1, value + 1))}
        >
          <ChevronRight size={16} />
        </button>
      </div>
      <div className="issueList">
        {issues.slice(0, 5).map((issue) => (
          <div className={`issue ${issue.level}`} key={`${issue.key}-${issue.message}`}>{issue.message}</div>
        ))}
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

function motorRoleLabel(motor: MotorState): string {
  switch (motor.role) {
    case "y_left":
      return "Y左";
    case "y_right":
      return "Y右";
    case "line_feed":
      return "走纸";
    case "press":
      return "按压";
    case "x":
    default:
      return "X";
  }
}

function connectionText(connection: ConnectionState): string {
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
