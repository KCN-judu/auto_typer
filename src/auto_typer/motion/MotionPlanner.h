#pragma once

#include "../typing_logic.h"
#include "MachineKinematics.h"

namespace auto_typer {

struct MotionBlockPlan {
  PlanStatus status;
  size_t count;
  MotionBlock blocks[256];
};

inline bool appendMotionBlock(MotionBlockPlan& plan, const MotionBlock& block) {
  if (plan.count >= sizeof(plan.blocks) / sizeof(plan.blocks[0])) {
    plan.status = PlanStatus::PlanFull;
    return false;
  }
  plan.blocks[plan.count] = block;
  ++plan.count;
  return true;
}

inline MotionProfile defaultMotionProfile(const TypingConfig& config, uint16_t settleMs = 0) {
  MotionProfile profile{};
  profile.maxRpm = config.motionRuntime.defaultMoveRpm;
  profile.acceleration = config.motionRuntime.defaultAccelerationRaw;
  profile.settleMs = settleMs;
  profile.timeoutMs = config.motionRuntime.motionTimeoutMs;
  return profile;
}

inline MotionBlockPlan planMotionBlocks(const TypingPlan& typingPlan, const TypingConfig& config) {
  MotionBlockPlan plan{};
  plan.status = typingPlan.status;
  MachinePointMm current = config.homePoint;
  if (typingPlan.status != PlanStatus::Ok) {
    return plan;
  }

  for (size_t i = 0; i < typingPlan.count; ++i) {
    const TypingStep& step = typingPlan.steps[i];
    MotionBlock block{};
    block.targetMm = step.point;
    block.profile = defaultMotionProfile(config);
    block.waitMs = step.waitMs;
    switch (step.kind) {
      case TypingStepKind::Release:
        block.kind = MotionBlockKind::ServoRelease;
        break;
      case TypingStepKind::Press:
        block.kind = MotionBlockKind::ServoPress;
        break;
      case TypingStepKind::CharacterRelease:
        block.kind = MotionBlockKind::CharacterRelease;
        block.deltaSteps.lineFeed =
            signedStepsForDirection(config.lineFeed.characterReleaseSteps, config.lineFeed.releaseDirection);
        block.profile.maxRpm = config.lineFeed.rpm;
        block.profile.acceleration = config.lineFeed.acceleration;
        block.profile.settleMs = config.lineFeed.characterReleaseSettleMs;
        break;
      case TypingStepKind::LineFeed:
        block.kind = MotionBlockKind::LineFeed;
        block.deltaSteps.lineFeed =
            signedStepsForDirection(config.lineFeed.returnTotalSteps, config.lineFeed.returnDirection);
        block.profile.maxRpm = config.lineFeed.rpm;
        block.profile.acceleration = config.lineFeed.acceleration;
        block.profile.settleMs = config.lineFeed.settleMs;
        block.targetMm = {config.homePoint.xMm, current.yMm};
        current.xMm = config.homePoint.xMm;
        break;
      case TypingStepKind::MoveTo:
        block.kind = MotionBlockKind::MoveXY;
        block.deltaSteps = xyDeltaSteps(current, step.point, config.calibration);
        block.profile.settleMs = config.xProfile.settleMs > config.yProfile.settleMs ? config.xProfile.settleMs
                                                                                      : config.yProfile.settleMs;
        current = step.point;
        break;
      case TypingStepKind::Wait:
      default:
        block.kind = MotionBlockKind::Wait;
        break;
    }
    if (!appendMotionBlock(plan, block)) {
      return plan;
    }
  }
  return plan;
}

}  // namespace auto_typer
