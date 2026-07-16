#pragma once

#include "../drivers/EmmV5Driver.h"
#include "MachineKinematics.h"

namespace auto_typer {

class YPairController {
 public:
  YPairController(const TypingConfig& config, EmmV5Driver& driver)
      : config_(config), driver_(driver) {}

  bool moveAbsolute(int32_t yLeftTarget, int32_t yRightTarget, uint16_t rpm, uint8_t acceleration) {
    const EmmV5Driver::MoveAbsoluteCommand commands[] = {
      {config_.topology.yLeftMotorId,
       directionForSignedSteps(yLeftTarget),
       rpm,
       acceleration,
       absoluteSteps(yLeftTarget),
       true},
      {config_.topology.yRightMotorId,
       directionForSignedSteps(yRightTarget),
       rpm,
       acceleration,
       absoluteSteps(yRightTarget),
       true},
    };
    return driver_.moveAbsoluteBatch(commands, sizeof(commands) / sizeof(commands[0]), true);
  }

  bool trigger() {
    return driver_.triggerSynchronousMotionBroadcast();
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
