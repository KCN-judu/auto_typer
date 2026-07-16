# Transport Session Kernel

This folder is a protocol-agnostic session kernel for connection, handshake, uncertainty, and recovery.

The goal is to keep this module reusable across protocol variants:

```text
session state + abstract event -> next state + declarative effects
```

## Separation of concerns

This kernel does not know:

- TCP frame formats
- hello/status/reset_fault message names
- device-specific fault semantics
- Electron, React, timers, or sockets

Those live in an outer adapter.

## What the kernel knows

- transport lifecycle
- handshake lifecycle
- one in-flight mutating command
- transition to `uncertain` after timeouts or protocol violations
- reconciliation through a protocol-supplied snapshot policy
- recovery vs manual blocking decisions

## Composition model

You provide:

- a `Snapshot` type
- a command kind union
- a recovery action union
- a `SessionPolicy.reconcile(snapshot)` function

The kernel returns generic effects such as:

- `open_transport`
- `begin_handshake`
- `request_snapshot`
- `perform_recovery`
- `await_manual_resolution`

Your adapter maps those effects to the actual protocol implementation.

## Intended layering

1. **Kernel**
   Pure state machine in this folder.
2. **Protocol adapter**
   Maps wire messages into abstract session events.
3. **Effect runner**
   Performs socket IO / IPC / timers.
4. **UI**
   Renders session state and operator affordances.

## Why this matters

This keeps protocol semantics replaceable:

- same kernel, different TCP protocol
- same kernel, mocked test adapter
- same kernel, future serial/WebSocket transport

Only the adapter and recovery policy need to change.
