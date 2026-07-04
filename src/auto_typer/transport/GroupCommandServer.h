#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

#include "../auto_typer_runtime.h"
#include "GroupCommandProtocol.h"

namespace auto_typer {

static constexpr uint16_t kGroupCommandPort = 7777;
static constexpr uint32_t kTcpHandshakeTimeoutMs = 3000;

class GroupCommandServer {
 public:
  GroupCommandServer(const TypingConfig& config,
                     AutoTyperApplication& app,
                     MotorTelemetryBuffer& motorTelemetry,
                     Print& log)
      : config_(config),
        app_(app),
        motorTelemetry_(motorTelemetry),
        log_(log),
        server_(kGroupCommandPort),
        handshaken_(false),
        telemetryIntervalMs_(350),
        lastTelemetryMs_(0),
        lastMotorStateUpdateMs_(0),
        clientConnectedAtMs_(0),
        telemetryForcePending_(false),
        outboundOrder_(0),
        clientSessionId_(0) {}

  void begin() {
    clearOutboundQueue();
    clearDedupeLedger();
    server_.begin();
    server_.setNoDelay(true);
    log_.print("[tcp] WiFi IP: ");
    log_.println(currentIp());
    log_.print("[tcp] NDJSON ready on port ");
    log_.println(kGroupCommandPort);
  }

  void tick() {
    closeStaleHandshakeClient();
    acceptClient();
    readClient();
    sendPendingEvents();
    sendMotorTelemetry();
    sendTelemetry(consumeTelemetryForcePending());
    flushOutboundQueue();
  }

 private:
  struct OutboundEvent {
    bool used;
    uint8_t priority;
    uint32_t order;
    String line;
  };

  struct DedupeEntry {
    bool used;
    uint32_t sessionId;
    char requestId[49];
    char groupId[49];
    uint32_t seq;
    bool accepted;
    char reason[40];
    char message[96];
    size_t blockCount;
  };

