#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <math.h>

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
        handshaken_(false),
        lastTelemetryMs_(0) {}

  void begin() {
    server_.begin();
    server_.setNoDelay(true);
    log_.print("[block] NDJSON TCP ready on port ");
    log_.println(kBlockCommandPort);
  }

  void tick() {
    acceptClient();
    readClient();
    sendPendingEvents();
    sendTelemetry();
  }

 private:
  static constexpr size_t kMaxLineBytes = 4096;
  static constexpr uint32_t kTelemetryIntervalMs = 350;

  void acceptClient() {
    WiFiClient incoming = server_.available();
    if (!incoming) {
      return;
    }
    incoming.setNoDelay(true);
    if (client_ && client_.connected()) {
      sendFault(incoming, "", "busy", "Another block client is connected");
      incoming.stop();
      return;
    }
    client_.stop();
    client_ = incoming;
    lineBuffer_ = "";
    handshaken_ = false;
    lastTelemetryMs_ = 0;
    log_.println("[block] client connected");
  }

  void readClient() {
    if (!client_) {
      return;
    }
    if (!client_.connected()) {
      disconnectClient();
      return;
    }
    uint16_t reads = 0;
    while (client_.available() > 0 && reads < 512) {
      const int value = client_.read();
      if (value < 0) {
        break;
      }
      ++reads;
      if (value == '\r') {
        continue;
      }
      if (value == '\n') {
        handleLine(lineBuffer_);
        lineBuffer_ = "";
        if (!client_ || !client_.connected()) {
          return;
        }
        continue;
      }
      if (lineBuffer_.length() >= kMaxLineBytes) {
        sendFault(client_, "", "line_too_large", "Block stream inbound line is too large");
        disconnectClient();
        return;
      }
      lineBuffer_ += static_cast<char>(value);
    }
  }

  void handleLine(const String& line) {
    if (line.length() == 0) {
      return;
    }
    DynamicJsonDocument request(line.length() + 512);
    const DeserializationError error = deserializeJson(request, line);
    if (error) {
      sendFault(client_, "", "invalid_json", "Invalid JSON line");
      return;
    }
    const char* type = request["type"] | "";
    const char* id = request["id"] | "";
    if (strcmp(type, "ping") == 0) {
      sendPong(id);
      return;
    }
    if (!handshaken_) {
      if (strcmp(type, "hello") != 0) {
        sendFault(client_, id, "handshake_required", "Hello is required before block commands");
        disconnectClient();
        return;
      }
      if ((request["v"] | 0) != 1 || strcmp(request["client"] | "", "desktop") != 0) {
        sendAck(id, false, "invalid_hello", "Invalid hello payload");
        disconnectClient();
        return;
      }
      handshaken_ = true;
      log_.println("[block] hello received");
      sendAck(id, true, "", "");
      sendSnapshot();
      return;
    }
    if (strcmp(type, "exec_block") == 0) {
      handleExecBlock(request);
      return;
    }
    if (strcmp(type, "cancel") == 0) {
      const bool accepted = app_.cancelCurrentJob();
      sendAck(id, accepted, accepted ? "" : "cancel_rejected", accepted ? "" : "No queued or running job");
      return;
    }
    if (strcmp(type, "reset_fault") == 0) {
      const bool accepted = app_.resetFault();
      sendAck(id, accepted, accepted ? "" : "reset_rejected", accepted ? "" : "Fault reset rejected");
      return;
    }
    if (strcmp(type, "probe") == 0) {
      sendAck(id, true, "", "");
      sendTelemetry(true);
      return;
    }
    sendAck(id, false, "unknown_type", "Unsupported block stream message type");
  }

  void handleExecBlock(JsonDocument& request) {
    const char* id = request["id"] | "";
    const char* blockId = request["blockId"] | "";
    if (blockId[0] == '\0') {
      sendAck(id, false, "invalid_block", "blockId is required");
      return;
    }
    RemoteMotionBlock block{};
    const char* parseCode = "";
    const char* parseMessage = "";
    if (!parseRemoteBlock(request["block"], block, parseCode, parseMessage)) {
      sendAck(id, false, parseCode, parseMessage);
      return;
    }
    log_.print("[block] exec_block blockId=");
    log_.print(blockId);
    log_.print(" kind=");
    log_.println(remoteBlockKindText(block.kind));
    const SubmitRemoteBlockResult result = app_.submitRemoteBlock(block, blockId);
    sendAck(id, result.accepted, result.rejectionCode, result.rejectionMessage);
  }

  bool parseRemoteBlock(JsonVariantConst value,
                        RemoteMotionBlock& out,
                        const char*& code,
                        const char*& message) const {
    if (!value.is<JsonObjectConst>()) {
      code = "invalid_block";
      message = "block object is required";
      return false;
    }
    JsonObjectConst block = value.as<JsonObjectConst>();
    const char* kind = block["kind"] | "";
    out = RemoteMotionBlock{};
    if (strcmp(kind, "move_xy") == 0) {
      out.kind = RemoteMotionBlockKind::MoveXY;
      out.dxMm = block["dxMm"] | NAN;
      out.dyMm = block["dyMm"] | NAN;
      if (!isfinite(out.dxMm) || !isfinite(out.dyMm)) {
        code = "invalid_block";
        message = "move_xy dxMm and dyMm must be finite";
        return false;
      }
      parseProfile(block["profile"], out.profile);
      return true;
    }
    if (strcmp(kind, "servo_press") == 0) {
      out.kind = RemoteMotionBlockKind::ServoPress;
      return true;
    }
    if (strcmp(kind, "servo_release") == 0) {
      out.kind = RemoteMotionBlockKind::ServoRelease;
      return true;
    }
    if (strcmp(kind, "character_release") == 0) {
      out.kind = RemoteMotionBlockKind::CharacterRelease;
      return true;
    }
    if (strcmp(kind, "line_feed") == 0) {
      out.kind = RemoteMotionBlockKind::LineFeed;
      return true;
    }
    if (strcmp(kind, "wait") == 0) {
      const uint32_t durationMs = block["durationMs"] | 0;
      if (durationMs > 65535) {
        code = "invalid_block";
        message = "wait durationMs is too large";
        return false;
      }
      out.kind = RemoteMotionBlockKind::Wait;
      out.durationMs = static_cast<uint16_t>(durationMs);
      return true;
    }
    code = "invalid_block";
    message = "Unsupported remote block kind";
    return false;
  }

  static void parseProfile(JsonVariantConst value, RemoteMotionProfile& profile) {
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

  void sendPendingEvents() {
    if (!client_ || !client_.connected()) {
      return;
    }
    char blockId[49];
    if (app_.consumeRemoteBlockStarted(blockId, sizeof(blockId))) {
      sendBlockStarted(blockId);
    }
    uint32_t durationMs = 0;
    if (app_.consumeRemoteBlockDone(blockId, sizeof(blockId), durationMs)) {
      sendBlockDone(blockId, durationMs);
    }
    const char* code = "";
    const char* message = "";
    if (app_.consumeRemoteFault(blockId, sizeof(blockId), code, message)) {
      sendFault(client_, blockId, code, message);
    }
  }

  void sendAck(const char* id, bool accepted, const char* code, const char* message) {
    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["type"] = "ack";
    doc["id"] = id != nullptr ? id : "";
    doc["accepted"] = accepted;
    doc["ok"] = accepted;
    if (code != nullptr && code[0] != '\0') {
      doc["code"] = code;
    }
    if (message != nullptr && message[0] != '\0') {
      doc["message"] = message;
    }
    log_.print("[block] ack accepted=");
    log_.print(accepted ? "true" : "false");
    if (code != nullptr && code[0] != '\0') {
      log_.print(" code=");
      log_.print(code);
    }
    log_.println();
    sendJson(client_, doc);
  }

  void sendBlockStarted(const char* blockId) {
    StaticJsonDocument<128> doc;
    doc["v"] = 1;
    doc["type"] = "block_started";
    doc["blockId"] = blockId != nullptr ? blockId : "";
    log_.print("[block] block_started ");
    log_.println(blockId != nullptr ? blockId : "");
    sendJson(client_, doc);
  }

  void sendBlockDone(const char* blockId, uint32_t durationMs) {
    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["type"] = "block_done";
    doc["blockId"] = blockId != nullptr ? blockId : "";
    doc["durationMs"] = durationMs;
    JsonObject point = doc.createNestedObject("currentPoint");
    const MachinePointMm current = app_.currentPoint();
    point["xMm"] = current.xMm;
    point["yMm"] = current.yMm;
    log_.print("[block] block_done ");
    log_.println(blockId != nullptr ? blockId : "");
    sendJson(client_, doc);
  }

  void sendFault(WiFiClient& client, const char* id, const char* code, const char* message) {
    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["type"] = "fault";
    if (id != nullptr && id[0] != '\0') {
      doc["id"] = id;
    }
    doc["code"] = code != nullptr ? code : "fault";
    doc["message"] = message != nullptr ? message : "Block stream fault";
    log_.print("[block] fault ");
    log_.println(code != nullptr ? code : "fault");
    sendJson(client, doc);
  }

  void sendPong(const char* id) {
    StaticJsonDocument<96> doc;
    doc["v"] = 1;
    doc["type"] = "pong";
    doc["id"] = id != nullptr ? id : "";
    sendJson(client_, doc);
  }

  void sendSnapshot() {
    if (!client_ || !client_.connected()) {
      return;
    }
    StaticJsonDocument<512> doc;
    doc["v"] = 1;
    doc["type"] = "snapshot";
    writeSnapshot(doc.createNestedObject("snapshot"));
    sendJson(client_, doc);
  }

  void sendTelemetry(bool force = false) {
    if (!client_ || !client_.connected() || !handshaken_) {
      return;
    }
    const uint32_t nowMs = millis();
    if (!force && lastTelemetryMs_ != 0 && nowMs - lastTelemetryMs_ < kTelemetryIntervalMs) {
      return;
    }
    lastTelemetryMs_ = nowMs;
    StaticJsonDocument<1536> doc;
    doc["v"] = 1;
    doc["type"] = "telemetry";
    doc["executor"] = app_.mode() == DeviceMode::Faulted ? "faulted" : (app_.executorRunning() ? "running" : "idle");
    const JobSnapshot snapshot = app_.snapshot();
    doc["jobState"] = jobStateText(snapshot.state);
    if (app_.currentRemoteCommandId()[0] != '\0') {
      doc["currentBlockId"] = app_.currentRemoteCommandId();
    }
    if (app_.lastCompletedRemoteCommandId()[0] != '\0') {
      doc["lastCompletedBlockId"] = app_.lastCompletedRemoteCommandId();
    }
    JsonObject point = doc.createNestedObject("currentPoint");
    const MachinePointMm current = app_.currentPoint();
    point["xMm"] = current.xMm;
    point["yMm"] = current.yMm;
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
    canJson["txRetryCount"] = can.txRetryCount;
    canJson["busOffCount"] = can.busOffCount;
    canJson["lastFaultCode"] = can.lastFaultCode;
    canJson["lastFaultMessage"] = can.lastFaultMessage;
    JsonArray motors = doc.createNestedArray("motors");
    writeTelemetryMotor(motors, config_.topology.xMotorId);
    writeTelemetryMotor(motors, config_.topology.yLeftMotorId);
    writeTelemetryMotor(motors, config_.topology.yRightMotorId);
    writeTelemetryMotor(motors, config_.topology.lineFeedMotorId);
    writeTelemetryMotor(motors, config_.topology.pressMotorId);
    sendJson(client_, doc);
  }

  void writeSnapshot(JsonObject snapshot) {
    snapshot["mode"] = modeText(app_.mode());
    if (app_.currentRemoteCommandId()[0] != '\0') {
      snapshot["currentBlockId"] = app_.currentRemoteCommandId();
    }
    if (app_.lastCompletedRemoteCommandId()[0] != '\0') {
      snapshot["lastCompletedBlockId"] = app_.lastCompletedRemoteCommandId();
    }
    JsonObject point = snapshot.createNestedObject("currentPoint");
    const MachinePointMm current = app_.currentPoint();
    point["xMm"] = current.xMm;
    point["yMm"] = current.yMm;
    if (app_.mode() == DeviceMode::Faulted) {
      const JobSnapshot job = app_.snapshot();
      JsonObject fault = snapshot.createNestedObject("fault");
      fault["code"] = job.faultCode;
      fault["message"] = job.faultMessage;
    }
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

  template <typename TDoc>
  bool sendJson(WiFiClient& client, TDoc& doc) {
    if (!client || !client.connected()) {
      return false;
    }
    serializeJson(doc, client);
    client.write('\n');
    return true;
  }

  void disconnectClient() {
    log_.println("[block] disconnect");
    client_.stop();
    lineBuffer_ = "";
    handshaken_ = false;
  }

  static const char* remoteBlockKindText(RemoteMotionBlockKind kind) {
    switch (kind) {
      case RemoteMotionBlockKind::MoveXY:
        return "move_xy";
      case RemoteMotionBlockKind::ServoPress:
        return "servo_press";
      case RemoteMotionBlockKind::ServoRelease:
        return "servo_release";
      case RemoteMotionBlockKind::CharacterRelease:
        return "character_release";
      case RemoteMotionBlockKind::LineFeed:
        return "line_feed";
      case RemoteMotionBlockKind::Wait:
      default:
        return "wait";
    }
  }

  static const char* modeText(DeviceMode mode) {
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
      case MotorRole::Press:
        return "press";
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
  String lineBuffer_;
  bool handshaken_;
  uint32_t lastTelemetryMs_;
};

}  // namespace auto_typer
