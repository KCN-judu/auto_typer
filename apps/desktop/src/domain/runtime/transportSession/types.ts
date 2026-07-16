export type SessionPhase =
  | "disconnected"
  | "connecting"
  | "handshaking"
  | "ready"
  | "uncertain"
  | "recovering"
  | "blocked";

export type SessionCommand<CommandKind extends string, CommandMeta = undefined> = {
  requestId: string;
  kind: CommandKind;
  sentAtMs: number;
  meta: CommandMeta;
};

export type SessionIssue<CommandKind extends string> =
  | {
      kind: "transport_disconnect";
      message: string;
      atMs: number;
    }
  | {
      kind: "connect_failed";
      message: string;
      atMs: number;
    }
  | {
      kind: "handshake_failed";
      message: string;
      atMs: number;
    }
  | {
      kind: "command_timeout";
      message: string;
      requestId: string;
      commandKind: CommandKind;
      atMs: number;
    }
  | {
      kind: "command_rejected";
      message: string;
      requestId: string;
      commandKind: CommandKind;
      atMs: number;
    }
  | {
      kind: "protocol_violation";
      message: string;
      atMs: number;
    }
  | {
      kind: "domain_blocked";
      message: string;
      atMs: number;
    };

export type ReconcileDirective<RecoveryAction extends string> =
  | { type: "resume" }
  | { type: "recover"; action: RecoveryAction }
  | { type: "block"; reason: string };

export type SessionState<
  Snapshot,
  CommandKind extends string,
  RecoveryAction extends string,
  CommandMeta = undefined,
> = {
  phase: SessionPhase;
  host?: string;
  port?: number;
  snapshot?: Snapshot;
  activeCommand?: SessionCommand<CommandKind, CommandMeta>;
  pendingRecovery?: ReconcileDirective<RecoveryAction>;
  lastIssue?: SessionIssue<CommandKind>;
  sessionNonce: number;
};

export type SessionEffect<RecoveryAction extends string> =
  | { type: "open_transport"; host: string; port: number }
  | { type: "close_transport" }
  | { type: "begin_handshake" }
  | { type: "request_snapshot" }
  | { type: "perform_recovery"; action: RecoveryAction }
  | { type: "await_manual_resolution"; reason: string }
  | { type: "emit_log"; message: string };

export type SessionTransition<
  Snapshot,
  CommandKind extends string,
  RecoveryAction extends string,
  CommandMeta = undefined,
> = {
  state: SessionState<Snapshot, CommandKind, RecoveryAction, CommandMeta>;
  effects: SessionEffect<RecoveryAction>[];
};

export type SessionEvent<
  Snapshot,
  CommandKind extends string,
  RecoveryAction extends string,
  CommandMeta = undefined,
> =
  | { type: "connect_requested"; host: string; port: number; nowMs: number }
  | { type: "transport_connected"; nowMs: number }
  | { type: "connect_failed"; message: string; nowMs: number }
  | { type: "handshake_succeeded"; nowMs: number }
  | { type: "handshake_failed"; message: string; nowMs: number }
  | { type: "snapshot_received"; snapshot: Snapshot; nowMs: number }
  | { type: "transport_disconnected"; message: string; nowMs: number }
  | {
      type: "command_sent";
      command: SessionCommand<CommandKind, CommandMeta>;
      nowMs: number;
    }
  | {
      type: "command_completed";
      requestId: string;
      commandKind: CommandKind;
      ok: boolean;
      message?: string;
      nowMs: number;
    }
  | {
      type: "command_timeout";
      requestId: string;
      commandKind: CommandKind;
      nowMs: number;
    }
  | { type: "reconcile_requested"; nowMs: number }
  | { type: "manual_resolution_confirmed"; snapshot: Snapshot; nowMs: number };

export type SessionPolicy<Snapshot, RecoveryAction extends string> = {
  reconcile(snapshot: Snapshot): ReconcileDirective<RecoveryAction>;
  describeDirective?(directive: ReconcileDirective<RecoveryAction>): string;
};

export function initialSessionState<
  Snapshot,
  CommandKind extends string,
  RecoveryAction extends string,
  CommandMeta = undefined,
>(): SessionState<Snapshot, CommandKind, RecoveryAction, CommandMeta> {
  return {
    phase: "disconnected",
    sessionNonce: 0,
  };
}
