import type { DeviceStatus, KeymapDocument } from "../../../../shared/protocol/auto-typer-protocol";
import { currentFeiyu200Keymap } from "./keymap";

export const mockStatus: DeviceStatus = {
  deviceId: "esp32-s3-auto-typer",
  firmwareVersion: "0.1.0",
  ipAddress: "192.168.4.42",
  mode: "idle",
  health: "ok",
  wifiRssi: -48,
  servoReady: true,
  motionReady: true,
  keymapVersion: 1,
  currentJob: undefined,
  motors: [
    { id: 1, enabled: true, fault: false, moving: false, estimatedPositionSteps: 0, observedPositionSteps: 0, velocityRpm: 0, lastFeedbackMs: 0 },
    { id: 2, enabled: true, fault: false, moving: false, estimatedPositionSteps: 0, observedPositionSteps: 0, velocityRpm: 0, lastFeedbackMs: 0 },
    { id: 3, enabled: true, fault: false, moving: false, estimatedPositionSteps: 0, observedPositionSteps: 0, velocityRpm: 0, lastFeedbackMs: 0 },
    { id: 4, enabled: true, fault: false, moving: false, estimatedPositionSteps: 0, observedPositionSteps: 0, velocityRpm: 0, lastFeedbackMs: 0 },
  ],
};

export const mockKeymap: KeymapDocument = currentFeiyu200Keymap();
