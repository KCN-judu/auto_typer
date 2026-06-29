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
}

export interface DeviceFault {
  code: string;
  message: string;
  recoverable: boolean;
}

export interface CreateJobRequest {
  text: string;
  options?: {
    dryRun?: boolean;
    startAtHome?: boolean;
  };
}

export interface CreateJobResponse {
  jobId: string;
  accepted: boolean;
  planStatus: "ok" | "key_not_found" | "plan_full";
  failedKey?: string;
  stepCount: number;
}

export interface JobStatus {
  jobId: string;
  state: "queued" | "running" | "completed" | "cancelled" | "failed";
  textLength: number;
  currentIndex: number;
  currentStep: number;
  totalSteps: number;
  currentPoint: MachinePointMm;
  message?: string;
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
  keymap: "/api/keymap",
  events: "/api/events",
  debugMotorMoveRelative: "/api/debug/motor/move-relative",
  debugMotorEnable: "/api/debug/motor/enable",
  debugMotorStop: "/api/debug/motor/stop",
  debugServoApply: "/api/debug/servo/apply",
  debugProbeKey: "/api/debug/probe-key",
} as const;
