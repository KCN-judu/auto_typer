export type DeviceMode = "idle" | "running" | "paused" | "faulted" | "debug";

export type DeviceHealth = "unknown" | "ok" | "not_ready" | "warning" | "fault";

export type MotorDirection = "cw" | "ccw";

export type MotorRole = "x" | "y_left" | "y_right" | "line_feed" | "press";

export const MAX_BLOCKS_PER_GROUP = 32;
export const MAX_TCP_MESSAGE_BYTES = 8192;
export const MIN_TELEMETRY_INTERVAL_MS = 50;
export const MAX_TELEMETRY_INTERVAL_MS = 2000;
export const MAX_GROUP_RUNTIME_MS = 30000;
export const MAX_BLOCK_TIMEOUT_MS = 10000;

export const TCP_COMMAND_TYPES = [
  "hello",
  "get_status",
  "subscribe_telemetry",
  "get_keymap",
  "get_wifi_status",
  "scan_wifi",
  "configure_wifi",
  "finish_wifi_setup",
  "probe",
  "press_diag_m5",
  "reset_fault",
  "release_line_feed_origin",
  "cancel",
  "finish_task",
  "exec_group",
  "ping",
] as const;

export const TCP_TERMINAL_RESPONSE_TYPES = [
  "hello_ack",
  "status",
  "telemetry_subscribed",
  "keymap",
  "wifi_status",
  "wifi_network",
  "wifi_networks",
  "wifi_config_result",
  "wifi_setup_finished",
  "probe_result",
  "press_diag_m5_result",
  "reset_fault_result",
  "release_line_feed_origin_result",
  "cancel_result",
  "finish_task_result",
  "group_accepted",
  "group_rejected",
  "protocol_error",
  "pong",
] as const;

export const MOTION_BLOCK_TYPES = [
  "move_xy",
  "press_down",
  "press_up",
  "character_release",
  "line_feed",
  "line_feed_home",
  "return_zero",
  "wait",
] as const;

export type TcpCommandType = (typeof TCP_COMMAND_TYPES)[number];
export type TcpTerminalResponseType = (typeof TCP_TERMINAL_RESPONSE_TYPES)[number];
export type MotionBlockType = (typeof MOTION_BLOCK_TYPES)[number];

export type MotorTelemetryRole = "X" | "YLeft" | "YRight" | "LineFeed" | "Press" | "Unknown";

export type MotorEventKind =
  | "ack"
  | "condition_not_met"
  | "malformed"
  | "motion_reached"
  | "velocity"
  | "input_pulse"
  | "realtime_angle"
  | "status_flags"
  | "unknown";

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
  pressReady: boolean;
  motionReady: boolean;
  lineFeedPrimeRequired: boolean;
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

export type MotorEventMessage = {
  v: 1;
  type: "motor_event";
  motorId: number;
  role: MotorTelemetryRole;
  eventKind: MotorEventKind;
  data: {
    command?: string;
    velocityRpm?: number;
    inputPulseSteps?: number;
    angleRaw?: number;
    angleDeg?: number;
    statusFlags?: number;
  };
  severity?: "info" | "warning" | "fault";
  timestampMs: number;
};

export type MotorStateUpdateMessage = {
  v: 1;
  type: "motor_state_update";
  motors: Array<{
    motorId: number;
    role: MotorTelemetryRole;
    readiness: MotorReadiness;
    hasVelocity: boolean;
    velocityRpm?: number;
    hasInputPulse: boolean;
    inputPulseSteps?: number;
    hasRealtimeAngle: boolean;
    angleRaw?: number;
    angleDeg?: number;
    hasStatusFlags: boolean;
    statusFlags?: number;
    lastUpdatedAtMs: number;
  }>;
  timestampMs: number;
};

export type TelemetryOverflowMessage = {
  v: 1;
  type: "telemetry_overflow";
  code: "telemetry_overflow";
  message: string;
  timestampMs: number;
};

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
  command: number;
  status: number;
  packetIndex: number;
  motionContext?: {
    groupId: string;
    seq: number;
    blockIndex: number;
    blockKind: string;
  };
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

export type WifiPhase = "connected" | "connecting" | "no_credentials" | "failed" | "idle";

export type WifiEncryption =
  | "open"
  | "wep"
  | "wpa"
  | "wpa2"
  | "wpa_wpa2"
  | "wpa2_enterprise"
  | "wpa3"
  | "wpa2_wpa3"
  | "unknown";

export interface WifiStatus {
  setupApActive: boolean;
  setupSsid: string;
  setupPassword: string;
  setupIpAddress: string;
  staConnected: boolean;
  staConnecting: boolean;
  staSsid: string;
  ipAddress: string;
  wifiRssi: number;
  savedCredentials: boolean;
  phase: WifiPhase;
  lastError?: string;
}

