#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <math.h>

namespace auto_typer {

enum class DesktopCommandOp : uint8_t {
  MoveTo,
  Wait,
  Press,
  Release,
  CharacterRelease,
  LineFeed,
  Cancel,
  ResetFault,
  EmergencyStop,
};

struct DesktopCommandProfile {
  bool hasRpm;
  bool hasAccelRaw;
  bool hasTimeoutMs;
  uint16_t rpm;
  uint8_t accelRaw;
  uint32_t timeoutMs;
};

struct DesktopCommand {
  char id[49];
  DesktopCommandOp op;
  float xMm;
  float yMm;
  uint32_t durationMs;
  DesktopCommandProfile profile;
};

struct DesktopCommandParseResult {
  bool ok;
  const char* code;
  const char* message;
};

inline bool validCommandIdChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
         c == ':';
}

inline bool validCommandId(const char* id) {
  if (id == nullptr || id[0] == '\0') {
    return false;
  }
  size_t bytes = 0;
  for (; id[bytes] != '\0'; ++bytes) {
    if (bytes >= 48 || !validCommandIdChar(id[bytes])) {
      return false;
    }
  }
  return bytes <= 48;
}

inline void copyCommandId(char* out, size_t outSize, const char* id) {
  if (outSize == 0) {
    return;
  }
  size_t i = 0;
  for (; i + 1 < outSize && id != nullptr && id[i] != '\0'; ++i) {
    out[i] = id[i];
  }
  out[i] = '\0';
}

inline DesktopCommandParseResult parseDesktopCommand(JsonDocument& request, DesktopCommand& command) {
  if ((request["v"] | 0) != 1 || String(request["type"] | "") != "command") {
    return {false, "invalid_command", "Command frame must contain a v1 command object"};
  }
  const char* id = request["id"] | "";
  if (!validCommandId(id)) {
    return {false, "invalid_id", "Command id is required and may contain only A-Z a-z 0-9 _ - :"};
  }
  copyCommandId(command.id, sizeof(command.id), id);
  command.xMm = NAN;
  command.yMm = NAN;
  command.durationMs = 0;
  command.profile = {};

  JsonVariant profile = request["profile"];
  if (profile.is<JsonObject>()) {
    if (!profile["rpm"].isNull()) {
      const int rpm = profile["rpm"] | 0;
      if (rpm <= 0 || rpm > 3000) {
        return {false, "invalid_profile", "Invalid rpm"};
      }
      command.profile.hasRpm = true;
      command.profile.rpm = static_cast<uint16_t>(rpm);
    }
    if (!profile["accelRaw"].isNull()) {
      const int accel = profile["accelRaw"] | 0;
      if (accel <= 0 || accel > 255) {
        return {false, "invalid_profile", "Invalid accelRaw"};
      }
      command.profile.hasAccelRaw = true;
      command.profile.accelRaw = static_cast<uint8_t>(accel);
    }
    if (!profile["timeoutMs"].isNull()) {
      const uint32_t timeout = profile["timeoutMs"] | 0;
      if (timeout == 0 || timeout > 120000) {
        return {false, "invalid_profile", "Invalid timeoutMs"};
      }
      command.profile.hasTimeoutMs = true;
      command.profile.timeoutMs = timeout;
    }
  }

  const String op = request["op"] | "";
  if (op == "move_to") {
    command.op = DesktopCommandOp::MoveTo;
    command.xMm = request["xMm"] | NAN;
    command.yMm = request["yMm"] | NAN;
    if (!isfinite(command.xMm) || !isfinite(command.yMm)) {
      return {false, "invalid_command", "move_to requires finite xMm and yMm"};
    }
    return {true, "", ""};
  }
  if (op == "wait") {
    command.op = DesktopCommandOp::Wait;
    command.durationMs = request["durationMs"] | 0;
    if (command.durationMs == 0 || command.durationMs > 65535) {
      return {false, "invalid_command", "wait requires durationMs 1..65535"};
    }
    return {true, "", ""};
  }
  if (op == "press") {
    command.op = DesktopCommandOp::Press;
    command.durationMs = request["durationMs"] | 0;
    if (command.durationMs > 65535) {
      return {false, "invalid_command", "press duration is too large"};
    }
    return {true, "", ""};
  }
  if (op == "release") {
    command.op = DesktopCommandOp::Release;
    command.durationMs = request["durationMs"] | 0;
    if (command.durationMs > 65535) {
      return {false, "invalid_command", "release duration is too large"};
    }
    return {true, "", ""};
  }
  if (op == "character_release") {
    command.op = DesktopCommandOp::CharacterRelease;
    return {true, "", ""};
  }
  if (op == "line_feed") {
    command.op = DesktopCommandOp::LineFeed;
    return {true, "", ""};
  }
  if (op == "cancel") {
    command.op = DesktopCommandOp::Cancel;
    return {true, "", ""};
  }
  if (op == "reset_fault") {
    command.op = DesktopCommandOp::ResetFault;
    return {true, "", ""};
  }
  if (op == "emergency_stop") {
    command.op = DesktopCommandOp::EmergencyStop;
    return {true, "", ""};
  }
  return {false, "unknown_op", "Unknown command op"};
}

inline const char* commandOpText(DesktopCommandOp op) {
  switch (op) {
    case DesktopCommandOp::MoveTo:
      return "move_to";
    case DesktopCommandOp::Wait:
      return "wait";
    case DesktopCommandOp::Press:
      return "press";
    case DesktopCommandOp::Release:
      return "release";
    case DesktopCommandOp::CharacterRelease:
      return "character_release";
    case DesktopCommandOp::LineFeed:
      return "line_feed";
    case DesktopCommandOp::Cancel:
      return "cancel";
    case DesktopCommandOp::ResetFault:
      return "reset_fault";
    case DesktopCommandOp::EmergencyStop:
      return "emergency_stop";
  }
  return "unknown";
}

}  // namespace auto_typer
