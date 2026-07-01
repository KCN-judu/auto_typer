export type DeviceMode = "idle" | "running" | "paused" | "faulted" | "debug";

export type DeviceHealth = "unknown" | "ok" | "warning" | "fault";

export type MotorDirection = "cw" | "ccw";

export type ServoCommand = "press" | "release" | "neutral";

export interface MachinePointMm {
  xMm: number;
  yMm: number;
}

export interface KeyBinding {
  key: string;
  point: MachinePointMm;
}

export interface KeymapDocument {
  version: number;
  machine: "feiyu200";
  updatedAt: string;
  bindings: KeyBinding[];
}

export interface DeviceStatus {
  deviceId: string;
  firmwareVersion: string;
  ipAddress: string;
  mode: DeviceMode;
  health: DeviceHealth;
  wifiRssi: number;
  servoReady: boolean;
  motionReady: boolean;
  keymapVersion: number;
  currentJob?: JobStatus;
  fault?: DeviceFault;
  canDiagnostics?: CanBusDiagnostics;
  motors?: MotorState[];
}

export interface DeviceFault {
  code: string;
  message: string;
  recoverable: boolean;
}

export interface CanBusDiagnostics {
  driverReady: boolean;
  motionReady: boolean;
  fatalFault: boolean;
  recoverable: boolean;
  lastAlerts: number;
  txFailedCount: number;
  busErrorCount: number;
  rxQueueFullCount: number;
  errPassiveCount: number;
  busOffCount: number;
  lastAlertAtMs: number;
  lastFaultAtMs: number;
  lastTxError: string;
  lastFaultCode: string;
  lastFaultMessage: string;
}

export interface CreateJobRequest {
  text: string;
  options?: {
    dryRun?: boolean;
    startAtHome?: boolean;
  };
}

export interface CreateJobResponse {
  jobId?: string;
  accepted: boolean;
  planStatus: "ok" | "key_not_found" | "plan_full" | "device_fault" | "device_busy" | "device_not_ready";
  rejectionCode?: string;
  rejectionMessage?: string;
  fault?: DeviceFault;
  failedKey?: string;
  stepCount: number;
}

export interface JobStatus {
  jobId?: string;
  state: "none" | "queued" | "planning" | "running" | "cancelling" | "completed" | "cancelled" | "failed";
  textLength: number;
  currentIndex: number;
  currentStep: number;
  totalSteps: number;
  currentBlock?: number;
  totalBlocks?: number;
  currentPoint: MachinePointMm;
  message?: string;
}

export interface MotorState {
  id: number;
  hasVelocity: boolean;
  hasRealtimeAngle: boolean;
  hasInputPulse: boolean;
  hasStatus: boolean;
  velocityRpm: number;
  realtimeAngleRaw65536: number;
  inputPulseSteps: number;
  statusFlags: number;
  driverFault: boolean;
  conditionNotMet: boolean;
  commandMalformed: boolean;
  lastAckCommand: number;
  lastConditionNotMetCommand: number;
  lastMalformedCommand: number;
  lastAckMs: number;
  lastConditionNotMetMs: number;
  lastMalformedMs: number;
  motionReached: boolean;
  lastMotionReachedMs: number;
  lastVelocityMs: number;
  lastRealtimeAngleMs: number;
  lastInputPulseMs: number;
  lastStatusMs: number;
  lastAnyFrameMs: number;
}

export interface ProtocolTraceItem {
  timeMs: number;
  dir: "tx" | "rx";
  canId: number;
  extd: boolean;
  dlc: number;
  data: number[];
  parsed: string;
  motorId: number;
  packetIndex: number;
}

export interface ProtocolTraceResponse {
  trace: ProtocolTraceItem[];
}

export interface ApiErrorBody {
  code: string;
  message: string;
  details?: unknown;
}

export interface MotorMoveRelativeRequest {
  /** Motor id, or 23 for the paired Y-axis motor group 2+3. */
  motorId: number;
  direction: MotorDirection;
  rpm: number;
  acceleration: number;
  steps: number;
  sync: boolean;
}

export interface MotorEnableRequest {
  /** Motor id, or 23 for the paired Y-axis motor group 2+3. */
  motorId: number;
  enabled: boolean;
  sync?: boolean;
}

export interface MotorStopRequest {
  /** Motor id, or 23 for the paired Y-axis motor group 2+3. */
  motorId: number;
  sync?: boolean;
}

export interface ServoApplyRequest {
  command: ServoCommand;
  durationMs?: number;
}

export interface ProbeKeyRequest {
  key: string;
  point: MachinePointMm;
}

export type DeviceEvent =
  | { type: "status"; status: DeviceStatus }
  | { type: "job_progress"; job: JobStatus }
  | { type: "log"; level: "info" | "warning" | "error"; message: string; at: string }
  | { type: "fault"; fault: DeviceFault }
  | { type: "keymap_updated"; keymapVersion: number };

export const protocolRoutes = {
  status: "/api/status",
  jobs: "/api/jobs",
  currentJob: "/api/jobs/current",
  cancelJob: "/api/jobs/current/cancel",
  machineStop: "/api/machine/stop",
  resetFault: "/api/machine/reset-fault",
  canDiagnostics: "/api/diagnostics/can",
  protocolTrace: "/api/diagnostics/protocol-trace",
  keymap: "/api/keymap",
  events: "/api/events",
  debugMotorMoveRelative: "/api/debug/motor/move-relative",
  debugMotorEnable: "/api/debug/motor/enable",
  debugMotorStop: "/api/debug/motor/stop",
  debugServoApply: "/api/debug/servo/apply",
  debugProbeKey: "/api/debug/probe-key",
} as const;