export interface WifiNetwork {
  ssid: string;
  rssi: number;
  channel: number;
  encryption: WifiEncryption;
}

export type RemoteMotionProfile = {
  rpm: number;
  accelRaw: number;
  timeoutMs: number;
};

export type MotionBlock =
  | ({ type: "move_xy"; dxSteps: number; dySteps: number } & RemoteMotionProfile)
  | ({ type: "press_down" } & RemoteMotionProfile)
  | ({ type: "press_up" } & RemoteMotionProfile)
  | ({ type: "character_release" } & RemoteMotionProfile)
  | ({ type: "line_feed"; lines: number } & RemoteMotionProfile)
  | ({ type: "line_feed_home" } & RemoteMotionProfile)
  | ({ type: "return_zero" } & RemoteMotionProfile)
  | { type: "wait"; durationMs: number };

export type TaskGroupPolicy = {
  maxRuntimeMs: number;
  onDisconnect: "cancel";
};

export type TaskGroup = {
  groupId: string;
  seq: number;
  policy: TaskGroupPolicy;
  blocks: MotionBlock[];
};

export type HelloMessage = {
  v: 1;
  requestId: string;
  type: "hello";
};

export type GetStatusMessage = {
  v: 1;
  requestId: string;
  type: "get_status";
};

export type SubscribeTelemetryMessage = {
  v: 1;
  requestId: string;
  type: "subscribe_telemetry";
  intervalMs: number;
};

export type GetKeymapMessage = {
  v: 1;
  requestId: string;
  type: "get_keymap";
};

export type GetWifiStatusMessage = {
  v: 1;
  requestId: string;
  type: "get_wifi_status";
};

export type ScanWifiMessage = {
  v: 1;
  requestId: string;
  type: "scan_wifi";
};

export type ConfigureWifiMessage = {
  v: 1;
  requestId: string;
  type: "configure_wifi";
  ssid: string;
  password: string;
};

export type FinishWifiSetupMessage = {
  v: 1;
  requestId: string;
  type: "finish_wifi_setup";
};

export type ExecGroupMessage = {
  v: 1;
  requestId: string;
  type: "exec_group";
  groupId: string;
  seq: number;
  policy: TaskGroupPolicy;
  blocks: MotionBlock[];
};

export type CancelMessage = {
  v: 1;
  requestId: string;
  type: "cancel";
  groupId?: string;
  seq?: number;
};

export type FinishTaskMessage = {
  v: 1;
  requestId: string;
  type: "finish_task";
};

export type ResetFaultMessage = {
  v: 1;
  requestId: string;
  type: "reset_fault";
};

export type ReleaseLineFeedOriginMessage = {
  v: 1;
  requestId: string;
  type: "release_line_feed_origin";
};

export type ProbeMessage = {
  v: 1;
  requestId: string;
  type: "probe";
};

export type PressDiagM5Message = {
  v: 1;
  requestId: string;
  type: "press_diag_m5";
};

export type PingMessage = {
  v: 1;
  requestId: string;
  type: "ping";
};

export type HelloAckMessage = {
  v: 1;
  type: "hello_ack";
  requestId: string;
  device: "auto_typer";
  protocol: "tcp_ndjson";
  capabilities: string[];
  limits: {
    maxBlocksPerGroup: number;
    maxMessageBytes: number;
    maxGroupRuntimeMs: number;
  };
};

export type StatusMessage = {
  v: 1;
  type: "status";
  requestId: string;
  status: DeviceStatus;
};

export type TelemetrySubscribedMessage = {
  v: 1;
  type: "telemetry_subscribed";
  requestId: string;
  intervalMs: number;
};

export type CoordinateSystem = {
  origin: "bottom_left";
  xPositive: "right";
  yPositive: "up";
  xMotorPositiveDirection: "CW";
  yMotorPositiveDirection: "CCW";
};

export type KeymapMessage = {
  v: 1;
  type: "keymap";
  requestId: string;
  coordinateSystem: CoordinateSystem;
  keys: Array<{ label: string; xMm: number; yMm: number }>;
};

export type WifiStatusMessage = {
  v: 1;
  type: "wifi_status";
  requestId: string;
  wifi: WifiStatus;
};

export type WifiNetworkMessage = {
  v: 1;
  type: "wifi_network";
  requestId: string;
  network: WifiNetwork;
};

export type WifiNetworksMessage = {
  v: 1;
  type: "wifi_networks";
  requestId: string;
  ok: boolean;
  networks: WifiNetwork[];
  code?: string;
  message?: string;
};

export type WifiConfigResultMessage = {
  v: 1;
  type: "wifi_config_result";
  requestId: string;
  ok: boolean;
  message: string;
  code?: string;
  wifi: WifiStatus;
};

