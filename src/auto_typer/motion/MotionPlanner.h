#pragma once

#include "../typing_logic.h"
#include "MachineKinematics.h"

namespace auto_typer {

struct MotionStepPlan {
  PlanStatus status;
  size_t count;
  MotionStep steps[256];
};

inline bool appendMotionStep(MotionStepPlan& plan, const MotionStep& step) {
  if (plan.count >= sizeof(plan.steps) / sizeof(plan.steps[0])) {
    plan.status = PlanStatus::PlanFull;
    return false;
  }
  plan.steps[plan.count] = step;
  ++plan.count;
  return true;
}

inline void resetMotionStepPlan(MotionStepPlan& plan, PlanStatus status = PlanStatus::Ok) {
  plan.status = status;
  plan.count = 0;
}

inline MotionProfile defaultMotionProfile(const TypingConfig& config, uint16_t settleMs = 0) {
  MotionProfile profile{};
  profile.maxRpm = config.motionRuntime.defaultMoveRpm;
  profile.acceleration = config.motionRuntime.defaultAccelerationRaw;
  profile.settleMs = settleMs;
  profile.timeoutMs = config.motionRuntime.motionTimeoutMs;
  return profile;
}

inline MotionStepPlan& planMotionStepsInto(MotionStepPlan& plan,
                                             const TypingPlan& typingPlan,
                                             const TypingConfig& config) {
  resetMotionStepPlan(plan, typingPlan.status);
  MachinePointMm current = config.homePoint;
  MotorTargetSteps targets{};
  if (typingPlan.status != PlanStatus::Ok) {
    return plan;
  }

  for (size_t i = 0; i < typingPlan.count; ++i) {
    const TypingStep& typingStep = typingPlan.steps[i];
    MotionStep motionStep{};
    motionStep.targetMm = typingStep.point;
    motionStep.profile = defaultMotionProfile(config);
    motionStep.waitMs = typingStep.waitMs;
    switch (typingStep.kind) {
      case TypingStepKind::Release:
        motionStep.kind = MotionStepKind::PressUp;
        motionStep.targetSteps.press = config.pressMotor.releaseTargetSteps;
        motionStep.hasPressTarget = true;
        motionStep.profile.maxRpm = config.pressMotor.rpm;
        motionStep.profile.acceleration = config.pressMotor.acceleration;
        motionStep.profile.settleMs = config.pressMotor.settleMs;
        motionStep.profile.timeoutMs = config.pressMotor.timeoutMs;
        break;
      case TypingStepKind::Press:
        motionStep.kind = MotionStepKind::PressDown;
        motionStep.targetSteps.press = config.pressMotor.pressTargetSteps;
        motionStep.hasPressTarget = true;
        motionStep.profile.maxRpm = config.pressMotor.rpm;
        motionStep.profile.acceleration = config.pressMotor.acceleration;
        motionStep.profile.settleMs = config.pressMotor.settleMs;
        motionStep.profile.timeoutMs = config.pressMotor.timeoutMs;
        break;
      case TypingStepKind::CharacterRelease:
        motionStep.kind = MotionStepKind::CharacterRelease;
        targets.lineFeed += signedStepsForDirection(config.lineFeed.characterReleaseSteps, config.lineFeed.releaseDirection);
        motionStep.targetSteps.lineFeed = targets.lineFeed;
        motionStep.hasLineFeedTarget = true;
        motionStep.profile.maxRpm = config.lineFeed.rpm;
        motionStep.profile.acceleration = config.lineFeed.acceleration;
        motionStep.profile.settleMs = config.lineFeed.characterReleaseSettleMs;
        break;
      case TypingStepKind::LineFeed:
        motionStep.kind = MotionStepKind::LineFeedHome;
        motionStep.targetSteps.lineFeed = config.lineFeed.homeForwardTargetSteps;
        motionStep.hasLineFeedTarget = true;
        motionStep.profile.maxRpm = config.lineFeed.rpm;
        motionStep.profile.acceleration = config.lineFeed.acceleration;
        motionStep.profile.settleMs = config.lineFeed.settleMs;
        motionStep.targetMm = {config.homePoint.xMm, current.yMm};
        current.xMm = config.homePoint.xMm;
        if (!appendMotionStep(plan, motionStep)) {
          return plan;
        }
        motionStep.kind = MotionStepKind::LineFeedHomeRelease;
        motionStep.targetSteps.lineFeed = config.lineFeed.homeRestTargetSteps;
        targets.lineFeed = config.lineFeed.homeRestTargetSteps;
        if (!appendMotionStep(plan, motionStep)) {
          return plan;
        }
        continue;
        break;
      case TypingStepKind::MoveTo: {
        motionStep.kind = MotionStepKind::MoveXY;
        targets.x = signedStepsForDirection(mmToMotorSteps(typingStep.point.xMm - config.homePoint.xMm,
                                                           config.calibration).steps,
                                            typingStep.point.xMm >= config.homePoint.xMm ? MotorDirection::Cw
                                                                                         : MotorDirection::Ccw);
        const AxisDeltaSteps yTarget = mmToMotorSteps(config.homePoint.yMm - typingStep.point.yMm,
                                                      config.calibration);
        targets.yLeft = signedStepsForDirection(yTarget.steps, yTarget.direction);
        targets.yRight = -targets.yLeft;
        motionStep.targetSteps = targets;
        motionStep.hasXTarget = true;
        motionStep.hasYTargets = true;
        motionStep.profile.settleMs = config.xProfile.settleMs > config.yProfile.settleMs
                                          ? config.xProfile.settleMs
                                          : config.yProfile.settleMs;
        current = typingStep.point;
        break;
      }
      case TypingStepKind::Wait:
      default:
        motionStep.kind = MotionStepKind::Wait;
        break;
    }
    if (!appendMotionStep(plan, motionStep)) {
      return plan;
    }
  }
  return plan;
}

inline MotionStepPlan planMotionSteps(const TypingPlan& typingPlan, const TypingConfig& config) {
  MotionStepPlan plan{};
  return planMotionStepsInto(plan, typingPlan, config);
}

}  // namespace auto_typer
