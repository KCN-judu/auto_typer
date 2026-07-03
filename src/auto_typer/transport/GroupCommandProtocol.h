#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "../auto_typer_types.h"

namespace auto_typer {

inline bool parseRequiredProfile(JsonObjectConst block, RemoteMotionProfile& profile, const char*& code, const char*& message) {
  profile = RemoteMotionProfile{};
  if (!block["rpm"].is<uint16_t>() || !block["accelRaw"].is<uint8_t>() || !block["timeoutMs"].is<uint32_t>()) {
    code = "invalid_block";
    message = "rpm, accelRaw, and timeoutMs are required";
    return false;
  }
  profile.hasRpm = true;
  profile.rpm = block["rpm"].as<uint16_t>();
  profile.hasAccelRaw = true;
  profile.accelRaw = block["accelRaw"].as<uint8_t>();
  profile.hasTimeoutMs = true;
  profile.timeoutMs = block["timeoutMs"].as<uint32_t>();
  if (profile.rpm == 0 || profile.accelRaw == 0 || profile.timeoutMs == 0 ||
      profile.timeoutMs > kMaxBlockTimeoutMs) {
    code = "invalid_block";
    message = "invalid rpm, accelRaw, or timeoutMs";
    return false;
  }
  return true;
}

inline bool parseRemoteStep(JsonVariantConst value,
                             RemoteMotionStep& out,
                             const char*& code,
                             const char*& message) {
  if (!value.is<JsonObjectConst>()) {
    code = "invalid_block";
    message = "block object is required";
    return false;
  }
  JsonObjectConst block = value.as<JsonObjectConst>();
  const char* kind = block["type"] | "";
  out = RemoteMotionStep{};
  if (strcmp(kind, "move_xy") == 0) {
    out.kind = RemoteMotionStepKind::MoveXY;
    if (!block["dxSteps"].is<int32_t>() || !block["dySteps"].is<int32_t>()) {
      code = "invalid_block";
      message = "move_xy dxSteps and dySteps are required";
      return false;
    }
    out.dxSteps = block["dxSteps"].as<int32_t>();
    out.dySteps = block["dySteps"].as<int32_t>();
    return parseRequiredProfile(block, out.profile, code, message);
  }
  if (strcmp(kind, "press_down") == 0) {
    out.kind = RemoteMotionStepKind::PressDown;
    return parseRequiredProfile(block, out.profile, code, message);
  }
  if (strcmp(kind, "press_up") == 0) {
    out.kind = RemoteMotionStepKind::PressUp;
    return parseRequiredProfile(block, out.profile, code, message);
  }
  if (strcmp(kind, "character_release") == 0) {
    out.kind = RemoteMotionStepKind::CharacterRelease;
    return parseRequiredProfile(block, out.profile, code, message);
  }
  if (strcmp(kind, "line_feed") == 0) {
    out.kind = RemoteMotionStepKind::LineFeed;
    if (!block["lines"].is<uint8_t>() || block["lines"].as<uint8_t>() == 0) {
      code = "invalid_block";
      message = "line_feed lines is required";
      return false;
    }
    out.lines = block["lines"].as<uint8_t>();
    return parseRequiredProfile(block, out.profile, code, message);
  }
  if (strcmp(kind, "line_feed_home") == 0) {
    out.kind = RemoteMotionStepKind::LineFeedHome;
    return parseRequiredProfile(block, out.profile, code, message);
  }
  if (strcmp(kind, "return_zero") == 0) {
    out.kind = RemoteMotionStepKind::ReturnZero;
    return parseRequiredProfile(block, out.profile, code, message);
  }
  if (strcmp(kind, "wait") == 0) {
    const uint32_t durationMs = block["durationMs"] | 0;
    if (durationMs > kMaxBlockTimeoutMs) {
      code = "invalid_block";
      message = "wait durationMs is too large";
      return false;
    }
    out.kind = RemoteMotionStepKind::Wait;
    out.durationMs = static_cast<uint16_t>(durationMs);
    return true;
  }
  code = "invalid_block";
  message = "Unsupported remote block type";
  return false;
}

inline bool parseRemoteGroup(JsonVariantConst value,
                             RemoteMotionStep* outSteps,
                             size_t capacity,
                             size_t& outCount,
                             const char*& code,
                             const char*& message) {
  outCount = 0;
  if (!value.is<JsonArrayConst>()) {
    code = "invalid_group";
    message = "blocks array is required";
    return false;
  }
  JsonArrayConst blocks = value.as<JsonArrayConst>();
  const size_t count = blocks.size();
  if (count == 0) {
    code = "invalid_group";
    message = "blocks array must not be empty";
    return false;
  }
  if (count > capacity) {
    code = "group_too_large";
    message = "exec_group contains too many blocks";
    return false;
  }
  for (JsonVariantConst block : blocks) {
    if (!parseRemoteStep(block, outSteps[outCount], code, message)) {
      return false;
    }
    ++outCount;
  }
  return true;
}

inline const char* remoteStepKindText(RemoteMotionStepKind kind) {
  switch (kind) {
    case RemoteMotionStepKind::MoveXY:
      return "move_xy";
    case RemoteMotionStepKind::PressDown:
      return "press_down";
    case RemoteMotionStepKind::PressUp:
      return "press_up";
    case RemoteMotionStepKind::CharacterRelease:
      return "character_release";
    case RemoteMotionStepKind::LineFeed:
      return "line_feed";
    case RemoteMotionStepKind::LineFeedHome:
      return "line_feed_home";
    case RemoteMotionStepKind::ReturnZero:
      return "return_zero";
    case RemoteMotionStepKind::Wait:
    default:
      return "wait";
  }
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

inline const char* motorReadinessText(MotorReadiness readiness) {
  switch (readiness) {
    case MotorReadiness::ConfigPending:
      return "config_pending";
    case MotorReadiness::ConfigSent:
      return "config_sent";
    case MotorReadiness::Acked:
      return "acked";
    case MotorReadiness::Ready:
      return "ready";
    case MotorReadiness::Offline:
      return "offline";
    case MotorReadiness::ConditionNotMet:
      return "condition_not_met";
    case MotorReadiness::Faulted:
      return "faulted";
    case MotorReadiness::Unknown:
    default:
      return "unknown";
  }
}

}  // namespace auto_typer
