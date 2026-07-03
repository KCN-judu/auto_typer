# Auto Typer

ESP32-S3 automatic typing machine workspace. The repository contains the firmware, Electron desktop controller, shared TypeScript protocol types, hardware test sketches, and reference materials.

## Repository Layout

- `src/auto_typer/`: main ESP32-S3 firmware. It controls the OLED, PCA9685 servo driver, Emm_V5.0 CAN motor drivers, HTTP API, and TCP group command stream.
- `apps/desktop/`: Electron + Vite + React desktop controller for connecting to the device, creating print tasks, debugging actuators, and maintaining keymaps.
- `shared/protocol/`: shared TypeScript protocol contracts used by the desktop app. The ESP32 firmware mirrors these JSON field names manually.
- `src/test/test/`: standalone Arduino connectivity test sketch for OLED, PCA9685, servo, and CAN motor checks.
- `src/test/docs/`: Typst hardware connectivity test documentation.
- `materials/`: motor, PCB, enclosure, manual, and vendor reference material.

## Common Commands

Commands in this workspace should be run through `rtk`.

```bash
rtk npm run desktop:dev
rtk npm run desktop:build
rtk npm run desktop:typecheck
rtk npm run firmware:test
```

## Firmware Entry Points

- `src/auto_typer/auto_typer.ino`: Arduino lifecycle entry. `setup()` initializes the app, HTTP server, and TCP group command server. `loop()` ticks HTTP, group commands, and the app runtime.
- `src/auto_typer/auto_typer_config.h`: default hardware, Wi-Fi, CAN, servo, motion, and machine topology configuration.
- `src/auto_typer/auto_typer_types.h`: core enums and data structures for job state, device mode, motion profiles, motor feedback, and remote groups.
- `src/auto_typer/typing_logic.h`: pure planning logic that maps text and keymaps to typing plans.
- `src/auto_typer/auto_typer_runtime.h`: application state machine and action execution.
- `src/auto_typer/http_control_server.h`: HTTP JSON API and Wi-Fi setup handlers.
- `src/auto_typer/transport/GroupCommandServer.h`: NDJSON-over-TCP command stream on port `7777`.
- `src/auto_typer/can/`: CAN bus, TX queue, RX task, motor feedback store, EMM_V5 event parsing, and protocol trace support.

## Control Surfaces

The firmware exposes two control surfaces:

- HTTP JSON on port `80` for status, jobs, keymap updates, Wi-Fi setup, diagnostics, emergency stop, fault reset, motor probing, and debug actuator commands.
- NDJSON TCP on port `7777` for desktop-driven remote motion groups. A client sends `hello` first, then may send `exec_group`, `task_end`, `cancel`, `reset_fault`, `probe`, or `ping` messages. The firmware responds with `ack`, `snapshot`, `telemetry`, `group_started`, `group_done`, `group_warn`, `fault`, and `pong` events.

The protocol route and message types live in `shared/protocol/auto-typer-protocol.ts`.

## Safety Notes

CAN transmit success is not the same as mechanical completion. The current firmware has motor feedback, CAN diagnostics, and protocol trace support, but motion execution still relies heavily on local estimates, cached motor feedback, and timeout windows. Lost steps, jams, missed sync triggers, stale feedback, or insufficient wait estimates can still make software state diverge from the machine.

任务执行中仍有阻塞式动作和等待；不要把 HTTP 返回成功直接理解为真实机械动作已经完成。
