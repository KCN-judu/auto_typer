#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "CanBus.h"
#include "EmmV5EventStore.h"
#include "ProtocolTrace.h"

namespace auto_typer {

enum class MotorTelemetryEventKind : uint8_t {
  Ack,
  ConditionNotMet,
  Malformed,
  MotionReached,
  Unknown,
};

struct MotorTelemetryEvent {
  MotorTelemetryEventKind kind;
  uint8_t motorId;
  uint8_t command;
  uint32_t timestampMs;
  bool faultSeverity;
};

struct MotorStateSnapshot {
  uint8_t motorId;
  MotorState state;
  uint32_t lastUpdatedAtMs;
};

class MotorFeedbackStore {
 public:
  MotorFeedbackStore() : mutex_(nullptr) {
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
    // These booleans are diagnostic latches only. Runtime motion decisions must
    // use last*Ms timestamps scoped to the current motion step.
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
        state.lastErrorCode = "condition_not_met";
        state.lastErrorMessage = "Motor reported command condition not met";
        break;
      case EmmV5EventKind::CommandMalformed:
        state.commandMalformed = true;
        state.lastMalformedCommand = event.command;
        state.lastMalformedMs = event.timeMs;
        state.lastErrorCode = "command_malformed";
        state.lastErrorMessage = "Motor reported malformed command";
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
        applyStatusFlags(state, event);
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

  MotorState getMotorState(uint8_t id) const {
    return get(id);
  }

  bool hasFreshInputPulse(uint8_t id, uint32_t nowMs, uint32_t maxAgeMs) const {
    const MotorState state = get(id);
    return state.hasInputPulse && state.lastInputPulseMs != 0 && nowMs - state.lastInputPulseMs <= maxAgeMs;
  }

  bool hasFreshVelocity(uint8_t id, uint32_t nowMs, uint32_t maxAgeMs) const {
    const MotorState state = get(id);
    return state.hasVelocity && state.lastVelocityMs != 0 && nowMs - state.lastVelocityMs <= maxAgeMs;
  }

 private:
  static constexpr uint8_t kCapacity = 32;

  static void applyStatusFlags(MotorState& state, const EmmV5Event& event) {
    state.driverFault = false;
    // TODO(kcn): Map documented EMM_V5 status bits when authoritative bit definitions are added to the repo.
    (void)state;
    (void)event;
  }

  uint8_t slotFor(uint8_t id) const {
    return id % kCapacity;
  }

  MotorState& stateFor(uint8_t id) {
    return states_[slotFor(id)];
  }

  bool lock() const {
    return ensureMutex() && xSemaphoreTake(mutex_, pdMS_TO_TICKS(5)) == pdTRUE;
  }

  void unlock() const {
    if (mutex_ != nullptr) {
      xSemaphoreGive(mutex_);
    }
  }

  bool ensureMutex() const {
    if (mutex_ == nullptr) {
      mutex_ = xSemaphoreCreateMutex();
    }
    return mutex_ != nullptr;
  }

  mutable SemaphoreHandle_t mutex_;
  MotorState states_[kCapacity];
};

class MotorTelemetryBuffer {
 public:
  MotorTelemetryBuffer()
      : mutex_(nullptr),
        criticalHead_(0),
        criticalCount_(0),
        overflow_(false) {
    for (uint8_t i = 0; i < kMotorCapacity; ++i) {
      dirty_[i] = false;
      snapshots_[i] = {};
    }
    for (uint8_t i = 0; i < kCriticalCapacity; ++i) {
      critical_[i] = {};
    }
  }

  void observe(const EmmV5Event& event, const MotorFeedbackStore& feedback) {
    if (event.motorId == 0 || event.kind == EmmV5EventKind::InvalidFrame || event.kind == EmmV5EventKind::None) {
      return;
    }
    switch (event.kind) {
      case EmmV5EventKind::CommandConditionNotMet:
        pushCriticalMotorEvent({MotorTelemetryEventKind::ConditionNotMet, event.motorId, event.command, event.timeMs, true});
        return;
      case EmmV5EventKind::CommandMalformed:
        pushCriticalMotorEvent({MotorTelemetryEventKind::Malformed, event.motorId, event.command, event.timeMs, true});
        return;
      case EmmV5EventKind::MotionReached:
        pushCriticalMotorEvent({MotorTelemetryEventKind::MotionReached, event.motorId, event.command, event.timeMs, false});
        return;
      case EmmV5EventKind::VelocityFeedback:
      case EmmV5EventKind::RealtimeAngleFeedback:
      case EmmV5EventKind::InputPulseFeedback:
      case EmmV5EventKind::StatusFlagsFeedback:
      case EmmV5EventKind::HomeStatusFeedback:
        markDirtyMotor(event.motorId, feedback.get(event.motorId));
        return;
      case EmmV5EventKind::CommandAcked:
      case EmmV5EventKind::UnknownFrame:
      case EmmV5EventKind::InvalidFrame:
      case EmmV5EventKind::None:
      default:
        return;
    }
  }

