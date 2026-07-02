export type DeviceMode = "idle" | "running" | "paused" | "faulted" | "debug";

export type DeviceHealth = "unknown" | "ok" | "not_ready" | "warning" | "fault";

export type MotorDirection = "cw" | "ccw";

export type ServoCommand = "press" | "release" | "neutral";

export type MotorRole = "x" | "y_left" | "y_right" | "line_feed";

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

export type PrimitiveCommandProfile = {
  rpm?: number;
  accelRaw?: number;
  timeoutMs?: number;
};

export type PrimitiveCommand =
  | {
      v: 1;
      type: "command";
      id: string;
      op: "move_to";
      xMm: number;
      yMm: number;
      profile?: PrimitiveCommandProfile;
    }
  | {
      v: 1;
      type: "command";
      id: string;
      op: "wait";
      durationMs: number;
    }
  | {
      v: 1;
      type: "command";
      id: string;
      op: "press" | "release";
      durationMs?: number;
    }
  | {
      v: 1;
      type: "command";
      id: string;
      op: "character_release" | "line_feed" | "cancel" | "reset_fault" | "emergency_stop";
    };

export type HelloMessage = {
  v: 1;
  type: "hello";
  client: "desktop";
};

export type HelloAckMessage = {
  v: 1;
  type: "hello_ack";
  snapshot: LinkSnapshot;
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

export type DoneMessage = {
  v: 1;
  type: "done";
  id: string;
  op?: PrimitiveCommand["op"];
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
  currentCommandId?: string;
  lastCompletedCommandId?: string;
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
  currentCommandId?: string;
  lastCompletedCommandId?: string;
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
  type: "ping";
};

export type PongMessage = {
  v: 1;
  type: "pong";
};

export type BlockStreamCommandMessage = HelloMessage | PrimitiveCommand | PingMessage;

export type BlockStreamEventMessage =
  | AckMessage
  | HelloAckMessage
  | DoneMessage
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
