#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

#include "../auto_typer_runtime.h"
#include "../protocol/DesktopCommand.h"
#include "FrameCodec.h"

namespace auto_typer {

static constexpr uint16_t kDesktopLinkPort = 7777;

class DesktopLinkServer {
 public:
  DesktopLinkServer(const TypingConfig& config, AutoTyperApplication& app, Print& log)
      : config_(config),
        app_(app),
        log_(log),
        server_(kDesktopLinkPort),
        handshaken_(false),
        txSeq_(0),
        lastTelemetryMs_(0) {}

  void begin() {
    server_.begin();
    server_.setNoDelay(true);
    log_.print("Desktop link TCP ready on port ");
    log_.println(kDesktopLinkPort);
  }

  void tick() {
    acceptClient();
    readClient();
    sendDoneOrFault();
    sendTelemetry();
  }

 private:
  static constexpr uint32_t kTelemetryIntervalMs = 350;

  void acceptClient() {
    WiFiClient incoming = server_.available();
    if (!incoming) {
      return;
    }
    incoming.setNoDelay(true);
    if (client_ && client_.connected()) {
      sendFault(incoming, "", "busy", "Another desktop client is connected");
      incoming.stop();
      return;
    }
    client_.stop();
    client_ = incoming;
    codec_.reset();
    handshaken_ = false;
    lastTelemetryMs_ = 0;
    log_.println("[link] client connected");
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
      const FrameCodec::ReadResult result = codec_.push(static_cast<uint8_t>(value));
      if (result == FrameCodec::ReadResult::ProtocolError) {
        sendFault(client_, "", codec_.protocolError(), "Invalid desktop link frame");
        disconnectClient();
        return;
      }
      DecodedFrame frame{};
      while (codec_.takeFrame(frame)) {
        handleFrame(frame);
        codec_.consumeFrame();
        if (!client_ || !client_.connected()) {
          return;
        }
      }
    }
  }

  void handleFrame(const DecodedFrame& frame) {
    DynamicJsonDocument request(frame.payloadLen + 512);
    const DeserializationError error = deserializeJson(request, frame.payload, frame.payloadLen);
    if (error) {
      sendFault(client_, "", "invalid_json", "Invalid JSON payload");
      return;
    }
    if (frame.type == DesktopFrameType::Ping) {
      sendPong();
      return;
    }
    if (!handshaken_) {
      if (frame.type != DesktopFrameType::Hello) {
        sendFault(client_, "", "handshake_required", "Hello is required before commands");
        disconnectClient();
        return;
      }
      if ((request["v"] | 0) != 1 || String(request["type"] | "") != "hello") {
        sendFault(client_, "", "invalid_hello", "Invalid hello payload");
        disconnectClient();
        return;
      }
      handshaken_ = true;
      sendHelloAck();
      sendSnapshot();
      return;
    }
    if (frame.type != DesktopFrameType::Command) {
      sendFault(client_, "", "unexpected_frame", "Unexpected desktop link frame type");
      return;
    }
    handleCommand(request);
  }

  void handleCommand(JsonDocument& request) {
    DesktopCommand command{};
    const DesktopCommandParseResult parsed = parseDesktopCommand(request, command);
    if (!parsed.ok) {
      const char* id = request["id"] | "";
      sendAck(id, false, parsed.code, parsed.message);
      return;
    }
    const SubmitRemoteCommandResult result = app_.submitRemoteCommand(command);
    sendAck(command.id, result.accepted, result.code, result.message);
  }

  void sendDoneOrFault() {
    if (!client_ || !client_.connected()) {
      return;
    }
    char commandId[49];
    const char* op = "";
    uint32_t durationMs = 0;
    MachinePointMm point{};
    if (app_.consumeRemoteDone(commandId, sizeof(commandId), op, durationMs, point)) {
      sendDone(commandId, op, durationMs, point);
    }
    const char* code = "";
    const char* message = "";
    if (app_.consumeRemoteFault(commandId, sizeof(commandId), code, message)) {
      sendFault(client_, commandId, code, message);
    }
  }

