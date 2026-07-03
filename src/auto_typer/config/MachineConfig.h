#pragma once

#include "../auto_typer_types.h"

namespace auto_typer {

inline MotionRuntimeConfig defaultMotionRuntimeConfig() {
  MotionRuntimeConfig config{};
  config.yPairVirtualMotorId = 23;
  config.defaultMoveRpm = 3000;
  config.defaultAccelerationPercent = 100;
  config.defaultAccelerationRaw = 200;
  config.positionToleranceSteps = 16;
  config.ySkewToleranceSteps = 800;
  config.idleVelocityThresholdRpm = 1.0f;
  config.motionPollIntervalMs = 80;
  config.motionTimeoutMs = 15000;
  config.motionCommandAckTimeoutMs = 750;
  config.motionNoMovementTimeoutMs = 1200;
  config.completionSamples = 3;
  config.minimumCoordinatedRpm = 50;
  return config;
}

}  // namespace auto_typer
