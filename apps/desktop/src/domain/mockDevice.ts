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
};

export const mockKeymap: KeymapDocument = currentFeiyu200Keymap();
