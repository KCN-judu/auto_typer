#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

#include "../auto_typer_runtime.h"

namespace auto_typer {

static constexpr uint16_t kBlockCommandPort = 7777;

class BlockCommandServer {
 public:
  BlockCommandServer(const TypingConfig& config, AutoTyperApplication& app, Print& log)
      : config_(config),
        app_(app),
        log_(log),
        server_(kBlockCommandPort),
        lineLength_(0),
        lineOverflow_(false),
        lastTelemetryMs_(0) {
    line_[0] = '\0';
  }

  void begin() {
    server_.begin();
    server_.setNoDelay(true);
    log_.print("Block command TCP ready on port ");
    log_.println(kBlockCommandPort);
  }

  void tick() {
    acceptClient();
    readClient();
    sendBlockEvents();
    sendTelemetry();
  }

 private:
  static constexpr size_t kMaxLineBytes = 1536;
  static constexpr uint32_t kTelemetryIntervalMs = 750;

  void acceptClient() {
    WiFiClient incoming = server_.available();
    if (!incoming) {
      return;
    }
    incoming.setNoDelay(true);
    if (client_ && client_.connected()) {
      sendError(incoming, "", "busy", "Another block stream client is connected");
      incoming.stop();
      return;
    }
    client_.stop();
    client_ = incoming;
    lineLength_ = 0;
    lineOverflow_ = false;
    lastTelemetryMs_ = 0;
    log_.println("[block] client connected");
  }

  void readClient() {
    if (!client_) {
      return;
    }
    if (!client_.connected()) {
      client_.stop();
      lineLength_ = 0;
      lineOverflow_ = false;
      return;
    }
    uint8_t reads = 0;
    while (client_.available() > 0 && reads < 64) {
      const char c = static_cast<char>(client_.read());
      ++reads;
      if (c == '\r') {
        continue;
      }
      if (c == '\n') {
        if (lineOverflow_) {
          sendError(client_, "", "line_too_long", "NDJSON line exceeds maximum length");
        } else if (lineLength_ > 0) {
          line_[lineLength_] = '\0';
          handleLine(line_);
        }
        lineLength_ = 0;
        lineOverflow_ = false;
        continue;
      }
      if (lineOverflow_) {
        continue;
      }
      if (lineLength_ + 1 >= kMaxLineBytes) {
        lineOverflow_ = true;
        lineLength_ = 0;
        continue;
      }
      line_[lineLength_] = c;
      ++lineLength_;
    }
  }

  void handleLine(const char* line) {
    StaticJsonDocument<1024> request;
    const DeserializationError error = deserializeJson(request, line);
    if (error) {
      sendError(client_, "", "invalid_json", "Invalid JSON line");
      return;
    }
    if ((request["v"] | 0) != 1) {
      sendAck(request["id"] | "", false, "unsupported_version", "Unsupported protocol version");
      return;
    }
    const String type = request["type"] | "";
    if (type == "hello") {
      sendAck(request["id"] | "", true, nullptr, nullptr);
      return;
    }
    if (type == "exec_block") {
      handleExecBlock(request);
      return;
    }
    if (type == "cancel") {
      app_.cancelCurrentJob();
      sendAck(request["id"] | "", true, nullptr, nullptr);
      return;
    }
    if (type == "reset_fault") {
      const bool ok = app_.resetFault();
      sendAck(request["id"] | "", ok, ok ? nullptr : "reset_rejected", ok ? nullptr : "Fault reset rejected");
      return;
    }
    if (type == "probe") {
      const bool ok = app_.probeMotors();
      sendAck(request["id"] | "", ok, ok ? nullptr : "probe_rejected", ok ? nullptr : "Probe rejected");
      return;
    }
    sendAck(request["id"] | "", false, "unknown_type", "Unknown block stream message type");
  }

