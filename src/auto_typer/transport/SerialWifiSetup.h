#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>

#include "../auto_typer_runtime.h"

namespace auto_typer {

class SerialWifiSetup {
 public:
  SerialWifiSetup(const TypingConfig& config, AutoTyperApplication& app, Stream& serial, Print& log)
      : config_(config),
        app_(app),
        serial_(serial),
        log_(log),
        staConnecting_(false),
        savedCredentials_(false),
        lastWifiAttemptMs_(0),
        lastWifiError_("") {}

  void begin() {
    String ssid;
    String password;
    savedCredentials_ = loadWifiCredentials(ssid, password);
    if (!savedCredentials_ && strlen(config_.wifiSsid) > 0) {
      ssid = config_.wifiSsid;
      password = config_.wifiPassword;
      savedCredentials_ = true;
    }
    WiFi.mode(WIFI_STA);
    if (savedCredentials_) {
      connectStation(ssid.c_str(), password.c_str());
    } else {
      lastWifiError_ = "no_credentials";
    }
    log_.print("[serial-wifi] WiFi IP: ");
    log_.println(currentIp());
  }

  void tick() {
    updateWifiConnectionState();
    while (serial_.available() > 0) {
      const int value = serial_.read();
      if (value < 0) {
        break;
      }
      if (value == '\r') {
        continue;
      }
      if (value == '\n') {
        handleLine(lineBuffer_);
        lineBuffer_ = "";
        continue;
      }
      if (lineBuffer_.length() >= kMaxSerialLineBytes) {
        lineBuffer_ = "";
        sendProtocolError("", "line_too_large", "Serial WiFi line is too large");
        continue;
      }
      lineBuffer_ += static_cast<char>(value);
    }
  }

 private:
  void handleLine(const String& line) {
    if (!line.startsWith(kRxPrefix)) {
      return;
    }
    const String payload = line.substring(strlen(kRxPrefix));
    DynamicJsonDocument request(payload.length() + 512);
    const DeserializationError error = deserializeJson(request, payload);
    if (error) {
      sendProtocolError("", "invalid_json", "Invalid serial WiFi JSON");
      return;
    }
    const char* type = request["type"] | "";
    const char* requestId = request["requestId"] | "";
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
    sendProtocolError(requestId, "unknown_type", "Unsupported serial WiFi message type");
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
    sendWifiConfigResult(requestId,
                         true,
                         "",
                         "WiFi credentials saved; station connection started");
  }

  void sendWifiStatus(const char* requestId) {
    StaticJsonDocument<512> doc;
    doc["v"] = 1;
    doc["type"] = "wifi_status";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    writeWifiStatus(doc.createNestedObject("wifi"));
    sendJson(doc);
  }

  void sendWifiNetworks(const char* requestId) {
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
      sendWifiNetworksComplete(requestId, false, 0, "wifi_scan_failed", "WiFi scan failed");
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
      sendWifiNetwork(requestId, entries[i]);
    }
    sendWifiNetworksComplete(requestId, true, count, "", "");
  }

  template <typename TEntry>
  void sendWifiNetwork(const char* requestId, const TEntry& entry) {
    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["type"] = "wifi_network";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    JsonObject network = doc.createNestedObject("network");
    network["ssid"] = entry.ssid;
    network["rssi"] = entry.rssi;
    network["channel"] = entry.channel;
    network["encryption"] = encryptionText(entry.encryption);
    sendJson(doc);
  }

  void sendWifiNetworksComplete(const char* requestId, bool ok, size_t count, const char* code, const char* message) {
    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["type"] = "wifi_networks";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    doc["ok"] = ok;
    doc["count"] = count;
    doc.createNestedArray("networks");
    if (!ok) {
      doc["code"] = code != nullptr ? code : "wifi_scan_failed";
      doc["message"] = message != nullptr ? message : "WiFi scan failed";
    }
    sendJson(doc);
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
    sendJson(doc);
  }

  void sendProtocolError(const char* requestId, const char* code, const char* message) {
    StaticJsonDocument<256> doc;
    doc["v"] = 1;
    doc["type"] = "protocol_error";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    doc["code"] = code;
    doc["message"] = message;
    sendJson(doc);
  }

  void writeWifiStatus(JsonObject json) {
    updateWifiConnectionState();
    json["setupApActive"] = false;
    json["setupSsid"] = "";
    json["setupPassword"] = "";
    json["setupIpAddress"] = "";
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

  void connectStation(const char* ssid, const char* password) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    staConnecting_ = true;
    lastWifiAttemptMs_ = millis();
    lastWifiError_ = "";
    log_.print("[serial-wifi] Connecting WiFi SSID: ");
    log_.println(ssid);
  }

  void updateWifiConnectionState() {
    if (WiFi.status() == WL_CONNECTED) {
      if (staConnecting_) {
        log_.print("[serial-wifi] WiFi connected, IP: ");
        log_.println(WiFi.localIP().toString());
      }
      staConnecting_ = false;
      lastWifiError_ = "";
      return;
    }
    if (staConnecting_ && millis() - lastWifiAttemptMs_ > kWifiConnectTimeoutMs) {
      staConnecting_ = false;
      lastWifiError_ = "station_connect_failed";
      log_.println("[serial-wifi] WiFi station connection timed out");
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
    return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
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

  template <typename TDoc>
  void sendJson(TDoc& doc) {
    String payload;
    serializeJson(doc, payload);
    String line;
    line.reserve(strlen(kTxPrefix) + payload.length() + 1);
    line += kTxPrefix;
    line += payload;
    line += '\n';
    const char* data = line.c_str();
    size_t remaining = line.length();
    while (remaining > 0) {
      const size_t chunk = remaining > kSerialTxChunkBytes ? kSerialTxChunkBytes : remaining;
      serial_.write(reinterpret_cast<const uint8_t*>(data), chunk);
      data += chunk;
      remaining -= chunk;
      delay(1);
    }
    serial_.flush();
  }

  const TypingConfig& config_;
  AutoTyperApplication& app_;
  Stream& serial_;
  Print& log_;
  String lineBuffer_;
  bool staConnecting_;
  bool savedCredentials_;
  uint32_t lastWifiAttemptMs_;
  String lastWifiError_;

  static constexpr const char* kRxPrefix = "ATWIFI>";
  static constexpr const char* kTxPrefix = "ATWIFI<";
  static constexpr size_t kMaxSerialLineBytes = 512;
  static constexpr size_t kSerialTxChunkBytes = 96;
  static constexpr size_t kMaxWifiNetworks = 24;
  static constexpr size_t kMaxWifiSsidLength = 32;
  static constexpr size_t kMaxWifiPasswordLength = 63;
  static constexpr uint32_t kWifiConnectTimeoutMs = 20000;
};

}  // namespace auto_typer
