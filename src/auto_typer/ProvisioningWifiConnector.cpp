#include "network/ProvisioningWifiConnector.h"

namespace auto_typer {

constexpr char ProvisioningWifiConnector::kProvisioningApSsid[];
constexpr char ProvisioningWifiConnector::kProvisioningApPassword[];

ProvisioningWifiConnector::ProvisioningWifiConnector(Print& log)
    : log_(log),
      server_(80),
      tcpReady_(false),
      lastWaitingLogAtMs_(0),
      attemptCount_(0),
      waitingLogsThisAttempt_(0),
      connectStartedAtMs_(0),
      apShutdownRequestedAtMs_(0),
      state_(State::ProvisioningAp),
      pendingApShutdown_(false),
      lastFailureReason_("NONE"),
      currentSsid_(),
      currentPassword_() {}

void ProvisioningWifiConnector::begin() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAP(kProvisioningApSsid, kProvisioningApPassword, kProvisioningApChannel);

  tcpReady_ = false;
  lastWaitingLogAtMs_ = 0;
  attemptCount_ = 0;
  waitingLogsThisAttempt_ = 0;
  connectStartedAtMs_ = 0;
  apShutdownRequestedAtMs_ = 0;
  state_ = State::ProvisioningAp;
  pendingApShutdown_ = false;
  lastFailureReason_ = "NONE";
  currentSsid_ = "";
  currentPassword_ = "";

  setupHttpRoutes();

}

void ProvisioningWifiConnector::tick() {
  server_.handleClient();
  finishProvisioningApIfDue();
  pollWifiState();
  if (state_ == State::StaConnecting) {
    logWaitingIfDue();
  }
}

bool ProvisioningWifiConnector::isConnected() const {
  return state_ == State::StaConnected;
}

bool ProvisioningWifiConnector::consumeTcpReady() {
  if (!tcpReady_) {
    return false;
  }
  tcpReady_ = false;
  return true;
}

String ProvisioningWifiConnector::localIp() const {
  return state_ == State::StaConnected ? WiFi.localIP().toString() : String("");
}

void ProvisioningWifiConnector::setupHttpRoutes() {
  server_.on("/api/status", HTTP_GET, [this]() { handleStatusRoute(); });
  server_.on("/api/provision", HTTP_POST, [this]() { handleProvisionRoute(); });
  server_.on("/api/finish", HTTP_POST, [this]() { handleFinishRoute(); });
  server_.on("/api/status", HTTP_OPTIONS, [this]() { handleOptions(); });
  server_.on("/api/provision", HTTP_OPTIONS, [this]() { handleOptions(); });
  server_.on("/api/finish", HTTP_OPTIONS, [this]() { handleOptions(); });
  server_.onNotFound([this]() { handleNotFound(); });
  server_.begin();
}

void ProvisioningWifiConnector::handleStatusRoute() {
  server_.sendHeader("Access-Control-Allow-Origin", "*");
  server_.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server_.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server_.send(200, "application/json", statusJson());
}

void ProvisioningWifiConnector::handleProvisionRoute() {
  const String body = server_.arg("plain");
  const String ssidMarker = "\"ssid\":\"";
  const String passwordMarker = "\"password\":\"";

  auto extractField = [&](const String& marker) {
    const int start = body.indexOf(marker);
    if (start < 0) {
      return String("");
    }
    const int valueStart = start + marker.length();
    int valueEnd = valueStart;
    while (valueEnd < body.length()) {
      if (body[valueEnd] == '"' && body[valueEnd - 1] != '\\') {
        break;
      }
      ++valueEnd;
    }
    if (valueEnd >= body.length()) {
      return String("");
    }
    String value = body.substring(valueStart, valueEnd);
    value.replace("\\\"", "\"");
    value.replace("\\\\", "\\");
    return value;
  };

  currentSsid_ = extractField(ssidMarker);
  currentPassword_ = extractField(passwordMarker);
  if (currentSsid_.length() == 0) {
    server_.sendHeader("Access-Control-Allow-Origin", "*");
    server_.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server_.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    server_.send(400, "application/json", "{\"error\":\"SSID_REQUIRED\"}");
    return;
  }

  startConnection();

  server_.sendHeader("Access-Control-Allow-Origin", "*");
  server_.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server_.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server_.send(200, "application/json", statusJson());
}

void ProvisioningWifiConnector::handleFinishRoute() {
  server_.sendHeader("Access-Control-Allow-Origin", "*");
  server_.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server_.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  if (state_ != State::StaConnected) {
    server_.send(409, "application/json", "{\"error\":\"NOT_CONNECTED\"}");
    return;
  }
  server_.send(200, "application/json", statusJson());
  pendingApShutdown_ = true;
  apShutdownRequestedAtMs_ = millis();
}

void ProvisioningWifiConnector::handleOptions() {
  server_.sendHeader("Access-Control-Allow-Origin", "*");
  server_.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server_.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server_.send(204, "application/json", "");
}

