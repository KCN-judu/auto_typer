#pragma once

#include <math.h>

#include "../auto_typer_types.h"

namespace auto_typer {

inline float kinematicsStepsPerMm(const MotionCalibration& calibration) {
  return static_cast<float>(calibration.stepsPerRev) /
         (calibration.beltPitchMm * static_cast<float>(calibration.pulleyTeeth));
}

inline int32_t mmDeltaToSignedSteps(float deltaMm, const MotionCalibration& calibration) {
  const float rawSteps = fabsf(deltaMm) * kinematicsStepsPerMm(calibration);
  const int32_t steps = static_cast<int32_t>(rawSteps + 0.5f);
  return deltaMm >= 0.0f ? steps : -steps;
}

inline MotorTargetSteps xyDeltaSteps(const MachinePointMm& current,
                                     const MachinePointMm& target,
                                     const MotionCalibration& calibration) {
  MotorTargetSteps delta{};
  delta.x = mmDeltaToSignedSteps(target.xMm - current.xMm, calibration);
  const int32_t y = mmDeltaToSignedSteps(current.yMm - target.yMm, calibration);
  delta.yLeft = y;
  delta.yRight = -y;
  delta.lineFeed = 0;
  return delta;
}

inline uint16_t scaledCoordinatedRpm(uint32_t axisSteps,
                                     uint32_t primarySteps,
                                     uint16_t maxRpm,
                                     uint16_t minimumRpm) {
  if (axisSteps == 0 || primarySteps == 0) {
    return 0;
  }
  if (axisSteps >= primarySteps) {
    return maxRpm;
  }
  const uint32_t rpm = (static_cast<uint32_t>(maxRpm) * axisSteps + primarySteps - 1) / primarySteps;
  return static_cast<uint16_t>(rpm < minimumRpm ? minimumRpm : rpm);
}

inline MotorDirection directionForSignedSteps(int32_t steps) {
  return steps >= 0 ? MotorDirection::Cw : MotorDirection::Ccw;
}

inline uint32_t absoluteSteps(int32_t steps) {
  const int64_t value = steps;
  return static_cast<uint32_t>(value >= 0 ? value : -value);
}

}  // namespace auto_typer
