import {
  AlertTriangle,
  Check,
  CircleAlert,
  CircleDashed,
  FileText,
  Grid3X3,
  Keyboard,
  PlugZap,
  Radio,
  RefreshCw,
  RotateCcw,
  Send,
  Settings,
  Square,
  SquareTerminal,
  Wifi,
} from "lucide-react";
import { useMemo, useState } from "react";
import type { ReactNode } from "react";
import type { DeviceStatus, KeymapDocument } from "../../../../shared/protocol/protocolTypes";
import { validateKeymap } from "../domain/keymap";
import { connectionText } from "./appState";
import { isDeviceBusy, type ConnectionState, type PrintTaskState, type View } from "./appTypes";
import { useDesktopSettings } from "./hooks/useDesktopSettings";
import { useDeviceConnection } from "./hooks/useDeviceConnection";
import { usePrintTaskController } from "./hooks/usePrintTaskController";
import { useTaskLog } from "./hooks/useTaskLog";
import { useWifiProvisioning } from "./hooks/useWifiProvisioning";
import { KeymapPage } from "./keymap/KeymapPage";

const pageCopy: Record<View, { eyebrow: string; title: string; description: string }> = {
  job: {
    eyebrow: "PRINT WORKSPACE",
    title: "开始一次打印",
    description: "输入文本，确认机器就绪，然后让任务按原子动作块依次执行。",
  },
  keymap: {
    eyebrow: "KEY BED",
    title: "键位映射",
    description: "检查字符坐标、当前打印头位置与机械坐标方向。",
  },
  settings: {
    eyebrow: "CONNECTION",
    title: "设备与网络",
    description: "配置 TCP 端点，或通过设备 SoftAP 完成首次入网。",
  },
};

export function App() {
  const [view, setView] = useState<View>("job");
  const { logLines, appendLog } = useTaskLog();
  const settings = useDesktopSettings();
  const device = useDeviceConnection({
    tcpHost: settings.tcpHost,
    tcpPort: settings.tcpPort,
    persistFields: settings.persistFields,
    appendLog,
  });
  const provisioning = useWifiProvisioning({
    wifiSsid: settings.wifiSsid,
    wifiPassword: settings.wifiPassword,
    savedWifiSsid: settings.savedWifiSsid,
    savedWifiPassword: settings.savedWifiPassword,
    setTcpHost: settings.setTcpHost,
    connectProvisionedDevice: device.connectProvisionedDevice,
    persistFields: settings.persistFields,
    appendLog,
  });
  const task = usePrintTaskController({
    streamClient: device.streamClient,
    connection: device.connection,
    status: device.status,
    setStatus: device.setStatus,
    keymap: device.keymap,
    appendLog,
  });

  const keymapIssues = useMemo(() => validateKeymap(device.keymap), [device.keymap]);
  const isBusy = isDeviceBusy(device.status, task.printTask);
  const machineReady = device.status.motionReady && device.status.pressReady && !device.status.lineFeedPrimeRequired;
  const needsRecovery = task.printTask.requiresRecovery || device.status.mode === "faulted";
  const canOperate = device.connection === "connected" && !isBusy && !needsRecovery;
  const canStartPrint = canOperate && machineReady && keymapIssues.length === 0 && task.jobText.length > 0;
  const currentPage = pageCopy[view];

  return (
    <div className="appFrame">
      <ProductHeader
        view={view}
        setView={setView}
        connection={device.connection}
        host={settings.tcpHost}
        isBusy={isBusy}
        onConnect={() => void device.connect()}
        onEmergencyStop={() => void task.emergencyStop()}
      />

      <MachineBar
        status={device.status}
        connection={device.connection}
        printTask={task.printTask}
        onRefresh={() => void device.refreshStatus()}
        onResetFault={() => void task.resetFault()}
        onEmergencyStop={() => void task.emergencyStop()}
        isBusy={isBusy}
      />

      <main className="pageCanvas">
        <PageIntro {...currentPage} />

        {view === "job" && (
          <JobWorkspace
            status={device.status}
            connection={device.connection}
            keymap={device.keymap}
            keymapIssueCount={keymapIssues.length}
            printTask={task.printTask}
            jobText={task.jobText}
            setJobText={task.setJobText}
            canStart={canStartPrint}
            isBusy={isBusy}
            machineReady={machineReady}
            needsRecovery={needsRecovery}
            logLines={logLines}
            onSubmit={() => void task.submitJob()}
            onCancel={() => void task.cancelJob()}
          />
        )}

        {view === "keymap" && <KeymapPage keymap={device.keymap} status={device.status} />}

        {view === "settings" && (
          <SettingsWorkspace
            settings={settings}
            connection={device.connection}
            provisioning={provisioning}
            isBusy={isBusy}
            onConnect={() => void device.connect()}
            onDisconnect={() => void device.disconnect()}
            logLines={logLines}
          />
        )}
      </main>
    </div>
  );
}