  void sendHelloAck() {
    StaticJsonDocument<512> doc;
    doc["v"] = 1;
    doc["type"] = "hello_ack";
    writeSnapshot(doc.createNestedObject("snapshot"));
    sendJson(DesktopFrameType::HelloAck, doc);
  }

  void sendSnapshot() {
    if (!client_ || !client_.connected()) {
      return;
    }
    StaticJsonDocument<512> doc;
    doc["v"] = 1;
    doc["type"] = "snapshot";
    writeSnapshot(doc.createNestedObject("snapshot"));
    sendJson(DesktopFrameType::Snapshot, doc);
  }

  void sendTelemetry() {
    if (!client_ || !client_.connected() || !handshaken_) {
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
    if (app_.currentRemoteCommandId()[0] != '\0') {
      doc["currentCommandId"] = app_.currentRemoteCommandId();
    }
    if (app_.lastCompletedRemoteCommandId()[0] != '\0') {
      doc["lastCompletedCommandId"] = app_.lastCompletedRemoteCommandId();
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
    sendJson(DesktopFrameType::Telemetry, doc);
  }

  void writeSnapshot(JsonObject snapshot) {
    snapshot["mode"] = modeText(app_.mode());
    if (app_.currentRemoteCommandId()[0] != '\0') {
      snapshot["currentCommandId"] = app_.currentRemoteCommandId();
    }
    if (app_.lastCompletedRemoteCommandId()[0] != '\0') {
      snapshot["lastCompletedCommandId"] = app_.lastCompletedRemoteCommandId();
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

  void sendAck(const char* id, bool ok, const char* code, const char* message) {
    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["type"] = "ack";
    doc["id"] = id != nullptr ? id : "";
    doc["ok"] = ok;
    doc["accepted"] = ok;
    if (code != nullptr && code[0] != '\0') {
      doc["code"] = code;
    }
    if (message != nullptr && message[0] != '\0') {
      doc["message"] = message;
    }
    sendJson(DesktopFrameType::Ack, doc);
  }

  void sendDone(const char* id, const char* op, uint32_t durationMs, MachinePointMm point) {
    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["type"] = "done";
    doc["id"] = id != nullptr ? id : "";
    if (op != nullptr && op[0] != '\0') {
      doc["op"] = op;
    }
    doc["durationMs"] = durationMs;
    JsonObject jsonPoint = doc.createNestedObject("currentPoint");
    jsonPoint["xMm"] = point.xMm;
    jsonPoint["yMm"] = point.yMm;
    sendJson(DesktopFrameType::Done, doc);
  }

  void sendFault(WiFiClient& client, const char* id, const char* code, const char* message) {
    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["type"] = "fault";
    if (id != nullptr && id[0] != '\0') {
      doc["id"] = id;
    }
    doc["code"] = code != nullptr ? code : "fault";
    doc["message"] = message != nullptr ? message : "Desktop link fault";
    sendJson(client, DesktopFrameType::Fault, doc);
  }

  void sendPong() {
    StaticJsonDocument<64> doc;
    doc["v"] = 1;
    doc["type"] = "pong";
    sendJson(DesktopFrameType::Pong, doc);
  }

  template <typename TDoc>
  bool sendJson(DesktopFrameType type, TDoc& doc) {
    return sendJson(client_, type, doc);
  }

  template <typename TDoc>
  bool sendJson(WiFiClient& client, DesktopFrameType type, TDoc& doc) {
    ++txSeq_;
    return FrameCodec::writeJsonFrame(client, type, txSeq_, doc);
  }

  void disconnectClient() {
    client_.stop();
    codec_.reset();
    handshaken_ = false;
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
  FrameCodec codec_;
  bool handshaken_;
  uint32_t txSeq_;
  uint32_t lastTelemetryMs_;
};

}  // namespace auto_typer
