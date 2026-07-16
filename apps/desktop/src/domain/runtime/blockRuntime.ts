import type { AtomicMotionBlock, MotionAction } from "../../../../../shared/protocol/protocolTypes";

const moveFallbackMs = 6000;
const waitMarginMs = 500;

export function estimateBlockRuntimeMs(block: AtomicMotionBlock): number {
  return block.reduce((maxRuntime, action) => Math.max(maxRuntime, estimateActionRuntimeMs(action)), 0);
}

function estimateActionRuntimeMs(action: MotionAction): number {
  switch (action.type) {
    case "motor_move":
      return action.timeoutMs;
    case "wait":
      return action.durationMs + waitMarginMs;
    default:
      return moveFallbackMs;
  }
}