function ProductHeader({
  view,
  setView,
  connection,
  host,
  isBusy,
  onConnect,
  onEmergencyStop,
}: {
  view: View;
  setView: (view: View) => void;
  connection: ConnectionState;
  host: string;
  isBusy: boolean;
  onConnect: () => void;
  onEmergencyStop: () => void;
}) {
  return (
    <header className="productHeader">
      <div className="brandLockup">
        <div className="brandMark">AT</div>
        <div>
          <div className="brandName">Auto Typer</div>
          <div className="brandDescriptor">Machine Console</div>
        </div>
      </div>

      <nav className="primaryNav" aria-label="主要页面">
        <NavButton active={view === "job"} icon={<Keyboard />} label="打印" onClick={() => setView("job")} />
        <NavButton active={view === "keymap"} icon={<Grid3X3 />} label="映射" onClick={() => setView("keymap")} />
        <NavButton active={view === "settings"} icon={<Settings />} label="设置" onClick={() => setView("settings")} />
      </nav>

      <div className="headerActions">
        <button
          className={`connectionTrigger ${connection}`}
          onClick={onConnect}
          disabled={connection === "connecting" || isBusy}
          title={`连接到 ${host}`}
        >
          <span className="statusBeacon" />
          <span>{connection === "connected" ? host : connection === "connecting" ? "连接中" : "连接设备"}</span>
        </button>
        <button className="emergencyButton" onClick={onEmergencyStop} disabled={connection !== "connected"}>
          <AlertTriangle size={16} />
          急停
        </button>
      </div>
    </header>
  );
}

function MachineBar({
  status,
  connection,
  printTask,
  onRefresh,
  onResetFault,
  onEmergencyStop,
  isBusy,
}: {
  status: DeviceStatus;
  connection: ConnectionState;
  printTask: PrintTaskState;
  onRefresh: () => void;
  onResetFault: () => void;
  onEmergencyStop: () => void;
  isBusy: boolean;
}) {
  return (
    <section className="machineBar" aria-label="机器状态">
      <div className="machineIdentity">
        <div className={`machineGlyph ${connection}`}><Radio size={17} /></div>
        <div>
          <strong>{status.deviceId || "Auto Typer"}</strong>
          <span>{connectionText(connection)} · TCP NDJSON</span>
        </div>
      </div>
      <div className="machineChecks">
        <MachineCheck label="运动系统" ready={status.motionReady} value={status.motionReady ? "就绪" : "等待"} />
        <MachineCheck label="按压电机" ready={status.pressReady} value={status.pressReady ? "就绪" : "等待"} />
        <MachineCheck label="走纸机构" ready={!status.lineFeedPrimeRequired} value={status.lineFeedPrimeRequired ? "需到位" : "就绪"} />
        <MachineCheck
          label="当前任务"
          ready={connection === "connected" && !printTask.requiresRecovery && status.mode !== "faulted"}
          active={isBusy}
          value={connection !== "connected" ? "离线" : isBusy ? printTask.currentLabel || "执行中" : printTask.requiresRecovery ? "需复位" : "空闲"}
        />
      </div>
      <div className="machineActions">
        <button className="iconButton" onClick={onRefresh} disabled={connection !== "connected" || isBusy} title="刷新设备状态">
          <RefreshCw size={16} />
        </button>
        <button className="quietButton" onClick={onResetFault} disabled={connection !== "connected" || isBusy}>
          <RotateCcw size={15} />
          复位并清故障
        </button>
        <button className="mobileEmergency" onClick={onEmergencyStop} disabled={connection !== "connected"} title="急停">
          <AlertTriangle size={16} />
        </button>
      </div>
    </section>
  );
}

