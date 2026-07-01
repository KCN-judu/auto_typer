#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>

#include "auto_typer_runtime.h"

namespace auto_typer {

class HttpControlServer {
 public:
  HttpControlServer(const TypingConfig& config, AutoTyperApplication& app, Print& log)
      : config_(config), app_(app), log_(log), server_(80) {}

  void begin() {
    if (strlen(config_.wifiSsid) == 0) {
      WiFi.mode(WIFI_AP);
      WiFi.softAP("auto-typer-setup");
      log_.println("WiFi credentials missing; setup AP started: auto-typer-setup");
    } else {
      WiFi.mode(WIFI_STA);
      WiFi.begin(config_.wifiSsid, config_.wifiPassword);
      log_.print("Connecting WiFi SSID: ");
      log_.println(config_.wifiSsid);
      const uint32_t startedAt = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 12000) {
        delay(50);
        app_.tick();
        log_.print(".");
      }
      log_.println();
      if (WiFi.status() != WL_CONNECTED) {
        WiFi.mode(WIFI_AP);
        WiFi.softAP("auto-typer-setup");
        log_.println("WiFi connection failed; setup AP started: auto-typer-setup");
      }
    }

    log_.print("HTTP IP: ");
    log_.println(currentIp());
    registerRoutes();
    server_.begin();
    log_.println("HTTP control ready");
  }

  void tick() {
    server_.handleClient();
  }

 private:
  void registerRoutes() {
    server_.on("/api/status", HTTP_GET, [this]() { sendStatus(); });
    server_.on("/api/jobs", HTTP_POST, [this]() { handleCreateJob(); });
    server_.on("/api/jobs/current", HTTP_GET, [this]() { sendJob(); });
    server_.on("/api/jobs/current/cancel", HTTP_POST, [this]() { handleCancelJob(); });
    server_.on("/api/machine/stop", HTTP_POST, [this]() { handleEmergencyStop(); });
    server_.on("/api/machine/reset-fault", HTTP_POST, [this]() { handleResetFault(); });
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
    StaticJsonDocument<512> request;
    if (!parseBody(request)) {
      return;
    }
    const char* text = request["text"] | "";
    if (strlen(text) == 0) {
      sendError(400, "invalid_job", "Missing text");
      return;
    }

    const bool accepted = app_.submitTextJob(text);
    const JobSnapshot snapshot = app_.snapshot();
    StaticJsonDocument<512> response;
    response["jobId"] = String(snapshot.jobId);
    response["accepted"] = accepted;
    response["planStatus"] = planStatusJson(snapshot.planStatus);
    response["stepCount"] = snapshot.totalBlocks;
    if (snapshot.failedKey != '\0') {
      char failedKey[2] = {snapshot.failedKey, '\0'};
      response["failedKey"] = failedKey;
    }
    sendJson(accepted ? 200 : 409, response);
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
    if (!app_.resetFault()) {
      sendError(409, "reset_fault_rejected", "Fault reset rejected");
      return;
    }
    sendStatus();
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
      ok = app_.debugServoNeutral(dwellMs);
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
    StaticJsonDocument<2048> response;
    response["deviceId"] = config_.deviceId;
    response["firmwareVersion"] = config_.firmwareVersion;
    response["ipAddress"] = currentIp();
    response["mode"] = modeJson(app_.mode());
    response["health"] = app_.mode() == DeviceMode::Faulted ? "fault" : "ok";
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
    if (app_.mode() == DeviceMode::Faulted) {
      JsonObject fault = response.createNestedObject("fault");
      fault["code"] = snapshot.faultCode;
      fault["message"] = snapshot.faultMessage;
      fault["recoverable"] = true;
    }
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
    json["currentBlock"] = snapshot.currentBlock;
    json["totalBlocks"] = snapshot.totalBlocks;
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
    };
    for (uint8_t id : ids) {
      const MotorState state = app_.motorState(id);
      JsonObject motor = motors.createNestedObject();
      motor["id"] = id;
      motor["enabled"] = state.enabled;
      motor["fault"] = state.fault;
      motor["moving"] = state.moving;
      motor["estimatedPositionSteps"] = state.estimatedPositionSteps;
      motor["observedPositionSteps"] = state.observedPositionSteps;
      motor["velocityRpm"] = state.velocityRpm;
      motor["lastFeedbackMs"] = state.lastFeedbackMs;
    }
  }

  template <typename T>
  void sendJson(int status, T& doc) {
    String json;
    serializeJson(doc, json);
    server_.send(status, "application/json", json);
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
    return WiFi.softAPIP().toString();
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
      case PlanStatus::Ok:
      default:
        return "ok";
    }
  }

  const TypingConfig& config_;
  AutoTyperApplication& app_;
  Print& log_;
  WebServer server_;
};

}  // namespace auto_typer
