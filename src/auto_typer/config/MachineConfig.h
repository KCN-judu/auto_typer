#pragma once

#include "../auto_typer_types.h"

namespace auto_typer {

inline MotionRuntimeConfig defaultMotionRuntimeConfig() {
  MotionRuntimeConfig config{};
  config.yPairVirtualMotorId = 23;
  config.defaultMoveRpm = 1600;
  config.defaultAccelerationPercent = 50;
  config.defaultAccelerationRaw = 128;
  config.positionToleranceSteps = 16;
  config.ySkewToleranceSteps = 80;
  config.idleVelocityThresholdRpm = 1.0f;
  config.motionPollIntervalMs = 80;
  config.motionTimeoutMs = 15000;
  config.completionSamples = 3;
  config.minimumCoordinatedRpm = 50;
  return config;
}

}  // namespace auto_typer
