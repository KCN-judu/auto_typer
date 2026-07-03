# Auto Typer TCP Protocol

This directory defines the JSON contract between the Electron controller and the ESP32 firmware.

## Transport

- Raw TCP NDJSON on port `7777`.
- One JSON object per line.
- The desktop sends one bounded `exec_group` at a time and waits for `group_done` or `fault`.
- After the final group, the desktop sends `finish_task`; until then the firmware keeps the printing display active.
- Firmware accepts at most one active group and rejects additional groups with `device_busy`.

## Responsibilities

- Desktop plans text, performs keymap lookup, converts machine coordinates to signed step deltas, chunks blocks into bounded groups, and schedules groups.
- Firmware validates the active group, executes through `MotionExecutor`, supervises CAN feedback, and reports status/events.
- Key pressing is M5 Press stepper motion. It is not PWM or a separate press actuator model.

## Message Types

Client messages:

- `hello`
- `get_status`
- `subscribe_telemetry`
- `get_keymap`
- `probe`
- `reset_fault`
- `release_line_feed_origin`
- `cancel`
- `finish_task`
- `exec_group`
- `ping`

Firmware messages:

- `hello_ack`
- `status`
- `telemetry_subscribed`
- `telemetry`
- `keymap`
- `probe_result`
- `reset_fault_result`
- `release_line_feed_origin_result`
- `cancel_result`
- `finish_task_result`
- `group_accepted`
- `group_rejected`
- `group_started`
- `block_started`
- `block_done`
- `group_done`
- `fault`
- `protocol_error`
- `pong`

## Motion Blocks

Primary block types are:

- `move_xy`
- `press_down`
- `press_up`
- `character_release`
- `line_feed`
- `line_feed_home`
- `return_zero`
- `wait`

`move_xy` uses signed `dxSteps` and `dySteps` in machine coordinates. Press blocks use firmware M5 calibration deltas.
`line_feed_home` enables M4, moves to the configured line-feed return total, then releases by the configured return-release steps. `release_line_feed_origin` disables M4 while idle so the operator can manually return paper feed to origin; status then reports `lineFeedPrimeRequired`.

## Bounds

Firmware enforces bounded groups:

- `maxBlocksPerGroup`: 32
- `maxMessageBytes`: 8192
- `maxGroupRuntimeMs`: 30000
- `maxBlockTimeoutMs`: 10000

Oversized or invalid messages are rejected without starting motion.