function PageIntro({ eyebrow, title, description }: { eyebrow: string; title: string; description: string }) {
  return (
    <div className="pageIntro">
      <span>{eyebrow}</span>
      <h1>{title}</h1>
      <p>{description}</p>
    </div>
  );
}

function JobWorkspace({
  status,
  connection,
  keymap,
  keymapIssueCount,
  printTask,
  jobText,
  setJobText,
  canStart,
  isBusy,
  machineReady,
  needsRecovery,
  logLines,
  onSubmit,
  onCancel,
}: {
  status: DeviceStatus;
  connection: ConnectionState;
  keymap: KeymapDocument;
  keymapIssueCount: number;
  printTask: PrintTaskState;
  jobText: string;
  setJobText: (value: string) => void;
  canStart: boolean;
  isBusy: boolean;
  machineReady: boolean;
  needsRecovery: boolean;
  logLines: string[];
  onSubmit: () => void;
  onCancel: () => void;
}) {
  const readinessMessage = printReadinessMessage({ connection, machineReady, needsRecovery, keymapIssueCount, jobText });
  return (
    <div className="workbenchLayout">
      <section className="composerSurface">
        <div className="surfaceHeader">
          <div className="surfaceTitle">
            <div className="surfaceIcon"><FileText size={18} /></div>
            <div>
              <h2>打印内容</h2>
              <p>支持键位表中的字符与换行</p>
            </div>
          </div>
          <div className="documentStats">
            <span><strong>{jobText.length}</strong> 字符</span>
            <span><strong>{printTask.totalBlocks}</strong> 动作块</span>
          </div>
        </div>

        <textarea
          className="jobEditor"
          value={jobText}
          onChange={(event) => setJobText(event.target.value)}
          spellCheck={false}
          disabled={isBusy}
          aria-label="待打印文本"
          placeholder="在这里输入要打印的内容…"
        />

        <div className="composerFooter">
          <div className={`readinessNote ${canStart ? "ready" : needsRecovery ? "fault" : ""}`}>
            {canStart ? <Check size={15} /> : needsRecovery ? <CircleAlert size={15} /> : <CircleDashed size={15} />}
            <span>{canStart ? `机器已就绪 · 键位表 v${keymap.version}` : readinessMessage}</span>
          </div>
          <div className="primaryActions">
            <button className="secondaryButton" onClick={onCancel} disabled={!printTask.running}>
              <Square size={15} />
              取消
            </button>
            <button className="primaryButton" onClick={onSubmit} disabled={!canStart}>
              <Send size={16} />
              开始打印
            </button>
          </div>
        </div>
      </section>

      <TaskSidebar status={status} printTask={printTask} logLines={logLines} />
    </div>
  );
}

