import type { MotionBlock } from "../../../../shared/protocol/auto-typer-protocol";

const stepsPerRev = 3200;
const moveFallbackMs = 6000;
const moveMinimumMs = 1200;
const moveSettleMarginMs = 500;
const pressEstimateMs = 2000;
const pressReleaseEstimateMs = 2000;
const characterReleaseEstimateMs = 2500;
const lineFeedEstimateMs = 6000;
const returnZeroEstimateMs = 10000;
const waitMarginMs = 500;

export function estimateBlockRuntimeMs(block: MotionBlock): number {
  switch (block.type) {
    case "move_xy": {
      const steps = Math.max(Math.abs(block.dxSteps), Math.abs(block.dySteps));
      if (steps === 0 || block.rpm <= 0) {
        return Math.max(moveMinimumMs, block.timeoutMs);
      }
      const cruiseMs = Math.ceil((steps * 60000) / (block.rpm * stepsPerRev));
      return Math.max(moveMinimumMs, cruiseMs + moveSettleMarginMs, block.timeoutMs);
    }
    case "press_down":
      return Math.max(pressEstimateMs, block.timeoutMs);
    case "press_up":
      return Math.max(pressReleaseEstimateMs, block.timeoutMs);
    case "character_release":
      return Math.max(characterReleaseEstimateMs, block.timeoutMs);
    case "line_feed":
      return Math.max(lineFeedEstimateMs, block.timeoutMs);
    case "return_zero":
      return Math.max(returnZeroEstimateMs, block.timeoutMs);
    case "wait":
      return block.durationMs + waitMarginMs;
    default:
      return moveFallbackMs;
  }
}

export function estimateGroupRuntimeMs(blocks: MotionBlock[]): number {
  return blocks.reduce((total, block) => total + estimateBlockRuntimeMs(block), 0);
}
