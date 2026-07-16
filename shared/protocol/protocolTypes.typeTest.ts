import type {
  AtomicMotionBlock,
  ExecuteBlockMessage,
  MotionProtocolCommandMessage,
  RemoteMotorMove,
} from "./protocolTypes";

const move: RemoteMotorMove = {
  type: "motor_move",
  motorId: 1,
  rpm: 800,
  accelRaw: 80,
  timeoutMs: 5000,
  target: 3200,
};

const block: AtomicMotionBlock = [move];
const command: ExecuteBlockMessage = {
  v: 1,
  requestId: "request-1",
  type: "execute_block",
  blockId: "block-1",
  seq: 1,
  policy: { maxRuntimeMs: 10000, onDisconnect: "cancel" },
  block,
};

const protocolCommand: MotionProtocolCommandMessage = command;
void protocolCommand;

// @ts-expect-error target objects are no longer valid
const relativeMove: RemoteMotorMove = { ...move, target: { mode: "relative_steps", steps: 1 } };
// @ts-expect-error atomic blocks must not be empty
const emptyBlock: AtomicMotionBlock = [];
// @ts-expect-error execute_block has one block, never blocks[]
const groupedCommand: ExecuteBlockMessage = { ...command, blocks: [block] };
// @ts-expect-error removed commands are not part of the protocol
const telemetryCommand: MotionProtocolCommandMessage = { v: 1, requestId: "old", type: "subscribe_telemetry", intervalMs: 100 };

void relativeMove;
void emptyBlock;
void groupedCommand;
void telemetryCommand;
