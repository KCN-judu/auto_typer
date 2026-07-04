#include "network/StaticWifiConnector.h"

namespace auto_typer {

constexpr char StaticWifiConnector::kProvisioningApSsid[];
constexpr char StaticWifiConnector::kProvisioningApPassword[];

StaticWifiConnector::StaticWifiConnector(Print& log)
    : log_(log),
      server_(80),
      ssid_(""),
      password_(""),
      tcpReady_(false),
      lastWaitingLogAtMs_(0),
      attemptCount_(0),
      waitingLogsThisAttempt_(0),
      connectStartedAtMs_(0),
      state_(State::ProvisioningAp),
      lastFailureReason_("NONE"),
      currentSsid_(),
      currentPassword_() {}

void StaticWifiConnector::begin(const WifiSecrets& secrets) {
  ssid_ = secrets.ssid != nullptr ? secrets.ssid : "";
  password_ = secrets.password != nullptr ? secrets.password : "";

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAP(kProvisioningApSsid, kProvisioningApPassword, kProvisioningApChannel);

  tcpReady_ = false;
  lastWaitingLogAtMs_ = 0;
  attemptCount_ = 0;
  waitingLogsThisAttempt_ = 0;
  connectStartedAtMs_ = 0;
  state_ = State::ProvisioningAp;
  lastFailureReason_ = "NONE";
  currentSsid_ = "";
  currentPassword_ = "";

  setupHttpRoutes();

  if (ssid_[0] != '\0') {
    currentSsid_ = ssid_;
    currentPassword_ = password_;
  }
}

void StaticWifiConnector::tick() {
  server_.handleClient();
  pollWifiState();
  if (state_ == State::StaConnecting) {
    logWaitingIfDue();
  }
}

bool StaticWifiConnector::isConnected() const {
  return state_ == State::StaConnected;
}

bool StaticWifiConnector::consumeTcpReady() {
  if (!tcpReady_) {
    return false;
  }
  tcpReady_ = false;
  return true;
}

String StaticWifiConnector::localIp() const {
  return state_ == State::StaConnected ? WiFi.localIP().toString() : String("");
}

void StaticWifiConnector::setupHttpRoutes() {
  server_.on("/api/status", HTTP_GET, [this]() { handleStatusRoute(); });
  server_.on("/api/provision", HTTP_POST, [this]() { handleProvisionRoute(); });
  server_.on("/api/finish", HTTP_POST, [this]() { handleFinishRoute(); });
  server_.on("/api/status", HTTP_OPTIONS, [this]() { handleOptions(); });
  server_.on("/api/provision", HTTP_OPTIONS, [this]() { handleOptions(); });
  server_.on("/api/finish", HTTP_OPTIONS, [this]() { handleOptions(); });
  server_.onNotFound([this]() { handleNotFound(); });
  server_.begin();
}

void StaticWifiConnector::handleStatusRoute() {
  server_.sendHeader("Access-Control-Allow-Origin", "*");
  server_.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server_.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server_.send(200, "application/json", statusJson());
}

void StaticWifiConnector::handleProvisionRoute() {
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

void StaticWifiConnector::handleFinishRoute() {
  server_.sendHeader("Access-Control-Allow-Origin", "*");
  server_.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server_.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  if (state_ != State::StaConnected) {
    server_.send(409, "application/json", "{\"error\":\"NOT_CONNECTED\"}");
    return;
  }
  WiFi.softAPdisconnect(true);
  tcpReady_ = true;
  server_.send(200, "application/json", statusJson());
}

void StaticWifiConnector::handleOptions() {
  server_.sendHeader("Access-Control-Allow-Origin", "*");
  server_.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server_.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server_.send(204, "application/json", "");
}

void StaticWifiConnector::handleNotFound() {
  server_.sendHeader("Access-Control-Allow-Origin", "*");
  server_.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server_.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server_.send(404, "application/json", "{\"error\":\"NOT_FOUND\"}");
}

void StaticWifiConnector::announceConnectedIpToSerial() const {
  Serial.println();
  Serial.print("[wifi] connected ssid=");
  Serial.print(currentSsid_);
  Serial.print(" ip=");
  Serial.println(WiFi.localIP().toString());
}

void StaticWifiConnector::startConnection() {
  attemptCount_ += 1;
  waitingLogsThisAttempt_ = 0;
  connectStartedAtMs_ = millis();
  lastWaitingLogAtMs_ = connectStartedAtMs_;
  lastFailureReason_ = "NONE";
  state_ = State::StaConnecting;
  tcpReady_ = false;

  log_.print("[wifi] connect attempt=");
  log_.print(attemptCount_);
  log_.print(" ssid=");
  log_.println(currentSsid_);

  WiFi.disconnect(false, true);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(false);
  WiFi.begin(currentSsid_.c_str(), currentPassword_.c_str());
}

void StaticWifiConnector::pollWifiState() {
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

  if (millis() - connectStartedAtMs_ > kConnectTimeoutMs) {
    lastFailureReason_ = wifiFailureReason(status);
    state_ = State::StaFailed;
    WiFi.disconnect(false, false);
    log_.print("[wifi] connect failed reason=");
    log_.println(lastFailureReason_);
  }
}

void StaticWifiConnector::logWaitingIfDue() {
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

String StaticWifiConnector::statusJson() const {
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

String StaticWifiConnector::jsonEscape(const String& value) {
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

const char* StaticWifiConnector::wifiFailureReason(wl_status_t status) {
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
