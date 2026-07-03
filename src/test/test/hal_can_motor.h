#pragma once

#include <Arduino.h>
#include "driver/twai.h"

#include "app_config.h"
#include "app_logic.h"
#include "app_types.h"

namespace test_app {

class CanMotorHal {
 public:
  using ResponseHook = CanRxState (*)(const CanRxState&, const CanRxFrame&);

  explicit CanMotorHal(const CanConfig& config)
      : config_(config), ready_(false), normalResponseHook_(nullptr), dataResponseHook_(nullptr) {}

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

  void setNormalResponseHook(ResponseHook hook) {
    normalResponseHook_ = hook;
  }

  void setDataResponseHook(ResponseHook hook) {
    dataResponseHook_ = hook;
  }

  void drainRx() const {
    twai_message_t message{};
    while (twai_receive(&message, 0) == ESP_OK) {
    }
  }

  bool enableMotor() const {
    const uint8_t command[] = {
      config_.motorAddress, 0xF3, 0xAB, 0x01, 0x00, 0x6B,
    };
    return sendCommand(command, sizeof(command));
  }

  bool setClosedLoopControlMode() const {
    const uint8_t command[] = {
      config_.motorAddress, 0x46, 0x69, 0x01, 0x02, 0x6B,
    };
    return sendCommand(command, sizeof(command));
  }

  bool runVelocityPlan(const CanCommandPlan& plan) const {
    const uint8_t command[] = {
      config_.motorAddress,
      0xF6,
      static_cast<uint8_t>(plan.direction),
      static_cast<uint8_t>((plan.rpm >> 8) & 0xFF),
      static_cast<uint8_t>(plan.rpm & 0xFF),
      plan.acceleration,
      0x00,
      0x6B,
    };
    return sendCommand(command, sizeof(command));
  }

  bool requestVelocity() const {
    const uint8_t command[] = {config_.motorAddress, 0x35, 0x6B};
    return sendCommand(command, sizeof(command));
  }

  bool stopMotor() const {
    const uint8_t command[] = {config_.motorAddress, 0xFE, 0x98, 0x00, 0x6B};
    return sendCommand(command, sizeof(command));
  }

  CanTestResult runConnectivityCheck(const CanCommandPlan& plan, uint32_t timeoutMs = 1500) {
    const bool controllerReady = begin();
    CanRxState feedback = defaultCanRxState();
    if (!controllerReady) {
      return summarizeCanTest(false, false, false, false, false, false, feedback);
    }

    drainRx();
    setNormalResponseHook(handleNormalResponse);
    setDataResponseHook(handleDataResponse);

    const bool modeCommandSent = setClosedLoopControlMode();
    delay(150);
    const bool enableCommandSent = enableMotor();
    delay(150);
    const bool velocityCommandSent = runVelocityPlan(plan);
    delay(1500);
    const bool requestSent = requestVelocity();
    feedback = collectRxEvents(feedback, timeoutMs);
    const bool stopSent = stopMotor();

    return summarizeCanTest(controllerReady,
                            modeCommandSent,
                            enableCommandSent,
                            velocityCommandSent,
                            requestSent,
                            stopSent,
                            feedback);
  }

  CanRxState pollRxEvents(const CanRxState& currentState) const {
    CanRxState nextState = currentState;
    if (!ready_) {
      return nextState;
    }

    uint32_t alerts = 0;
    if (twai_read_alerts(&alerts, 0) != ESP_OK) {
      return nextState;
    }

    if (alerts & TWAI_ALERT_BUS_ERROR) {
      nextState.busError = true;
    }
    if (alerts & TWAI_ALERT_ERR_PASS) {
      nextState.errorPassive = true;
    }
    if (alerts & TWAI_ALERT_BUS_OFF) {
      nextState.busOff = true;
    }
    if (alerts & TWAI_ALERT_RX_QUEUE_FULL) {
      nextState.rxQueueFull = true;
    }
    if (alerts & TWAI_ALERT_TX_FAILED) {
      nextState.txFailed = true;
    }

    if (!(alerts & TWAI_ALERT_RX_DATA)) {
      return nextState;
    }

    twai_message_t message{};
    while (twai_receive(&message, 0) == ESP_OK) {
      const CanRxFrame frame = toRxFrame(message);
      nextState = dispatchFrame(nextState, frame);
    }

    return nextState;
  }

