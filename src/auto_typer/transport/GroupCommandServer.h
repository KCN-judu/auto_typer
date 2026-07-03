#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>

#include "../auto_typer_runtime.h"
#include "GroupCommandProtocol.h"

namespace auto_typer {

static constexpr uint16_t kGroupCommandPort = 7777;

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
        setupApActive_(false),
        staConnecting_(false),
        savedCredentials_(false),
        lastWifiAttemptMs_(0),
        lastWifiError_("") {}

  void begin() {
    beginWifi();
    server_.begin();
    server_.setNoDelay(true);
    log_.print("[tcp] NDJSON ready on port ");
    log_.println(kGroupCommandPort);
  }

  void tick() {
    updateWifiConnectionState();
    acceptClient();
    readClient();
    sendPendingEvents();
    sendMotorTelemetry();
    sendTelemetry(false);
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
    handshaken_ = false;
    lastTelemetryMs_ = 0;
    log_.println("[tcp] client connected");
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
      sendProtocolError(client_, "", "invalid_json", "Invalid JSON line");
      return;
    }
    const char* type = request["type"] | "";
    const char* requestId = request["requestId"] | "";
    if (strcmp(type, "ping") == 0) {
      sendPong(requestId);
      return;
    }
    if (!handshaken_) {
      if (strcmp(type, "hello") != 0 || (request["v"] | 0) != 1) {
        sendProtocolError(client_, requestId, "handshake_required", "hello is required before commands");
        return;
      }
      handshaken_ = true;
      sendHelloAck(requestId);
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
      sendTelemetry(true);
      return;
    }
    if (strcmp(type, "get_keymap") == 0) {
      sendKeymap(requestId);
      return;
    }
    if (strcmp(type, "get_wifi_status") == 0) {
      sendWifiStatus(requestId);
      return;
    }
    if (strcmp(type, "scan_wifi") == 0) {
      sendWifiNetworks(requestId);
      return;
    }
    if (strcmp(type, "configure_wifi") == 0) {
      handleConfigureWifi(request);
      return;
    }
    if (strcmp(type, "finish_wifi_setup") == 0) {
      handleFinishWifiSetup(requestId);
      return;
    }
    if (strcmp(type, "probe") == 0) {
      const bool ok = app_.probeMotors();
      sendProbeResult(requestId, ok);
      sendTelemetry(true);
      return;
    }
    if (strcmp(type, "reset_fault") == 0) {
      const bool ok = app_.resetFault();
      sendResetFaultResult(requestId, ok);
      sendTelemetry(true);
      return;
    }
    if (strcmp(type, "cancel") == 0) {
      const bool ok = app_.cancelCurrentJob();
      sendCancelResult(requestId, ok);
      sendTelemetry(true);
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
    if (groupId[0] == '\0') {
      sendGroupRejected(requestId, groupId, seq, "invalid_group", "groupId is required");
      return;
    }
    const uint32_t maxRuntimeMs = request["policy"]["maxRuntimeMs"] | 0;
    if (maxRuntimeMs == 0 || maxRuntimeMs > kMaxGroupRuntimeMs) {
      sendGroupRejected(requestId, groupId, seq, "invalid_group", "policy.maxRuntimeMs is invalid");
      return;
    }
    RemoteMotionStep steps[kRemoteGroupMaxSteps];
    size_t count = 0;
    const char* parseCode = "";
    const char* parseMessage = "";
    if (!parseRemoteGroup(request["blocks"], steps, kRemoteGroupMaxSteps, count, parseCode, parseMessage)) {
      sendGroupRejected(requestId, groupId, seq, normalizeRejectReason(parseCode), parseMessage);
      return;
    }
    const SubmitRemoteGroupResult result = app_.submitRemoteGroup(steps, count, groupId, seq);
    if (!result.accepted) {
      sendGroupRejected(requestId, groupId, seq, normalizeRejectReason(result.rejectionCode), result.rejectionMessage);
      return;
    }
    sendGroupAccepted(requestId, groupId, seq, count);
  }

