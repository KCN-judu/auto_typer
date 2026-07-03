#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include "auto_typer_runtime.h"

namespace auto_typer {

class HttpControlServer {
 public:
  HttpControlServer(const TypingConfig& config, AutoTyperApplication& app, Print& log)
      : config_(config),
        app_(app),
        log_(log),
        server_(80),
        setupApActive_(false),
        staConnecting_(false),
        savedCredentials_(false),
        lastWifiAttemptMs_(0),
        lastWifiError_("") {}

  void begin() {
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

    log_.print("HTTP IP: ");
    log_.println(currentIp());
    registerRoutes();
    server_.begin();
    log_.println("HTTP control ready");
  }

  void tick() {
    updateWifiConnectionState();
    server_.handleClient();
  }

 private:
  static constexpr size_t kMaxJobRequestBytes = 8192;
  static constexpr size_t kMaxWifiNetworks = 24;
  static constexpr size_t kMaxWifiSsidLength = 32;
  static constexpr size_t kMaxWifiPasswordLength = 63;
  static constexpr uint32_t kWifiConnectTimeoutMs = 20000;

  void registerRoutes() {
    server_.on("/api/status", HTTP_GET, [this]() { sendStatus(); });
    server_.on("/api/jobs", HTTP_POST, [this]() { handleCreateJob(); });
    server_.on("/api/jobs/current", HTTP_GET, [this]() { sendJob(); });
    server_.on("/api/jobs/current/cancel", HTTP_POST, [this]() { handleCancelJob(); });
    server_.on("/api/machine/stop", HTTP_POST, [this]() { handleEmergencyStop(); });
    server_.on("/api/machine/reset-fault", HTTP_POST, [this]() { handleResetFault(); });
    server_.on("/api/machine/probe-motors", HTTP_POST, [this]() { handleProbeMotors(); });
    server_.on("/api/diagnostics/can", HTTP_GET, [this]() { sendCanDiagnostics(); });
    server_.on("/api/diagnostics/protocol-trace", HTTP_GET, [this]() { sendProtocolTrace(); });
    server_.on("/api/wifi/status", HTTP_GET, [this]() { sendWifiStatus(); });
    server_.on("/api/wifi/networks", HTTP_GET, [this]() { sendWifiNetworks(); });
    server_.on("/api/wifi/config", HTTP_POST, [this]() { handleWifiConfig(); });
    server_.on("/api/wifi/setup/finish", HTTP_POST, [this]() { handleWifiSetupFinish(); });
    server_.on("/api/keymap", HTTP_GET, [this]() { sendKeymap(); });
    server_.on("/api/keymap", HTTP_PUT, [this]() { handlePutKeymap(); });
    server_.on("/api/debug/motor/move-relative", HTTP_POST, [this]() { handleMotorMove(); });
    server_.on("/api/debug/motor/enable", HTTP_POST, [this]() { handleMotorEnable(); });
    server_.on("/api/debug/motor/stop", HTTP_POST, [this]() { handleMotorStop(); });
    server_.on("/api/debug/servo/apply", HTTP_POST, [this]() { handleServoApply(); });
    server_.on("/api/debug/probe-key", HTTP_POST, [this]() { handleProbeKey(); });
    server_.onNotFound([this]() { sendError(404, "not_found", "Route not found"); });
  }

  bool parseBody(JsonDocument& doc) {
    const DeserializationError error = deserializeJson(doc, server_.arg("plain"));
    if (error) {
      sendError(400, "invalid_json", "Invalid JSON body");
      return false;
    }
    return true;
  }