  CanRxState collectRxEvents(const CanRxState& currentState, uint32_t timeoutMs) const {
    CanRxState nextState = currentState;
    const uint32_t startedAt = millis();
    while (millis() - startedAt < timeoutMs) {
      nextState = pollRxEvents(nextState);
      if (nextState.hasVelocity || nextState.hasPosition || nextState.hasStatusFlags) {
        break;
      }
      delay(5);
    }
    return nextState;
  }

  bool ready() const {
    return ready_;
  }

  static void printHex32(Print& log, uint32_t value) {
    for (int shift = 28; shift >= 0; shift -= 4) {
      const uint8_t nibble = (value >> shift) & 0x0F;
      log.print(nibble < 10 ? char('0' + nibble) : char('A' + nibble - 10));
    }
  }

 private:
  static twai_timing_config_t timingForBitrate(uint32_t bitrate) {
    switch (bitrate) {
      case 250000:
        return TWAI_TIMING_CONFIG_250KBITS();
      case 500000:
      default:
        return TWAI_TIMING_CONFIG_500KBITS();
      case 1000000:
        return TWAI_TIMING_CONFIG_1MBITS();
    }
  }

  CanRxFrame toRxFrame(const twai_message_t& message) const {
    CanRxFrame frame{};
    frame.lastIdentifier = message.identifier;
    frame.lastDlc = message.data_length_code;
    frame.lastCommand = message.data[0];
    frame.isExtended = message.extd;
    frame.isRemoteRequest = message.rtr;
    for (uint8_t i = 0; i < 8; ++i) {
      frame.data[i] = message.data[i];
    }
    return frame;
  }

  CanRxState dispatchFrame(const CanRxState& currentState, const CanRxFrame& frame) const {
    CanRxState nextState = currentState;
    nextState.sawAnyFrame = true;
    nextState.anyFrameCount += 1;
    nextState.lastIdentifier = frame.lastIdentifier;
    nextState.lastDlc = frame.lastDlc;
    nextState.lastCommand = frame.lastCommand;
    nextState.lastIsExtended = frame.isExtended;
    nextState.lastIsRemoteRequest = frame.isRemoteRequest;
    for (uint8_t i = 0; i < 8; ++i) {
      nextState.lastData[i] = frame.data[i];
    }

    switch (classifyResponse(frame)) {
      case CanResponseKind::Normal:
        if (normalResponseHook_ != nullptr) {
          return normalResponseHook_(nextState, frame);
        }
        nextState.hasNormalResponse = true;
        nextState.normalResponseCount += 1;
        return nextState;
      case CanResponseKind::Data:
        if (dataResponseHook_ != nullptr) {
          return dataResponseHook_(nextState, frame);
        }
        nextState.dataResponseCount += 1;
        return nextState;
      case CanResponseKind::Unknown:
      default:
        nextState.sawUnexpectedFrame = true;
        nextState.unexpectedFrameCount += 1;
        return nextState;
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

      prepareCommandFrame(message, command[0], packetNumber, command[1], chunkLen);

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

  static void prepareCommandFrame(twai_message_t& message,
                                  uint8_t motorAddress,
                                  uint8_t packetNumber,
                                  uint8_t commandId,
                                  size_t payloadLen) {
    message.extd = 1;
    message.identifier = commandFrameIdentifier(motorAddress, packetNumber);
    message.data_length_code = static_cast<uint8_t>(payloadLen + 1);
    message.data[0] = commandId;
  }

  static uint32_t commandFrameIdentifier(uint8_t motorAddress, uint8_t packetNumber) {
    return (static_cast<uint32_t>(motorAddress) << 8) | packetNumber;
  }

  CanConfig config_;
  bool ready_;
  ResponseHook normalResponseHook_;
  ResponseHook dataResponseHook_;
};

}  // namespace test_app
