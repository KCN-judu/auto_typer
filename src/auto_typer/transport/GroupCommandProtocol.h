#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <math.h>

#include "../auto_typer_types.h"

namespace auto_typer {

inline void parseRemoteProfile(JsonVariantConst value, RemoteMotionProfile& profile) {
  profile = RemoteMotionProfile{};
  if (!value.is<JsonObjectConst>()) {
    return;
  }
  JsonObjectConst object = value.as<JsonObjectConst>();
  if (object["rpm"].is<uint16_t>()) {
    profile.hasRpm = true;
    profile.rpm = object["rpm"].as<uint16_t>();
  }
  if (object["accelRaw"].is<uint8_t>()) {
    profile.hasAccelRaw = true;
    profile.accelRaw = object["accelRaw"].as<uint8_t>();
  }
  if (object["timeoutMs"].is<uint32_t>()) {
    profile.hasTimeoutMs = true;
    profile.timeoutMs = object["timeoutMs"].as<uint32_t>();
  }
}

inline bool parseRemoteStep(JsonVariantConst value,
                             RemoteMotionStep& out,
                             const char*& code,
                             const char*& message) {
  if (!value.is<JsonObjectConst>()) {
    code = "invalid_step";
    message = "step object is required";
    return false;
  }
  JsonObjectConst step = value.as<JsonObjectConst>();
  const char* kind = step["kind"] | "";
  out = RemoteMotionStep{};
  if (strcmp(kind, "move_xy") == 0) {
    out.kind = RemoteMotionStepKind::MoveXY;
    out.dxMm = step["dxMm"] | NAN;
    out.dyMm = step["dyMm"] | NAN;
    if (!isfinite(out.dxMm) || !isfinite(out.dyMm)) {
      code = "invalid_step";
      message = "move_xy dxMm and dyMm must be finite";
      return false;
    }
    parseRemoteProfile(step["profile"], out.profile);
    return true;
  }
  if (strcmp(kind, "servo_press") == 0) {
    out.kind = RemoteMotionStepKind::ServoPress;
    return true;
  }
  if (strcmp(kind, "servo_release") == 0) {
    out.kind = RemoteMotionStepKind::ServoRelease;
    return true;
  }
  if (strcmp(kind, "character_release") == 0) {
    out.kind = RemoteMotionStepKind::CharacterRelease;
    return true;
  }
  if (strcmp(kind, "line_feed") == 0) {
    out.kind = RemoteMotionStepKind::LineFeed;
    return true;
  }
  if (strcmp(kind, "wait") == 0) {
    const uint32_t durationMs = step["durationMs"] | 0;
    if (durationMs > 65535) {
      code = "invalid_step";
      message = "wait durationMs is too large";
      return false;
    }
    out.kind = RemoteMotionStepKind::Wait;
    out.durationMs = static_cast<uint16_t>(durationMs);
    return true;
  }
  code = "invalid_step";
  message = "Unsupported remote step kind";
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
    message = "steps array is required";
    return false;
  }
  JsonArrayConst steps = value.as<JsonArrayConst>();
  const size_t count = steps.size();
  if (count == 0) {
    code = "invalid_group";
    message = "steps array must not be empty";
    return false;
  }
  if (count > capacity) {
    code = "group_too_large";
    message = "exec_group contains too many steps";
    return false;
  }
  for (JsonVariantConst step : steps) {
    if (!parseRemoteStep(step, outSteps[outCount], code, message)) {
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
    case RemoteMotionStepKind::ServoPress:
      return "servo_press";
    case RemoteMotionStepKind::ServoRelease:
      return "servo_release";
    case RemoteMotionStepKind::CharacterRelease:
      return "character_release";
    case RemoteMotionStepKind::LineFeed:
      return "line_feed";
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