  void handleConfigureWifi(JsonDocument& request) {
    const char* requestId = request["requestId"] | "";
    const char* ssid = request["ssid"] | "";
    const char* password = request["password"] | "";
    if (strlen(ssid) == 0 || strlen(ssid) > kMaxWifiSsidLength) {
      sendWifiConfigResult(requestId, false, "invalid_wifi_ssid", "WiFi SSID is required");
      return;
    }
    if (strlen(password) > 0 && (strlen(password) < 8 || strlen(password) > kMaxWifiPasswordLength)) {
      sendWifiConfigResult(requestId, false, "invalid_wifi_password", "WiFi password must be empty or 8-63 characters");
      return;
    }
    if (!saveWifiCredentials(String(ssid), String(password))) {
      sendWifiConfigResult(requestId, false, "wifi_credentials_save_failed", "WiFi credentials could not be saved");
      return;
    }
    savedCredentials_ = true;
    connectStation(ssid, password);
    const bool connected = waitForStation(12000);
    if (!connected) {
      lastWifiError_ = "station_connect_failed";
      startSetupAp();
    }
    sendWifiConfigResult(requestId,
                         connected,
                         connected ? "" : "station_connect_failed",
                         connected ? "WiFi station connected" : "WiFi station connection failed");
  }

  void handleFinishWifiSetup(const char* requestId) {
    updateWifiConnectionState();
    if (WiFi.status() != WL_CONNECTED) {
      sendWifiSetupFinished(requestId, false, "wifi_not_connected", "Station WiFi is not connected");
      return;
    }
    if (setupApActive_) {
      WiFi.softAPdisconnect(true);
      setupApActive_ = false;
      WiFi.mode(WIFI_STA);
    }
    sendWifiSetupFinished(requestId, true, "", "WiFi setup finished");
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
    caps.add("reset_fault");
    caps.add("wifi_setup");
    caps.add("press_motor");
    JsonObject limits = doc.createNestedObject("limits");
    limits["maxBlocksPerGroup"] = kRemoteGroupMaxSteps;
    limits["maxMessageBytes"] = kMaxTcpMessageBytes;
    limits["maxGroupRuntimeMs"] = kMaxGroupRuntimeMs;
    sendJson(client_, doc);
  }

  void sendStatus(const char* requestId) {
    DynamicJsonDocument doc(3072);
    doc["v"] = 1;
    doc["type"] = "status";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    writeStatus(doc.createNestedObject("status"));
    sendJson(client_, doc);
  }

  void sendTelemetrySubscribed(const char* requestId) {
    StaticJsonDocument<128> doc;
    doc["v"] = 1;
    doc["type"] = "telemetry_subscribed";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    doc["intervalMs"] = telemetryIntervalMs_;
    sendJson(client_, doc);
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
    sendJson(client_, doc);
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
    sendJson(client_, doc);
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
    sendJson(client_, doc);
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
    sendJson(client_, doc);
  }

  void sendWifiStatus(const char* requestId) {
    StaticJsonDocument<512> doc;
    doc["v"] = 1;
    doc["type"] = "wifi_status";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    writeWifiStatus(doc.createNestedObject("wifi"));
    sendJson(client_, doc);
  }

