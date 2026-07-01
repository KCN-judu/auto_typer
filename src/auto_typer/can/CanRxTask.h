#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "CanBus.h"

namespace auto_typer {

class MotorFeedbackStore {
 public:
  MotorFeedbackStore() : mutex_(xSemaphoreCreateMutex()) {
    for (uint8_t i = 0; i < kCapacity; ++i) {
      states_[i] = {};
    }
  }

  void updateVelocity(uint8_t id, float rpm, uint32_t nowMs) {
    if (!lock()) {
      return;
    }
    MotorState& state = stateFor(id);
    state.id = id;
    state.velocityRpm = rpm;
    state.lastFeedbackMs = nowMs;
    unlock();
  }

  void updatePosition(uint8_t id, int32_t steps, uint32_t nowMs) {
    if (!lock()) {
      return;
    }
    MotorState& state = stateFor(id);
    state.id = id;
    state.observedPositionSteps = steps;
    state.lastFeedbackMs = nowMs;
    unlock();
  }

  void updateFault(uint8_t id, bool fault, uint32_t nowMs) {
    if (!lock()) {
      return;
    }
    MotorState& state = stateFor(id);
    state.id = id;
    state.fault = fault;
    state.lastFeedbackMs = nowMs;
    unlock();
  }

  MotorState get(uint8_t id) const {
    if (!lock()) {
      MotorState fallback{};
      fallback.id = id;
      return fallback;
    }
    MotorState state = states_[slotFor(id)];
    if (state.id == 0) {
      state.id = id;
    }
    unlock();
    return state;
  }

 private:
  static constexpr uint8_t kCapacity = 32;

  uint8_t slotFor(uint8_t id) const {
    return id % kCapacity;
  }

  MotorState& stateFor(uint8_t id) {
    return states_[slotFor(id)];
  }

  bool lock() const {
    return mutex_ == nullptr || xSemaphoreTake(mutex_, pdMS_TO_TICKS(5)) == pdTRUE;
  }

  void unlock() const {
    if (mutex_ != nullptr) {
      xSemaphoreGive(mutex_);
    }
  }

  mutable SemaphoreHandle_t mutex_;
  MotorState states_[kCapacity];
};

class CanRxTask {
 public:
  CanRxTask(CanBus& bus, MotorFeedbackStore& feedback) : bus_(bus), feedback_(feedback) {}

  void tick() {
    bus_.readAlerts(0);
    CanFrame frame{};
    while (bus_.receive(frame, 0)) {
      parseFrame(frame);
    }
  }

 private:
  void parseFrame(const CanFrame& frame) {
    if (frame.data_length_code == 0) {
      return;
    }
    const uint8_t motorId = frame.extd ? static_cast<uint8_t>((frame.identifier >> 8) & 0xFF) : 0;
    const uint8_t command = frame.data[0];
    const uint32_t nowMs = millis();
    if (command == 0x35 && frame.data_length_code >= 4) {
      const uint16_t rawRpm = (static_cast<uint16_t>(frame.data[2]) << 8) | frame.data[3];
      float rpm = static_cast<float>(rawRpm);
      if (frame.data[1] != 0) {
        rpm = -rpm;
      }
      feedback_.updateVelocity(motorId, rpm, nowMs);
      return;
    }
    if (command == 0x36 && frame.data_length_code >= 6) {
      const uint32_t rawPosition = (static_cast<uint32_t>(frame.data[2]) << 24) |
                                   (static_cast<uint32_t>(frame.data[3]) << 16) |
                                   (static_cast<uint32_t>(frame.data[4]) << 8) |
                                   static_cast<uint32_t>(frame.data[5]);
      const int32_t signedPosition = frame.data[1] == 0 ? static_cast<int32_t>(rawPosition)
                                                        : -static_cast<int32_t>(rawPosition);
      feedback_.updatePosition(motorId, signedPosition, nowMs);
      return;
    }
    if (command == 0x3A || command == 0x3B) {
      feedback_.updateFault(motorId, false, nowMs);
    }
  }

  CanBus& bus_;
  MotorFeedbackStore& feedback_;
};

}  // namespace auto_typer