  void handleCreateJob() {
    const String body = server_.arg("plain");
    log_.print("[http] POST /api/jobs bodyBytes=");
    log_.println(body.length());
    if (body.length() == 0) {
      sendError(400, "invalid_json", "Missing JSON body");
      return;
    }
    if (body.length() > kMaxJobRequestBytes) {
      log_.println("[http] POST /api/jobs body too large");
      sendError(413, "job_too_large", "Job request body too large");
      return;
    }

    DynamicJsonDocument request(body.length() + 512);
    const DeserializationError error = deserializeJson(request, body);
    if (error) {
      log_.print("[http] POST /api/jobs invalid_json ");
      log_.println(error.c_str());
      sendError(400, "invalid_json", "Invalid JSON body");
      return;
    }
    const char* text = request["text"] | "";
    log_.print("[http] POST /api/jobs textLength=");
    log_.println(strlen(text));
    if (strlen(text) == 0) {
      log_.println("[http] POST /api/jobs missing text");
      sendError(400, "invalid_job", "Missing text");
      return;
    }

    const SubmitJobResult result = app_.submitTextJob(text);
    const JobSnapshot snapshot = app_.snapshot();
    StaticJsonDocument<768> response;
    if (result.accepted) {
      response["jobId"] = String(result.jobId);
    }
    response["accepted"] = result.accepted;
    response["planStatus"] = planStatusJson(result.planStatus);
    response["stepCount"] = result.stepCount;
    if (!result.accepted) {
      response["rejectionCode"] = result.rejectionCode;
      response["rejectionMessage"] = result.rejectionMessage;
    }
    if (result.planStatus == PlanStatus::DeviceFault || app_.mode() == DeviceMode::Faulted) {
      JsonObject fault = response.createNestedObject("fault");
      fault["code"] = snapshot.faultCode;
      fault["message"] = snapshot.faultMessage;
      fault["recoverable"] = app_.canDiagnostics().recoverable;
    }
    if (result.failedKey != '\0') {
      char failedKey[2] = {result.failedKey, '\0'};
      response["failedKey"] = failedKey;
    }
    log_.print("[http] POST /api/jobs accepted=");
    log_.print(result.accepted ? "1" : "0");
    log_.print(" planStatus=");
    log_.print(planStatusJson(result.planStatus));
    if (!result.accepted) {
      log_.print(" rejection=");
      log_.print(result.rejectionCode);
    }
    log_.println();
    sendJson(200, response);
    log_.println("[http] POST /api/jobs response sent");
  }

  void handleCancelJob() {
    if (!app_.cancelCurrentJob()) {
      sendError(409, "no_active_job", "No queued or running job");
      return;
    }
    sendStatus();
  }

  void handleEmergencyStop() {
    app_.emergencyStop();
    sendStatus();
  }

  void handleResetFault() {
    app_.resetFault();
    sendStatus();
  }

  void handleProbeMotors() {
    if (!app_.probeMotors()) {
      sendError(409, "probe_rejected", "Motor probe rejected while device is busy");
      return;
    }
    sendStatus();
  }

  void sendWifiStatus() {
    StaticJsonDocument<768> response;
    writeWifiStatus(response.to<JsonObject>());
    sendJson(200, response);
  }

  void sendWifiNetworks() {
    struct WifiScanEntry {
      String ssid;
      int32_t rssi;
      int32_t channel;
      wifi_auth_mode_t encryption;
    };

    WifiScanEntry networks[kMaxWifiNetworks];
    size_t count = 0;
    const int found = WiFi.scanNetworks(false, false);
    if (found < 0) {
      sendError(503, "wifi_scan_failed", "WiFi scan failed");
      return;
    }

    for (int i = 0; i < found; ++i) {
      const String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) {
        continue;
      }
      const int32_t rssi = WiFi.RSSI(i);
      size_t existing = kMaxWifiNetworks;
      for (size_t j = 0; j < count; ++j) {
        if (networks[j].ssid == ssid) {
          existing = j;
          break;
        }
      }
      if (existing < kMaxWifiNetworks) {
        if (rssi > networks[existing].rssi) {
          networks[existing].rssi = rssi;
          networks[existing].channel = WiFi.channel(i);
          networks[existing].encryption = WiFi.encryptionType(i);
        }
        continue;
      }
      if (count >= kMaxWifiNetworks) {
        continue;
      }
      networks[count] = {ssid, rssi, WiFi.channel(i), WiFi.encryptionType(i)};
      ++count;
    }
    WiFi.scanDelete();