  void sendWifiNetworks(const char* requestId) {
    DynamicJsonDocument doc(4096);
    doc["v"] = 1;
    doc["type"] = "wifi_networks";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    JsonArray networks = doc.createNestedArray("networks");
    struct NetworkEntry {
      String ssid;
      int32_t rssi;
      int32_t channel;
      wifi_auth_mode_t encryption;
    };
    NetworkEntry entries[kMaxWifiNetworks];
    size_t count = 0;
    const int found = WiFi.scanNetworks(false, false);
    if (found < 0) {
      doc["ok"] = false;
      doc["code"] = "wifi_scan_failed";
      doc["message"] = "WiFi scan failed";
      sendJson(client_, doc);
      return;
    }
    for (int i = 0; i < found && count < kMaxWifiNetworks; ++i) {
      const String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) {
        continue;
      }
      const int32_t rssi = WiFi.RSSI(i);
      int existing = -1;
      for (size_t j = 0; j < count; ++j) {
        if (entries[j].ssid == ssid) {
          existing = static_cast<int>(j);
          break;
        }
      }
      if (existing >= 0) {
        if (rssi > entries[existing].rssi) {
          entries[existing].rssi = rssi;
          entries[existing].channel = WiFi.channel(i);
          entries[existing].encryption = WiFi.encryptionType(i);
        }
        continue;
      }
      entries[count] = {ssid, rssi, WiFi.channel(i), WiFi.encryptionType(i)};
      ++count;
    }
    WiFi.scanDelete();
    for (size_t i = 0; i < count; ++i) {
      JsonObject item = networks.createNestedObject();
      item["ssid"] = entries[i].ssid;
      item["rssi"] = entries[i].rssi;
      item["channel"] = entries[i].channel;
      item["encryption"] = encryptionText(entries[i].encryption);
    }
    doc["ok"] = true;
    sendJson(client_, doc);
  }

  void sendWifiConfigResult(const char* requestId, bool ok, const char* code, const char* message) {
    StaticJsonDocument<640> doc;
    doc["v"] = 1;
    doc["type"] = "wifi_config_result";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    doc["ok"] = ok;
    if (!ok && code != nullptr && code[0] != '\0') {
      doc["code"] = code;
    }
    doc["message"] = message != nullptr ? message : "";
    writeWifiStatus(doc.createNestedObject("wifi"));
    sendJson(client_, doc);
  }

  void sendWifiSetupFinished(const char* requestId, bool ok, const char* code, const char* message) {
    StaticJsonDocument<640> doc;
    doc["v"] = 1;
    doc["type"] = "wifi_setup_finished";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    doc["ok"] = ok;
    if (!ok && code != nullptr && code[0] != '\0') {
      doc["code"] = code;
    }
    doc["message"] = message != nullptr ? message : "";
    writeWifiStatus(doc.createNestedObject("wifi"));
    sendJson(client_, doc);
  }

  void sendResetFaultResult(const char* requestId, bool ok) {
    DynamicJsonDocument doc(3072);
    doc["v"] = 1;
    doc["type"] = "reset_fault_result";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    doc["ok"] = ok;
    writeStatus(doc.createNestedObject("status"));
    sendJson(client_, doc);
  }

  void sendCancelResult(const char* requestId, bool ok) {
    StaticJsonDocument<128> doc;
    doc["v"] = 1;
    doc["type"] = "cancel_result";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    doc["ok"] = ok;
    sendJson(client_, doc);
  }

  void sendGroupAccepted(const char* requestId, const char* groupId, uint32_t seq, size_t blockCount) {
    StaticJsonDocument<192> doc;
    doc["v"] = 1;
    doc["type"] = "group_accepted";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    doc["groupId"] = groupId != nullptr ? groupId : "";
    doc["seq"] = seq;
    doc["blockCount"] = blockCount;
    sendJson(client_, doc);
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
    sendJson(client_, doc);
  }

  void sendGroupStarted(const char* groupId) {
    StaticJsonDocument<128> doc;
    doc["v"] = 1;
    doc["type"] = "group_started";
    doc["groupId"] = groupId != nullptr ? groupId : "";
    doc["seq"] = app_.currentRemoteSeq();
    sendJson(client_, doc);
  }

  void sendBlockEvent(const char* type, const char* groupId, uint32_t seq, size_t blockIndex, const char* blockType) {
    StaticJsonDocument<192> doc;
    doc["v"] = 1;
    doc["type"] = type;
    doc["groupId"] = groupId != nullptr ? groupId : "";
    doc["seq"] = seq;
    doc["blockIndex"] = blockIndex;
    doc["blockType"] = blockType != nullptr ? blockType : "wait";
    sendJson(client_, doc);
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
    sendJson(client_, doc);
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
    sendJson(client_, doc);
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
    sendJson(client_, doc);
  }

  void sendTelemetryOverflow() {
    StaticJsonDocument<160> doc;
    doc["v"] = 1;
    doc["type"] = "telemetry_overflow";
    doc["code"] = "telemetry_overflow";
    doc["message"] = "Motor telemetry queue overflowed";
    doc["timestampMs"] = millis();
    sendJson(client_, doc);
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
    sendJson(client_, doc);
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

  void writeWifiStatus(JsonObject json) {
    updateWifiConnectionState();
    json["setupApActive"] = setupApActive_;
    json["setupSsid"] = setupSsid_;
    json["setupPassword"] = setupPassword_;
    json["setupIpAddress"] = WiFi.softAPIP().toString();
    json["staConnected"] = WiFi.status() == WL_CONNECTED;
    json["staConnecting"] = staConnecting_;
    json["staSsid"] = WiFi.SSID();
    json["ipAddress"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
    json["wifiRssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
    json["savedCredentials"] = savedCredentials_;
    json["phase"] = wifiPhaseText();
    if (lastWifiError_.length() > 0) {
      json["lastError"] = lastWifiError_;
    }
  }

  void beginWifi() {
    configureSetupApIdentity();
    String ssid;
    String password;
    savedCredentials_ = loadWifiCredentials(ssid, password);
    if (!savedCredentials_ && strlen(config_.wifiSsid) > 0) {
      ssid = config_.wifiSsid;
      password = config_.wifiPassword;
      savedCredentials_ = true;
    }
    if (savedCredentials_) {
      connectStation(ssid.c_str(), password.c_str());
      if (!waitForStation(12000)) {
        lastWifiError_ = "station_connect_failed";
        startSetupAp();
      }
    } else {
      lastWifiError_ = "no_credentials";
      startSetupAp();
    }
    log_.print("[tcp] WiFi IP: ");
    log_.println(currentIp());
  }

  void configureSetupApIdentity() {
    const String mac = WiFi.macAddress();
    String suffix = mac;
    suffix.replace(":", "");
    if (suffix.length() > 6) {
      suffix = suffix.substring(suffix.length() - 6);
    }
    setupSsid_ = String("auto-typer-setup-") + suffix.substring(suffix.length() > 4 ? suffix.length() - 4 : 0);
    setupPassword_ = String("ATSETUP-") + suffix;
  }

  bool loadWifiCredentials(String& ssid, String& password) {
    Preferences prefs;
    if (!prefs.begin("auto-typer", true)) {
      return false;
    }
    ssid = prefs.getString("wifiSsid", "");
    password = prefs.getString("wifiPass", "");
    prefs.end();
    return ssid.length() > 0;
  }

  bool saveWifiCredentials(const String& ssid, const String& password) {
    Preferences prefs;
    if (!prefs.begin("auto-typer", false)) {
      return false;
    }
    const bool ok = prefs.putString("wifiSsid", ssid) > 0 && prefs.putString("wifiPass", password) == password.length();
    prefs.end();
    return ok;
  }

  void startSetupAp() {
    if (setupApActive_) {
      return;
    }
    WiFi.mode(WIFI_AP_STA);
    setupApActive_ = WiFi.softAP(setupSsid_.c_str(), setupPassword_.c_str());
    if (setupApActive_) {
      log_.print("[tcp] WiFi setup AP started: ");
      log_.print(setupSsid_);
      log_.print(" password=");
      log_.println(setupPassword_);
      char message[64];
      snprintf(message, sizeof(message), "Setup WiFi %s", setupSsid_.c_str());
      app_.showMessage(message);
    } else {
      log_.println("[tcp] WiFi setup AP failed");
      lastWifiError_ = "setup_ap_failed";
    }
  }

  void connectStation(const char* ssid, const char* password) {
    WiFi.mode(setupApActive_ ? WIFI_AP_STA : WIFI_STA);
    WiFi.begin(ssid, password);
    staConnecting_ = true;
    lastWifiAttemptMs_ = millis();
    lastWifiError_ = "";
    log_.print("[tcp] Connecting WiFi SSID: ");
    log_.println(ssid);
  }

  bool waitForStation(uint32_t timeoutMs) {
    const uint32_t startedAt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startedAt < timeoutMs) {
      delay(50);
      app_.tick();
      log_.print(".");
    }
    log_.println();
    updateWifiConnectionState();
    return WiFi.status() == WL_CONNECTED;
  }

  void updateWifiConnectionState() {
    if (WiFi.status() == WL_CONNECTED) {
      if (staConnecting_) {
        log_.print("[tcp] WiFi connected, IP: ");
        log_.println(WiFi.localIP().toString());
      }
      staConnecting_ = false;
      lastWifiError_ = "";
      return;
    }
    if (staConnecting_ && millis() - lastWifiAttemptMs_ > kWifiConnectTimeoutMs) {
      staConnecting_ = false;
      lastWifiError_ = "station_connect_failed";
      startSetupAp();
      log_.println("[tcp] WiFi station connection timed out");
    }
  }

  const char* wifiPhaseText() const {
    if (WiFi.status() == WL_CONNECTED) {
      return "connected";
    }
    if (staConnecting_) {
      return "connecting";
    }
    if (!savedCredentials_) {
      return "no_credentials";
    }
    if (lastWifiError_.length() > 0) {
      return "failed";
    }
    return "idle";
  }

  String currentIp() const {
    if (WiFi.status() == WL_CONNECTED) {
      return WiFi.localIP().toString();
    }
    return setupApActive_ ? WiFi.softAPIP().toString() : "";
  }

  static const char* encryptionText(wifi_auth_mode_t encryption) {
    switch (encryption) {
      case WIFI_AUTH_OPEN:
        return "open";
      case WIFI_AUTH_WEP:
        return "wep";
      case WIFI_AUTH_WPA_PSK:
        return "wpa";
      case WIFI_AUTH_WPA2_PSK:
        return "wpa2";
      case WIFI_AUTH_WPA_WPA2_PSK:
        return "wpa_wpa2";
      case WIFI_AUTH_WPA2_ENTERPRISE:
        return "wpa2_enterprise";
#if defined(WIFI_AUTH_WPA3_PSK)
      case WIFI_AUTH_WPA3_PSK:
        return "wpa3";
#endif
#if defined(WIFI_AUTH_WPA2_WPA3_PSK)
      case WIFI_AUTH_WPA2_WPA3_PSK:
        return "wpa2_wpa3";
#endif
      default:
        return "unknown";
    }
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
  bool sendJson(WiFiClient& client, TDoc& doc) {
    if (!client || !client.connected()) {
      return false;
    }
    serializeJson(doc, client);
    client.write('\n');
    return true;
  }

  void disconnectClient() {
    log_.println("[tcp] disconnect");
    if (app_.executorRunning()) {
      app_.cancelCurrentJob();
    }
    client_.stop();
    lineBuffer_ = "";
    handshaken_ = false;
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
  bool setupApActive_;
  bool staConnecting_;
  bool savedCredentials_;
  uint32_t lastWifiAttemptMs_;
  String setupSsid_;
  String setupPassword_;
  String lastWifiError_;
  static constexpr uint16_t kMotorStateUpdateIntervalMs = 25;
  static constexpr size_t kMaxWifiNetworks = 24;
  static constexpr size_t kMaxWifiSsidLength = 32;
  static constexpr size_t kMaxWifiPasswordLength = 63;
  static constexpr uint32_t kWifiConnectTimeoutMs = 20000;
};

}  // namespace auto_typer
