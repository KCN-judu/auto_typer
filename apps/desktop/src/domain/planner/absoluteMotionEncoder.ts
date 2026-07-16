import type {
  AtomicMotionBlock,
  MotionAction,
  RemoteMotorMove,
} from "../../../../../shared/protocol/protocolTypes";

export type MotorMotionProfile = {
  rpm: number;
  accelRaw: number;
  timeoutMs: number;
};

export type MotionPositionState = {
  x: number;
  y: number;
  l: number;
  z: number;
};

export type EncodedMotion = {
  block: AtomicMotionBlock;
  position: MotionPositionState;
};

export const lineFeedForwardTarget = 16400;
export const lineFeedRestTarget = 10000;

const motorTopology = {
  x: 1,
  yLeft: 2,
  yRight: 3,
  lineFeed: 4,
  press: 5,
} as const;

export const printMoveProfile = { rpm: 1600, accelRaw: 128, timeoutMs: 10000 } as const;
export const pressProfile = { rpm: 3000, accelRaw: 255, timeoutMs: 8000 } as const;
export const lineFeedProfile = { rpm: 500, accelRaw: 10, timeoutMs: 10000 } as const;

export function initialMotionPosition(): MotionPositionState {
  return { x: 0, y: 0, l: 0, z: 0 };
}

export function encodeXyMove(
  position: MotionPositionState,
  xTarget: number,
  yTarget: number,
  timeoutMs: number,
  profile: Pick<MotorMotionProfile, "rpm" | "accelRaw"> = printMoveProfile,
): EncodedMotion | undefined {
  if (position.x === xTarget && position.y === yTarget) {
    return undefined;
  }
  const next = { ...position, x: xTarget, y: yTarget };
  const commands: RemoteMotorMove[] = [];
  if (position.x !== xTarget) {
    commands.push(motorMoveCommand(motorTopology.x, next.x, { ...profile, timeoutMs }));
  }
  if (position.y !== yTarget) {
    commands.push(motorMoveCommand(motorTopology.yLeft, -next.y, { ...profile, timeoutMs }));
    commands.push(motorMoveCommand(motorTopology.yRight, next.y, { ...profile, timeoutMs }));
  }
  return { block: nonEmptyBlock(commands), position: next };
}

export function encodeLineFeedMove(
  position: MotionPositionState,
  target: number,
  profile: MotorMotionProfile = lineFeedProfile,
): EncodedMotion {
  const next = { ...position, l: target };
  return {
    block: [motorMoveCommand(motorTopology.lineFeed, next.l, profile)],
    position: next,
  };
}

export function encodeLineFeedHome(position: MotionPositionState): EncodedMotion[] {
  const forward = encodeLineFeedMove(position, lineFeedForwardTarget);
  const rest = encodeLineFeedMove(forward.position, lineFeedRestTarget);
  return [forward, rest];
}

export function encodePressMove(
  position: MotionPositionState,
  target: number,
  profile: MotorMotionProfile = pressProfile,
): EncodedMotion {
  const next = { ...position, z: target };
  return {
    block: [motorMoveCommand(motorTopology.press, next.z, profile)],
    position: next,
  };
}

export function encodeReturnToZero(position: MotionPositionState): EncodedMotion[] {
  const blocks: EncodedMotion[] = [];
  let current = position;
  const xy = encodeXyMove(current, 0, 0, printMoveProfile.timeoutMs);
  if (xy) {
    blocks.push(xy);
    current = xy.position;
  }
  const lineFeedHome = encodeLineFeedHome(current);
  blocks.push(...lineFeedHome);
  current = lineFeedHome.at(-1)?.position ?? current;
  if (current.z !== 0) {
    const press = encodePressMove(current, 0);
    blocks.push(press);
  }
  return blocks;
}

export function motorMoveBlock(input: MotorMotionProfile & {
  motorId: number;
  target: number;
}): AtomicMotionBlock {
  return [motorMoveCommand(input.motorId, input.target, input)];
}

export function atomicMotionBlock(actions: MotionAction[]): AtomicMotionBlock {
  return nonEmptyBlock(actions);
}

export function waitBlock(durationMs: number): AtomicMotionBlock {
  return [{ type: "wait", durationMs }];
}

function motorMoveCommand(
  motorId: number,
  target: number,
  profile: MotorMotionProfile,
): RemoteMotorMove {
  return {
    type: "motor_move",
    motorId,
    rpm: profile.rpm,
    accelRaw: profile.accelRaw,
    timeoutMs: profile.timeoutMs,
    target: target === 0 ? 0 : target,
  };
}

function nonEmptyBlock(actions: MotionAction[]): AtomicMotionBlock {
  if (actions.length === 0) {
    throw new Error("Atomic motion block must not be empty");
  }
  return actions as AtomicMotionBlock;
}
