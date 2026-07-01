#pragma once

#include <Arduino.h>
#include "driver/twai.h"

#include "../auto_typer_types.h"
#include "CanFrame.h"

namespace auto_typer {

class CanBus {
 public:
  explicit CanBus(const CanBusConfig& config) : config_(config), ready_(false), fault_(CanBusFault::None) {}

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
        TWAI_ALERT_ERR_PASS | TWAI_ALERT_BUS_OFF;
    if (twai_reconfigure_alerts(alertsToEnable, nullptr) != ESP_OK) {
      twai_stop();
      twai_driver_uninstall();
      ready_ = false;
      return false;
    }

    ready_ = true;
    fault_ = CanBusFault::None;
    return true;
  }

  bool transmit(const CanFrame& frame, TickType_t timeoutTicks = pdMS_TO_TICKS(100)) {
    if (!ready_) {
      return false;
    }
    if (twai_transmit(&frame, timeoutTicks) != ESP_OK) {
      fault_ = CanBusFault::TxFailed;
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
    if ((alerts & TWAI_ALERT_BUS_OFF) != 0) {
      fault_ = CanBusFault::BusOff;
    } else if ((alerts & TWAI_ALERT_RX_QUEUE_FULL) != 0) {
      fault_ = CanBusFault::RxQueueFull;
    } else if ((alerts & TWAI_ALERT_TX_FAILED) != 0) {
      fault_ = CanBusFault::TxFailed;
    } else if ((alerts & TWAI_ALERT_BUS_ERROR) != 0) {
      fault_ = CanBusFault::BusError;
    }
    return alerts;
  }

  bool ready() const {
    return ready_ && fault_ == CanBusFault::None;
  }

  CanBusFault fault() const {
    return fault_;
  }

  void clearFaultForRecovery() {
    fault_ = CanBusFault::None;
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

  CanBusConfig config_;
  bool ready_;
  CanBusFault fault_;
};

}  // namespace auto_typer
