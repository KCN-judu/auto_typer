#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

#include "../auto_typer_runtime.h"
#include "MotionProtocolParser.h"

namespace auto_typer {

static constexpr uint16_t kMotionProtocolPort = 7777;
static constexpr uint32_t kMotionHandshakeTimeoutMs = 3000;

class MotionProtocolServer {
 public:
  MotionProtocolServer(const TypingConfig& config, AutoTyperApplication& app, Print& log)
      : config_(config),
        app_(app),
        log_(log),
        server_(kMotionProtocolPort),
        handshaken_(false),
        connectedAtMs_(0),
        discardingOversizeLine_(false) {}

  void begin() {
    server_.begin();
    server_.setNoDelay(true);
    log_.print("[tcp] atomic motion protocol ready on port ");
    log_.println(kMotionProtocolPort);
  }

  void tick() {
    closeStaleHandshakeClient();
    detectDisconnect();
    drainRuntimeEvents();
    acceptClient();
    readClient();
    drainRuntimeEvents();
  }

 private:
  void acceptClient() {
    WiFiClient incoming = server_.available();
    if (!incoming) {
      return;
    }
    incoming.setNoDelay(true);
    if (client_ && client_.connected()) {
      sendProtocolError(incoming, "", "device_busy", "Another TCP client is connected");
      incoming.stop();
      return;
    }
    client_.stop();
    client_ = incoming;
    lineBuffer_ = "";
    discardingOversizeLine_ = false;
    handshaken_ = false;
    connectedAtMs_ = millis();
    log_.print("[tcp] client connected remote=");
    log_.println(client_.remoteIP().toString());
  }

  void detectDisconnect() {
    if (client_ && !client_.connected()) {
      disconnectClient();
    }
  }

  void closeStaleHandshakeClient() {
    if (!client_ || !client_.connected() || handshaken_ ||
        millis() - connectedAtMs_ < kMotionHandshakeTimeoutMs) {
      return;
    }
    log_.println("[tcp] handshake timeout");
    disconnectClient();
  }

  void disconnectClient() {
    app_.cancelQueuedRemoteBlock(nullptr, 0, false);
    client_.stop();
    lineBuffer_ = "";
    discardingOversizeLine_ = false;
    handshaken_ = false;
  }

  void readClient() {
    if (!client_ || !client_.connected()) {
      return;
    }
    uint16_t reads = 0;
    while (client_.available() > 0 && reads < 512) {
      const int value = client_.read();
      if (value < 0) {
        return;
      }
      ++reads;
      if (value == '\r') {
        continue;
      }
      if (discardingOversizeLine_) {
        if (value == '\n') {
          discardingOversizeLine_ = false;
        }
        continue;
      }
      if (value == '\n') {
        handleLine(lineBuffer_);
        lineBuffer_ = "";
        continue;
      }
      if (lineBuffer_.length() >= kMaxTcpMessageBytes) {
        sendProtocolError(client_, "", "message_too_large", "TCP line exceeds maxMessageBytes");
        lineBuffer_ = "";
        discardingOversizeLine_ = true;
        continue;
      }
      lineBuffer_ += static_cast<char>(value);
    }
  }

  void handleLine(const String& line) {
    if (line.length() == 0) {
      return;
    }
    DynamicJsonDocument request(line.length() + 512);
    if (deserializeJson(request, line)) {
      sendProtocolError(client_, "", "invalid_json", "Invalid JSON line");
      return;
    }
    const char* requestId = request["requestId"] | "";
    const char* type = request["type"] | "";
    if ((request["v"] | 0) != 1) {
      sendProtocolError(client_, requestId, "unsupported_version", "Only protocol version 1 is supported");
      return;
    }
    if (requestId[0] == '\0') {
      sendProtocolError(client_, "", "invalid_request", "requestId is required");
      return;
    }
    if (!handshaken_) {
      if (strcmp(type, "handshake") != 0) {
        sendProtocolError(client_, requestId, "handshake_required", "handshake is required before commands");
        return;
      }
      handshaken_ = true;
      sendHandshakeAck(requestId);
      return;
    }
    if (strcmp(type, "handshake") == 0) {
      sendProtocolError(client_, requestId, "invalid_request", "handshake is already complete");
    } else if (strcmp(type, "heartbeat") == 0) {
      sendHeartbeatAck(requestId);
    } else if (strcmp(type, "get_snapshot") == 0) {
      sendSnapshot(requestId);
    } else if (strcmp(type, "execute_block") == 0) {
      handleExecuteBlock(request);
    } else if (strcmp(type, "cancel") == 0) {
      handleCancel(request);
    } else if (strcmp(type, "finish_task") == 0) {
      sendFinishTaskResult(requestId, app_.finishRemoteTask());
    } else if (strcmp(type, "emergency_stop") == 0) {
      sendEmergencyStopResult(requestId, app_.emergencyStop());
    } else if (strcmp(type, "reset_fault") == 0) {
      sendResetFaultResult(requestId, app_.resetFault());
    } else {
      sendProtocolError(client_, requestId, "unknown_type", "Unsupported motion protocol message type");
    }
  }