function TaskSidebar({ status, printTask, logLines }: { status: DeviceStatus; printTask: PrintTaskState; logLines: string[] }) {
  const progress = printTask.totalBlocks > 0 ? Math.min(100, Math.round((printTask.completedBlocks / printTask.totalBlocks) * 100)) : 0;
  return (
    <aside className="taskSidebar">
      <section className="taskProgressSurface">
        <div className="taskStateLine">
          <StatusBadge tone={taskTone(printTask, status)}>{taskLabel(printTask, status)}</StatusBadge>
          <span className="progressNumber">{progress}%</span>
        </div>
        <div className="progressTrack" aria-label={`任务进度 ${progress}%`}>
          <span style={{ width: `${progress}%` }} />
        </div>
        <div className="currentOperation">
          <span>当前动作</span>
          <strong>{printTask.currentLabel || (printTask.running ? "准备执行" : "暂无任务")}</strong>
        </div>
        <div className="taskFacts">
          <Fact label="动作块" value={`${printTask.currentIndex} / ${printTask.totalBlocks}`} />
          <Fact label="已完成" value={`${printTask.completedBlocks} / ${printTask.totalBlocks}`} />
          <Fact label="设备模式" value={status.mode} />
          <Fact label="设备健康" value={status.health} />
        </div>
        {(printTask.fault || status.fault) && (
          <div className="faultCallout">
            <CircleAlert size={17} />
            <div>
              <strong>需要处理</strong>
              <p>{printTask.fault ?? `${status.fault?.code}: ${status.fault?.message}`}</p>
            </div>
          </div>
        )}
      </section>
      <ActivityLog lines={logLines} />
    </aside>
  );
}

function SettingsWorkspace({ settings, connection, provisioning, isBusy, onConnect, onDisconnect, logLines }: {
  settings: ReturnType<typeof useDesktopSettings>;
  connection: ConnectionState;
  provisioning: ReturnType<typeof useWifiProvisioning>;
  isBusy: boolean;
  onConnect: () => void;
  onDisconnect: () => void;
  logLines: string[];
}) {
  return (
    <div className="settingsLayout">
      <section className="settingsSurface">
        <div className="surfaceHeader">
          <div className="surfaceTitle"><div className="surfaceIcon"><PlugZap size={18} /></div><div><h2>TCP 设备</h2><p>控制链路与设备端点</p></div></div>
          <StatusBadge tone={connection === "connected" ? "ready" : connection === "connecting" ? "active" : "muted"}>{connectionText(connection)}</StatusBadge>
        </div>
        <div className="formGrid">
          <TextField label="设备地址" value={settings.tcpHost} onChange={settings.setTcpHost} placeholder="192.168.4.42" />
          <NumberField label="端口" value={settings.tcpPort} onChange={settings.setTcpPort} />
        </div>
        <div className="formActions">
          <button className="secondaryButton" onClick={onDisconnect} disabled={connection === "disconnected" || isBusy}>断开</button>
          <button className="primaryButton" onClick={onConnect} disabled={connection === "connecting" || isBusy}><PlugZap size={15} />{connection === "connected" ? "重新连接" : "连接设备"}</button>
        </div>
      </section>

      <section className="settingsSurface">
        <div className="surfaceHeader">
          <div className="surfaceTitle"><div className="surfaceIcon"><Wifi size={18} /></div><div><h2>Wi-Fi 配网</h2><p>通过 SoftAP 写入局域网凭据</p></div></div>
          <StatusBadge tone={provisioning.provisionState === "sta_failed" ? "fault" : provisioning.provisionState === "sta_connected" ? "ready" : "muted"}>{provisioning.provisionStatus?.state ?? provisioning.provisionState}</StatusBadge>
        </div>
        <div className="formGrid">
          <TextField label="Wi-Fi 名称" value={settings.wifiSsid} onChange={settings.setWifiSsid} placeholder="SSID" />
          <PasswordField label="Wi-Fi 密码" value={settings.wifiPassword} onChange={settings.setWifiPassword} />
        </div>
        <div className="provisionFacts">
          <Fact label="SoftAP" value={provisioning.provisionStatus?.ap?.ip ?? "192.168.4.1"} />
          <Fact label="目标网络" value={provisioning.provisionStatus?.targetSsid || settings.wifiSsid || "—"} />
          <Fact label="设备 IP" value={provisioning.provisionStatus?.ip || "—"} />
        </div>
        {provisioning.provisionStatus?.reason && <div className="inlineNotice fault"><CircleAlert size={15} />{provisioning.provisionStatus.reason}</div>}
        <div className="formActions">
          <button className="secondaryButton" disabled={!settings.savedWifiSsid || isBusy} onClick={() => void provisioning.provisionWifi(true)}>使用已保存网络</button>
          <button className="primaryButton" disabled={!settings.wifiSsid || isBusy} onClick={() => void provisioning.provisionWifi(false)}><Wifi size={15} />开始配网</button>
        </div>
      </section>
      <ActivityLog lines={logLines} expanded />
    </div>
  );
}

