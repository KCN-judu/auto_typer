#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

#include "../auto_typer_runtime.h"
#include "GroupCommandProtocol.h"

namespace auto_typer {

static constexpr uint16_t kGroupCommandPort = 7777;

class GroupCommandServer {
 public:
  GroupCommandServer(const TypingConfig& config, AutoTyperApplication& app, Print& log)
      : config_(config),
        app_(app),
        log_(log),
        server_(kGroupCommandPort),
        handshaken_(false),
        lastTelemetryMs_(0) {}

  void begin() {
    server_.begin();
    server_.setNoDelay(true);
    log_.print("[group] NDJSON TCP ready on port ");
    log_.println(kGroupCommandPort);
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
      sendFault(incoming, "", "busy", "Another group client is connected");
      incoming.stop();
      return;
    }
    client_.stop();
    client_ = incoming;
    lineBuffer_ = "";
    handshaken_ = false;
    lastTelemetryMs_ = 0;
    log_.println("[group] client connected");
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
        sendFault(client_, "", "line_too_large", "Group stream inbound line is too large");
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
        sendFault(client_, id, "handshake_required", "Hello is required before group commands");
        disconnectClient();
        return;
      }
      if ((request["v"] | 0) != 1 || strcmp(request["client"] | "", "desktop") != 0) {
        sendAck(id, false, "invalid_hello", "Invalid hello payload");
        disconnectClient();
        return;
      }
      handshaken_ = true;
      log_.println("[group] hello received");
      sendAck(id, true, "", "");
      sendSnapshot();
      return;
    }
    if (strcmp(type, "exec_group") == 0) {
      handleExecGroup(request);
      return;
    }
    if (strcmp(type, "task_end") == 0) {
      handleTaskEnd(request);
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
      const bool accepted = app_.probeMotors();
      sendAck(id, accepted, accepted ? "" : "probe_rejected", accepted ? "" : "Motor probe rejected");
      sendTelemetry(true);
      return;
    }
    sendAck(id, false, "unknown_type", "Unsupported group stream message type");
  }

  void handleExecGroup(JsonDocument& request) {
    const char* id = request["id"] | "";
    const char* groupId = request["groupId"] | "";
    if (groupId[0] == '\0') {
      sendAck(id, false, "invalid_group", "groupId is required");
      return;
    }
    RemoteMotionStep steps[kRemoteGroupMaxSteps];
    size_t count = 0;
    const char* parseCode = "";
    const char* parseMessage = "";
    if (!parseRemoteGroup(request["steps"], steps, kRemoteGroupMaxSteps, count, parseCode, parseMessage)) {
      sendAck(id, false, parseCode, parseMessage);
      return;
    }
    log_.print("[group] exec_group groupId=");
    log_.print(groupId);
    log_.print(" count=");
    log_.println(count);
    const SubmitRemoteGroupResult result = app_.submitRemoteGroup(steps, count, groupId);
    sendAck(id, result.accepted, result.rejectionCode, result.rejectionMessage);
  }

  void handleTaskEnd(JsonDocument& request) {
    const char* id = request["id"] | "";
    const char* taskId = request["taskId"] | "";
    if (taskId[0] == '\0') {
      sendAck(id, false, "invalid_task_end", "taskId is required");
      return;
    }
    const bool accepted = app_.finishRemoteTask();
    sendAck(id, accepted, accepted ? "" : "task_end_rejected", accepted ? "" : "Task end rejected while device is busy");
    if (accepted) {
      sendSnapshot();
    }
  }

  void sendPendingEvents() {
    if (!client_ || !client_.connected()) {
      return;
    }
    char groupId[49];
    if (app_.consumeRemoteGroupStarted(groupId, sizeof(groupId))) {
      sendGroupStarted(groupId);
    }
    uint32_t durationMs = 0;
    if (app_.consumeRemoteGroupDone(groupId, sizeof(groupId), durationMs)) {
      sendGroupDone(groupId, durationMs);
    }
    const char* warnCode = "";
    const char* warnMessage = "";
    if (app_.consumeRemoteGroupWarn(groupId, sizeof(groupId), warnCode, warnMessage)) {
      sendGroupWarn(groupId, warnCode, warnMessage);
    }
    const char* code = "";
    const char* message = "";
    if (app_.consumeRemoteFault(groupId, sizeof(groupId), code, message)) {
      sendFault(client_, groupId, code, message);
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
    log_.print("[group] ack accepted=");
    log_.print(accepted ? "true" : "false");
    if (code != nullptr && code[0] != '\0') {
      log_.print(" code=");
      log_.print(code);
    }
    log_.println();
    sendJson(client_, doc);
  }

  void sendGroupStarted(const char* groupId) {
    StaticJsonDocument<128> doc;
    doc["v"] = 1;
    doc["type"] = "group_started";
    doc["groupId"] = groupId != nullptr ? groupId : "";
    log_.print("[group] group_started ");
    log_.println(groupId != nullptr ? groupId : "");
    sendJson(client_, doc);
  }

  void sendGroupDone(const char* groupId, uint32_t durationMs) {
    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["type"] = "group_done";
    doc["groupId"] = groupId != nullptr ? groupId : "";
    doc["durationMs"] = durationMs;
    JsonObject point = doc.createNestedObject("currentPoint");
    const MachinePointMm current = app_.currentPoint();
    point["xMm"] = current.xMm;
    point["yMm"] = current.yMm;
    log_.print("[group] group_done ");
    log_.println(groupId != nullptr ? groupId : "");
    sendJson(client_, doc);
  }

  void sendGroupWarn(const char* groupId, const char* code, const char* message) {
    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["type"] = "group_warn";
    doc["groupId"] = groupId != nullptr ? groupId : "";
    doc["code"] = code != nullptr ? code : "group_warning";
    doc["message"] = message != nullptr ? message : "Remote group completed with warning";
    JsonObject point = doc.createNestedObject("currentPoint");
    const MachinePointMm current = app_.currentPoint();
    point["xMm"] = current.xMm;
    point["yMm"] = current.yMm;
    log_.print("[group] group_warn ");
    log_.println(groupId != nullptr ? groupId : "");
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
    doc["message"] = message != nullptr ? message : "Group stream fault";
    log_.print("[group] fault ");
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
    DynamicJsonDocument doc(1536);
    doc["v"] = 1;
    doc["type"] = "telemetry";
    doc["executor"] = app_.mode() == DeviceMode::Faulted ? "faulted" : (app_.executorRunning() ? "running" : "idle");
    const JobSnapshot snapshot = app_.snapshot();
    doc["jobState"] = jobStateText(snapshot.state);
    if (app_.currentRemoteCommandId()[0] != '\0') {
      doc["currentGroupId"] = app_.currentRemoteCommandId();
    }
    if (app_.lastCompletedRemoteCommandId()[0] != '\0') {
      doc["lastCompletedGroupId"] = app_.lastCompletedRemoteCommandId();
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
      snapshot["currentGroupId"] = app_.currentRemoteCommandId();
    }
    if (app_.lastCompletedRemoteCommandId()[0] != '\0') {
      snapshot["lastCompletedGroupId"] = app_.lastCompletedRemoteCommandId();
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
    motor["readiness"] = motorReadinessText(state.readiness);
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
    log_.println("[group] disconnect");
    client_.stop();
    lineBuffer_ = "";
    handshaken_ = false;
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