    for (size_t i = 0; i < count; ++i) {
      for (size_t j = i + 1; j < count; ++j) {
        if (networks[j].rssi > networks[i].rssi) {
          const WifiScanEntry swap = networks[i];
          networks[i] = networks[j];
          networks[j] = swap;
        }
      }
    }

    DynamicJsonDocument response(4096);
    JsonArray array = response.createNestedArray("networks");
    for (size_t i = 0; i < count; ++i) {
      JsonObject item = array.createNestedObject();
      item["ssid"] = networks[i].ssid;
      item["rssi"] = networks[i].rssi;
      item["channel"] = networks[i].channel;
      item["encryption"] = encryptionText(networks[i].encryption);
      item["secure"] = networks[i].encryption != WIFI_AUTH_OPEN;
    }
    sendJson(200, response);
  }

  void handleWifiConfig() {
    StaticJsonDocument<384> request;
    if (!parseBody(request)) {
      return;
    }

    const String ssid = request["ssid"] | "";
    const String password = request["password"] | "";
    if (ssid.length() == 0 || ssid.length() > kMaxWifiSsidLength) {
      sendError(400, "invalid_wifi_ssid", "WiFi SSID is required");
      return;
    }
    if (password.length() > kMaxWifiPasswordLength || (password.length() > 0 && password.length() < 8)) {
      sendError(400, "invalid_wifi_password", "WiFi password must be empty or 8-63 characters");
      return;
    }
    if (!saveWifiCredentials(ssid, password)) {
      sendError(500, "wifi_credentials_save_failed", "WiFi credentials could not be saved");
      return;
    }

    savedCredentials_ = true;
    startSetupAp();
    connectStation(ssid.c_str(), password.c_str());
    sendWifiStatus();
  }

  void handleWifiSetupFinish() {
    if (WiFi.status() != WL_CONNECTED) {
      sendError(409, "wifi_not_connected", "Station WiFi is not connected");
      return;
    }
    if (setupApActive_) {
      WiFi.softAPdisconnect(true);
      setupApActive_ = false;
      WiFi.mode(WIFI_STA);
    }
    sendWifiStatus();
  }

  void handleMotorMove() {
    StaticJsonDocument<384> request;
    if (!parseBody(request)) {
      return;
    }
    const uint8_t motorId = request["motorId"] | 0;
    const uint16_t rpm = request["rpm"] | 0;
    const uint8_t acceleration = request["acceleration"] | 0;
    const uint32_t steps = request["steps"] | 0;
    const bool sync = request["sync"] | false;
    const String directionText = request["direction"] | "cw";
    const MotorDirection direction = directionText == "ccw" ? MotorDirection::Ccw : MotorDirection::Cw;
    if (motorId == 0 || rpm == 0 || steps == 0) {
      sendError(400, "invalid_motor_move", "motorId or motor group, rpm and steps are required");
      return;
    }
    if (!app_.debugMotorMoveRelative(motorId, direction, rpm, acceleration, steps, sync)) {
      sendError(409, "motor_move_rejected", "Motor move rejected");
      return;
    }
    sendStatus();
  }

  void handleMotorEnable() {
    StaticJsonDocument<256> request;
    if (!parseBody(request)) {
      return;
    }
    const uint8_t motorId = request["motorId"] | 0;
    const bool enabled = request["enabled"] | true;
    const bool sync = request["sync"] | false;
    if (motorId == 0 || !app_.debugMotorEnable(motorId, enabled, sync)) {
      sendError(409, "motor_enable_rejected", "Motor enable rejected");
      return;
    }
    sendStatus();
  }

  void handleMotorStop() {
    StaticJsonDocument<256> request;
    if (!parseBody(request)) {
      return;
    }
    const uint8_t motorId = request["motorId"] | 0;
    const bool sync = request["sync"] | false;
    if (motorId == 0 || !app_.debugMotorStop(motorId, sync)) {
      sendError(409, "motor_stop_rejected", "Motor stop rejected");
      return;
    }
    sendStatus();
  }

