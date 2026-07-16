#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

namespace auto_typer {

class ProvisioningWifiConnector {
 public:
  explicit ProvisioningWifiConnector(Print& log);

  void begin();
  void tick();

  bool isConnected() const;
  bool consumeTcpReady();
  String localIp() const;

 private:
  enum class State {
    ProvisioningAp,
    StaConnecting,
    StaConnected,
    StaFailed,
  };

  void setupHttpRoutes();
  void handleStatusRoute();
  void handleProvisionRoute();
  void handleFinishRoute();
  void handleOptions();
  void handleNotFound();
  void announceConnectedIpToSerial() const;
  void startConnection();
  void failConnection(const char* reason);
  void finishProvisioningApIfDue();
  void pollWifiState();
  void logWaitingIfDue();
  String statusJson() const;
  static String jsonEscape(const String& value);
  static const char* immediateWifiFailureReason(wl_status_t status);
  static const char* wifiFailureReason(wl_status_t status);

  static constexpr uint32_t kConnectTimeoutMs = 20000;
  static constexpr uint32_t kFinishApShutdownDelayMs = 500;
  static constexpr uint32_t kWaitingLogIntervalMs = 5000;
  static constexpr uint8_t kMaxWaitingLogsPerAttempt = 3;
  static constexpr char kProvisioningApSsid[] = "wifi-setup";
  static constexpr char kProvisioningApPassword[] = "admin123";
  static constexpr uint8_t kProvisioningApChannel = 6;

  Print& log_;
  WebServer server_;
  bool tcpReady_;
  uint32_t lastWaitingLogAtMs_;
  uint32_t attemptCount_;
  uint8_t waitingLogsThisAttempt_;
  uint32_t connectStartedAtMs_;
  uint32_t apShutdownRequestedAtMs_;
  State state_;
  bool pendingApShutdown_;
  String lastFailureReason_;
  String currentSsid_;
  String currentPassword_;
};

}  // namespace auto_typer
