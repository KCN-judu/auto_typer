#pragma once

#include <Arduino.h>
#include "driver/twai.h"

#include "../auto_typer_types.h"
#include "CanFrame.h"

namespace auto_typer {

class CanBus {
 public:
  explicit CanBus(const CanBusConfig& config)
      : config_(config),
        ready_(false),
        fatalFault_(false),
        fault_(CanBusFault::None),
        diagnostics_{},
        firstSoftFaultAtMs_(0),
        softFaultWindowCount_(0),
        softFaultEscalationEnabled_(true) {
    diagnostics_.lastTxError = "";
    diagnostics_.lastFaultCode = "";
    diagnostics_.lastFaultMessage = "";
    diagnostics_.recoverable = true;
  }

  bool begin() {
    if (ready_) {
      return true;
    }

    twai_general_config_t generalConfig =
        TWAI_GENERAL_CONFIG_DEFAULT(config_.txPin, config_.rxPin, TWAI_MODE_NORMAL);
    generalConfig.tx_queue_len = 16;
    generalConfig.rx_queue_len = 32;
    generalConfig.clkout_io = TWAI_IO_UNUSED;
    generalConfig.bus_off_io = TWAI_IO_UNUSED;

    twai_timing_config_t timingConfig = timingForBitrate(config_.bitrate);
    twai_filter_config_t filterConfig = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&generalConfig, &timingConfig, &filterConfig) != ESP_OK) {
      ready_ = false;
      return false;
    }
    if (twai_start() != ESP_OK) {
      twai_driver_uninstall();
      ready_ = false;
      return false;
    }

    const uint32_t alertsToEnable =
        TWAI_ALERT_RX_DATA | TWAI_ALERT_RX_QUEUE_FULL | TWAI_ALERT_TX_FAILED | TWAI_ALERT_BUS_ERROR |
        TWAI_ALERT_ERR_PASS | TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED;
    if (twai_reconfigure_alerts(alertsToEnable, nullptr) != ESP_OK) {
      twai_stop();
      twai_driver_uninstall();
      ready_ = false;
      return false;
    }

