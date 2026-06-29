#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

#include "auto_typer_runtime.h"

namespace auto_typer {

class HttpControlServer {
 public:
  HttpControlServer(const TypingConfig& config, AutoTyperApplication& app, Print& log)
      : config_(config), app_(app), log_(log), server_(80) {}

  void begin() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(config_.wifiSsid, config_.wifiPassword);
    log_.print("Connecting WiFi SSID: ");
    log_.println(config_.wifiSsid);

    const uint32_t startedAt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 12000) {
      delay(250);
      log_.print(".");
    }
    log_.println();

    if (WiFi.status() == WL_CONNECTED) {
      log_.print("WiFi IP: ");
      log_.println(WiFi.localIP());
    } else {
      log_.println("WiFi connection failed; HTTP control offline");
      return;
    }

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
    server_.on("/api/keymap", HTTP_GET, [this]() { sendKeymap(); });
    server_.on("/api/keymap", HTTP_PUT, [this]() { handlePutKeymap(); });
    server_.on("/api/debug/motor/move-relative", HTTP_POST, [this]() { handleMotorMove(); });
    server_.on("/api/debug/motor/enable", HTTP_POST, [this]() { handleMotorEnable(); });
    server_.on("/api/debug/motor/stop", HTTP_POST, [this]() { handleMotorStop(); });
    server_.on("/api/debug/servo/apply", HTTP_POST, [this]() { handleServoApply(); });
    server_.on("/api/debug/probe-key", HTTP_POST, [this]() { handleProbeKey(); });
    server_.onNotFound([this]() { sendError(404, "not_found", "Route not found"); });
  }

  void handleCreateJob() {
    const String body = server_.arg("plain");
    const String text = extractString(body, "text");
    if (text.length() == 0) {
      sendError(400, "invalid_job", "Missing text");
      return;
    }

    const bool accepted = app_.submitTextJob(text.c_str());
    const JobSnapshot snapshot = app_.snapshot();
    String json = "{";
    json += "\"jobId\":\"";
    json += snapshot.jobId;
    json += "\",\"accepted\":";
    json += accepted ? "true" : "false";
    json += ",\"planStatus\":\"";
    json += planStatusJson(snapshot.planStatus);
    json += "\",\"stepCount\":";
    json += snapshot.totalSteps;
    if (snapshot.failedKey != '\0') {
      json += ",\"failedKey\":\"";
      json += escapeJsonChar(snapshot.failedKey);
      json += "\"";
    }
    json += "}";
    sendJson(accepted ? 200 : 409, json);
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

  void handleMotorMove() {
    const String body = server_.arg("plain");
    const uint8_t motorId = static_cast<uint8_t>(extractInt(body, "motorId", 0));
    const uint16_t rpm = static_cast<uint16_t>(extractInt(body, "rpm", 0));
    const uint8_t acceleration = static_cast<uint8_t>(extractInt(body, "acceleration", 0));
    const uint32_t steps = static_cast<uint32_t>(extractInt(body, "steps", 0));
    const bool sync = extractBool(body, "sync", false);
    const MotorDirection direction = extractString(body, "direction") == "ccw" ? MotorDirection::Ccw : MotorDirection::Cw;

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
    const String body = server_.arg("plain");
    const uint8_t motorId = static_cast<uint8_t>(extractInt(body, "motorId", 0));
    const bool enabled = extractBool(body, "enabled", true);
    const bool sync = extractBool(body, "sync", false);
    if (motorId == 0 || !app_.debugMotorEnable(motorId, enabled, sync)) {
      sendError(409, "motor_enable_rejected", "Motor enable rejected");
      return;
    }
    sendStatus();
  }

  void handleMotorStop() {
    const String body = server_.arg("plain");
    const uint8_t motorId = static_cast<uint8_t>(extractInt(body, "motorId", 0));
    const bool sync = extractBool(body, "sync", false);
    if (motorId == 0 || !app_.debugMotorStop(motorId, sync)) {
      sendError(409, "motor_stop_rejected", "Motor stop rejected");
      return;
    }
    sendStatus();
  }

  void handleServoApply() {
    const String command = extractString(server_.arg("plain"), "command");
    bool ok = false;
    if (command == "press") {
      ok = app_.debugServo(PressAction::Press);
    } else if (command == "release") {
      ok = app_.debugServo(PressAction::Release);
    } else if (command == "neutral") {
      ok = app_.debugServoNeutral();
    }
    if (!ok) {
      sendError(409, "servo_rejected", "Servo command rejected");
      return;
    }
    sendStatus();
  }

  void handleProbeKey() {
    const String body = server_.arg("plain");
    const String key = extractString(body, "key");
    const float xMm = extractFloat(body, "xMm", extractFloat(body, "x", 0.0f));
    const float yMm = extractFloat(body, "yMm", extractFloat(body, "y", 0.0f));
    if (key.length() == 0 || !app_.upsertKeyBinding(key[0], {xMm, yMm})) {
      sendError(400, "invalid_probe", "Invalid key probe");
      return;
    }
    sendKeymap();
  }

  void handlePutKeymap() {
    KeyBinding parsed[64];
    size_t count = 0;
    if (!parseBindings(server_.arg("plain"), parsed, sizeof(parsed) / sizeof(parsed[0]), count)) {
      sendError(400, "invalid_keymap", "Invalid keymap bindings");
      return;
    }
    if (!app_.replaceKeymap(parsed, count)) {
      sendError(409, "keymap_rejected", "Keymap rejected");
      return;
    }
    sendKeymap();
  }

  void sendStatus() {
    const JobSnapshot snapshot = app_.snapshot();
    String json = "{";
    json += "\"deviceId\":\"";
    json += config_.deviceId;
    json += "\",\"firmwareVersion\":\"";
    json += config_.firmwareVersion;
    json += "\",\"ipAddress\":\"";
    json += WiFi.localIP().toString();
    json += "\",\"mode\":\"";
    json += modeJson(app_.mode());
    json += "\",\"health\":\"";
    json += app_.mode() == DeviceMode::Faulted ? "fault" : "ok";
    json += "\",\"wifiRssi\":";
    json += WiFi.RSSI();
    json += ",\"servoReady\":";
    json += app_.servoReady() ? "true" : "false";
    json += ",\"motionReady\":";
    json += app_.motionReady() ? "true" : "false";
    json += ",\"keymapVersion\":";
    json += app_.keymapVersion();
    if (snapshot.state != JobState::None) {
      json += ",\"currentJob\":";
      json += jobJson(snapshot);
    }
    json += "}";
    sendJson(200, json);
  }

  void sendJob() {
    sendJson(200, jobJson(app_.snapshot()));
  }

  void sendKeymap() {
    String json = "{\"version\":";
    json += app_.keymapVersion();
    json += ",\"machine\":\"feiyu200\",\"updatedAt\":\"device\",\"bindings\":[";
    const KeyBinding* bindings = app_.keymap();
    for (size_t i = 0; i < app_.keymapCount(); ++i) {
      if (i > 0) {
        json += ",";
      }
      json += "{\"key\":\"";
      json += escapeJsonChar(bindings[i].key);
      json += "\",\"point\":{\"xMm\":";
      json += String(bindings[i].point.xMm, 3);
      json += ",\"yMm\":";
      json += String(bindings[i].point.yMm, 3);
      json += "}}";
    }
    json += "]}";
    sendJson(200, json);
  }

  String jobJson(const JobSnapshot& snapshot) const {
    String json = "{";
    json += "\"jobId\":\"";
    json += snapshot.jobId;
    json += "\",\"state\":\"";
    json += jobStateJson(snapshot.state);
    json += "\",\"textLength\":";
    json += snapshot.textLength;
    json += ",\"currentIndex\":";
    json += snapshot.currentIndex;
    json += ",\"currentStep\":";
    json += snapshot.currentStep;
    json += ",\"totalSteps\":";
    json += snapshot.totalSteps;
    json += ",\"currentPoint\":{\"xMm\":";
    json += String(snapshot.currentPoint.xMm, 3);
    json += ",\"yMm\":";
    json += String(snapshot.currentPoint.yMm, 3);
    json += "}}";
    return json;
  }

  void sendJson(int status, const String& json) {
    server_.send(status, "application/json", json);
  }

  void sendError(int status, const char* code, const char* message) {
    String json = "{\"code\":\"";
    json += code;
    json += "\",\"message\":\"";
    json += message;
    json += "\"}";
    sendJson(status, json);
  }

  static String extractString(const String& json, const char* key) {
    const String marker = String("\"") + key + "\":";
    int start = json.indexOf(marker);
    if (start < 0) {
      return "";
    }
    start = json.indexOf('"', start + marker.length());
    if (start < 0) {
      return "";
    }
    const int end = json.indexOf('"', start + 1);
    if (end < 0) {
      return "";
    }
    return json.substring(start + 1, end);
  }

  static long extractInt(const String& json, const char* key, long fallback) {
    const String marker = String("\"") + key + "\":";
    int start = json.indexOf(marker);
    if (start < 0) {
      return fallback;
    }
    start += marker.length();
    return json.substring(start).toInt();
  }

  static float extractFloat(const String& json, const char* key, float fallback) {
    const String marker = String("\"") + key + "\":";
    int start = json.indexOf(marker);
    if (start < 0) {
      return fallback;
    }
    start += marker.length();
    return json.substring(start).toFloat();
  }

  static bool extractBool(const String& json, const char* key, bool fallback) {
    const String marker = String("\"") + key + "\":";
    int start = json.indexOf(marker);
    if (start < 0) {
      return fallback;
    }
    start += marker.length();
    return json.substring(start).startsWith("true");
  }

  static bool parseBindings(const String& json, KeyBinding* bindings, size_t capacity, size_t& count) {
    count = 0;
    int cursor = 0;
    while (count < capacity) {
      const int keyMarker = json.indexOf("\"key\":", cursor);
      if (keyMarker < 0) {
        break;
      }
      const int keyQuote = json.indexOf('"', keyMarker + 6);
      if (keyQuote < 0) {
        return false;
      }
      const int keyEnd = json.indexOf('"', keyQuote + 1);
      if (keyEnd < 0) {
        return false;
      }
      const String keyText = json.substring(keyQuote + 1, keyEnd);
      const int xMarker = json.indexOf("\"xMm\":", keyEnd);
      const int yMarker = json.indexOf("\"yMm\":", keyEnd);
      if (keyText.length() == 0 || xMarker < 0 || yMarker < 0) {
        return false;
      }
      bindings[count] = {keyText[0],
                         {json.substring(xMarker + 6).toFloat(), json.substring(yMarker + 6).toFloat()}};
      ++count;
      cursor = keyEnd + 1;
    }
    return count > 0;
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
      case JobState::Running:
        return "running";
      case JobState::Completed:
        return "completed";
      case JobState::Cancelled:
        return "cancelled";
      case JobState::Failed:
        return "failed";
      case JobState::None:
      default:
        return "queued";
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

  static String escapeJsonChar(char value) {
    if (value == '"') {
      return "\\\"";
    }
    if (value == '\\') {
      return "\\\\";
    }
    if (value == ' ') {
      return " ";
    }
    return String(value);
  }

  const TypingConfig& config_;
  AutoTyperApplication& app_;
  Print& log_;
  WebServer server_;
};

}  // namespace auto_typer
