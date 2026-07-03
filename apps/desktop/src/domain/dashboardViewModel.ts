import type {
  CanBusDiagnostics,
  DeviceHealth,
  DeviceMode,
  DeviceStatus,
  JobStatus,
  MotorReadiness,
  MotorRole,
  MotorState,
} from "../../../../shared/protocol/auto-typer-protocol";

// ─── Status Tone ────────────────────────────────────────────────────────────
export type StatusTone = "ok" | "warning" | "fault" | "unknown";

export function healthToTone(health: DeviceHealth): StatusTone {
  switch (health) {
    case "ok":
      return "ok";
    case "warning":
    case "not_ready":
      return "warning";
    case "fault":
      return "fault";
    default:
      return "unknown";
  }
}

export function readinessToTone(readiness: MotorReadiness): StatusTone {
  switch (readiness) {
    case "ready":
      return "ok";
    case "config_pending":
    case "config_sent":
    case "acked":
    case "condition_not_met":
      return "warning";
    case "faulted":
    case "offline":
      return "fault";
    default:
      return "unknown";
  }
}

// ─── Dashboard Summary ──────────────────────────────────────────────────────
export interface SummaryItem {
  label: string;
  value: string;
  tone: StatusTone;
}

export interface DashboardSummary {
  connection: SummaryItem;
  mode: SummaryItem;
  health: SummaryItem;
  jobState: SummaryItem;
  coordinates: SummaryItem;
  groupProgress: SummaryItem;
  canState: SummaryItem;
}

export function buildDashboardSummary(
  status: DeviceStatus,
  connectionState: string,
): DashboardSummary {
  const job = status.currentJob;
  const can = status.canDiagnostics;

  return {
    connection: {
      label: "连接",
      value: connectionSummaryValue(connectionState),
      tone: connectionState === "connected" ? "ok" : connectionState === "transport_fault" || connectionState === "desync" ? "fault" : "unknown",
    },
    mode: {
      label: "模式",
      value: status.mode.toUpperCase(),
      tone: modeToTone(status.mode),
    },
    health: {
      label: "健康",
      value: status.health.toUpperCase(),
      tone: healthToTone(status.health),
    },
    jobState: {
      label: "任务",
      value: (job?.state ?? "none").toUpperCase(),
      tone: jobStateTone(job?.state),
    },
    coordinates: {
      label: "坐标",
      value: job ? `${job.currentPoint.xMm.toFixed(1)}, ${job.currentPoint.yMm.toFixed(1)} mm` : "0.0, 0.0 mm",
      tone: "unknown",
    },
    groupProgress: {
      label: "Group",
      value: job ? `${job.currentStep} / ${job.totalSteps}` : "— / —",
      tone: "unknown",
    },
    canState: {
      label: "CAN",
      value: can ? (can.fatalFault ? "FATAL" : can.driverReady && can.motionReady ? "READY" : "WAIT") : "N/A",
      tone: can ? (can.fatalFault ? "fault" : can.driverReady && can.motionReady ? "ok" : "warning") : "unknown",
    },
  };
}

function connectionSummaryValue(connectionState: string): string {
  switch (connectionState) {
    case "connected":
      return "ONLINE";
    case "connecting":
      return "CONNECTING";
    case "desync":
      return "DESYNC";
    case "transport_fault":
      return "TRANSPORT";
    default:
      return "OFFLINE";
  }
}

function modeToTone(mode: DeviceMode): StatusTone {
  switch (mode) {
    case "idle":
    case "debug":
      return "ok";
    case "running":
    case "paused":
      return "warning";
    case "faulted":
      return "fault";
    default:
      return "unknown";
  }
}

function jobStateTone(state?: string): StatusTone {
  if (!state || state === "none") return "unknown";
  if (state === "completed") return "ok";
  if (state === "failed") return "fault";
  if (state === "running" || state === "queued" || state === "planning" || state === "cancelling") return "warning";
  return "unknown";
}

// ─── Motor Card View Model ──────────────────────────────────────────────────
export interface MotorCardViewModel {
  id: number;
  label: string;
  role: string;
  readiness: MotorReadiness;
  tone: StatusTone;
  rpm: number | null;
  inputPulseSteps: number | null;
  angleDeg: number | null;
  current: number | null;
  temperature: number | null;
  lastUpdateMs: number | null;
  lastEvent: string | null;
  hasFault: boolean;
  faultText: string | null;
}

export function buildMotorCards(status: DeviceStatus): MotorCardViewModel[] {
  const motors = status.motors;
  if (!motors || motors.length === 0) {
    return defaultMotorCards();
  }
  return motors.map((m) => motorToViewModel(m));
}

function motorToViewModel(m: MotorState): MotorCardViewModel {
  const angleDeg = m.hasRealtimeAngle ? (m.realtimeAngleRaw65536 / 65536) * 360 : null;
  const lastUpdateMs = m.lastAnyFrameMs > 0 ? m.lastAnyFrameMs : null;

  let lastEvent: string | null = null;
  if (m.lastErrorCode) {
    lastEvent = `${m.lastErrorCode}: ${m.lastErrorMessage}`;
  } else if (m.motionReached) {
    lastEvent = "Motion reached";
  }

  return {
    id: m.id,
    label: `M${m.id}`,
    role: roleLabel(m.role),
    readiness: m.readiness,
    tone: readinessToTone(m.readiness),
    rpm: m.hasVelocity || m.hasRecentVelocity ? m.velocityRpm : null,
    inputPulseSteps: m.hasInputPulse || m.hasRecentInputPulse ? m.inputPulseSteps : null,
    angleDeg,
    current: null, // firmware does not currently expose motor current
    temperature: null, // firmware does not currently expose motor temperature
    lastUpdateMs,
    lastEvent,
    hasFault: m.driverFault || m.readiness === "faulted",
    faultText: m.driverFault ? (m.lastErrorCode || "Driver fault") : null,
  };
}