  void handleExecBlock(JsonDocument& request) {
    const char* id = request["id"] | "";
    const char* blockId = request["blockId"] | "";
    JsonVariant blockJson = request["block"];
    RemoteMotionBlock block{};
    const char* errorCode = nullptr;
    const char* errorMessage = nullptr;
    if (strlen(blockId) == 0 || !parseRemoteBlock(blockJson, block, errorCode, errorMessage)) {
      sendAck(id, false, errorCode != nullptr ? errorCode : "invalid_block", errorMessage != nullptr ? errorMessage : "Invalid block");
      return;
    }
    const SubmitRemoteBlockResult result = app_.submitRemoteBlock(block, blockId);
    sendAck(id, result.accepted, result.rejectionCode, result.rejectionMessage);
  }

  bool parseRemoteBlock(JsonVariant json, RemoteMotionBlock& block, const char*& code, const char*& message) {
    if (!json.is<JsonObject>()) {
      code = "invalid_block";
      message = "Block must be an object";
      return false;
    }
    const String kind = json["kind"] | "";
    block.profile = {};
    JsonVariant profile = json["profile"];
    if (profile.is<JsonObject>()) {
      if (!profile["rpm"].isNull()) {
        const int rpm = profile["rpm"] | 0;
        if (rpm <= 0 || rpm > 3000) {
          code = "invalid_block";
          message = "Invalid rpm";
          return false;
        }
        block.profile.hasRpm = true;
        block.profile.rpm = static_cast<uint16_t>(rpm);
      }
      if (!profile["accelRaw"].isNull()) {
        const int accel = profile["accelRaw"] | 0;
        if (accel <= 0 || accel > 255) {
          code = "invalid_block";
          message = "Invalid accelRaw";
          return false;
        }
        block.profile.hasAccelRaw = true;
        block.profile.accelRaw = static_cast<uint8_t>(accel);
      }
      if (!profile["timeoutMs"].isNull()) {
        const uint32_t timeout = profile["timeoutMs"] | 0;
        if (timeout == 0 || timeout > 120000) {
          code = "invalid_block";
          message = "Invalid timeoutMs";
          return false;
        }
        block.profile.hasTimeoutMs = true;
        block.profile.timeoutMs = timeout;
      }
    }
    if (kind == "move_xy") {
      block.kind = RemoteMotionBlockKind::MoveXY;
      block.dxMm = json["dxMm"] | NAN;
      block.dyMm = json["dyMm"] | NAN;
      return true;
    }
    if (kind == "servo_press") {
      block.kind = RemoteMotionBlockKind::ServoPress;
      return true;
    }
    if (kind == "servo_release") {
      block.kind = RemoteMotionBlockKind::ServoRelease;
      return true;
    }
    if (kind == "character_release") {
      block.kind = RemoteMotionBlockKind::CharacterRelease;
      return true;
    }
    if (kind == "line_feed") {
      block.kind = RemoteMotionBlockKind::LineFeed;
      return true;
    }
    if (kind == "wait") {
      block.kind = RemoteMotionBlockKind::Wait;
      block.durationMs = json["durationMs"] | 0;
      if (block.durationMs == 0 || block.durationMs > 65535) {
        code = "invalid_block";
        message = "Invalid wait duration";
        return false;
      }
      return true;
    }
    code = "invalid_block";
    message = "Unknown block kind";
    return false;
  }

  void sendBlockEvents() {
    if (!client_ || !client_.connected()) {
      return;
    }
    char blockId[48];
    if (app_.consumeRemoteBlockStarted(blockId, sizeof(blockId))) {
      StaticJsonDocument<128> doc;
      doc["v"] = 1;
      doc["type"] = "block_started";
      doc["blockId"] = blockId;
      sendJsonLine(client_, doc);
    }
    uint32_t durationMs = 0;
    if (app_.consumeRemoteBlockDone(blockId, sizeof(blockId), durationMs)) {
      StaticJsonDocument<160> doc;
      doc["v"] = 1;
      doc["type"] = "block_done";
      doc["blockId"] = blockId;
      doc["durationMs"] = durationMs;
      sendJsonLine(client_, doc);
    }
    if (app_.mode() == DeviceMode::Faulted) {
      const JobSnapshot snapshot = app_.snapshot();
      StaticJsonDocument<192> doc;
      doc["v"] = 1;
      doc["type"] = "fault";
      doc["code"] = snapshot.faultCode;
      doc["message"] = snapshot.faultMessage;
      sendJsonLine(client_, doc);
    }
  }