  void handleExecuteBlock(JsonDocument& request) {
    const char* requestId = request["requestId"] | "";
    const char* blockId = request["blockId"] | "";
    if (blockId[0] == '\0' || strlen(blockId) > 48 || !request["seq"].is<uint32_t>()) {
      sendProtocolError(client_, requestId, "invalid_block", "blockId and integer seq are required");
      return;
    }
    if (strcmp(request["policy"]["onDisconnect"] | "", "cancel") != 0 ||
        !request["policy"]["maxRuntimeMs"].is<uint32_t>()) {
      sendProtocolError(client_, requestId, "invalid_block", "policy must specify maxRuntimeMs and onDisconnect=cancel");
      return;
    }
    const uint32_t maxRuntimeMs = request["policy"]["maxRuntimeMs"].as<uint32_t>();
    if (maxRuntimeMs == 0 || maxRuntimeMs > kMaxBlockRuntimeMs) {
      sendProtocolError(client_, requestId, "invalid_block", "policy.maxRuntimeMs is outside the supported range");
      return;
    }
    RemoteMotionStep step{};
    const char* code = "invalid_block";
    const char* message = "Invalid atomic motion block";
    if (!parseAtomicMotionBlock(request["block"], step, code, message)) {
      sendProtocolError(client_, requestId, code, message);
      return;
    }
    const uint32_t seq = request["seq"].as<uint32_t>();
    const SubmitRemoteBlockResult result = app_.submitRemoteBlock(&step, 1, blockId, seq, maxRuntimeMs);
    if (!result.accepted) {
      sendProtocolError(client_, requestId, result.rejectionCode, result.rejectionMessage);
      return;
    }
    sendBlockAck(requestId, blockId, seq);
  }

  void handleCancel(JsonDocument& request) {
    const char* requestId = request["requestId"] | "";
    const bool hasBlockId = request["blockId"].is<const char*>();
    const bool hasSeq = request["seq"].is<uint32_t>();
    if (hasBlockId != hasSeq) {
      sendProtocolError(client_, requestId, "invalid_request", "cancel blockId and seq must be provided together");
      return;
    }
    const char* blockId = hasBlockId ? request["blockId"].as<const char*>() : nullptr;
    const uint32_t seq = hasSeq ? request["seq"].as<uint32_t>() : 0;
    sendCancelResult(requestId, app_.cancelQueuedRemoteBlock(blockId, seq, hasBlockId));
  }

  void drainRuntimeEvents() {
    char blockId[49];
    uint32_t seq = 0;
    uint32_t durationMs = 0;
    const char* status = "";
    const char* code = "";
    const char* message = "";
    if (app_.consumeRemoteBlockFinal(blockId, sizeof(blockId), seq, status, code, message, durationMs) &&
        client_ && client_.connected() && handshaken_) {
      sendBlockResult(blockId, seq, status, code, message, durationMs);
    }
    if (app_.consumeRemoteFault(blockId, sizeof(blockId), code, message) && client_ && client_.connected() && handshaken_) {
      sendFault(code, message);
    }
  }

  void sendHandshakeAck(const char* requestId) {
    StaticJsonDocument<384> doc;
    doc["v"] = 1;
    doc["type"] = "handshake_ack";
    doc["requestId"] = requestId;
    doc["device"] = "auto_typer";
    doc["protocol"] = "tcp_ndjson";
    JsonArray capabilities = doc.createNestedArray("capabilities");
    capabilities.add("absolute_target_pulses");
    capabilities.add("execute_block");
    capabilities.add("explicit_snapshot");
    JsonObject limits = doc.createNestedObject("limits");
    limits["maxMessageBytes"] = kMaxTcpMessageBytes;
    limits["maxBlockRuntimeMs"] = kMaxBlockRuntimeMs;
    limits["maxActionTimeoutMs"] = kMaxBlockTimeoutMs;
    sendJson(client_, doc);
  }

  void sendHeartbeatAck(const char* requestId) {
    StaticJsonDocument<96> doc;
    doc["v"] = 1;
    doc["type"] = "heartbeat_ack";
    doc["requestId"] = requestId;
    sendJson(client_, doc);
  }

