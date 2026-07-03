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
        diagnostics_{} {
    diagnostics_.lastTxError = "";
    diagnostics_.lastCommandQueueError = "";
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
    const twai_message_t message = toTwaiFrame(frame);
    const esp_err_t result = twai_transmit(&message, timeoutTicks);
    if (result != ESP_OK) {
      ++diagnostics_.txFailedCount;
      diagnostics_.lastTxError = txErrorText(result);
      diagnostics_.lastAlertAtMs = millis();
      return false;
    }
    return true;
  }

  bool receive(CanFrame& frame, TickType_t timeoutTicks = 0) {
    if (!ready_) {
      return false;
    }
    twai_message_t message{};
    if (twai_receive(&message, timeoutTicks) != ESP_OK) {
      return false;
    }
    frame = fromTwaiFrame(message);
    return true;
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
      if (recoverBus()) {
        diagnostics_.fatalFault = false;
        diagnostics_.lastFault = CanBusFault::BusOff;
        diagnostics_.lastFaultAtMs = millis();
        diagnostics_.lastFaultCode = "can_bus_off_recovered";
        diagnostics_.lastFaultMessage = "CAN controller recovered from bus-off";
        diagnostics_.recoverable = true;
        flushRx();
      } else {
        recordTransportFault(CanBusFault::BusOff, "can_bus_off", "CAN bus-off recovery failed");
      }
    }
    if ((alerts & TWAI_ALERT_RX_QUEUE_FULL) != 0) {
      ++diagnostics_.rxQueueFullCount;
    }
    if ((alerts & TWAI_ALERT_ERR_PASS) != 0) {
      ++diagnostics_.errPassiveCount;
    }
    if ((alerts & TWAI_ALERT_TX_FAILED) != 0) {
      ++diagnostics_.txFailedCount;
    }
    if ((alerts & TWAI_ALERT_BUS_ERROR) != 0) {
      ++diagnostics_.busErrorCount;
    }
    return alerts;
  }

  bool ready() const {
    return ready_;
  }

  bool driverReady() const {
    return ready_;
  }

  bool motionReady() const {
    return ready_ && !diagnostics_.fatalFault;
  }

  CanBusDiagnostics diagnostics() const {
    CanBusDiagnostics snapshot = diagnostics_;
    snapshot.driverReady = ready_;
    snapshot.motionReady = motionReady();
    snapshot.recoverable = true;
    return snapshot;
  }

  void recordTxRetry() {
    ++diagnostics_.txRetryCount;
    diagnostics_.lastAlertAtMs = millis();
  }

  void recordCommandQueueFull() {
    ++diagnostics_.commandQueueFullCount;
    diagnostics_.lastCommandQueueError = "command_queue_full";
    diagnostics_.lastAlertAtMs = millis();
  }

  void setPendingFrameValid(bool valid) {
    diagnostics_.pendingFrameValid = valid;
  }

  bool recoverOrClearFault() {
    if (!ready_) {
      return begin();
    }
    if (diagnostics_.busOffCount > 0) {
      if (!recoverBus()) {
        recordTransportFault(CanBusFault::BusOff, "can_bus_off", "CAN bus-off recovery failed");
        return false;
      }
    }
    flushRx();
    clearDiagnostics();
    return true;
  }

 private:
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

  static twai_message_t toTwaiFrame(const CanFrame& frame) {
    twai_message_t message{};
    message.identifier = frame.identifier;
    message.data_length_code = frame.data_length_code > 8 ? 8 : frame.data_length_code;
    message.extd = frame.extd ? 1 : 0;
    message.rtr = frame.rtr ? 1 : 0;
    for (uint8_t i = 0; i < message.data_length_code; ++i) {
      message.data[i] = frame.data[i];
    }
    return message;
  }

  static CanFrame fromTwaiFrame(const twai_message_t& message) {
    CanFrame frame{};
    frame.identifier = message.identifier;
    frame.data_length_code = message.data_length_code > 8 ? 8 : message.data_length_code;
    frame.extd = message.extd != 0;
    frame.rtr = message.rtr != 0;
    for (uint8_t i = 0; i < frame.data_length_code; ++i) {
      frame.data[i] = message.data[i];
    }
    return frame;
  }

  void recordTransportFault(CanBusFault fault, const char* code, const char* message) {
    diagnostics_.fatalFault = true;
    diagnostics_.lastFault = fault;
    diagnostics_.lastFaultAtMs = millis();
    diagnostics_.lastFaultCode = code;
    diagnostics_.lastFaultMessage = message;
    diagnostics_.recoverable = true;
  }

  void clearDiagnostics() {
    diagnostics_.lastAlerts = 0;
    diagnostics_.driverReady = ready_;
    diagnostics_.motionReady = ready_;
    diagnostics_.txFailedCount = 0;
    diagnostics_.txRetryCount = 0;
    diagnostics_.commandQueueFullCount = 0;
    diagnostics_.busErrorCount = 0;
    diagnostics_.rxQueueFullCount = 0;
    diagnostics_.errPassiveCount = 0;
    diagnostics_.busOffCount = 0;
    diagnostics_.pendingFrameValid = false;
    diagnostics_.lastFaultAtMs = 0;
    diagnostics_.lastAlertAtMs = 0;
    diagnostics_.lastTxError = "";
    diagnostics_.lastCommandQueueError = "";
    diagnostics_.recoverable = true;
    diagnostics_.fatalFault = false;
    diagnostics_.lastFault = CanBusFault::None;
    diagnostics_.lastFaultCode = "";
    diagnostics_.lastFaultMessage = "";
  }

  void flushRx() {
    twai_message_t message{};
    for (uint8_t i = 0; i < 32 && twai_receive(&message, 0) == ESP_OK; ++i) {
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
          const esp_err_t startResult = twai_start();
          return startResult == ESP_OK || startResult == ESP_ERR_INVALID_STATE;
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
  CanBusDiagnostics diagnostics_;
};

}  // namespace auto_typer
