import type {
  ReconcileDirective,
  SessionEffect,
  SessionEvent,
  SessionIssue,
  SessionPolicy,
  SessionState,
  SessionTransition,
} from "./types";

export function reduceSession<
  Snapshot,
  CommandKind extends string,
  RecoveryAction extends string,
  CommandMeta = undefined,
>(
  state: SessionState<Snapshot, CommandKind, RecoveryAction, CommandMeta>,
  event: SessionEvent<Snapshot, CommandKind, RecoveryAction, CommandMeta>,
  policy: SessionPolicy<Snapshot, RecoveryAction>,
): SessionTransition<Snapshot, CommandKind, RecoveryAction, CommandMeta> {
  switch (event.type) {
    case "connect_requested":
      return transition(
        {
          phase: "connecting",
          host: event.host,
          port: event.port,
          sessionNonce: state.sessionNonce + 1,
        },
        [
          { type: "open_transport", host: event.host, port: event.port },
          { type: "emit_log", message: `transport connect requested ${event.host}:${event.port}` },
        ],
      );

    case "transport_connected":
      if (state.phase !== "connecting") {
        return protocolViolation(state, `transport_connected in ${state.phase}`, event.nowMs);
      }
      return transition(
        { ...state, phase: "handshaking" },
        [
          { type: "begin_handshake" },
          { type: "emit_log", message: "transport connected, beginning handshake" },
        ],
      );

    case "connect_failed":
      return transition(
        {
          phase: "disconnected",
          sessionNonce: state.sessionNonce,
          lastIssue: issue("connect_failed", event.message, event.nowMs),
        },
        [{ type: "emit_log", message: `transport connect failed: ${event.message}` }],
      );

    case "handshake_succeeded":
      if (state.phase !== "handshaking") {
        return protocolViolation(state, `handshake_succeeded in ${state.phase}`, event.nowMs);
      }
      return transition(
        {
          ...state,
          phase: "recovering",
          pendingRecovery: undefined,
        },
        [
          { type: "request_snapshot" },
          { type: "emit_log", message: "handshake succeeded, reconciling snapshot" },
        ],
      );

    case "handshake_failed":
      return transition(
        {
          phase: "disconnected",
          sessionNonce: state.sessionNonce,
          host: state.host,
          port: state.port,
          lastIssue: issue("handshake_failed", event.message, event.nowMs),
        },
        [
          { type: "close_transport" },
          { type: "emit_log", message: `handshake failed: ${event.message}` },
        ],
      );

    case "snapshot_received":
      return settleSnapshot(state, event.snapshot, policy, false, event.nowMs);

    case "command_sent":
      if (state.phase !== "ready") {
        return protocolViolation(state, `command_sent in ${state.phase}`, event.nowMs);
      }
      return transition(
        {
          ...state,
          activeCommand: event.command,
        },
        [{ type: "emit_log", message: `command sent: ${event.command.kind}` }],
      );

    case "command_completed":
      if (state.activeCommand?.requestId !== event.requestId || state.activeCommand.kind !== event.commandKind) {
        return protocolViolation(
          state,
          `command_completed mismatch request=${event.requestId} kind=${event.commandKind}`,
          event.nowMs,
        );
      }
      if (event.ok) {
        return transition(
          { ...state, activeCommand: undefined },
          [{ type: "emit_log", message: `command completed: ${event.commandKind}` }],
        );
      }
      return transition(
        {
          ...state,
          phase: "uncertain",
          activeCommand: undefined,
          pendingRecovery: undefined,
          lastIssue: issue(
            "command_rejected",
            event.message ?? `${event.commandKind} rejected`,
            event.nowMs,
            event.requestId,
            event.commandKind,
          ),
        },
        [
          { type: "request_snapshot" },
          { type: "emit_log", message: `command rejected: ${event.commandKind}` },
        ],
      );

    case "command_timeout":
      return transition(
        {
          ...state,
          phase: "uncertain",
          activeCommand: undefined,
          pendingRecovery: undefined,
          lastIssue: issue(
            "command_timeout",
            `${event.commandKind} timed out`,
            event.nowMs,
            event.requestId,
            event.commandKind,
          ),
        },
        [
          { type: "request_snapshot" },
          { type: "emit_log", message: `${event.commandKind} timed out, snapshot reconciliation required` },
        ],
      );

    case "reconcile_requested":
      if (state.phase === "disconnected" || state.phase === "connecting" || state.phase === "handshaking") {
        return protocolViolation(state, `reconcile_requested in ${state.phase}`, event.nowMs);
      }
      return transition(
        {
          ...state,
          phase: "recovering",
        },
        [
          { type: "request_snapshot" },
          { type: "emit_log", message: "reconciliation requested" },
        ],
      );

    case "manual_resolution_confirmed":
      return settleSnapshot(
        {
          ...state,
          phase: "recovering",
        },
        event.snapshot,
        policy,
        false,
        event.nowMs,
      );

    case "transport_disconnected":
      return transition(
        {
          phase: "disconnected",
          host: state.host,
          port: state.port,
          snapshot: state.snapshot,
          sessionNonce: state.sessionNonce,
          pendingRecovery: state.pendingRecovery,
          lastIssue: issue("transport_disconnect", event.message, event.nowMs),
        },
        [{ type: "emit_log", message: `transport disconnected: ${event.message}` }],
      );
  }
}