  void acceptClient() {
    WiFiClient incoming = server_.available();
    if (!incoming) {
      return;
    }
    incoming.setNoDelay(true);
    if (client_ && client_.connected()) {
      if (!handshaken_ && millis() - clientConnectedAtMs_ >= kTcpHandshakeTimeoutMs) {
        log_.println("[tcp] handshake timeout, closing client");
        client_.stop();
        lineBuffer_ = "";
        handshaken_ = false;
        clearOutboundQueue();
      } else {
        sendProtocolError(incoming, "", "device_busy", "Another TCP client is connected");
        incoming.stop();
        return;
      }
    }
    client_.stop();
    client_ = incoming;
    lineBuffer_ = "";
    handshaken_ = false;
    clientConnectedAtMs_ = millis();
    lastTelemetryMs_ = 0;
    ++clientSessionId_;
    clearDedupeLedger();
    clearOutboundQueue();
    log_.print("[tcp] client connected remote=");
    log_.print(client_.remoteIP().toString());
    log_.print(":");
    log_.println(client_.remotePort());
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
        flushOutboundQueue();
        if (!client_ || !client_.connected()) {
          return;
        }
        continue;
      }
      if (lineBuffer_.length() >= kMaxTcpMessageBytes) {
        sendProtocolError(client_, "", "group_too_large", "TCP line exceeds MAX_TCP_MESSAGE_BYTES");
        lineBuffer_ = "";
        return;
      }
      lineBuffer_ += static_cast<char>(value);
    }
  }

  void handleLine(const String& line) {
    if (line.length() == 0) {
      return;
    }
    DynamicJsonDocument request(line.length() + 768);
    const DeserializationError error = deserializeJson(request, line);
    if (error) {
      log_.println("[tcp] rx invalid_json");
      sendProtocolError(client_, "", "invalid_json", "Invalid JSON line");
      return;
    }
    const char* type = request["type"] | "";
    const char* requestId = request["requestId"] | "";
    const bool isPing = strcmp(type, "ping") == 0;
    if (!isPing) {
      log_.print("[tcp] rx line len=");
      log_.println(line.length());
      log_.print("[tcp] rx type=");
      log_.print(type);
      log_.print(" requestId=");
      log_.print(requestId);
      log_.print(" v=");
      log_.print(request["v"] | 0);
      log_.print(" handshaken=");
      log_.println(handshaken_ ? 1 : 0);
    }
    if (isPing) {
      sendPong(requestId);
      return;
    }
    if (!handshaken_) {
      if (strcmp(type, "hello") != 0 || (request["v"] | 0) != 1) {
        log_.print("[tcp] rx before hello rejected type=");
        log_.println(type);
        sendProtocolError(client_, requestId, "handshake_required", "hello is required before commands");
        return;
      }
      handshaken_ = true;
      log_.print("[tcp] tx type=hello_ack requestId=");
      log_.println(requestId);
      sendHelloAck(requestId);
      log_.print("[tcp] tx type=status requestId=");
      log_.println(requestId);
      sendStatus(requestId);
      return;
    }
    if (strcmp(type, "get_status") == 0) {
      sendStatus(requestId);
      return;
    }
    if (strcmp(type, "subscribe_telemetry") == 0) {
      const uint32_t requested = request["intervalMs"] | telemetryIntervalMs_;
      telemetryIntervalMs_ = clampTelemetryInterval(requested);
      sendTelemetrySubscribed(requestId);
      requestTelemetrySnapshot();
      return;
    }
    if (strcmp(type, "get_keymap") == 0) {
      sendKeymap(requestId);
      return;
    }
    if (strcmp(type, "probe") == 0) {
      const bool ok = app_.probeMotors();
      sendProbeResult(requestId, ok);
      requestTelemetrySnapshot();
      return;
    }
    if (strcmp(type, "press_diag_m5") == 0) {
      const PressDiagResult result = app_.debugPressDiagM5();
      sendPressDiagM5Result(requestId, result);
      requestTelemetrySnapshot();
      return;
    }
    if (strcmp(type, "reset_fault") == 0) {
      const bool ok = app_.resetFault();
      sendResetFaultResult(requestId, ok);
      requestTelemetrySnapshot();
      return;
    }
    if (strcmp(type, "release_line_feed_origin") == 0) {
      const bool ok = app_.releaseLineFeedOrigin();
      sendReleaseLineFeedOriginResult(requestId, ok);
      requestTelemetrySnapshot();
      return;
    }
    if (strcmp(type, "cancel") == 0) {
      const char* groupId = request["groupId"] | "";
      const uint32_t seq = request["seq"] | app_.currentRemoteSeq();
      const bool targetMatches = groupId[0] == '\0' ||
                                 (strcmp(groupId, app_.currentRemoteCommandId()) == 0 && seq == app_.currentRemoteSeq());
      const bool ok = targetMatches && app_.cancelCurrentJob();
      sendCancelResult(requestId, ok);
      requestTelemetrySnapshot();
      return;
    }
    if (strcmp(type, "finish_task") == 0) {
      const bool ok = app_.finishRemoteTask();
      sendFinishTaskResult(requestId, ok);
      requestTelemetrySnapshot();
      return;
    }
    if (strcmp(type, "exec_group") == 0) {
      handleExecGroup(request);
      return;
    }
    sendProtocolError(client_, requestId, "unknown_type", "Unsupported TCP message type");
  }

  void handleExecGroup(JsonDocument& request) {
    const char* requestId = request["requestId"] | "";
    const char* groupId = request["groupId"] | "";
    const uint32_t seq = request["seq"] | 0;
    DedupeEntry* duplicate = findDedupeEntry(requestId, groupId, seq);
    if (duplicate != nullptr) {
      replayDedupeEntry(*duplicate, requestId);
      return;
    }
    if (groupId[0] == '\0') {
      sendGroupRejected(requestId, groupId, seq, "invalid_group", "groupId is required");
      recordDedupeEntry(requestId, groupId, seq, false, "invalid_group", "groupId is required", 0);
      return;
    }
    const uint32_t maxRuntimeMs = request["policy"]["maxRuntimeMs"] | 0;
    if (maxRuntimeMs == 0 || maxRuntimeMs > kMaxGroupRuntimeMs) {
      sendGroupRejected(requestId, groupId, seq, "invalid_group", "policy.maxRuntimeMs is invalid");
      recordDedupeEntry(requestId, groupId, seq, false, "invalid_group", "policy.maxRuntimeMs is invalid", 0);
      return;
    }
    RemoteMotionStep steps[kRemoteGroupMaxSteps];
    size_t count = 0;
    const char* parseCode = "";
    const char* parseMessage = "";
    if (!parseRemoteGroup(request["blocks"], steps, kRemoteGroupMaxSteps, count, parseCode, parseMessage)) {
      const char* reason = normalizeRejectReason(parseCode);
      sendGroupRejected(requestId, groupId, seq, reason, parseMessage);
      recordDedupeEntry(requestId, groupId, seq, false, reason, parseMessage, 0);
      return;
    }
    const SubmitRemoteGroupResult result = app_.submitRemoteGroup(steps, count, groupId, seq);
    if (!result.accepted) {
      const char* reason = normalizeRejectReason(result.rejectionCode);
      sendGroupRejected(requestId, groupId, seq, reason, result.rejectionMessage);
      recordDedupeEntry(requestId, groupId, seq, false, reason, result.rejectionMessage, 0);
      return;
    }
    sendGroupAccepted(requestId, groupId, seq, count);
    recordDedupeEntry(requestId, groupId, seq, true, "", "", count);
  }

  void sendPendingEvents() {
    if (!client_ || !client_.connected()) {
      return;
    }
    char groupId[49];
    if (app_.consumeRemoteGroupStarted(groupId, sizeof(groupId))) {
      sendGroupStarted(groupId);
    }
    uint32_t seq = 0;
    size_t blockIndex = 0;
    const char* blockType = "";
    if (app_.consumeRemoteBlockStarted(groupId, sizeof(groupId), seq, blockIndex, blockType)) {
      sendBlockEvent("block_started", groupId, seq, blockIndex, blockType);
    }
    if (app_.consumeRemoteBlockDone(groupId, sizeof(groupId), seq, blockIndex, blockType)) {
      sendBlockEvent("block_done", groupId, seq, blockIndex, blockType);
    }
    uint32_t durationMs = 0;
    if (app_.consumeRemoteGroupDone(groupId, sizeof(groupId), durationMs)) {
      sendGroupDone(groupId, durationMs);
    }
    const char* code = "";
    const char* message = "";
    const char* finalStatus = "";
    uint32_t finalSeq = 0;
    if (app_.consumeRemoteGroupFinal(groupId, sizeof(groupId), finalSeq, finalStatus, code, message, durationMs)) {
      sendGroupFinal(groupId, finalSeq, finalStatus, code, message, durationMs);
    }
    if (app_.consumeRemoteFault(groupId, sizeof(groupId), code, message)) {
      sendFault(groupId, code, message);
    }
  }

  void sendHelloAck(const char* requestId) {
    StaticJsonDocument<384> doc;
    doc["v"] = 1;
    doc["type"] = "hello_ack";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    doc["device"] = "auto_typer";
    doc["protocol"] = "tcp_ndjson";
    JsonArray caps = doc.createNestedArray("capabilities");
    caps.add("exec_group");
    caps.add("telemetry");
    caps.add("get_status");
    caps.add("get_keymap");
    caps.add("probe");
    caps.add("cancel");
    caps.add("finish_task");
    caps.add("reset_fault");
    caps.add("release_line_feed_origin");
    caps.add("line_feed_home");
    caps.add("press_motor");
    caps.add("press_diag_m5");
    JsonObject limits = doc.createNestedObject("limits");
    limits["maxBlocksPerGroup"] = kRemoteGroupMaxSteps;
    limits["maxMessageBytes"] = kMaxTcpMessageBytes;
    limits["maxGroupRuntimeMs"] = kMaxGroupRuntimeMs;
    enqueueJson(0, doc);
  }

  void sendStatus(const char* requestId) {
    DynamicJsonDocument doc(3072);
    doc["v"] = 1;
    doc["type"] = "status";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    writeStatus(doc.createNestedObject("status"));
    enqueueJson(0, doc);
  }

  void sendTelemetrySubscribed(const char* requestId) {
    StaticJsonDocument<128> doc;
    doc["v"] = 1;
    doc["type"] = "telemetry_subscribed";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    doc["intervalMs"] = telemetryIntervalMs_;
    enqueueJson(0, doc);
  }

  void sendTelemetry(bool force) {
    if (!client_ || !client_.connected() || !handshaken_) {
      return;
    }
    const uint32_t nowMs = millis();
    if (!force && lastTelemetryMs_ != 0 && nowMs - lastTelemetryMs_ < telemetryIntervalMs_) {
      return;
    }
    lastTelemetryMs_ = nowMs;
    DynamicJsonDocument doc(3072);
    doc["v"] = 1;
    doc["type"] = "telemetry";
    writeStatus(doc.createNestedObject("status"));
    enqueueJson(3, doc);
  }

  void closeStaleHandshakeClient() {
    if (!client_ || !client_.connected() || handshaken_) {
      return;
    }
    if (millis() - clientConnectedAtMs_ < kTcpHandshakeTimeoutMs) {
      return;
    }
    log_.println("[tcp] handshake timeout, closing client");
    client_.stop();
    lineBuffer_ = "";
    handshaken_ = false;
    clearOutboundQueue();
  }

  void requestTelemetrySnapshot() {
    telemetryForcePending_ = true;
  }

  bool consumeTelemetryForcePending() {
    const bool force = telemetryForcePending_;
    telemetryForcePending_ = false;
    return force;
  }

  void sendMotorTelemetry() {
    if (!client_ || !client_.connected() || !handshaken_) {
      return;
    }
    if (motorTelemetry_.hasOverflow()) {
      sendTelemetryOverflow();
      motorTelemetry_.clearOverflow();
    }
    MotorTelemetryEvent events[8];
    const size_t eventCount = motorTelemetry_.drainCriticalEvents(events, sizeof(events) / sizeof(events[0]));
    for (size_t i = 0; i < eventCount; ++i) {
      sendMotorEvent(events[i]);
    }
    const uint32_t nowMs = millis();
    if (lastMotorStateUpdateMs_ != 0 && nowMs - lastMotorStateUpdateMs_ < kMotorStateUpdateIntervalMs) {
      return;
    }
    MotorStateSnapshot snapshots[5];
    const size_t stateCount = motorTelemetry_.drainDirtyMotorStates(snapshots, sizeof(snapshots) / sizeof(snapshots[0]));
    if (stateCount == 0) {
      return;
    }
    lastMotorStateUpdateMs_ = nowMs;
    DynamicJsonDocument doc(1536);
    doc["v"] = 1;
    doc["type"] = "motor_state_update";
    doc["timestampMs"] = nowMs;
    JsonArray motors = doc.createNestedArray("motors");
    for (size_t i = 0; i < stateCount; ++i) {
      writeMotorStateUpdate(motors, snapshots[i]);
    }
    enqueueJson(2, doc);
  }

  void sendKeymap(const char* requestId) {
    DynamicJsonDocument doc(4096);
    doc["v"] = 1;
    doc["type"] = "keymap";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    JsonObject coordinate = doc.createNestedObject("coordinateSystem");
    coordinate["origin"] = "bottom_left";
    coordinate["xPositive"] = "right";
    coordinate["yPositive"] = "up";
    coordinate["xMotorPositiveDirection"] = "CW";
    coordinate["yMotorPositiveDirection"] = "CCW";
    JsonArray keys = doc.createNestedArray("keys");
    const KeyBinding* keymap = app_.keymap();
    for (size_t i = 0; i < app_.keymapCount(); ++i) {
      JsonObject key = keys.createNestedObject();
      char label[2] = {keymap[i].key, '\0'};
      key["label"] = label;
      key["xMm"] = keymap[i].point.xMm;
      key["yMm"] = keymap[i].point.yMm;
    }
    enqueueJson(0, doc);
  }

  void sendProbeResult(const char* requestId, bool ok) {
    DynamicJsonDocument doc(2048);
    doc["v"] = 1;
    doc["type"] = "probe_result";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    doc["ok"] = ok;
    JsonArray motors = doc.createNestedArray("motors");
    writeMotor(motors, config_.topology.xMotorId);
    writeMotor(motors, config_.topology.yLeftMotorId);
    writeMotor(motors, config_.topology.yRightMotorId);
    writeMotor(motors, config_.topology.lineFeedMotorId);
    writeMotor(motors, config_.topology.pressMotorId);
    enqueueJson(0, doc);
  }

  void sendPressDiagM5Result(const char* requestId, const PressDiagResult& result) {
    DynamicJsonDocument doc(1024);
    doc["v"] = 1;
    doc["type"] = "press_diag_m5_result";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    doc["ok"] = result.ok;
    doc["code"] = result.code != nullptr ? result.code : "";
    doc["message"] = result.message != nullptr ? result.message : "";
    doc["initialPulse"] = result.initialPulse;
    doc["downTargetPulse"] = result.downTargetPulse;
    doc["downPulse"] = result.downPulse;
    doc["upTargetPulse"] = result.upTargetPulse;
    doc["finalPulse"] = result.finalPulse;
    doc["downAckSeen"] = result.downAckSeen;
    doc["downReachedSeen"] = result.downReachedSeen;
    doc["upAckSeen"] = result.upAckSeen;
    doc["upReachedSeen"] = result.upReachedSeen;
    doc["traceCount"] = result.traceCount;
    enqueueJson(0, doc);
  }

  void sendResetFaultResult(const char* requestId, bool ok) {
    DynamicJsonDocument doc(3072);
    doc["v"] = 1;
    doc["type"] = "reset_fault_result";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    doc["ok"] = ok;
    writeStatus(doc.createNestedObject("status"));
    enqueueJson(0, doc);
  }

  void sendReleaseLineFeedOriginResult(const char* requestId, bool ok) {
    DynamicJsonDocument doc(3072);
    doc["v"] = 1;
    doc["type"] = "release_line_feed_origin_result";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    doc["ok"] = ok;
    writeStatus(doc.createNestedObject("status"));
    enqueueJson(0, doc);
  }

  void sendCancelResult(const char* requestId, bool ok) {
    StaticJsonDocument<128> doc;
    doc["v"] = 1;
    doc["type"] = "cancel_result";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    doc["ok"] = ok;
    enqueueJson(0, doc);
  }

  void sendFinishTaskResult(const char* requestId, bool ok) {
    StaticJsonDocument<128> doc;
    doc["v"] = 1;
    doc["type"] = "finish_task_result";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    doc["ok"] = ok;
    enqueueJson(0, doc);
  }

  void sendGroupAccepted(const char* requestId, const char* groupId, uint32_t seq, size_t blockCount) {
    StaticJsonDocument<192> doc;
    doc["v"] = 1;
    doc["type"] = "group_accepted";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    doc["groupId"] = groupId != nullptr ? groupId : "";
    doc["seq"] = seq;
    doc["blockCount"] = blockCount;
    enqueueJson(0, doc);
  }

  void sendGroupRejected(const char* requestId,
                         const char* groupId,
                         uint32_t seq,
                         const char* reason,
                         const char* message) {
    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["type"] = "group_rejected";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    if (groupId != nullptr && groupId[0] != '\0') {
      doc["groupId"] = groupId;
    }
    doc["seq"] = seq;
    doc["reason"] = reason != nullptr && reason[0] != '\0' ? reason : "invalid_group";
    doc["message"] = message != nullptr && message[0] != '\0' ? message : "Group rejected";
    enqueueJson(0, doc);
  }

  void sendGroupStarted(const char* groupId) {
    StaticJsonDocument<128> doc;
    doc["v"] = 1;
    doc["type"] = "group_started";
    doc["groupId"] = groupId != nullptr ? groupId : "";
    doc["seq"] = app_.currentRemoteSeq();
    enqueueJson(1, doc);
  }

  void sendBlockEvent(const char* type, const char* groupId, uint32_t seq, size_t blockIndex, const char* blockType) {
    StaticJsonDocument<192> doc;
    doc["v"] = 1;
    doc["type"] = type;
    doc["groupId"] = groupId != nullptr ? groupId : "";
    doc["seq"] = seq;
    doc["blockIndex"] = blockIndex;
    doc["blockType"] = blockType != nullptr ? blockType : "wait";
    enqueueJson(1, doc);
  }

  void sendGroupDone(const char* groupId, uint32_t durationMs) {
    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["type"] = "group_done";
    doc["groupId"] = groupId != nullptr ? groupId : "";
    doc["seq"] = app_.currentRemoteSeq();
    doc["ok"] = true;
    doc["durationMs"] = durationMs;
    JsonObject point = doc.createNestedObject("currentPoint");
    const MachinePointMm current = app_.currentPoint();
    point["xMm"] = current.xMm;
    point["yMm"] = current.yMm;
    enqueueJson(1, doc);
  }

  void sendGroupFinal(const char* groupId,
                      uint32_t seq,
                      const char* status,
                      const char* code,
                      const char* message,
                      uint32_t durationMs) {
    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["type"] = "group_final";
    doc["groupId"] = groupId != nullptr ? groupId : "";
    doc["seq"] = seq;
    doc["status"] = status != nullptr && status[0] != '\0' ? status : "failed";
    if (code != nullptr && code[0] != '\0') {
      doc["code"] = code;
    }
    if (message != nullptr && message[0] != '\0') {
      doc["message"] = message;
    }
    doc["durationMs"] = durationMs;
    enqueueJson(0, doc);
  }

  void sendFault(const char* groupId, const char* code, const char* message) {
    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["type"] = "fault";
    if (groupId != nullptr && groupId[0] != '\0') {
      doc["groupId"] = groupId;
    }
    doc["seq"] = app_.currentRemoteSeq();
    doc["code"] = code != nullptr && code[0] != '\0' ? code : "fault";
    doc["message"] = message != nullptr && message[0] != '\0' ? message : "Motion fault";
    enqueueJson(0, doc);
  }

  void sendMotorEvent(const MotorTelemetryEvent& event) {
    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["type"] = "motor_event";
    doc["motorId"] = event.motorId;
    doc["role"] = motorRoleDisplayText(event.motorId);
    doc["eventKind"] = motorTelemetryEventKindText(event.kind);
    JsonObject data = doc.createNestedObject("data");
    char command[3];
    writeHexByte(command, sizeof(command), event.command);
    data["command"] = command;
    if (event.faultSeverity) {
      doc["severity"] = "fault";
    }
    doc["timestampMs"] = event.timestampMs;
    enqueueJson(2, doc);
  }

  void sendTelemetryOverflow() {
    StaticJsonDocument<160> doc;
    doc["v"] = 1;
    doc["type"] = "telemetry_overflow";
    doc["code"] = "telemetry_overflow";
    doc["message"] = "Motor telemetry queue overflowed";
    doc["timestampMs"] = millis();
    enqueueJson(2, doc);
  }

  void sendProtocolError(WiFiClient& client, const char* requestId, const char* code, const char* message) {
    StaticJsonDocument<192> doc;
    doc["v"] = 1;
    doc["type"] = "protocol_error";
    if (requestId != nullptr && requestId[0] != '\0') {
      doc["requestId"] = requestId;
    }
    doc["code"] = code != nullptr ? code : "protocol_error";
    doc["message"] = message != nullptr ? message : "Protocol error";
    sendJson(client, doc);
  }

  void sendPong(const char* requestId) {
    StaticJsonDocument<96> doc;
    doc["v"] = 1;
    doc["type"] = "pong";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    if (!enqueueJson(0, doc)) {
      log_.print("[tcp] pong enqueue failed requestId=");
      log_.println(requestId != nullptr ? requestId : "");
    }
  }

  void writeStatus(JsonObject status) {
    status["deviceId"] = config_.deviceId;
    status["firmwareVersion"] = config_.firmwareVersion;
    status["ipAddress"] = currentIp();
    status["mode"] = modeText(app_.mode());
    status["health"] = app_.mode() == DeviceMode::Faulted ? "fault" : (app_.healthNotReady() ? "not_ready" : "ok");
    status["wifiRssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
    status["pressReady"] = app_.pressReady();
    status["motionReady"] = app_.motionReady();
    status["lineFeedPrimeRequired"] = app_.lineFeedPrimeRequired();
    status["keymapVersion"] = app_.keymapVersion();
    const JobSnapshot snapshot = app_.snapshot();
    JsonObject job = status.createNestedObject("currentJob");
    job["state"] = jobStateText(snapshot.state);
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

  void writeCan(JsonObject canJson) {
    const CanBusDiagnostics can = app_.canDiagnostics();
    canJson["driverReady"] = can.driverReady;
    canJson["motionReady"] = can.motionReady;
    canJson["fatalFault"] = can.fatalFault;
    canJson["recoverable"] = can.recoverable;
    canJson["lastAlerts"] = can.lastAlerts;
    canJson["txFailedCount"] = can.txFailedCount;
    canJson["txRetryCount"] = can.txRetryCount;
    canJson["commandQueueFullCount"] = can.commandQueueFullCount;
    canJson["busErrorCount"] = can.busErrorCount;
    canJson["rxQueueFullCount"] = can.rxQueueFullCount;
    canJson["errPassiveCount"] = can.errPassiveCount;
    canJson["busOffCount"] = can.busOffCount;
    canJson["pendingFrameValid"] = can.pendingFrameValid;
    canJson["lastAlertAtMs"] = can.lastAlertAtMs;
    canJson["lastFaultAtMs"] = can.lastFaultAtMs;
    canJson["lastTxError"] = can.lastTxError;
    canJson["lastCommandQueueError"] = can.lastCommandQueueError;
    canJson["lastFaultCode"] = can.lastFaultCode;
    canJson["lastFaultMessage"] = can.lastFaultMessage;
  }

  void writeMotor(JsonArray motors, uint8_t motorId) {
    const MotorState state = app_.motorState(motorId);
    JsonObject motor = motors.createNestedObject();
    motor["id"] = motorId;
    motor["role"] = motorRoleText(state.role);
    motor["readiness"] = motorReadinessText(state.readiness);
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

  void writeMotorStateUpdate(JsonArray motors, const MotorStateSnapshot& snapshot) {
    const MotorState state = app_.motorState(snapshot.motorId);
    JsonObject motor = motors.createNestedObject();
    motor["motorId"] = snapshot.motorId;
    motor["role"] = motorRoleDisplayText(snapshot.motorId);
    motor["readiness"] = motorReadinessText(state.readiness);
    motor["hasVelocity"] = state.hasVelocity;
    if (state.hasVelocity) {
      motor["velocityRpm"] = state.velocityRpm;
    }
    motor["hasInputPulse"] = state.hasInputPulse;
    if (state.hasInputPulse) {
      motor["inputPulseSteps"] = state.inputPulseSteps;
    }
    motor["hasRealtimeAngle"] = state.hasRealtimeAngle;
    if (state.hasRealtimeAngle) {
      motor["angleRaw"] = state.realtimeAngleRaw65536;
      motor["angleDeg"] = static_cast<float>(state.realtimeAngleRaw65536) * 360.0f / 65536.0f;
    }
    motor["hasStatusFlags"] = state.hasStatus;
    if (state.hasStatus) {
      motor["statusFlags"] = state.statusFlags;
    }
    motor["lastUpdatedAtMs"] = snapshot.lastUpdatedAtMs;
  }

  String currentIp() const {
    return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  }

  uint16_t clampTelemetryInterval(uint32_t intervalMs) const {
    if (intervalMs < kMinTelemetryIntervalMs) {
      return kMinTelemetryIntervalMs;
    }
    if (intervalMs > kMaxTelemetryIntervalMs) {
      return kMaxTelemetryIntervalMs;
    }
    return static_cast<uint16_t>(intervalMs);
  }

  const char* normalizeRejectReason(const char* reason) const {
    if (reason == nullptr || reason[0] == '\0') {
      return "invalid_group";
    }
    if (strcmp(reason, "faulted") == 0) {
      return "device_fault";
    }
    return reason;
  }

  const char* motorRoleDisplayText(uint8_t motorId) const {
    if (motorId == config_.topology.xMotorId) {
      return "X";
    }
    if (motorId == config_.topology.yLeftMotorId) {
      return "YLeft";
    }
    if (motorId == config_.topology.yRightMotorId) {
      return "YRight";
    }
    if (motorId == config_.topology.lineFeedMotorId) {
      return "LineFeed";
    }
    if (motorId == config_.topology.pressMotorId) {
      return "Press";
    }
    return "Unknown";
  }

  static const char* motorTelemetryEventKindText(MotorTelemetryEventKind kind) {
    switch (kind) {
      case MotorTelemetryEventKind::Ack:
        return "ack";
      case MotorTelemetryEventKind::ConditionNotMet:
        return "condition_not_met";
      case MotorTelemetryEventKind::Malformed:
        return "malformed";
      case MotorTelemetryEventKind::MotionReached:
        return "motion_reached";
      case MotorTelemetryEventKind::Unknown:
      default:
        return "unknown";
    }
  }

  static void writeHexByte(char* out, size_t outSize, uint8_t value) {
    if (outSize < 3) {
      return;
    }
    static const char* digits = "0123456789ABCDEF";
    out[0] = digits[(value >> 4) & 0x0F];
    out[1] = digits[value & 0x0F];
    out[2] = '\0';
  }

  template <typename TDoc>
  bool enqueueJson(uint8_t priority, TDoc& doc) {
    if (!client_ || !client_.connected()) {
      return false;
    }
    String line;
    serializeJson(doc, line);
    size_t slot = kOutboundQueueCapacity;
    for (size_t i = 0; i < kOutboundQueueCapacity; ++i) {
      if (!outboundQueue_[i].used) {
        slot = i;
        break;
      }
    }
    if (slot == kOutboundQueueCapacity) {
      if (priority >= 3) {
        return false;
      }
      for (size_t i = 0; i < kOutboundQueueCapacity; ++i) {
        if (outboundQueue_[i].used && outboundQueue_[i].priority > priority) {
          slot = i;
          break;
        }
      }
      if (slot == kOutboundQueueCapacity) {
        return false;
      }
    }
    outboundQueue_[slot].used = true;
    outboundQueue_[slot].priority = priority;
    outboundQueue_[slot].order = ++outboundOrder_;
    outboundQueue_[slot].line = line;
    return true;
  }

  void flushOutboundQueue() {
    if (!client_ || !client_.connected()) {
      clearOutboundQueue();
      return;
    }
    while (true) {
      int selected = -1;
      for (size_t i = 0; i < kOutboundQueueCapacity; ++i) {
        if (!outboundQueue_[i].used) {
          continue;
        }
        if (selected < 0 || outboundQueue_[i].priority < outboundQueue_[selected].priority ||
            (outboundQueue_[i].priority == outboundQueue_[selected].priority &&
             outboundQueue_[i].order < outboundQueue_[selected].order)) {
          selected = static_cast<int>(i);
        }
      }
      if (selected < 0) {
        return;
      }
      sendLine(client_, outboundQueue_[selected].line);
      outboundQueue_[selected].used = false;
      outboundQueue_[selected].line = "";
    }
  }

  void clearOutboundQueue() {
    for (size_t i = 0; i < kOutboundQueueCapacity; ++i) {
      outboundQueue_[i].used = false;
      outboundQueue_[i].line = "";
    }
  }

  DedupeEntry* findDedupeEntry(const char* requestId, const char* groupId, uint32_t seq) {
    for (size_t i = 0; i < kDedupeLedgerCapacity; ++i) {
      DedupeEntry& entry = dedupeLedger_[i];
      if (!entry.used || entry.sessionId != clientSessionId_ || entry.seq != seq) {
        continue;
      }
      const bool sameRequest = requestId != nullptr && requestId[0] != '\0' && strcmp(entry.requestId, requestId) == 0;
      const bool sameGroup = groupId != nullptr && groupId[0] != '\0' && strcmp(entry.groupId, groupId) == 0;
      if (sameRequest || sameGroup) {
        return &entry;
      }
    }
    return nullptr;
  }

  void replayDedupeEntry(const DedupeEntry& entry, const char* requestId) {
    if (entry.accepted) {
      sendGroupAccepted(requestId, entry.groupId, entry.seq, entry.blockCount);
      return;
    }
    sendGroupRejected(requestId, entry.groupId, entry.seq, entry.reason, entry.message);
  }

  void recordDedupeEntry(const char* requestId,
                         const char* groupId,
                         uint32_t seq,
                         bool accepted,
                         const char* reason,
                         const char* message,
                         size_t blockCount) {
    size_t slot = clientSessionId_ % kDedupeLedgerCapacity;
    for (size_t i = 0; i < kDedupeLedgerCapacity; ++i) {
      if (!dedupeLedger_[i].used) {
        slot = i;
        break;
      }
    }
    DedupeEntry& entry = dedupeLedger_[slot];
    entry.used = true;
    entry.sessionId = clientSessionId_;
    copyCString(entry.requestId, sizeof(entry.requestId), requestId);
    copyCString(entry.groupId, sizeof(entry.groupId), groupId);
    entry.seq = seq;
    entry.accepted = accepted;
    copyCString(entry.reason, sizeof(entry.reason), reason);
    copyCString(entry.message, sizeof(entry.message), message);
    entry.blockCount = blockCount;
  }

  void clearDedupeLedger() {
    for (size_t i = 0; i < kDedupeLedgerCapacity; ++i) {
      dedupeLedger_[i].used = false;
    }
  }

  static void copyCString(char* out, size_t outSize, const char* value) {
    if (outSize == 0) {
      return;
    }
    const char* source = value != nullptr ? value : "";
    size_t i = 0;
    for (; i + 1 < outSize && source[i] != '\0'; ++i) {
      out[i] = source[i];
    }
    out[i] = '\0';
  }

  template <typename TDoc>
  bool sendJson(WiFiClient& client, TDoc& doc) {
    if (!client || !client.connected()) {
      logTxResult(0, 0, false);
      return false;
    }
    String line;
    serializeJson(doc, line);
    return sendLine(client, line);
  }

  bool sendLine(WiFiClient& client, const String& line) {
    if (!client || !client.connected()) {
      logTxResult(line.length() + 1, 0, false);
      return false;
    }
    const size_t txLen = line.length() + 1;
    size_t written = client.print(line);
    written += client.write('\n');
    client.flush();
    if (written != txLen || !client.connected()) {
      logTxResult(txLen, written, client.connected());
    }
    return written == txLen;
  }

  void logTxResult(size_t txLen, size_t written, bool connected) {
    log_.print("[tcp] tx_len=");
    log_.print(txLen);
    log_.print(" written=");
    log_.print(written);
    log_.print(" connected=");
    log_.println(connected ? 1 : 0);
  }

  void disconnectClient() {
    log_.println("[tcp] disconnect");
    if (app_.executorRunning()) {
      app_.cancelCurrentJob();
    }
    client_.stop();
    lineBuffer_ = "";
    handshaken_ = false;
    clearOutboundQueue();
  }

  const TypingConfig& config_;
  AutoTyperApplication& app_;
  MotorTelemetryBuffer& motorTelemetry_;
  Print& log_;
  WiFiServer server_;
  WiFiClient client_;
  String lineBuffer_;
  bool handshaken_;
  uint16_t telemetryIntervalMs_;
  uint32_t lastTelemetryMs_;
  uint32_t lastMotorStateUpdateMs_;
  uint32_t clientConnectedAtMs_;
  bool telemetryForcePending_;
  OutboundEvent outboundQueue_[32];
  uint32_t outboundOrder_;
  DedupeEntry dedupeLedger_[8];
  uint32_t clientSessionId_;
  static constexpr size_t kOutboundQueueCapacity = 32;
  static constexpr size_t kDedupeLedgerCapacity = 8;
  static constexpr uint16_t kMotorStateUpdateIntervalMs = 25;
};

}  // namespace auto_typer