  void sendTelemetry() {
    if (!client_ || !client_.connected()) {
      return;
    }
    const uint32_t nowMs = millis();
    if (lastTelemetryMs_ != 0 && nowMs - lastTelemetryMs_ < kTelemetryIntervalMs) {
      return;
    }
    lastTelemetryMs_ = nowMs;
    StaticJsonDocument<1536> doc;
    doc["v"] = 1;
    doc["type"] = "telemetry";
    doc["executor"] = app_.mode() == DeviceMode::Faulted ? "faulted" : (app_.executorRunning() ? "running" : "idle");
    const JobSnapshot snapshot = app_.snapshot();
    doc["jobState"] = jobStateText(snapshot.state);
    if (app_.mode() == DeviceMode::Faulted) {
      JsonObject fault = doc.createNestedObject("fault");
      fault["code"] = snapshot.faultCode;
      fault["message"] = snapshot.faultMessage;
    }
    const CanBusDiagnostics can = app_.canDiagnostics();
    JsonObject canJson = doc.createNestedObject("can");
    canJson["driverReady"] = can.driverReady;
    canJson["motionReady"] = can.motionReady;
    canJson["fatalFault"] = can.fatalFault;
    canJson["txFailedCount"] = can.txFailedCount;
    canJson["busOffCount"] = can.busOffCount;
    JsonArray motors = doc.createNestedArray("motors");
    writeTelemetryMotor(motors, config_.topology.xMotorId);
    writeTelemetryMotor(motors, config_.topology.yLeftMotorId);
    writeTelemetryMotor(motors, config_.topology.yRightMotorId);
    writeTelemetryMotor(motors, config_.topology.lineFeedMotorId);
    sendJsonLine(client_, doc);
  }

  void writeTelemetryMotor(JsonArray motors, uint8_t motorId) {
    const MotorState state = app_.motorState(motorId);
    JsonObject motor = motors.createNestedObject();
    motor["id"] = motorId;
    motor["role"] = motorRoleText(state.role);
    motor["rpm"] = state.velocityRpm;
    motor["inputPulse"] = state.inputPulseSteps;
    motor["angle"] = state.realtimeAngleRaw65536;
    motor["fresh"] = state.hasRecentInputPulse && state.hasRecentVelocity;
  }

  void sendAck(const char* id, bool accepted, const char* code, const char* message) {
    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["type"] = "ack";
    doc["id"] = id;
    doc["accepted"] = accepted;
    if (!accepted) {
      doc["code"] = code != nullptr ? code : "rejected";
      doc["message"] = message != nullptr ? message : "Command rejected";
    }
    sendJsonLine(client_, doc);
  }

  void sendError(WiFiClient& client, const char* id, const char* code, const char* message) {
    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["type"] = "ack";
    doc["id"] = id;
    doc["accepted"] = false;
    doc["code"] = code;
    doc["message"] = message;
    sendJsonLine(client, doc);
  }

  template <typename T>
  void sendJsonLine(WiFiClient& client, T& doc) {
    if (!client || !client.connected()) {
      return;
    }
    serializeJson(doc, client);
    client.write('\n');
  }

  static const char* jobStateText(JobState state) {
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

  static const char* motorRoleText(MotorRole role) {
    switch (role) {
      case MotorRole::YLeft:
        return "y_left";
      case MotorRole::YRight:
        return "y_right";
      case MotorRole::LineFeed:
        return "line_feed";
      case MotorRole::X:
      default:
        return "x";
    }
  }

  const TypingConfig& config_;
  AutoTyperApplication& app_;
  Print& log_;
  WiFiServer server_;
  WiFiClient client_;
  char line_[kMaxLineBytes];
  size_t lineLength_;
  bool lineOverflow_;
  uint32_t lastTelemetryMs_;
};

}  // namespace auto_typer