  void handleServoApply() {
    StaticJsonDocument<256> request;
    if (!parseBody(request)) {
      return;
    }
    const String command = request["command"] | "";
    const uint16_t dwellMs = request["durationMs"] | 0;
    bool ok = false;
    if (command == "press") {
      ok = app_.debugServo(PressAction::Press, dwellMs);
    } else if (command == "release") {
      ok = app_.debugServo(PressAction::Release, dwellMs);
    } else if (command == "neutral") {
      ok = app_.debugServo(PressAction::Neutral, dwellMs);
    }
    if (!ok) {
      sendError(409, "servo_rejected", "Servo command rejected");
      return;
    }
    sendStatus();
  }

  void handleProbeKey() {
    StaticJsonDocument<384> request;
    if (!parseBody(request)) {
      return;
    }
    const String key = request["key"] | "";
    const JsonVariant point = request["point"];
    const float xMm = point["xMm"] | (request["xMm"] | (request["x"] | 0.0f));
    const float yMm = point["yMm"] | (request["yMm"] | (request["y"] | 0.0f));
    if (key.length() == 0 || !app_.upsertKeyBinding(key[0], {xMm, yMm})) {
      sendError(400, "invalid_probe", "Invalid key probe");
      return;
    }
    sendKeymap();
  }

  void handlePutKeymap() {
    StaticJsonDocument<8192> request;
    if (!parseBody(request)) {
      return;
    }
    KeyBinding parsed[64];
    size_t count = 0;
    for (JsonObject binding : request["bindings"].as<JsonArray>()) {
      if (count >= sizeof(parsed) / sizeof(parsed[0])) {
        sendError(400, "invalid_keymap", "Too many keymap bindings");
        return;
      }
      const String key = binding["key"] | "";
      if (key.length() == 0) {
        sendError(400, "invalid_keymap", "Invalid key binding");
        return;
      }
      parsed[count] = {key[0], {binding["point"]["xMm"] | 0.0f, binding["point"]["yMm"] | 0.0f}};
      ++count;
    }
    if (count == 0 || !app_.replaceKeymap(parsed, count)) {
      sendError(409, "keymap_rejected", "Keymap rejected");
      return;
    }
    sendKeymap();
  }