function ActivityLog({ lines, expanded = false }: { lines: string[]; expanded?: boolean }) {
  return (
    <details className="activityLog" open={expanded}>
      <summary><span><SquareTerminal size={15} />运行记录</span><small>最近 {lines.length} 条</small></summary>
      <div className="logLines">{lines.map((line, index) => <div key={`${index}-${line}`}>{line}</div>)}</div>
    </details>
  );
}

function MachineCheck({ label, value, ready, active = false }: { label: string; value: string; ready: boolean; active?: boolean }) {
  return <div className={`machineCheck ${active ? "active" : ready ? "ready" : "waiting"}`}><span>{label}</span><strong><i />{value}</strong></div>;
}

function NavButton({ active, icon, label, onClick }: { active: boolean; icon: ReactNode; label: string; onClick: () => void }) {
  return <button className={`navButton ${active ? "active" : ""}`} onClick={onClick}>{icon}<span>{label}</span></button>;
}

function StatusBadge({ tone, children }: { tone: "ready" | "active" | "fault" | "muted"; children: ReactNode }) {
  return <span className={`statusBadge ${tone}`}><i />{children}</span>;
}

function Fact({ label, value }: { label: string; value: string }) {
  return <div className="fact"><span>{label}</span><code>{value}</code></div>;
}

function TextField({ label, value, onChange, placeholder }: { label: string; value: string; onChange: (value: string) => void; placeholder?: string }) {
  return <label className="formField"><span>{label}</span><input value={value} placeholder={placeholder} onChange={(event) => onChange(event.target.value)} /></label>;
}

function PasswordField({ label, value, onChange }: { label: string; value: string; onChange: (value: string) => void }) {
  return <label className="formField"><span>{label}</span><input type="password" value={value} onChange={(event) => onChange(event.target.value)} /></label>;
}

function NumberField({ label, value, onChange }: { label: string; value: number; onChange: (value: number) => void }) {
  return <label className="formField"><span>{label}</span><input type="number" value={value} onChange={(event) => onChange(Number(event.target.value))} /></label>;
}

function printReadinessMessage(input: { connection: ConnectionState; machineReady: boolean; needsRecovery: boolean; keymapIssueCount: number; jobText: string }): string {
  if (input.connection !== "connected") return input.connection === "connecting" ? "正在连接设备…" : "连接设备后才能开始打印";
  if (input.needsRecovery) return "请手动复位机器，然后执行“复位并清故障”";
  if (!input.machineReady) return "等待运动、按压与走纸机构全部就绪";
  if (input.keymapIssueCount > 0) return `键位表存在 ${input.keymapIssueCount} 个问题`;
  if (!input.jobText) return "输入要打印的文本";
  return "正在检查任务…";
}

function taskTone(printTask: PrintTaskState, status: DeviceStatus): "ready" | "active" | "fault" | "muted" {
  if (printTask.requiresRecovery || printTask.fault || status.mode === "faulted") return "fault";
  if (printTask.running) return "active";
  if (printTask.completedBlocks > 0 && printTask.completedBlocks === printTask.totalBlocks) return "ready";
  return "muted";
}

function taskLabel(printTask: PrintTaskState, status: DeviceStatus): string {
  if (printTask.requiresRecovery || status.mode === "faulted") return "需要复位";
  if (printTask.running) return "正在执行";
  if (printTask.completedBlocks > 0 && printTask.completedBlocks === printTask.totalBlocks) return "任务完成";
  return "等待任务";
}
