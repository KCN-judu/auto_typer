import type {
  DeviceStatus,
  MotorRole,
  MotorState,
} from "../../../../shared/protocol/protocolTypes";
import type { ConnectionState } from "./appTypes";

export function connectionText(connection: ConnectionState) {
  switch (connection) {
    case "connected":
      return "ONLINE";
    case "connecting":
      return "CONNECT";
    case "desync":
      return "DESYNC";
    case "transport_fault":
      return "TRANSPORT";
    case "disconnected":
    default:
      return "OFFLINE";
  }
}

export function motorByRole(status: DeviceStatus, role: MotorRole): MotorState | undefined {
  return ensureMotorList(status.motors).find((motor) => motor.role === role);
}

export function clampInteger(value: number, minValue: number, maxValue: number): number {
  if (!Number.isFinite(value)) {
    return minValue;
  }
  return Math.min(Math.max(Math.round(value), minValue), maxValue);
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
  return motors.sort((a, b) => (a.id ?? 0) - (b.id ?? 0));
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