export type WifiSetupFinishedMessage = {
  v: 1;
  type: "wifi_setup_finished";
  requestId: string;
  ok: boolean;
  message: string;
  code?: string;
  wifi: WifiStatus;
};

export type ProbeResultMessage = {
  v: 1;
  type: "probe_result";
  requestId: string;
  ok: boolean;
  motors: MotorState[];
};

export type PressDiagM5ResultMessage = {
  v: 1;
  type: "press_diag_m5_result";
  requestId: string;
  ok: boolean;
  code: string;
  message: string;
  initialPulse: number;
  downTargetPulse: number;
  downPulse: number;
  upTargetPulse: number;
  finalPulse: number;
  downAckSeen: boolean;
  downReachedSeen: boolean;
  upAckSeen: boolean;
  upReachedSeen: boolean;
  traceCount: number;
};

export type ResetFaultResultMessage = {
  v: 1;
  type: "reset_fault_result";
  requestId: string;
  ok: boolean;
  status?: DeviceStatus;
};

export type ReleaseLineFeedOriginResultMessage = {
  v: 1;
  type: "release_line_feed_origin_result";
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

export type GroupRejectReason =
  | "invalid_json"
  | "invalid_group"
  | "invalid_block"
  | "group_too_large"
  | "device_busy"
  | "device_fault"
  | "line_feed_prime_required"
  | "motion_transport_not_ready"
  | "queue_full";

export type GroupAcceptedMessage = {
  v: 1;
  type: "group_accepted";
  requestId: string;
  groupId: string;
  seq: number;
  blockCount: number;
};

export type GroupRejectedMessage = {
  v: 1;
  type: "group_rejected";
  requestId: string;
  groupId?: string;
  seq?: number;
  reason: GroupRejectReason;
  message: string;
};

export type GroupStartedMessage = {
  v: 1;
  type: "group_started";
  groupId: string;
  seq: number;
};

export type BlockStartedMessage = {
  v: 1;
  type: "block_started";
  groupId: string;
  seq: number;
  blockIndex: number;
  blockType: MotionBlock["type"];
};

export type BlockDoneMessage = {
  v: 1;
  type: "block_done";
  groupId: string;
  seq: number;
  blockIndex: number;
  blockType: MotionBlock["type"];
};

export type GroupDoneMessage = {
  v: 1;
  type: "group_done";
  groupId: string;
  seq: number;
  ok: boolean;
  durationMs?: number;
  currentPoint?: MachinePointMm;
};

export type GroupFinalStatus = "done" | "failed" | "cancelled";

export type GroupFinalMessage = {
  v: 1;
  type: "group_final";
  groupId: string;
  seq: number;
  status: GroupFinalStatus;
  code?: string;
  message?: string;
  durationMs?: number;
};

export type GroupFaultMessage = {
  v: 1;
  type: "fault";
  requestId?: string;
  groupId?: string;
  seq?: number;
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

export type TelemetryMessage = {
  v: 1;
  type: "telemetry";
  status: DeviceStatus;
};

export type PongMessage = {
  v: 1;
  type: "pong";
  requestId?: string;
};

export type GroupStreamCommandMessage =
  | HelloMessage
  | GetStatusMessage
  | SubscribeTelemetryMessage
  | GetKeymapMessage
  | GetWifiStatusMessage
  | ScanWifiMessage
  | ConfigureWifiMessage
  | FinishWifiSetupMessage
  | ExecGroupMessage
  | CancelMessage
  | FinishTaskMessage
  | ResetFaultMessage
  | ReleaseLineFeedOriginMessage
  | ProbeMessage
  | PressDiagM5Message
  | PingMessage;

export type GroupStreamEventMessage =
  | HelloAckMessage
  | StatusMessage
  | TelemetrySubscribedMessage
  | KeymapMessage
  | WifiStatusMessage
  | WifiNetworkMessage
  | WifiNetworksMessage
  | WifiConfigResultMessage
  | WifiSetupFinishedMessage
  | ProbeResultMessage
  | PressDiagM5ResultMessage
  | ResetFaultResultMessage
  | ReleaseLineFeedOriginResultMessage
  | CancelResultMessage
  | FinishTaskResultMessage
  | GroupAcceptedMessage
  | GroupRejectedMessage
  | GroupStartedMessage
  | BlockStartedMessage
  | BlockDoneMessage
  | GroupDoneMessage
  | GroupFinalMessage
  | GroupFaultMessage
  | ProtocolErrorMessage
  | TelemetryMessage
  | MotorEventMessage
  | MotorStateUpdateMessage
  | TelemetryOverflowMessage
  | PongMessage;

export type GroupStreamMessage =
  | GroupStreamCommandMessage
  | GroupStreamEventMessage;