    ready_ = true;
    clearDiagnostics();
    return true;
  }

  bool transmit(const CanFrame& frame, TickType_t timeoutTicks = pdMS_TO_TICKS(100)) {
    if (!ready_) {
      diagnostics_.lastTxError = "driver_not_ready";
      return false;
    }
    const esp_err_t result = twai_transmit(&frame, timeoutTicks);
    if (result != ESP_OK) {
      ++diagnostics_.txFailedCount;
      diagnostics_.lastTxError = txErrorText(result);
      diagnostics_.lastAlertAtMs = millis();
      registerSoftFault(CanBusFault::TxFailed, "can_tx_failed", "CAN transmit failed");
      return false;
    }
    return true;
  }

  bool receive(CanFrame& frame, TickType_t timeoutTicks = 0) {
    if (!ready_) {
      return false;
    }
    return twai_receive(&frame, timeoutTicks) == ESP_OK;
  }

  uint32_t readAlerts(TickType_t timeoutTicks = 0) {
    uint32_t alerts = 0;
    if (!ready_ || twai_read_alerts(&alerts, timeoutTicks) != ESP_OK) {
      return 0;
    }
    if (alerts == 0) {
      return 0;
    }
    diagnostics_.lastAlerts = alerts;
    diagnostics_.lastAlertAtMs = millis();
    if ((alerts & TWAI_ALERT_BUS_OFF) != 0) {
      ++diagnostics_.busOffCount;
      setFatalFault(CanBusFault::BusOff, "can_bus_off", "CAN controller entered bus-off");
    }
    if ((alerts & TWAI_ALERT_RX_QUEUE_FULL) != 0) {
      ++diagnostics_.rxQueueFullCount;
    }
    if ((alerts & TWAI_ALERT_ERR_PASS) != 0) {
      ++diagnostics_.errPassiveCount;
    }
    if ((alerts & TWAI_ALERT_TX_FAILED) != 0) {
      ++diagnostics_.txFailedCount;
      registerSoftFault(CanBusFault::TxFailed, "can_tx_failed", "CAN transmit failed");
    }
    if ((alerts & TWAI_ALERT_BUS_ERROR) != 0) {
      ++diagnostics_.busErrorCount;
      registerSoftFault(CanBusFault::BusError, "can_bus_error", "CAN bus error threshold exceeded");
    }
    return alerts;
  }

  bool ready() const {
    return ready_;
  }

  bool driverReady() const {
    return ready_;
  }

  bool hasFatalFault() const {
    return fatalFault_;
  }

  bool motionReady() const {
    return ready_ && !fatalFault_ && diagnostics_.txFailedCount == 0 && diagnostics_.busErrorCount == 0 &&
           diagnostics_.rxQueueFullCount == 0;
  }

  CanBusFault fault() const {
    return fault_;
  }

  CanBusDiagnostics diagnostics() const {
    CanBusDiagnostics snapshot = diagnostics_;
    snapshot.driverReady = ready_;
    snapshot.motionReady = motionReady();
    snapshot.fatalFault = fatalFault_;
    snapshot.lastFault = fault_;
    snapshot.recoverable = fault_ != CanBusFault::BusOff;
    return snapshot;
  }

  void setSoftFaultEscalationEnabled(bool enabled) {
    softFaultEscalationEnabled_ = enabled;
    if (!enabled) {
      firstSoftFaultAtMs_ = 0;
      softFaultWindowCount_ = 0;
    }
  }

  bool recoverOrClearFault() {
    if (!ready_) {
      return begin();
    }
    if (fault_ == CanBusFault::BusOff || diagnostics_.busOffCount > 0) {
      if (!recoverBus()) {
        setFatalFault(CanBusFault::BusOff, "can_bus_off", "CAN bus-off recovery failed");
        return false;
      }
    }
    flushRx();
    clearDiagnostics();
    return true;
  }

 private:
  static constexpr uint32_t kSoftFaultWindowMs = 1000;
  static constexpr uint8_t kSoftFaultThreshold = 10;

  static twai_timing_config_t timingForBitrate(uint32_t bitrate) {
    switch (bitrate) {
      case 250000:
        return TWAI_TIMING_CONFIG_250KBITS();
      case 1000000:
        return TWAI_TIMING_CONFIG_1MBITS();
      case 500000:
      default:
        return TWAI_TIMING_CONFIG_500KBITS();
    }
  }

  static const char* txErrorText(esp_err_t error) {
    switch (error) {
      case ESP_ERR_TIMEOUT:
        return "timeout_or_queue_full";
      case ESP_ERR_INVALID_STATE:
        return "invalid_state";
      case ESP_ERR_INVALID_ARG:
        return "invalid_arg";
      case ESP_FAIL:
        return "driver_failed";
      default:
        return "unknown";
    }
  }

  void registerSoftFault(CanBusFault fault, const char* code, const char* message) {
    const uint32_t nowMs = millis();
    if (firstSoftFaultAtMs_ == 0 || nowMs - firstSoftFaultAtMs_ > kSoftFaultWindowMs) {
      firstSoftFaultAtMs_ = nowMs;
      softFaultWindowCount_ = 1;
    } else {
      ++softFaultWindowCount_;
    }
    if (!softFaultEscalationEnabled_) {
      return;
    }
    if (softFaultWindowCount_ >= kSoftFaultThreshold) {
      setFatalFault(fault, code, message);
    }
  }

  void setFatalFault(CanBusFault fault, const char* code, const char* message) {
    fatalFault_ = true;
    fault_ = fault;
    diagnostics_.fatalFault = true;
    diagnostics_.lastFault = fault;
    diagnostics_.lastFaultAtMs = millis();
    diagnostics_.lastFaultCode = code;
    diagnostics_.lastFaultMessage = message;
    diagnostics_.recoverable = fault != CanBusFault::BusOff;
  }

  void clearDiagnostics() {
    fatalFault_ = false;
    fault_ = CanBusFault::None;
    diagnostics_.lastAlerts = 0;
    diagnostics_.driverReady = ready_;
    diagnostics_.motionReady = ready_;
    diagnostics_.txFailedCount = 0;
    diagnostics_.busErrorCount = 0;
    diagnostics_.rxQueueFullCount = 0;
    diagnostics_.errPassiveCount = 0;
    diagnostics_.busOffCount = 0;
    diagnostics_.lastFaultAtMs = 0;
    diagnostics_.lastAlertAtMs = 0;
    diagnostics_.lastTxError = "";
    diagnostics_.recoverable = true;
    diagnostics_.fatalFault = false;
    diagnostics_.lastFault = CanBusFault::None;
    diagnostics_.lastFaultCode = "";
    diagnostics_.lastFaultMessage = "";
    firstSoftFaultAtMs_ = 0;
    softFaultWindowCount_ = 0;
  }

  void flushRx() {
    CanFrame frame{};
    for (uint8_t i = 0; i < 32 && twai_receive(&frame, 0) == ESP_OK; ++i) {
    }
    uint32_t ignoredAlerts = 0;
    twai_read_alerts(&ignoredAlerts, 0);
  }

  bool recoverBus() {
    uint32_t ignoredAlerts = 0;
    twai_read_alerts(&ignoredAlerts, 0);
    if (twai_initiate_recovery() == ESP_OK) {
      const uint32_t startedAt = millis();
      while (millis() - startedAt < 800) {
        uint32_t alerts = 0;
        if (twai_read_alerts(&alerts, pdMS_TO_TICKS(20)) == ESP_OK &&
            (alerts & TWAI_ALERT_BUS_RECOVERED) != 0) {
          return twai_start() == ESP_OK || twai_start() == ESP_ERR_INVALID_STATE;
        }
      }
    }

    if (twai_stop() != ESP_OK && ready_) {
      // Continue with restart attempts below.
    }
    if (twai_start() == ESP_OK) {
      return true;
    }

    twai_driver_uninstall();
    ready_ = false;
    return begin();
  }

  CanBusConfig config_;
  bool ready_;
  bool fatalFault_;
  CanBusFault fault_;
  CanBusDiagnostics diagnostics_;
  uint32_t firstSoftFaultAtMs_;
  uint8_t softFaultWindowCount_;
  bool softFaultEscalationEnabled_;
};

}  // namespace auto_typer