  void sendSnapshot(const char* requestId) {
    DynamicJsonDocument doc(3072);
    doc["v"] = 1;
    doc["type"] = "snapshot";
    doc["requestId"] = requestId;
    writeStatus(doc.createNestedObject("status"));
    sendJson(client_, doc);
  }

  void sendBlockAck(const char* requestId, const char* blockId, uint32_t seq) {
    StaticJsonDocument<160> doc;
    doc["v"] = 1;
    doc["type"] = "block_ack";
    doc["requestId"] = requestId;
    doc["blockId"] = blockId;
    doc["seq"] = seq;
    sendJson(client_, doc);
  }

  void sendBlockResult(const char* blockId,
                       uint32_t seq,
                       const char* status,
                       const char* code,
                       const char* message,
                       uint32_t durationMs) {
    StaticJsonDocument<320> doc;
    doc["v"] = 1;
    doc["type"] = "block_result";
    doc["blockId"] = blockId;
    doc["seq"] = seq;
    doc["status"] = status != nullptr && status[0] != '\0' ? status : "failed";
    if (code != nullptr && code[0] != '\0') doc["code"] = code;
    if (message != nullptr && message[0] != '\0') doc["message"] = message;
    doc["durationMs"] = durationMs;
    sendJson(client_, doc);
  }

  void sendCancelResult(const char* requestId, bool ok) {
    sendBooleanResult("cancel_result", requestId, ok, false);
  }

  void sendFinishTaskResult(const char* requestId, bool ok) {
    sendBooleanResult("finish_task_result", requestId, ok, false);
  }

  void sendEmergencyStopResult(const char* requestId, bool ok) {
    sendBooleanResult("emergency_stop_result", requestId, ok, true);
  }

  void sendResetFaultResult(const char* requestId, bool ok) {
    sendBooleanResult("reset_fault_result", requestId, ok, true);
  }

  void sendBooleanResult(const char* type, const char* requestId, bool ok, bool includeStatus) {
    DynamicJsonDocument doc(includeStatus ? 3072 : 160);
    doc["v"] = 1;
    doc["type"] = type;
    doc["requestId"] = requestId;
    doc["ok"] = ok;
    if (includeStatus) writeStatus(doc.createNestedObject("status"));
    sendJson(client_, doc);
  }

  void sendFault(const char* code, const char* message) {
    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["type"] = "fault";
    doc["code"] = code != nullptr && code[0] != '\0' ? code : "device_fault";
    doc["message"] = message != nullptr && message[0] != '\0' ? message : "Device fault";
    sendJson(client_, doc);
  }

  void sendProtocolError(WiFiClient& client, const char* requestId, const char* code, const char* message) {
    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["type"] = "protocol_error";
    if (requestId != nullptr && requestId[0] != '\0') doc["requestId"] = requestId;
    doc["code"] = code;
    doc["message"] = message;
    sendJson(client, doc);
  }

