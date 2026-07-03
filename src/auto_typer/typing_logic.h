#pragma once

#include <math.h>

#include "auto_typer_types.h"

namespace auto_typer {

inline float stepsPerMm(const MotionCalibration& calibration) {
  return static_cast<float>(calibration.stepsPerRev) /
         (calibration.beltPitchMm * static_cast<float>(calibration.pulleyTeeth));
}

inline AxisDeltaSteps mmToMotorSteps(float deltaMm, const MotionCalibration& calibration) {
  const float rawSteps = fabsf(deltaMm) * stepsPerMm(calibration);
  AxisDeltaSteps delta{};
  delta.direction = deltaMm >= 0.0f ? MotorDirection::Cw : MotorDirection::Ccw;
  delta.steps = static_cast<uint32_t>(rawSteps + 0.5f);
  return delta;
}

inline bool lookupKey(char key, const KeyBinding* keymap, size_t keymapCount, MachinePointMm& outPoint) {
  const char normalized = (key >= 'A' && key <= 'Z') ? static_cast<char>(key - 'A' + 'a') : key;
  for (size_t i = 0; i < keymapCount; ++i) {
    if (keymap[i].key == normalized) {
      outPoint = keymap[i].point;
      return true;
    }
  }
  return false;
}

inline MovePlan planMoveTo(const MachinePointMm& current,
                           const MachinePointMm& target,
                           const MotionCalibration& calibration) {
  MovePlan plan{};
  plan.x = mmToMotorSteps(target.xMm - current.xMm, calibration);
  plan.y = mmToMotorSteps(current.yMm - target.yMm, calibration);
  plan.target = target;
  return plan;
}

inline bool appendStep(TypingPlan& plan, const TypingStep& step) {
  if (plan.count >= sizeof(plan.steps) / sizeof(plan.steps[0])) {
    plan.status = PlanStatus::PlanFull;
    return false;
  }
  plan.steps[plan.count] = step;
  ++plan.count;
  return true;
}

inline void resetTypingPlan(TypingPlan& plan) {
  plan.status = PlanStatus::Ok;
  plan.failedKey = '\0';
  plan.count = 0;
}

inline TypingPlan& planTextInto(TypingPlan& plan,
                                const char* text,
                                const KeyBinding* keymap,
                                size_t keymapCount,
                                const TypingConfig& config) {
  resetTypingPlan(plan);

  bool servoReleased = true;

  MachinePointMm current = config.homePoint;
  for (size_t i = 0; text[i] != '\0'; ++i) {
    if (text[i] == '\n' || text[i] == '\r') {
      if (text[i] == '\r' && text[i + 1] == '\n') {
        ++i;
      }
      if (!servoReleased) {
        if (!appendStep(plan, {TypingStepKind::Release, current, config.pressMotor.releaseMs})) {
          return plan;
        }
        servoReleased = true;
      }
      if (!appendStep(plan, {TypingStepKind::LineFeed, current, 0})) {
        return plan;
      }
      current.xMm = config.homePoint.xMm;
      continue;
    }

    MachinePointMm target{};
    if (!lookupKey(text[i], keymap, keymapCount, target)) {
      plan.status = PlanStatus::KeyNotFound;
      plan.failedKey = text[i];
      return plan;
    }

    if (!servoReleased) {
      if (!appendStep(plan, {TypingStepKind::Release, current, config.pressMotor.releaseMs})) {
        return plan;
      }
      servoReleased = true;
    }
    if (!appendStep(plan, {TypingStepKind::MoveTo, target, 0})) {
      return plan;
    }
    if (!appendStep(plan, {TypingStepKind::Wait, target, config.pressMotor.settleMs})) {
      return plan;
    }
    if (!appendStep(plan, {TypingStepKind::Press, target, config.pressMotor.pressMs})) {
      return plan;
    }
    servoReleased = false;
    if (!appendStep(plan, {TypingStepKind::Release, target, config.pressMotor.releaseMs})) {
      return plan;
    }
    servoReleased = true;
    if (!appendStep(plan, {TypingStepKind::CharacterRelease, target, 0})) {
      return plan;
    }

    current = target;
  }

  return plan;
}

inline TypingPlan planText(const char* text,
                           const KeyBinding* keymap,
                           size_t keymapCount,
                           const TypingConfig& config) {
  TypingPlan plan{};
  return planTextInto(plan, text, keymap, keymapCount, config);
}

inline const char* statusText(PlanStatus status) {
  switch (status) {
    case PlanStatus::Ok:
      return "OK";
    case PlanStatus::KeyNotFound:
      return "KEY_NOT_FOUND";
    case PlanStatus::PlanFull:
      return "PLAN_FULL";
    case PlanStatus::DeviceFault:
      return "DEVICE_FAULT";
    case PlanStatus::DeviceBusy:
      return "DEVICE_BUSY";
    case PlanStatus::DeviceNotReady:
      return "DEVICE_NOT_READY";
    default:
      return "UNKNOWN";
  }
}

}  // namespace auto_typer
