export type DeviceMode = "idle" | "running" | "paused" | "faulted" | "debug";

export type DeviceHealth = "unknown" | "ok" | "not_ready" | "warning" | "fault";

export type MotorDirection = "cw" | "ccw";

export type ServoCommand = "press" | "release" | "neutral";

export type MotorRole = "x" | "y_left" | "y_right" | "line_feed" | "press";

export type MotorReadiness =
  | "unknown"
  | "config_pending"
  | "config_sent"
  | "acked"
  | "ready"
  | "offline"
  | "condition_not_met"
  | "faulted";

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
  txRetryCount: number;
  commandQueueFullCount: number;
  busErrorCount: number;
  rxQueueFullCount: number;
  errPassiveCount: number;
  busOffCount: number;
  pendingFrameValid: boolean;
  lastAlertAtMs: number;
  lastFaultAtMs: number;
  lastTxError: string;
  lastCommandQueueError: string;
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
  role: MotorRole;
  readiness: MotorReadiness;
  hasVelocity: boolean;
  hasRealtimeAngle: boolean;
  hasInputPulse: boolean;
  hasStatus: boolean;
  hasRecentStatus: boolean;
  hasRecentInputPulse: boolean;
  hasRecentVelocity: boolean;
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
  lastProbeMs: number;
  lastErrorCode: string;
  lastErrorMessage: string;
}

export interface ProtocolTraceItem {
  timeMs: number;
  dir: "tx_queued" | "tx_sent" | "tx_retry" | "rx";
  canId: number;
  extd: boolean;
  dlc: number;
  data: number[];
  dataHex: string;
  parsed: string;
  motorId: number;
  packetIndex: number;
}

export interface ProtocolTraceResponse {
  trace: ProtocolTraceItem[];
  diagnostics: {
    unknownFrameCount: number;
    invalidFrameCount: number;
    lastEventAtMs: number;
    lastInvalidAtMs: number;
    lastInvalidError: string;
    lastEventKind: string;
  };
}

export type RemoteMotionProfile = {
  rpm?: number;
  accelRaw?: number;
  timeoutMs?: number;
};

export type RemoteMotionBlock =
  | { kind: "move_xy"; dxMm: number; dyMm: number; profile?: RemoteMotionProfile }
  | { kind: "servo_press" }
  | { kind: "servo_release" }
  | { kind: "character_release" }
  | { kind: "line_feed" }
  | { kind: "wait"; durationMs: number };

export type HelloMessage = {
  v: 1;
  id: string;
  type: "hello";
  client: "desktop";
};

export type ExecBlockMessage = {
  v: 1;
  id: string;
  type: "exec_block";
  blockId: string;
  block: RemoteMotionBlock;
};

export type CancelMessage = {
  v: 1;
  id: string;
  type: "cancel";
};

export type ResetFaultMessage = {
  v: 1;
  id: string;
  type: "reset_fault";
};

export type ProbeMessage = {
  v: 1;
  id: string;
  type: "probe";
};

export type AckMessage = {
  v: 1;
  type: "ack";
  id: string;
  ok: boolean;
  accepted: boolean;
  code?: string;
  message?: string;
};

export type BlockStartedMessage = {
  v: 1;
  type: "block_started";
  blockId: string;
};

export type BlockDoneMessage = {
  v: 1;
  type: "block_done";
  blockId: string;
  durationMs?: number;
  currentPoint?: MachinePointMm;
};

export type BlockFaultMessage = {
  v: 1;
  type: "fault";
  id?: string;
  code: string;
  message: string;
};

export type LinkSnapshot = {
  mode: DeviceMode;
  currentBlockId?: string;
  lastCompletedBlockId?: string;
  currentPoint: MachinePointMm;
  fault?: {
    code: string;
    message: string;
  };
};

export type TelemetryMessage = {
  v: 1;
  type: "telemetry";
  executor: "idle" | "running" | "faulted";
  jobState: JobStatus["state"];
  currentBlockId?: string;
  lastCompletedBlockId?: string;
  currentPoint: MachinePointMm;
  fault?: {
    code: string;
    message: string;
  };
  can?: Partial<CanBusDiagnostics>;
  motors?: Array<{
    id: number;
    role: MotorRole;
    rpm: number;
    inputPulse: number;
    angle: number;
    fresh: boolean;
  }>;
};

export type SnapshotMessage = {
  v: 1;
  type: "snapshot";
  snapshot: LinkSnapshot;
};

export type PingMessage = {
  v: 1;
  id: string;
  type: "ping";
};

export type PongMessage = {
  v: 1;
  type: "pong";
  id?: string;
};

export type BlockStreamCommandMessage =
  | HelloMessage
  | ExecBlockMessage
  | CancelMessage
  | ResetFaultMessage
  | ProbeMessage
  | PingMessage;

export type BlockStreamEventMessage =
  | AckMessage
  | BlockStartedMessage
  | BlockDoneMessage
  | BlockFaultMessage
  | TelemetryMessage
  | SnapshotMessage
  | PongMessage;

export type BlockStreamMessage =
  | BlockStreamCommandMessage
  | BlockStreamEventMessage;

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
  probeMotors: "/api/machine/probe-motors",
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