function defaultMotorCards(): MotorCardViewModel[] {
  const roles: Array<{ id: number; role: string }> = [
    { id: 1, role: "X" },
    { id: 2, role: "Y-left" },
    { id: 3, role: "Y-right" },
    { id: 4, role: "LineFeed" },
    { id: 5, role: "Press" },
  ];
  return roles.map((r) => ({
    id: r.id,
    label: `M${r.id}`,
    role: r.role,
    readiness: "unknown" as MotorReadiness,
    tone: "unknown" as StatusTone,
    rpm: null,
    inputPulseSteps: null,
    angleDeg: null,
    current: null,
    temperature: null,
    lastUpdateMs: null,
    lastEvent: null,
    hasFault: false,
    faultText: null,
  }));
}

function roleLabel(role: MotorRole): string {
  switch (role) {
    case "x": return "X";
    case "y_left": return "Y-left";
    case "y_right": return "Y-right";
    case "line_feed": return "LineFeed";
    case "press": return "Press";
    default: return role;
  }
}

// ─── CAN Overview ───────────────────────────────────────────────────────────
export interface CanOverviewViewModel {
  available: boolean;
  driverReady: boolean;
  motionReady: boolean;
  fatalFault: boolean;
  txFailed: number;
  txRetry: number;
  busErrors: number;
  queueFull: number;
  rxQueueFull: number;
  pendingFrame: boolean;
  lastFault: string | null;
  lastTxError: string | null;
  tone: StatusTone;
}

export function buildCanOverview(can?: CanBusDiagnostics): CanOverviewViewModel {
  if (!can) {
    return {
      available: false,
      driverReady: false,
      motionReady: false,
      fatalFault: false,
      txFailed: 0,
      txRetry: 0,
      busErrors: 0,
      queueFull: 0,
      rxQueueFull: 0,
      pendingFrame: false,
      lastFault: null,
      lastTxError: null,
      tone: "unknown",
    };
  }
  return {
    available: true,
    driverReady: can.driverReady,
    motionReady: can.motionReady,
    fatalFault: can.fatalFault,
    txFailed: can.txFailedCount,
    txRetry: can.txRetryCount,
    busErrors: can.busErrorCount,
    queueFull: can.commandQueueFullCount,
    rxQueueFull: can.rxQueueFullCount,
    pendingFrame: can.pendingFrameValid,
    lastFault: can.lastFaultCode ? `${can.lastFaultCode}: ${can.lastFaultMessage}` : null,
    lastTxError: can.lastTxError || null,
    tone: can.fatalFault ? "fault" : can.driverReady && can.motionReady ? "ok" : "warning",
  };
}

// ─── Job Overview ───────────────────────────────────────────────────────────
export interface JobOverviewViewModel {
  hasJob: boolean;
  jobId: string;
  state: string;
  textLength: number;
  currentIndex: number;
  currentStep: number;
  totalSteps: number;
  position: string;
  message: string | null;
  progressPercent: number;
}

export function buildJobOverview(job?: JobStatus): JobOverviewViewModel {
  if (!job || job.state === "none") {
    return {
      hasJob: false,
      jobId: "—",
      state: "none",
      textLength: 0,
      currentIndex: 0,
      currentStep: 0,
      totalSteps: 0,
      position: "0.0, 0.0",
      message: null,
      progressPercent: 0,
    };
  }
  const progress = job.totalSteps > 0 ? Math.round((job.currentStep / job.totalSteps) * 100) : 0;
  return {
    hasJob: true,
    jobId: job.jobId ?? "—",
    state: job.state,
    textLength: job.textLength,
    currentIndex: job.currentIndex,
    currentStep: job.currentStep,
    totalSteps: job.totalSteps,
    position: `${job.currentPoint.xMm.toFixed(1)}, ${job.currentPoint.yMm.toFixed(1)}`,
    message: job.message ?? null,
    progressPercent: progress,
  };
}

// ─── Fault Summary ──────────────────────────────────────────────────────────
export interface FaultSummaryViewModel {
  hasFault: boolean;
  code: string;
  message: string;
  recoverable: boolean;
  motorFaults: string[];
  canFault: string | null;
}

export function buildFaultSummary(status: DeviceStatus): FaultSummaryViewModel {
  const motorFaults = (status.motors ?? [])
    .filter((m) => m.driverFault || m.lastErrorCode)
    .map((m) => `M${m.id}: ${m.lastErrorCode || "driver fault"}`);

  const canFault = status.canDiagnostics?.lastFaultCode
    ? `${status.canDiagnostics.lastFaultCode}: ${status.canDiagnostics.lastFaultMessage}`
    : null;

  if (status.fault) {
    return {
      hasFault: true,
      code: status.fault.code,
      message: status.fault.message,
      recoverable: status.fault.recoverable,
      motorFaults,
      canFault,
    };
  }

  return {
    hasFault: motorFaults.length > 0 || canFault !== null,
    code: "",
    message: "",
    recoverable: true,
    motorFaults,
    canFault,
  };
}

// ─── Utility: format age ────────────────────────────────────────────────────
export function formatAge(ms: number | null): string {
  if (ms === null || ms <= 0) return "N/A";
  if (ms < 1000) return `${ms} ms ago`;
  if (ms < 60000) return `${(ms / 1000).toFixed(1)} s ago`;
  return `${Math.floor(ms / 60000)} min ago`;
}
