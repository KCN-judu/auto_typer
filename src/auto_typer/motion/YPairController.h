#pragma once

#include "../drivers/EmmV5Driver.h"
#include "MachineKinematics.h"

namespace auto_typer {

class YPairController {
 public:
  YPairController(const TypingConfig& config, EmmV5Driver& driver)
      : config_(config), driver_(driver) {}

  bool moveRelative(int32_t yLeftSteps, uint16_t rpm, uint8_t acceleration) {
    const uint32_t steps = absoluteSteps(yLeftSteps);
    if (steps == 0) {
      return true;
    }
    const MotorDirection leftDirection = directionForSignedSteps(yLeftSteps);
    const MotorDirection rightDirection = leftDirection == MotorDirection::Cw ? MotorDirection::Ccw
                                                                              : MotorDirection::Cw;
    return driver_.moveRelative(config_.topology.yLeftMotorId, leftDirection, rpm, acceleration, steps, true) &&
           driver_.moveRelative(config_.topology.yRightMotorId, rightDirection, rpm, acceleration, steps, true);
  }

  bool trigger() {
    return driver_.triggerSynchronousMotion(config_.topology.yLeftMotorId) &&
           driver_.triggerSynchronousMotion(config_.topology.yRightMotorId);
  }

  bool stop() {
    return driver_.stopNow(config_.topology.yLeftMotorId) &&
           driver_.stopNow(config_.topology.yRightMotorId);
  }

 private:
  const TypingConfig& config_;
  EmmV5Driver& driver_;
};

}  // namespace auto_typer
