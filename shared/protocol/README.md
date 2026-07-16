# Auto Typer Atomic Motion Protocol

This directory defines the version 1 JSON contract between the Electron controller and the ESP32 firmware.

The contract is intentionally controller-planned and device-executed. The desktop owns the keymap, text planning, coordinate conversion, and absolute pulse-position state. The firmware accepts concrete absolute motor targets and does not interpret text, keys, machine coordinates, or movement deltas.

The desktop and firmware both implement this contract.

## Transport

- Raw TCP NDJSON on port `7777`.
- One JSON object per line, with `v: 1` and a non-empty `requestId` on commands.
- Only one motion block may be active at a time.
- A client sends the next block only after receiving the terminal result for the active block.
- The connection uses an initial handshake, explicit snapshots, and heartbeat request/response messages. The firmware does not emit unsolicited telemetry or motor-state messages.

## Messages

Desktop commands:

- `handshake`
- `get_snapshot`
- `heartbeat`
- `execute_block`
- `cancel`
- `finish_task`
- `emergency_stop`
- `reset_fault`

Device responses and events:

- `handshake_ack`
- `snapshot`
- `heartbeat_ack`
- `block_ack`
- `block_result`
- `cancel_result`
- `finish_task_result`
- `emergency_stop_result`
- `reset_fault_result`
- `fault`
- `protocol_error`

## Execution Lifecycle

```text
execute_block
  -> block_ack
  -> block_result(status: done | failed | cancelled)
```

`block_ack` means the device parsed and validated the entire request and reserved its execution slot. It is not motion completion. A request that does not receive this acknowledgement is uncertain and must not be resent automatically.

`block_result` is the only terminal event for an accepted block. A result with `failed` or `cancelled` prevents the controller from sending the next block. `fault` is reserved for device or transport failures that are not a normal result for an accepted block.

## Task Completion

The desktop sends `finish_task` only after its motion queue is empty and the final accepted block has returned `block_result(status: "done")`. The transport rejects a local finish attempt while a block is still active.

After accepting `finish_task`, the firmware changes the OLED to `Complete` for 3000 ms and then returns the display text to `Idle`. This display dwell does not create another motion block.

## Emergency Stop

`emergency_stop` is independent from normal cancellation and may be sent while a block is active. It is not delayed behind ordinary mutating commands.

The firmware clears queued CAN work, immediately stops all five motors, disables all five motors, sends the Emm_V5.0 clear-stall-protection command to all five motors, marks the device faulted, and switches the OLED to `Error`. Error remains latched until a successful `reset_fault`; it never returns to Idle on a timer.

## Motion Block

Each `execute_block` contains exactly one non-empty `block` array:

```json
{
  "v": 1,
  "requestId": "request-42",
  "type": "execute_block",
  "blockId": "job-7-0042",
  "seq": 42,
  "policy": {
    "maxRuntimeMs": 20000,
    "onDisconnect": "cancel"
  },
  "block": [
    {
      "type": "motor_move",
      "motorId": 1,
      "rpm": 1600,
      "accelRaw": 128,
      "timeoutMs": 10000,
      "target": 12800
    }
  ]
}
```

Supported actions are `motor_move` and `wait`. Every motor movement includes `motorId`, `rpm`, `accelRaw`, `timeoutMs`, and numeric `target`. The target unit is pulses and its meaning is always the motor's absolute target position.

A block with one action is sequential. A block with multiple actions is permitted only for XY motion and may contain M1, M2, and M3 commands. Other motor actions must remain single-action blocks.

## Position Mapping

Before every power-on, the operator manually returns the complete mechanism to its defined mechanical origin. After enabling the motors, firmware sends the Emm_V5.0 position-clear command to all five motors and verifies near-zero input-pulse feedback before accepting motion. All motion commands then use absolute targets and never fall back to relative wire semantics.

The desktop maintains logical position state `{ x, y, l, z }` and expands it to physical motor targets:

- M1 target is `x`.
- M2 target is `-y`.
- M3 target is `y`.
- M4 target is `l`.
- M5 target is `z`.

Planning is cumulative. For example, two M4 advances of `-180` pulses produce targets `-180` and `-360`. Press down and release use Z targets `-2700` and `0` when starting from the zero position.

M4 line-feed home is a fixed absolute two-stage sequence: move to `16400`, then return to the paper-holding position `10000`. It is not represented as an increment from the current M4 position.

## Normal Cancellation

Normal cancellation is cooperative. The desktop stops consuming queued blocks but lets the currently acknowledged block reach `block_result(status: "done")`. It then discards every unsent block and submits an absolute return sequence: M1/M2/M3 to `0`, M4 to `16400` then `10000`, and M5 to `0`. A normally cancelled task does not send `finish_task`.

The protocol `cancel` command only cancels a matching block that has been acknowledged but has not started motion. It returns `ok: false` for a running block. `policy.onDisconnect: "cancel"` has the same queued-only boundary: disconnect never interrupts a running block, its unsent terminal result is discarded, and a later connection obtains state through `get_snapshot`.

Only `emergency_stop` may interrupt the active block immediately.

## Bounds

- `maxMessageBytes`: 8192
- `maxBlockRuntimeMs`: 30000
- `maxActionTimeoutMs`: 10000

An atomic block must contain at least one action. Numeric values must be finite and within the limits reported by `handshake_ack`.