export function replaySession<
  Snapshot,
  CommandKind extends string,
  RecoveryAction extends string,
  CommandMeta = undefined,
>(
  events: readonly SessionEvent<Snapshot, CommandKind, RecoveryAction, CommandMeta>[],
  policy: SessionPolicy<Snapshot, RecoveryAction>,
  seed: SessionState<Snapshot, CommandKind, RecoveryAction, CommandMeta>,
): SessionTransition<Snapshot, CommandKind, RecoveryAction, CommandMeta> {
  return events.reduce<SessionTransition<Snapshot, CommandKind, RecoveryAction, CommandMeta>>(
    (current, event) => reduceSession(current.state, event, policy),
    transition(seed, []),
  );
}

function settleSnapshot<
  Snapshot,
  CommandKind extends string,
  RecoveryAction extends string,
  CommandMeta = undefined,
>(
  state: SessionState<Snapshot, CommandKind, RecoveryAction, CommandMeta>,
  snapshot: Snapshot,
  policy: SessionPolicy<Snapshot, RecoveryAction>,
  fromUnexpectedMessage: boolean,
  nowMs?: number,
): SessionTransition<Snapshot, CommandKind, RecoveryAction, CommandMeta> {
  const directive = policy.reconcile(snapshot);
  const description = policy.describeDirective?.(directive) ?? directive.type;
  const baseMessage = fromUnexpectedMessage ? "unsolicited snapshot reconciled" : "snapshot reconciled";

  switch (directive.type) {
    case "resume":
      return transition(
        {
          ...state,
          phase: "ready",
          snapshot,
          activeCommand: undefined,
          pendingRecovery: directive,
          lastIssue: undefined,
        },
        [{ type: "emit_log", message: `${baseMessage}: ${description}` }],
      );

    case "recover":
      return transition(
        {
          ...state,
          phase: "recovering",
          snapshot,
          activeCommand: undefined,
          pendingRecovery: directive,
        },
        [
          { type: "perform_recovery", action: directive.action },
          { type: "emit_log", message: `${baseMessage}: ${description}` },
        ],
      );

    case "block":
      return transition(
        {
          ...state,
          phase: "blocked",
          snapshot,
          activeCommand: undefined,
          pendingRecovery: directive,
          lastIssue: issue("domain_blocked", directive.reason, nowMs ?? 0),
        },
        [
          { type: "await_manual_resolution", reason: directive.reason },
          { type: "emit_log", message: `${baseMessage}: ${description}` },
        ],
      );
  }
}

function protocolViolation<
  Snapshot,
  CommandKind extends string,
  RecoveryAction extends string,
  CommandMeta = undefined,
>(
  state: SessionState<Snapshot, CommandKind, RecoveryAction, CommandMeta>,
  message: string,
  atMs: number,
): SessionTransition<Snapshot, CommandKind, RecoveryAction, CommandMeta> {
  return transition(
    {
      ...state,
      phase: "uncertain",
      activeCommand: undefined,
      lastIssue: issue("protocol_violation", message, atMs),
    },
    [
      { type: "request_snapshot" },
      { type: "emit_log", message: `protocol violation: ${message}` },
    ],
  );
}

function transition<
  Snapshot,
  CommandKind extends string,
  RecoveryAction extends string,
  CommandMeta = undefined,
>(
  state: SessionState<Snapshot, CommandKind, RecoveryAction, CommandMeta>,
  effects: SessionEffect<RecoveryAction>[],
): SessionTransition<Snapshot, CommandKind, RecoveryAction, CommandMeta> {
  return { state, effects };
}

function issue<CommandKind extends string>(
  kind: SessionIssue<CommandKind>["kind"],
  message: string,
  atMs: number,
  requestId?: string,
  commandKind?: CommandKind,
): SessionIssue<CommandKind> {
  if (kind === "command_timeout" || kind === "command_rejected") {
    return {
      kind,
      message,
      requestId: requestId ?? "unknown",
      commandKind: commandKind ?? ("unknown" as CommandKind),
      atMs,
    };
  }
  return { kind, message, atMs };
}
