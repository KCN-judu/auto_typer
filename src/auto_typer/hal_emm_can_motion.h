#pragma once

#include <Arduino.h>
#include "driver/twai.h"

#include "auto_typer_types.h"

namespace auto_typer {

class EmmCanMotionHal {
 public:
  enum class WaitStatus : uint8_t {
    Completed,
    Timeout,
    TransportError,
  };

  struct MotorFeedback {
    bool hasVelocity;
    bool hasPosition;
    bool hasStatusFlags;
    bool sawAnyFrame;
    bool transportError;
    float velocityRpm;
    float positionDegrees;
    uint32_t statusFlags;
  };

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

  bool moveAbsolute(uint8_t motorId,
                    MotorDirection direction,
                    uint16_t rpm,
                    uint8_t acceleration,
                    uint32_t targetSteps,
                    bool sync) const {
    const uint8_t command[] = {
      motorId,
      0xFD,
      static_cast<uint8_t>(direction),
      static_cast<uint8_t>((rpm >> 8) & 0xFF),
      static_cast<uint8_t>(rpm & 0xFF),
      acceleration,
      static_cast<uint8_t>((targetSteps >> 24) & 0xFF),
      static_cast<uint8_t>((targetSteps >> 16) & 0xFF),
      static_cast<uint8_t>((targetSteps >> 8) & 0xFF),
      static_cast<uint8_t>(targetSteps & 0xFF),
      0x01,
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

  void drainRx() const {
    if (!ready_) {
      return;
    }
    twai_message_t message{};
    while (twai_receive(&message, 0) == ESP_OK) {
    }
  }

  bool requestVelocity(uint8_t motorId) const {
    const uint8_t command[] = {motorId, 0x35, 0x6B};
    return sendCommand(command, sizeof(command));
  }

  bool requestRealtimeAngle(uint8_t motorId) const {
    const uint8_t command[] = {motorId, 0x36, 0x6B};
    return sendCommand(command, sizeof(command));
  }

  bool requestStatusFlags(uint8_t motorId) const {
    const uint8_t command[] = {motorId, 0x3A, 0x6B};
    return sendCommand(command, sizeof(command));
  }

  MotorFeedback pollFeedback(uint8_t motorId) const {
    MotorFeedback feedback = {};
    if (!ready_) {
      feedback.transportError = true;
      return feedback;
    }

    uint32_t alerts = 0;
    if (twai_read_alerts(&alerts, 0) != ESP_OK) {
      feedback.transportError = true;
      return feedback;
    }
    feedback.transportError = (alerts & TWAI_ALERT_BUS_OFF) != 0;

    if ((alerts & TWAI_ALERT_RX_DATA) == 0) {
      return feedback;
    }

    twai_message_t message{};
    while (twai_receive(&message, 0) == ESP_OK) {
      if (message.data_length_code == 0) {
        continue;
      }
      if (!isFrameForMotor(message, motorId)) {
        continue;
      }
      feedback.sawAnyFrame = true;
      applyFeedbackFrame(message, feedback);
    }
    return feedback;
  }

  bool ready() const {
    return ready_;
  }

 private:
  static bool isFrameForMotor(const twai_message_t& message, uint8_t motorId) {
    if (!message.extd) {
      return true;
    }
    return static_cast<uint8_t>((message.identifier >> 8) & 0xFF) == motorId;
  }

  static void applyFeedbackFrame(const twai_message_t& message, MotorFeedback& feedback) {
    const uint8_t command = message.data[0];
    switch (command) {
      case 0x35:
        applyVelocityFrame(message, feedback);
        break;
      case 0x36:
        applyPositionFrame(message, feedback);
        break;
      case 0x3A:
      case 0x3B:
        applyStatusFrame(message, feedback);
        break;
      default:
        break;
    }
  }

  static void applyVelocityFrame(const twai_message_t& message, MotorFeedback& feedback) {
    if (message.data_length_code < 4) {
      return;
    }
    const uint16_t rawRpm = (static_cast<uint16_t>(message.data[2]) << 8) | message.data[3];
    feedback.velocityRpm = static_cast<float>(rawRpm);
    if (message.data[1] != 0) {
      feedback.velocityRpm = -feedback.velocityRpm;
    }
    feedback.hasVelocity = true;
  }

  static void applyPositionFrame(const twai_message_t& message, MotorFeedback& feedback) {
    if (message.data_length_code < 6) {
      return;
    }
    const uint32_t rawPosition = (static_cast<uint32_t>(message.data[2]) << 24) |
                                 (static_cast<uint32_t>(message.data[3]) << 16) |
                                 (static_cast<uint32_t>(message.data[4]) << 8) |
                                 static_cast<uint32_t>(message.data[5]);
    feedback.positionDegrees = static_cast<float>(rawPosition) * 360.0f / 65536.0f;
    if (message.data[1] != 0) {
      feedback.positionDegrees = -feedback.positionDegrees;
    }
    feedback.hasPosition = true;
  }

  static void applyStatusFrame(const twai_message_t& message, MotorFeedback& feedback) {
    uint32_t value = 0;
    for (uint8_t i = 1; i < message.data_length_code && i < 5; ++i) {
      value = (value << 8) | message.data[i];
    }
    feedback.statusFlags = value;
    feedback.hasStatusFlags = true;
  }

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