  bool pushCriticalMotorEvent(const MotorTelemetryEvent& event) {
    if (!lock()) {
      return false;
    }
    const bool pushed = pushCriticalLocked(event);
    unlock();
    return pushed;
  }

  void markDirtyMotor(uint8_t motorId, const MotorState& state) {
    if (motorId == 0 || !lock()) {
      return;
    }
    const uint8_t slot = slotFor(motorId);
    dirty_[slot] = true;
    snapshots_[slot].motorId = motorId;
    snapshots_[slot].state = state;
    snapshots_[slot].lastUpdatedAtMs = state.lastAnyFrameMs;
    unlock();
  }

  size_t drainCriticalEvents(MotorTelemetryEvent* out, size_t maxCount) {
    if (out == nullptr || maxCount == 0 || !lock()) {
      return 0;
    }
    const size_t n = criticalCount_ < maxCount ? criticalCount_ : maxCount;
    for (size_t i = 0; i < n; ++i) {
      out[i] = critical_[criticalHead_];
      criticalHead_ = (criticalHead_ + 1) % kCriticalCapacity;
      --criticalCount_;
    }
    unlock();
    return n;
  }

  size_t drainDirtyMotorStates(MotorStateSnapshot* out, size_t maxCount) {
    if (out == nullptr || maxCount == 0 || !lock()) {
      return 0;
    }
    size_t count = 0;
    for (uint8_t i = 0; i < kMotorCapacity && count < maxCount; ++i) {
      if (!dirty_[i]) {
        continue;
      }
      dirty_[i] = false;
      out[count] = snapshots_[i];
      ++count;
    }
    unlock();
    return count;
  }

  bool hasOverflow() const {
    if (!lock()) {
      return true;
    }
    const bool value = overflow_;
    unlock();
    return value;
  }

  void clearOverflow() {
    if (!lock()) {
      return;
    }
    overflow_ = false;
    unlock();
  }

 private:
  static constexpr uint8_t kMotorCapacity = 32;
  static constexpr uint8_t kCriticalCapacity = 16;

  uint8_t slotFor(uint8_t motorId) const {
    return motorId % kMotorCapacity;
  }

  bool pushCriticalLocked(const MotorTelemetryEvent& event) {
    if (criticalCount_ < kCriticalCapacity) {
      const uint8_t index = (criticalHead_ + criticalCount_) % kCriticalCapacity;
      critical_[index] = event;
      ++criticalCount_;
      return true;
    }
    overflow_ = true;
    if (!event.faultSeverity) {
      return false;
    }
    for (uint8_t i = 0; i < kCriticalCapacity; ++i) {
      const uint8_t index = (criticalHead_ + i) % kCriticalCapacity;
      if (!critical_[index].faultSeverity) {
        critical_[index] = event;
        return true;
      }
    }
    const uint8_t newest = (criticalHead_ + kCriticalCapacity - 1) % kCriticalCapacity;
    critical_[newest] = event;
    return true;
  }

  bool lock() const {
    return ensureMutex() && xSemaphoreTake(mutex_, pdMS_TO_TICKS(5)) == pdTRUE;
  }

  void unlock() const {
    if (mutex_ != nullptr) {
      xSemaphoreGive(mutex_);
    }
  }

  bool ensureMutex() const {
    if (mutex_ == nullptr) {
      mutex_ = xSemaphoreCreateMutex();
    }
    return mutex_ != nullptr;
  }

  mutable SemaphoreHandle_t mutex_;
  MotorTelemetryEvent critical_[kCriticalCapacity];
  uint8_t criticalHead_;
  uint8_t criticalCount_;
  bool dirty_[kMotorCapacity];
  MotorStateSnapshot snapshots_[kMotorCapacity];
  bool overflow_;
};

class CanRxTask {
 public:
  CanRxTask(CanBus& bus,
            MotorFeedbackStore& feedback,
            EmmV5EventStore& events,
            ProtocolTrace& trace,
            MotorTelemetryBuffer* telemetry = nullptr)
      : bus_(bus), feedback_(feedback), events_(events), trace_(trace), telemetry_(telemetry) {}

  void tick(size_t maxFrames = 16) {
    bus_.readAlerts(0);
    CanFrame frame{};
    size_t received = 0;
    while (received < maxFrames && bus_.receive(frame, 0)) {
      EmmV5Event event = parser_.parse(frame, millis());
      trace_.addRx(frame, event);
      events_.push(event);
      feedback_.apply(event);
      if (telemetry_ != nullptr) {
        telemetry_->observe(event, feedback_);
      }
      ++received;
    }
  }

 private:
  CanBus& bus_;
  MotorFeedbackStore& feedback_;
  EmmV5EventStore& events_;
  ProtocolTrace& trace_;
  MotorTelemetryBuffer* telemetry_;
  EmmV5ProtocolParser parser_;
};

}  // namespace auto_typer
