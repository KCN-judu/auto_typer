export type DeviceMode = "idle" | "running" | "paused" | "faulted" | "debug";

export type DeviceHealth = "unknown" | "ok" | "not_ready" | "warning" | "fault";

export type MotorRole = "x" | "y_left" | "y_right" | "line_feed" | "press";

export type MotorDisplayRole = "X" | "YLeft" | "YRight" | "LineFeed" | "Press";

export type MotorReadiness = "unknown" | "ready" | "not_ready" | "fault";

export const MAX_TCP_MESSAGE_BYTES = 8192;
export const MAX_BLOCK_RUNTIME_MS = 30000;
export const MAX_ACTION_TIMEOUT_MS = 10000;

export const MOTION_PROTOCOL_COMMAND_TYPES = [
  "handshake",
  "get_snapshot",
  "execute_block",
  "cancel",
  "finish_task",
  "emergency_stop",
  "reset_fault",
  "heartbeat",
] as const;

export const MOTION_PROTOCOL_RESPONSE_TYPES = [
  "handshake_ack",
  "snapshot",
  "block_ack",
  "cancel_result",
  "finish_task_result",
  "emergency_stop_result",
  "reset_fault_result",
  "protocol_error",
  "heartbeat_ack",
] as const;

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
  pressReady: boolean;
  motionReady: boolean;
  lineFeedPrimeRequired: boolean;
  keymapVersion: number;
  canDiagnostics?: CanDiagnostics;
  motors?: MotorState[];
  currentJob?: JobStatus;
  fault?: DeviceFault;
}

export interface DeviceFault {
  code: string;
  message: string;
  recoverable: boolean;
}

export interface JobStatus {
  jobId?: string;
  state: "none" | "queued" | "planning" | "running" | "cancelling" | "completed" | "cancelled" | "failed";
  textLength: number;
  currentIndex: number;
  currentStep: number;
  totalSteps: number;
  currentPoint: MachinePointMm;
  message?: string;
}

export interface CanDiagnostics {
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
  lastTxError: number;
  lastCommandQueueError: number;
  lastFaultCode: string;
  lastFaultMessage: string;
}

export interface MotorState {
  id?: number;
  motorId?: number;
  role: MotorRole | MotorDisplayRole;
  readiness?: MotorReadiness;
  hasVelocity: boolean;
  hasRealtimeAngle: boolean;
  hasInputPulse: boolean;
  hasStatus?: boolean;
  hasStatusFlags?: boolean;
  hasRecentStatus?: boolean;
  hasRecentInputPulse?: boolean;
  hasRecentVelocity?: boolean;
  velocityRpm?: number;
  realtimeAngleRaw65536?: number;
  angleRaw?: number;
  angleDeg?: number;
  inputPulseSteps?: number;
  statusFlags?: number;
  driverFault?: boolean;
  conditionNotMet?: boolean;
  commandMalformed?: boolean;
  lastAckCommand?: number;
  lastConditionNotMetCommand?: number;
  lastMalformedCommand?: number;
  lastAckMs?: number;
  lastConditionNotMetMs?: number;
  lastMalformedMs?: number;
  motionReached?: boolean;
  lastMotionReachedMs?: number;
  lastVelocityMs?: number;
  lastRealtimeAngleMs?: number;
  lastInputPulseMs?: number;
  lastStatusMs?: number;
  lastAnyFrameMs?: number;
  lastProbeMs?: number;
  lastErrorCode?: string;
  lastErrorMessage?: string;
  lastUpdatedAtMs?: number;
}

export type RemoteMotionProfile = {
  rpm: number;
  accelRaw: number;
  timeoutMs: number;
};

export type RemoteMotorMove = RemoteMotionProfile & {
  type: "motor_move";
  motorId: number;
  target: number;
};

export type RemoteWaitAction = {
  type: "wait";
  durationMs: number;
};

export type MotionAction = RemoteMotorMove | RemoteWaitAction;
export type AtomicMotionBlock = [MotionAction, ...MotionAction[]];
export type AtomicMotionBlockKind = MotionAction["type"] | "parallel";

