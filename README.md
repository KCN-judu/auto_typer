# Auto Typer

ESP32-S3 automatic typing machine workspace. The repository contains the firmware, Electron desktop controller, shared protocol types, Arduino packaging tools, a standalone hardware test sketch, and reference materials.

## Start Here

- Full operator tutorial: [`src/test/docs/auto_typer_flash_and_usage_guide.typ`](src/test/docs/auto_typer_flash_and_usage_guide.typ)
- Atomic motion protocol: [`shared/protocol/README.md`](shared/protocol/README.md)
- Standalone hardware test sketch: [`src/test/test/README.md`](src/test/test/README.md)

## Repository Layout

- `src/auto_typer/`: main ESP32-S3 firmware. It initializes the OLED and five Emm_V5.0 CAN motors, exposes SoftAP provisioning, and serves the TCP atomic-motion protocol.
- `apps/desktop/`: Electron + Vite + React controller for provisioning, TCP connection, printing, task recovery, and keymap inspection.
- `shared/protocol/`: TypeScript source of truth for the TCP NDJSON v1 contract. Firmware mirrors these JSON fields manually.
- `dist/arduino/AutoTyperCore/`: generated precompiled Arduino library for the supported ESP32-S3 target.
- `dist/arduino/AutoTyperWorkspace/`: generated macOS offline Arduino workspace with the toolchain, custom board profile, library, and starter sketch.
- `src/test/test/`: standalone OLED, PCA9685, continuous-rotation servo, and CAN motor connectivity sketch. It is not the production firmware entry point.
- `src/test/docs/`: Typst source and real UI screenshots for the firmware flashing and desktop usage tutorial.
- `materials/`: motor, PCB, enclosure, manual, and vendor reference material.

## Common Commands

```bash
npm run desktop:dev
npm run desktop:typecheck
npm run desktop:test
npm run desktop:build
npm run desktop:pack
npm run desktop:dist:mac
npm run desktop:dist:win

npm run firmware:test
npm run firmware:compile
npm run firmware:package
npm run firmware:package:verify
npm run firmware:package:upload
npm run firmware:workspace
npm run firmware:workspace:verify
```

## Current Runtime Model

### Boot and provisioning

1. Firmware starts SoftAP `wifi-setup` with password `admin123` and HTTP provisioning at `192.168.4.1`. There is no compile-time or sketch-injected Wi-Fi credential path.
2. Firmware initializes the OLED and CAN runtime, configures all five motors, clears their pulse positions, and verifies near-zero feedback.
3. The desktop sends target Wi-Fi credentials to `/api/provision`, polls `/api/status`, then calls `/api/finish` after the device reports its station IP.
4. `/api/finish` closes the SoftAP and releases TCP server startup on port `7777`.

Position clearing establishes the electrical pulse origin only. The operator must place the complete mechanism at its defined mechanical origin before every power-on.

### Motion control

- The desktop owns the keymap, text planning, coordinate conversion, cumulative position state, and block scheduling.
- The firmware accepts absolute pulse targets only. It does not accept relative wire commands or interpret text and keyboard coordinates from TCP.
- Physical mapping is M1=`x`, M2=`-y`, M3=`y`, M4=`l`, and M5=`z`.
- Line-feed home is the fixed absolute M4 sequence `16400 -> 10000`; press down/release uses M5 targets `-2700 -> 0`.
- An accepted block is supervised using scoped command ACK, input-pulse feedback, velocity feedback, and timeouts.

### Protocol lifecycle

```text
TCP connect
  -> handshake / handshake_ack
  -> get_snapshot / snapshot
  -> execute_block / block_ack
  -> block_result(done | failed | cancelled)
  -> ... next block ...
  -> finish_task / finish_task_result
```

`block_ack` is validation and execution-slot admission, not mechanical completion. Only the matching `block_result` is terminal for an accepted block.

Normal cancellation is cooperative: the desktop waits for the current block, discards unsent work, then submits absolute return blocks. Protocol `cancel` and disconnect only cancel a queued block. `emergency_stop` is the immediate interruption path and latches a fault until `reset_fault` restores CAN and motor readiness.

Known desktop limitation: fresh firmware reports `lineFeedPrimeRequired: true`, and printing is disabled until M4 completes `16400 -> 10000`. The controller hook implements `runLineFeedHome()`, but the current `App.tsx` does not expose it through a visible UI control. Do not document or assume a complete first-print UI flow until that control is wired and verified.

## Firmware Packaging

- `npm run firmware:compile`: compile the editable firmware tree with repo-local Arduino artifacts.
- `npm run firmware:package`: rebuild `dist/arduino/AutoTyperCore/` and its precompiled archive.
- `npm run firmware:package:verify`: compile the generated package as a consumer sketch.
- `npm run firmware:package:upload`: package, compile, and upload the provisioning-only example; append `-- --port=/dev/cu.usbmodem...` when auto-detection is unsuitable.
- `npm run firmware:workspace`: generate the macOS offline Arduino workspace. A local Arduino IDE installation is required.
- `npm run firmware:workspace:verify`: regenerate the workspace, verify that only the custom board is exposed, and compile the starter sketch.

Manual Arduino IDE settings for the packaged library:

- Board: `ESP32S3 Dev Module`
- Flash Size: `16MB (128Mb)`
- Flash Mode: `OPI 80MHz`
- PSRAM: `OPI PSRAM`
- Partition Scheme: `16M Flash (3MB APP/9.9MB FATFS)`

## Desktop Packaging

- `npm run desktop:pack`: build an unpacked application in `apps/desktop/release/`.
- `npm run desktop:dist:mac`: build macOS `.dmg` and `.zip` artifacts on macOS.
- `npm run desktop:dist:win`: build the Windows portable artifact on Windows.
- Packaging uses `electron-builder` and `apps/desktop/resources/`.
- Signing is intentionally disabled by default. Standard `CSC_*`, `APPLE_*`, and `WIN_CSC_*` variables can be supplied when certificates are available.

## Safety

CAN transmission and command ACK are not mechanical completion. Jams, stale feedback, bus faults, direction errors, or a mechanism moved away from its cleared origin can fail a block or make the physical machine disagree with the desktop's planned position. Always preserve a clear emergency-stop path and re-establish the mechanical origin after an abnormal stop.