  void writeStatus(JsonObject status) {
    status["deviceId"] = config_.deviceId;
    status["firmwareVersion"] = config_.firmwareVersion;
    status["ipAddress"] = currentIp();
    status["mode"] = modeText(app_.mode());
    status["health"] = app_.mode() == DeviceMode::Faulted
                           ? "fault"
                           : (app_.healthNotReady() ? "not_ready" : (app_.healthWarning() ? "warning" : "ok"));
    status["wifiRssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
    status["pressReady"] = app_.pressReady();
    status["motionReady"] = app_.motionReady();
    status["lineFeedPrimeRequired"] = app_.lineFeedPrimeRequired();
    status["keymapVersion"] = app_.keymapVersion();

    const JobSnapshot snapshot = app_.snapshot();
    JsonObject job = status.createNestedObject("currentJob");
    job["state"] = jobStateText(snapshot.state);
    job["textLength"] = snapshot.textLength;
    job["currentIndex"] = snapshot.currentIndex;
    job["currentStep"] = snapshot.currentStep;
    job["totalSteps"] = snapshot.totalSteps;
    JsonObject point = job.createNestedObject("currentPoint");
    const MachinePointMm current = app_.currentPoint();
    point["xMm"] = current.xMm;
    point["yMm"] = current.yMm;
    if (app_.mode() == DeviceMode::Faulted) {
      JsonObject fault = status.createNestedObject("fault");
      fault["code"] = snapshot.faultCode;
      fault["message"] = snapshot.faultMessage;
      fault["recoverable"] = true;
    }
    writeCan(status.createNestedObject("canDiagnostics"));
    JsonArray motors = status.createNestedArray("motors");
    writeMotor(motors, config_.topology.xMotorId);
    writeMotor(motors, config_.topology.yLeftMotorId);
    writeMotor(motors, config_.topology.yRightMotorId);
    writeMotor(motors, config_.topology.lineFeedMotorId);
    writeMotor(motors, config_.topology.pressMotorId);
  }

  void writeCan(JsonObject output) {
    const CanBusDiagnostics value = app_.canDiagnostics();
    output["driverReady"] = value.driverReady;
    output["motionReady"] = value.motionReady;
    output["fatalFault"] = value.fatalFault;
    output["recoverable"] = value.recoverable;
    output["lastAlerts"] = value.lastAlerts;
    output["txFailedCount"] = value.txFailedCount;
    output["txRetryCount"] = value.txRetryCount;
    output["commandQueueFullCount"] = value.commandQueueFullCount;
    output["busErrorCount"] = value.busErrorCount;
    output["rxQueueFullCount"] = value.rxQueueFullCount;
    output["errPassiveCount"] = value.errPassiveCount;
    output["busOffCount"] = value.busOffCount;
    output["pendingFrameValid"] = value.pendingFrameValid;
    output["lastAlertAtMs"] = value.lastAlertAtMs;
    output["lastFaultAtMs"] = value.lastFaultAtMs;
    output["lastTxError"] = value.lastTxError;
    output["lastCommandQueueError"] = value.lastCommandQueueError;
    output["lastFaultCode"] = value.lastFaultCode;
    output["lastFaultMessage"] = value.lastFaultMessage;
  }

  void writeMotor(JsonArray motors, uint8_t motorId) {
    const MotorState state = app_.motorState(motorId);
    JsonObject motor = motors.createNestedObject();
    motor["id"] = motorId;
    motor["role"] = motorRoleText(state.role);
    motor["readiness"] = protocolMotorReadinessText(state.readiness);
    motor["hasVelocity"] = state.hasVelocity;
    motor["hasRealtimeAngle"] = state.hasRealtimeAngle;
    motor["hasInputPulse"] = state.hasInputPulse;
    motor["hasStatus"] = state.hasStatus;
    motor["hasRecentStatus"] = state.hasRecentStatus;
    motor["hasRecentInputPulse"] = state.hasRecentInputPulse;
    motor["hasRecentVelocity"] = state.hasRecentVelocity;
    motor["velocityRpm"] = state.velocityRpm;
    motor["realtimeAngleRaw65536"] = state.realtimeAngleRaw65536;
    motor["inputPulseSteps"] = state.inputPulseSteps;
    motor["statusFlags"] = state.statusFlags;
    motor["driverFault"] = state.driverFault;
    motor["conditionNotMet"] = state.conditionNotMet;
    motor["commandMalformed"] = state.commandMalformed;
    motor["lastAckCommand"] = state.lastAckCommand;
    motor["lastConditionNotMetCommand"] = state.lastConditionNotMetCommand;
    motor["lastMalformedCommand"] = state.lastMalformedCommand;
    motor["lastAckMs"] = state.lastAckMs;
    motor["lastConditionNotMetMs"] = state.lastConditionNotMetMs;
    motor["lastMalformedMs"] = state.lastMalformedMs;
    motor["motionReached"] = state.motionReached;
    motor["lastMotionReachedMs"] = state.lastMotionReachedMs;
    motor["lastVelocityMs"] = state.lastVelocityMs;
    motor["lastRealtimeAngleMs"] = state.lastRealtimeAngleMs;
    motor["lastInputPulseMs"] = state.lastInputPulseMs;
    motor["lastStatusMs"] = state.lastStatusMs;
    motor["lastAnyFrameMs"] = state.lastAnyFrameMs;
    motor["lastProbeMs"] = state.lastProbeMs;
    motor["lastErrorCode"] = state.lastErrorCode;
    motor["lastErrorMessage"] = state.lastErrorMessage;
  }

  template <typename TDoc>
  bool sendJson(WiFiClient& client, TDoc& doc) {
    if (!client || !client.connected()) {
      return false;
    }
    String line;
    serializeJson(doc, line);
    const size_t expected = line.length() + 1;
    size_t written = client.print(line);
    written += client.write('\n');
    return written == expected;
  }

  String currentIp() const {
    return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  }

  const TypingConfig& config_;
  AutoTyperApplication& app_;
  Print& log_;
  WiFiServer server_;
  WiFiClient client_;
  String lineBuffer_;
  bool handshaken_;
  uint32_t connectedAtMs_;
  bool discardingOversizeLine_;
};

}  // namespace auto_typer