export type MotionBlockPolicy = {
  maxRuntimeMs: number;
  onDisconnect: "cancel";
};

export type MotionBlockRequest = {
  blockId: string;
  seq: number;
  policy: MotionBlockPolicy;
  block: AtomicMotionBlock;
};

export type HandshakeMessage = {
  v: 1;
  requestId: string;
  type: "handshake";
};

export type GetSnapshotMessage = {
  v: 1;
  requestId: string;
  type: "get_snapshot";
};

export type ExecuteBlockMessage = MotionBlockRequest & {
  v: 1;
  requestId: string;
  type: "execute_block";
};

export type CancelMessage = {
  v: 1;
  requestId: string;
  type: "cancel";
  blockId?: string;
  seq?: number;
};

export type FinishTaskMessage = {
  v: 1;
  requestId: string;
  type: "finish_task";
};

export type EmergencyStopMessage = {
  v: 1;
  requestId: string;
  type: "emergency_stop";
};

export type ResetFaultMessage = {
  v: 1;
  requestId: string;
  type: "reset_fault";
};

export type HeartbeatMessage = {
  v: 1;
  requestId: string;
  type: "heartbeat";
};

export type HandshakeAckMessage = {
  v: 1;
  type: "handshake_ack";
  requestId: string;
  device: "auto_typer";
  protocol: "tcp_ndjson";
  capabilities: string[];
  limits: {
    maxMessageBytes: number;
    maxBlockRuntimeMs: number;
    maxActionTimeoutMs: number;
  };
};

export type SnapshotMessage = {
  v: 1;
  type: "snapshot";
  requestId: string;
  status: DeviceStatus;
};

export type ResetFaultResultMessage = {
  v: 1;
  type: "reset_fault_result";
  requestId: string;
  ok: boolean;
  status?: DeviceStatus;
};

export type CancelResultMessage = {
  v: 1;
  type: "cancel_result";
  requestId: string;
  ok: boolean;
};

export type FinishTaskResultMessage = {
  v: 1;
  type: "finish_task_result";
  requestId: string;
  ok: boolean;
};

export type EmergencyStopResultMessage = {
  v: 1;
  type: "emergency_stop_result";
  requestId: string;
  ok: boolean;
  status?: DeviceStatus;
};

export type BlockAckMessage = {
  v: 1;
  type: "block_ack";
  requestId: string;
  blockId: string;
  seq: number;
};

export type BlockResultStatus = "done" | "failed" | "cancelled";

export type BlockResultMessage = {
  v: 1;
  type: "block_result";
  blockId: string;
  seq: number;
  status: BlockResultStatus;
  code?: string;
  message?: string;
  durationMs?: number;
};

export type DeviceFaultMessage = {
  v: 1;
  type: "fault";
  requestId?: string;
  code: string;
  message: string;
};

export type ProtocolErrorMessage = {
  v: 1;
  type: "protocol_error";
  requestId?: string;
  code: string;
  message: string;
};

export type HeartbeatAckMessage = {
  v: 1;
  type: "heartbeat_ack";
  requestId: string;
};

export type MotionProtocolCommandMessage =
  | HandshakeMessage
  | GetSnapshotMessage
  | ExecuteBlockMessage
  | CancelMessage
  | FinishTaskMessage
  | EmergencyStopMessage
  | ResetFaultMessage
  | HeartbeatMessage;

export type MotionProtocolEventMessage =
  | HandshakeAckMessage
  | SnapshotMessage
  | ResetFaultResultMessage
  | CancelResultMessage
  | FinishTaskResultMessage
  | EmergencyStopResultMessage
  | BlockAckMessage
  | BlockResultMessage
  | DeviceFaultMessage
  | ProtocolErrorMessage
  | HeartbeatAckMessage;

export type MotionProtocolMessage = MotionProtocolCommandMessage | MotionProtocolEventMessage;
