#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "CanBus.h"
#include "EmmV5EventStore.h"
#include "ProtocolTrace.h"

namespace auto_typer {

class MotorFeedbackStore {
 public:
  MotorFeedbackStore() : mutex_(xSemaphoreCreateMutex()) {
    for (uint8_t i = 0; i < kCapacity; ++i) {
      states_[i] = {};
    }
  }

  void apply(const EmmV5Event& event) {
    if (event.kind == EmmV5EventKind::InvalidFrame || event.motorId == 0) {
      return;
    }
    if (!lock()) {
      return;
    }
    MotorState& state = stateFor(event.motorId);
    state.id = event.motorId;
    state.lastAnyFrameMs = event.timeMs;
    switch (event.kind) {
      case EmmV5EventKind::CommandAcked:
        state.lastAckCommand = event.command;
        state.lastAckMs = event.timeMs;
        break;
      case EmmV5EventKind::CommandConditionNotMet:
        state.conditionNotMet = true;
        state.lastConditionNotMetCommand = event.command;
        state.lastConditionNotMetMs = event.timeMs;
        break;
      case EmmV5EventKind::CommandMalformed:
        state.commandMalformed = true;
        state.lastMalformedCommand = event.command;
        state.lastMalformedMs = event.timeMs;
        break;
      case EmmV5EventKind::MotionReached:
        state.motionReached = true;
        state.lastMotionReachedMs = event.timeMs;
        break;
      case EmmV5EventKind::VelocityFeedback:
        state.hasVelocity = true;
        state.velocityRpm = event.velocityRpm;
        state.lastVelocityMs = event.timeMs;
        break;
      case EmmV5EventKind::RealtimeAngleFeedback:
        state.hasRealtimeAngle = true;
        state.realtimeAngleRaw65536 = event.angleRaw65536;
        state.lastRealtimeAngleMs = event.timeMs;
        break;
      case EmmV5EventKind::InputPulseFeedback:
        state.hasInputPulse = true;
        state.inputPulseSteps = event.inputPulseSteps;
        state.lastInputPulseMs = event.timeMs;
        break;
      case EmmV5EventKind::StatusFlagsFeedback:
      case EmmV5EventKind::HomeStatusFeedback:
        state.hasStatus = true;
        state.statusFlags = event.statusFlags;
        state.lastStatusMs = event.timeMs;
        break;
      case EmmV5EventKind::None:
      case EmmV5EventKind::UnknownFrame:
      case EmmV5EventKind::InvalidFrame:
      default:
        break;
    }
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
  CanRxTask(CanBus& bus, MotorFeedbackStore& feedback, EmmV5EventStore& events, ProtocolTrace& trace)
      : bus_(bus), feedback_(feedback), events_(events), trace_(trace) {}

  void tick(size_t maxFrames = 16) {
    bus_.readAlerts(0);
    CanFrame frame{};
    size_t received = 0;
    while (received < maxFrames && bus_.receive(frame, 0)) {
      EmmV5Event event = parser_.parse(frame);
      trace_.addRx(frame, event);
      events_.push(event);
      feedback_.apply(event);
      ++received;
    }
  }

 private:
  CanBus& bus_;
  MotorFeedbackStore& feedback_;
  EmmV5EventStore& events_;
  ProtocolTrace& trace_;
  EmmV5ProtocolParser parser_;
};

}  // namespace auto_typer