  void sendStatus() {
    StaticJsonDocument<4096> response;
    response["deviceId"] = config_.deviceId;
    response["firmwareVersion"] = config_.firmwareVersion;
    response["ipAddress"] = currentIp();
    response["mode"] = modeJson(app_.mode());
    response["health"] =
        app_.mode() == DeviceMode::Faulted ? "fault" : (app_.healthNotReady() ? "not_ready" : (app_.healthWarning() ? "warning" : "ok"));
    response["wifiRssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
    response["servoReady"] = app_.servoReady();
    response["motionReady"] = app_.motionReady();
    response["keymapVersion"] = app_.keymapVersion();
    const JobSnapshot snapshot = app_.snapshot();
    if (snapshot.state == JobState::None) {
      response["currentJob"] = nullptr;
    } else {
      writeJob(response.createNestedObject("currentJob"), snapshot);
    }
    writeMotorStates(response.createNestedArray("motors"));
    writeCanDiagnostics(response.createNestedObject("canDiagnostics"), app_.canDiagnostics());
    if (app_.mode() == DeviceMode::Faulted) {
      JsonObject fault = response.createNestedObject("fault");
      fault["code"] = snapshot.faultCode;
      fault["message"] = snapshot.faultMessage;
      fault["recoverable"] = app_.canDiagnostics().recoverable;
    }
    sendJson(200, response);
  }

  void sendCanDiagnostics() {
    StaticJsonDocument<768> response;
    writeCanDiagnostics(response.to<JsonObject>(), app_.canDiagnostics());
    sendJson(200, response);
  }

  void sendProtocolTrace() {
    ProtocolTraceItem items[ProtocolTrace::capacity()];
    const size_t count = app_.protocolTraceSnapshot(items, ProtocolTrace::capacity());
    DynamicJsonDocument response(24576);
    JsonArray trace = response.createNestedArray("trace");
    for (size_t i = 0; i < count; ++i) {
      JsonObject item = trace.createNestedObject();
      item["timeMs"] = items[i].timeMs;
      item["dir"] = items[i].dir;
      item["canId"] = items[i].canId;
      item["extd"] = items[i].extd;
      item["dlc"] = items[i].dlc;
      JsonArray data = item.createNestedArray("data");
      for (uint8_t b = 0; b < items[i].dlc; ++b) {
        data.add(items[i].data[b]);
      }
      item["dataHex"] = items[i].dataHex;
      item["parsed"] = items[i].parsed;
      item["motorId"] = items[i].motorId;
      item["packetIndex"] = items[i].packetIndex;
    }
    writeProtocolDiagnostics(response.createNestedObject("diagnostics"), app_.protocolDiagnostics());
    sendJson(200, response);
  }

  void sendJob() {
    StaticJsonDocument<1024> response;
    const JobSnapshot snapshot = app_.snapshot();
    if (snapshot.state == JobState::None) {
      response["currentJob"] = nullptr;
    } else {
      writeJob(response.createNestedObject("currentJob"), snapshot);
    }
    sendJson(200, response);
  }

  void sendKeymap() {
    StaticJsonDocument<8192> response;
    response["version"] = app_.keymapVersion();
    response["machine"] = "feiyu200";
    response["updatedAt"] = "device";
    JsonArray bindings = response.createNestedArray("bindings");
    const KeyBinding* keymap = app_.keymap();
    for (size_t i = 0; i < app_.keymapCount(); ++i) {
      JsonObject binding = bindings.createNestedObject();
      char key[2] = {keymap[i].key, '\0'};
      binding["key"] = key;
      JsonObject point = binding.createNestedObject("point");
      point["xMm"] = keymap[i].point.xMm;
      point["yMm"] = keymap[i].point.yMm;
    }
    sendJson(200, response);
  }

  void writeJob(JsonObject json, const JobSnapshot& snapshot) {
    json["jobId"] = String(snapshot.jobId);
    json["state"] = jobStateJson(snapshot.state);
    json["textLength"] = snapshot.textLength;
    json["currentIndex"] = snapshot.currentIndex;
    json["currentStep"] = snapshot.currentStep;
    json["totalSteps"] = snapshot.totalSteps;
    JsonObject point = json.createNestedObject("currentPoint");
    point["xMm"] = snapshot.currentPoint.xMm;
    point["yMm"] = snapshot.currentPoint.yMm;
    if (snapshot.faultCode != nullptr && strlen(snapshot.faultCode) > 0) {
      json["message"] = snapshot.faultMessage;
    }
  }

  void writeMotorStates(JsonArray motors) {
    const uint8_t ids[] = {
      config_.topology.xMotorId,
      config_.topology.yLeftMotorId,
      config_.topology.yRightMotorId,
      config_.topology.lineFeedMotorId,
      config_.topology.pressMotorId,
    };
    for (uint8_t id : ids) {
      const MotorState state = app_.motorState(id);
      JsonObject motor = motors.createNestedObject();
      motor["id"] = id;
      motor["role"] = motorRoleJson(state.role);
      motor["readiness"] = motorReadinessJson(state.readiness);
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
  }

  void writeCanDiagnostics(JsonObject json, const CanBusDiagnostics& diagnostics) {
    json["driverReady"] = diagnostics.driverReady;
    json["motionReady"] = diagnostics.motionReady;
    json["fatalFault"] = diagnostics.fatalFault;
    json["recoverable"] = diagnostics.recoverable;
    json["lastAlerts"] = diagnostics.lastAlerts;
    json["txFailedCount"] = diagnostics.txFailedCount;
    json["txRetryCount"] = diagnostics.txRetryCount;
    json["commandQueueFullCount"] = diagnostics.commandQueueFullCount;
    json["busErrorCount"] = diagnostics.busErrorCount;
    json["rxQueueFullCount"] = diagnostics.rxQueueFullCount;
    json["errPassiveCount"] = diagnostics.errPassiveCount;
    json["busOffCount"] = diagnostics.busOffCount;
    json["pendingFrameValid"] = diagnostics.pendingFrameValid;
    json["lastAlertAtMs"] = diagnostics.lastAlertAtMs;
    json["lastFaultAtMs"] = diagnostics.lastFaultAtMs;
    json["lastTxError"] = diagnostics.lastTxError;
    json["lastCommandQueueError"] = diagnostics.lastCommandQueueError;
    json["lastFaultCode"] = diagnostics.lastFaultCode;
    json["lastFaultMessage"] = diagnostics.lastFaultMessage;
  }

  void writeProtocolDiagnostics(JsonObject json, const ProtocolDiagnostics& diagnostics) {
    json["unknownFrameCount"] = diagnostics.unknownFrameCount;
    json["invalidFrameCount"] = diagnostics.invalidFrameCount;
    json["lastEventAtMs"] = diagnostics.lastEventAtMs;
    json["lastInvalidAtMs"] = diagnostics.lastInvalidAtMs;
    json["lastInvalidError"] = diagnostics.lastInvalidError;
    json["lastEventKind"] = EmmV5ProtocolParser::kindText(diagnostics.lastEventKind);
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
      log_.print("WiFi setup AP started: ");
      log_.print(setupSsid_);
      log_.print(" password=");
      log_.println(setupPassword_);
      char message[64];
      snprintf(message, sizeof(message), "Setup WiFi %s", setupSsid_.c_str());
      app_.showMessage(message);
    } else {
      log_.println("WiFi setup AP failed");
      lastWifiError_ = "setup_ap_failed";
    }
  }

  void connectStation(const char* ssid, const char* password) {
    WiFi.mode(setupApActive_ ? WIFI_AP_STA : WIFI_STA);
    WiFi.begin(ssid, password);
    staConnecting_ = true;
    lastWifiAttemptMs_ = millis();
    lastWifiError_ = "";
    log_.print("Connecting WiFi SSID: ");
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
        log_.print("WiFi connected, IP: ");
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
      log_.println("WiFi station connection timed out");
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

  template <typename T>
  void sendJson(int status, T& doc) {
    String json;
    serializeJson(doc, json);
    server_.sendHeader("Connection", "close");
    server_.sendHeader("Cache-Control", "no-store");
    server_.send(status, "application/json", json);
    log_.print("[http] response status=");
    log_.print(status);
    log_.print(" bytes=");
    log_.println(json.length());
  }

  void sendError(int status, const char* code, const char* message) {
    StaticJsonDocument<256> response;
    response["code"] = code;
    response["message"] = message;
    JsonObject details = response.createNestedObject("details");
    details["status"] = status;
    sendJson(status, response);
  }

  String currentIp() const {
    if (WiFi.status() == WL_CONNECTED) {
      return WiFi.localIP().toString();
    }
    return setupApActive_ ? WiFi.softAPIP().toString() : "";
  }

  static const char* modeJson(DeviceMode mode) {
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

  static const char* jobStateJson(JobState state) {
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

  static const char* planStatusJson(PlanStatus status) {
    switch (status) {
      case PlanStatus::KeyNotFound:
        return "key_not_found";
      case PlanStatus::PlanFull:
        return "plan_full";
      case PlanStatus::DeviceFault:
        return "device_fault";
      case PlanStatus::DeviceBusy:
        return "device_busy";
      case PlanStatus::DeviceNotReady:
        return "device_not_ready";
      case PlanStatus::Ok:
      default:
        return "ok";
    }
  }

  static const char* motorRoleJson(MotorRole role) {
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

  static const char* motorReadinessJson(MotorReadiness readiness) {
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

  const TypingConfig& config_;
  AutoTyperApplication& app_;
  Print& log_;
  WebServer server_;
  bool setupApActive_;
  bool staConnecting_;
  bool savedCredentials_;
  uint32_t lastWifiAttemptMs_;
  String setupSsid_;
  String setupPassword_;
  String lastWifiError_;
};

}  // namespace auto_typer
