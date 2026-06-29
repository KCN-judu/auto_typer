#pragma once

#include <Arduino.h>
#include "driver/twai.h"

#include "auto_typer_types.h"

namespace auto_typer {

class EmmCanMotionHal {
 public:
  explicit EmmCanMotionHal(const CanBusConfig& config) : config_(config), ready_(false) {}

  bool begin() {
    if (ready_) {
      return true;
    }

    twai_general_config_t generalConfig =
        TWAI_GENERAL_CONFIG_DEFAULT(config_.txPin, config_.rxPin, TWAI_MODE_NORMAL);
    generalConfig.tx_queue_len = 8;
    generalConfig.rx_queue_len = 8;
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

    ready_ = true;
    return true;
  }

  bool setClosedLoopControlMode(uint8_t motorId) const {
    const uint8_t command[] = {motorId, 0x46, 0x69, 0x00, 0x02, 0x6B};
    return sendCommand(command, sizeof(command));
  }

  bool enableMotor(uint8_t motorId, bool sync = false) const {
    const uint8_t command[] = {motorId, 0xF3, 0xAB, 0x01, static_cast<uint8_t>(sync), 0x6B};
    return sendCommand(command, sizeof(command));
  }

  bool disableMotor(uint8_t motorId, bool sync = false) const {
    const uint8_t command[] = {motorId, 0xF3, 0xAB, 0x00, static_cast<uint8_t>(sync), 0x6B};
    return sendCommand(command, sizeof(command));
  }

  bool moveRelative(uint8_t motorId,
                    MotorDirection direction,
                    uint16_t rpm,
                    uint8_t acceleration,
                    uint32_t steps,
                    bool sync) const {
    const uint8_t command[] = {
      motorId,
      0xFD,
      static_cast<uint8_t>(direction),
      static_cast<uint8_t>((rpm >> 8) & 0xFF),
      static_cast<uint8_t>(rpm & 0xFF),
      acceleration,
      static_cast<uint8_t>((steps >> 24) & 0xFF),
      static_cast<uint8_t>((steps >> 16) & 0xFF),
      static_cast<uint8_t>((steps >> 8) & 0xFF),
      static_cast<uint8_t>(steps & 0xFF),
      0x00,
      static_cast<uint8_t>(sync),
      0x6B,
    };
    return sendCommand(command, sizeof(command));
  }

  bool triggerSynchronousMotion(uint8_t motorId) const {
    const uint8_t command[] = {motorId, 0xFF, 0x66, 0x6B};
    return sendCommand(command, sizeof(command));
  }

  bool stopNow(uint8_t motorId, bool sync = false) const {
    const uint8_t command[] = {motorId, 0xFE, 0x98, static_cast<uint8_t>(sync), 0x6B};
    return sendCommand(command, sizeof(command));
  }

  bool ready() const {
    return ready_;
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

  bool sendCommand(const uint8_t* command, size_t len) const {
    if (!ready_ || len < 2) {
      return false;
    }

    const size_t payloadLen = len - 2;
    size_t offset = 0;
    uint8_t packetNumber = 0;

    while (offset < payloadLen) {
      twai_message_t message{};
      const size_t remaining = payloadLen - offset;
      const size_t chunkLen = remaining < 7 ? remaining : 7;

      message.extd = 1;
      message.identifier = (static_cast<uint32_t>(command[0]) << 8) | packetNumber;
      message.data_length_code = static_cast<uint8_t>(chunkLen + 1);
      message.data[0] = command[1];

      for (size_t i = 0; i < chunkLen; ++i) {
        message.data[i + 1] = command[offset + 2];
        ++offset;
      }

      if (twai_transmit(&message, pdMS_TO_TICKS(100)) != ESP_OK) {
        return false;
      }
      ++packetNumber;
    }

    return true;
  }

  CanBusConfig config_;
  bool ready_;
};

}  // namespace auto_typer
