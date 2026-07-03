# Auto Typer LAN Protocol

This directory defines the JSON contract between the Electron controller and the ESP32 firmware.

The TypeScript file is the source of truth for the desktop application. The ESP32 implementation mirrors the same field names in its HTTP handlers.

## Transport

- HTTP JSON for commands and queries.
- NDJSON-over-TCP on port `7777` for streaming remote motion groups and telemetry.

`protocolRoutes.events` currently reserves `/api/events`, and `DeviceEvent` describes the intended SSE payload shape, but the firmware does not register an HTTP SSE route yet.

## Authority

- The ESP32 is the authority for the active keymap.
- The desktop application may cache and export keymaps, but writes complete keymap documents back to the ESP32.

## HTTP Routes

- `GET /api/status`: device status, optional current job, CAN diagnostics, and motor feedback summary.
- `POST /api/jobs`: submit a firmware-planned text job. The firmware currently reads `text`; `options.dryRun` and `options.startAtHome` are protocol fields but are not implemented by the firmware job planner.
- `GET /api/jobs/current`: current job snapshot, or `currentJob: null` when no job is active.
- `POST /api/jobs/current/cancel`: cancel a queued or running job.
- `POST /api/machine/stop`: emergency stop and enter faulted mode.
- `POST /api/machine/reset-fault`: clear recoverable fault state and refresh motor readiness.
- `POST /api/machine/probe-motors`: request a best-effort motor feedback probe when the device is not busy.
- `GET /api/diagnostics/can`: CAN driver, queue, alert, and fault counters.
- `GET /api/diagnostics/protocol-trace`: recent CAN TX/RX trace entries.
- `GET /api/wifi/status`: setup AP and station connection state.
- `GET /api/wifi/networks`: scan nearby Wi-Fi networks.
- `POST /api/wifi/config`: save station credentials and start connection.
- `POST /api/wifi/setup/finish`: stop setup AP after station connection succeeds.
- `GET /api/keymap`: current keymap document.
- `PUT /api/keymap`: replace the keymap document.
- `POST /api/debug/motor/move-relative`: issue a debug relative motor move. Motor id `23` addresses the paired Y-axis group.
- `POST /api/debug/motor/enable`: enable or disable a motor, or motor group `23`.
- `POST /api/debug/motor/stop`: stop a motor, or motor group `23`.
- `POST /api/debug/servo/apply`: apply `press`, `release`, or `neutral`.
- `POST /api/debug/probe-key`: upsert a key binding. Preferred request shape is `{ "key": "...", "point": { "xMm": 0, "yMm": 0 } }`; the firmware also accepts legacy top-level `xMm`/`yMm` and `x`/`y` fields.

## Group Command Stream

The group command stream is an NDJSON TCP connection on port `7777`. Each message is one JSON object followed by `\n`.

Client command messages:

- `hello`: required first message, with `v: 1` and `client: "desktop"`.
- `exec_group`: submit a remote motion group with `groupId`, `steps`, and optional timeout fields.
- `task_end`: tell the firmware the desktop task is complete.
- `cancel`: cancel queued or running work.
- `reset_fault`: request fault reset.
- `probe`: refresh motor readiness/feedback.
- `ping`: keepalive; returns `pong`.

Firmware event messages:

- `ack`: command acceptance or rejection.
- `snapshot`: current link state after handshake or task end.
- `telemetry`: periodic executor, job, point, CAN, and motor summary.
- `group_started`: remote group execution started.
- `group_done`: remote group completed.
- `group_warn`: remote group completed with a warning.
- `fault`: stream or machine fault.
- `pong`: response to `ping`.

Remote motion step kinds are `move_xy`, `servo_press`, `servo_release`, `character_release`, `line_feed`, and `wait`.

## Reliability Notes

The firmware records motor feedback and CAN diagnostics, but many commands still use best-effort delivery and timeout-based completion. `tx_queued`, `tx_sent`, and `tx_retry` trace directions describe CAN transport progress; they do not by themselves prove that an actuator reached the requested physical position.