void ProvisioningWifiConnector::handleNotFound() {
  server_.sendHeader("Access-Control-Allow-Origin", "*");
  server_.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server_.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server_.send(404, "application/json", "{\"error\":\"NOT_FOUND\"}");
}

void ProvisioningWifiConnector::announceConnectedIpToSerial() const {
  Serial.println();
  Serial.print("[wifi] connected ssid=");
  Serial.print(currentSsid_);
  Serial.print(" ip=");
  Serial.println(WiFi.localIP().toString());
}

void ProvisioningWifiConnector::startConnection() {
  attemptCount_ += 1;
  waitingLogsThisAttempt_ = 0;
  connectStartedAtMs_ = millis();
  lastWaitingLogAtMs_ = connectStartedAtMs_;
  lastFailureReason_ = "NONE";
  state_ = State::StaConnecting;
  tcpReady_ = false;
  pendingApShutdown_ = false;

  log_.print("[wifi] connect attempt=");
  log_.print(attemptCount_);
  log_.print(" ssid=");
  log_.println(currentSsid_);

  WiFi.disconnect(false, true);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(false);
  WiFi.begin(currentSsid_.c_str(), currentPassword_.c_str());
}

void ProvisioningWifiConnector::failConnection(const char* reason) {
  lastFailureReason_ = reason;
  state_ = State::StaFailed;
  WiFi.disconnect(false, false);
  log_.print("[wifi] connect failed reason=");
  log_.println(lastFailureReason_);
}

void ProvisioningWifiConnector::finishProvisioningApIfDue() {
  if (!pendingApShutdown_) {
    return;
  }
  if (millis() - apShutdownRequestedAtMs_ < kFinishApShutdownDelayMs) {
    return;
  }
  pendingApShutdown_ = false;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  tcpReady_ = true;
}

void ProvisioningWifiConnector::pollWifiState() {
  if (state_ != State::StaConnecting) {
    return;
  }

  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    state_ = State::StaConnected;
    log_.print("[wifi] connected ip=");
    log_.println(WiFi.localIP().toString());
    announceConnectedIpToSerial();
    return;
  }

  const char* immediateFailureReason = immediateWifiFailureReason(status);
  if (immediateFailureReason != nullptr) {
    failConnection(immediateFailureReason);
    return;
  }

  if (millis() - connectStartedAtMs_ > kConnectTimeoutMs) {
    failConnection(wifiFailureReason(status));
  }
}

void ProvisioningWifiConnector::logWaitingIfDue() {
  if (waitingLogsThisAttempt_ >= kMaxWaitingLogsPerAttempt) {
    return;
  }
  const uint32_t now = millis();
  if (now - lastWaitingLogAtMs_ < kWaitingLogIntervalMs) {
    return;
  }
  waitingLogsThisAttempt_ += 1;
  lastWaitingLogAtMs_ = now;
  log_.print("[wifi] waiting for connection attempt=");
  log_.println(attemptCount_);
}

String ProvisioningWifiConnector::statusJson() const {
  String json = "{";
  json += "\"state\":\"";
  switch (state_) {
    case State::ProvisioningAp:
      json += "IDLE";
      break;
    case State::StaConnecting:
      json += "CONNECTING";
      break;
    case State::StaConnected:
      json += "CONNECTED";
      break;
    case State::StaFailed:
      json += "FAILED";
      break;
  }
  json += "\"";
  json += ",\"reason\":\"" + jsonEscape(lastFailureReason_) + "\"";
  json += ",\"targetSsid\":\"" + jsonEscape(currentSsid_) + "\"";
  json += ",\"ap\":{\"ssid\":\"" + String(kProvisioningApSsid) + "\",\"ip\":\"" + WiFi.softAPIP().toString() + "\"}";
  if (state_ == State::StaConnected) {
    json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  }
  json += "}";
  return json;
}

String ProvisioningWifiConnector::jsonEscape(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t index = 0; index < value.length(); ++index) {
    const char ch = value[index];
    if (ch == '\\' || ch == '"') {
      escaped += '\\';
    }
    escaped += ch;
  }
  return escaped;
}

const char* ProvisioningWifiConnector::immediateWifiFailureReason(wl_status_t status) {
  switch (status) {
    case WL_NO_SSID_AVAIL:
      return "NO_SSID";
    case WL_CONNECT_FAILED:
      return "AUTH_FAIL";
    default:
      return nullptr;
  }
}

const char* ProvisioningWifiConnector::wifiFailureReason(wl_status_t status) {
  switch (status) {
    case WL_NO_SSID_AVAIL:
      return "NO_SSID";
    case WL_CONNECT_FAILED:
      return "AUTH_FAIL";
    case WL_CONNECTION_LOST:
      return "CONNECTION_LOST";
    case WL_DISCONNECTED:
      return "DISCONNECTED";
    case WL_IDLE_STATUS:
      return "IDLE";
    default:
      return "TIMEOUT";
  }
}

}  // namespace auto_typer
