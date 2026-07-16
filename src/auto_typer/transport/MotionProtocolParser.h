#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "../auto_typer_types.h"

namespace auto_typer {

inline bool parseRequiredMotionProfile(JsonObjectConst action,
                                       RemoteMotionProfile& profile,
                                       const char*& code,
                                       const char*& message) {
  profile = RemoteMotionProfile{};
  if (!action["rpm"].is<uint16_t>() || !action["accelRaw"].is<uint8_t>() ||
      !action["timeoutMs"].is<uint32_t>()) {
    code = "invalid_block";
    message = "rpm, accelRaw, and timeoutMs are required integers";
    return false;
  }
  profile.hasRpm = true;
  profile.rpm = action["rpm"].as<uint16_t>();
  profile.hasAccelRaw = true;
  profile.accelRaw = action["accelRaw"].as<uint8_t>();
  profile.hasTimeoutMs = true;
  profile.timeoutMs = action["timeoutMs"].as<uint32_t>();
  if (profile.rpm == 0 || profile.accelRaw == 0 || profile.timeoutMs == 0 ||
      profile.timeoutMs > kMaxBlockTimeoutMs) {
    code = "invalid_block";
    message = "rpm, accelRaw, or timeoutMs is outside the supported range";
    return false;
  }
  return true;
}

inline bool motionProfilesEqual(const RemoteMotionProfile& left, const RemoteMotionProfile& right) {
  return left.rpm == right.rpm && left.accelRaw == right.accelRaw && left.timeoutMs == right.timeoutMs;
}

inline bool parseMotorMoveAction(JsonObjectConst action,
                                 RemoteMotionStep& out,
                                 bool& profileSet,
                                 uint8_t& seenMotorMask,
                                 const char*& code,
                                 const char*& message) {
  if (strcmp(action["type"] | "", "motor_move") != 0 || !action["motorId"].is<uint8_t>() ||
      !action["target"].is<int32_t>()) {
    code = "invalid_block";
    message = "motor_move requires integer motorId and absolute target";
    return false;
  }
  RemoteMotionProfile profile{};
  if (!parseRequiredMotionProfile(action, profile, code, message)) {
    return false;
  }
  if (profileSet && !motionProfilesEqual(out.profile, profile)) {
    code = "invalid_block";
    message = "parallel motor_move actions must share one profile";
    return false;
  }
  out.profile = profile;
  profileSet = true;

  const uint8_t motorId = action["motorId"].as<uint8_t>();
  if (motorId < 1 || motorId > 5) {
    code = "invalid_block";
    message = "motorId must be between 1 and 5";
    return false;
  }
  const uint8_t motorBit = static_cast<uint8_t>(1U << (motorId - 1));
  if ((seenMotorMask & motorBit) != 0) {
    code = "invalid_block";
    message = "atomic block contains a duplicate motorId";
    return false;
  }
  seenMotorMask |= motorBit;

  const int32_t target = action["target"].as<int32_t>();
  switch (motorId) {
    case 1:
      out.targetSteps.x = target;
      out.hasXTarget = true;
      return true;
    case 2:
      out.targetSteps.yLeft = target;
      out.hasYLeftTarget = true;
      return true;
    case 3:
      out.targetSteps.yRight = target;
      out.hasYRightTarget = true;
      return true;
    case 4:
      out.targetSteps.lineFeed = target;
      out.hasLineFeedTarget = true;
      return true;
    case 5:
      out.targetSteps.press = target;
      out.hasPressTarget = true;
      return true;
    default:
      return false;
  }
}

inline bool parseAtomicMotionBlock(JsonVariantConst value,
                                   RemoteMotionStep& out,
                                   const char*& code,
                                   const char*& message) {
  if (!value.is<JsonArrayConst>()) {
    code = "invalid_block";
    message = "block must be an array";
    return false;
  }
  const JsonArrayConst block = value.as<JsonArrayConst>();
  out = RemoteMotionStep{};
  if (block.size() == 0) {
    code = "invalid_block";
    message = "block must not be empty";
    return false;
  }
  if (block.size() == 1 && block[0].is<JsonObjectConst>() && strcmp(block[0]["type"] | "", "wait") == 0) {
    const JsonObjectConst wait = block[0].as<JsonObjectConst>();
    if (!wait["durationMs"].is<uint16_t>()) {
      code = "invalid_block";
      message = "wait durationMs must be a non-negative integer within the action limit";
      return false;
    }
    out.kind = RemoteMotionStepKind::Wait;
    out.durationMs = wait["durationMs"].as<uint16_t>();
    return true;
  }
  if (block.size() > 3) {
    code = "invalid_block";
    message = "parallel block may contain at most M1, M2, and M3";
    return false;
  }

  bool profileSet = false;
  uint8_t seenMotorMask = 0;
  for (JsonVariantConst action : block) {
    if (!action.is<JsonObjectConst>() ||
        !parseMotorMoveAction(action.as<JsonObjectConst>(), out, profileSet, seenMotorMask, code, message)) {
      return false;
    }
  }
  if (block.size() == 1 && out.hasLineFeedTarget) {
    out.kind = RemoteMotionStepKind::CharacterRelease;
    return true;
  }
  if (block.size() == 1 && out.hasPressTarget) {
    out.kind = out.targetSteps.press == 0 ? RemoteMotionStepKind::PressUp : RemoteMotionStepKind::PressDown;
    return true;
  }
  if (out.hasLineFeedTarget || out.hasPressTarget || out.hasYLeftTarget != out.hasYRightTarget ||
      (!out.hasXTarget && !out.hasYLeftTarget)) {
    code = "invalid_block";
    message = "parallel block may contain only M1 and an optional complete M2/M3 pair";
    return false;
  }
  out.kind = RemoteMotionStepKind::MoveXY;
  return true;
}

inline const char* modeText(DeviceMode mode) {
  switch (mode) {
    case DeviceMode::Running:
      return "running";
    case DeviceMode::Faulted:
      return "faulted";
    case DeviceMode::Debug:
      return "debug";
    case DeviceMode::Idle:
    default:
      return "idle";
  }
}

inline const char* jobStateText(JobState state) {
  switch (state) {
    case JobState::Queued:
      return "queued";
    case JobState::Planning:
      return "planning";
    case JobState::Running:
      return "running";
    case JobState::Cancelling:
      return "cancelling";
    case JobState::Completed:
      return "completed";
    case JobState::Cancelled:
      return "cancelled";
    case JobState::Failed:
      return "failed";
    case JobState::None:
    default:
      return "none";
  }
}

inline const char* motorRoleText(MotorRole role) {
  switch (role) {
    case MotorRole::YLeft:
      return "y_left";
    case MotorRole::YRight:
      return "y_right";
    case MotorRole::LineFeed:
      return "line_feed";
    case MotorRole::Press:
      return "press";
    case MotorRole::X:
    default:
      return "x";
  }
}

inline const char* protocolMotorReadinessText(MotorReadiness readiness) {
  switch (readiness) {
    case MotorReadiness::Ready:
      return "ready";
    case MotorReadiness::Faulted:
    case MotorReadiness::ConditionNotMet:
      return "fault";
    case MotorReadiness::ConfigPending:
    case MotorReadiness::ConfigSent:
    case MotorReadiness::Acked:
    case MotorReadiness::Offline:
      return "not_ready";
    case MotorReadiness::Unknown:
    default:
      return "unknown";
  }
}

}  // namespace auto_typer
