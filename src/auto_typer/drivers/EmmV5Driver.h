#pragma once

#include <Arduino.h>

#include "../can/CanTxQueue.h"
#include "../protocol/EmmV5CommandCodec.h"

namespace auto_typer {

class EmmV5Driver {
 public:
  struct MoveAbsoluteCommand {
    uint8_t motorId;
    MotorDirection direction;
    uint16_t rpm;
    uint8_t acceleration;
    uint32_t targetSteps;
    bool sync;
  };

  explicit EmmV5Driver(CanTxQueue& tx) : tx_(tx) {}

  bool setClosedLoopControlMode(uint8_t motorId, bool store = false) {
    return sendCommand(EmmV5CommandCodec::setClosedLoopControlMode(motorId, store));
  }

  bool enableMotor(uint8_t motorId, bool sync = false) {
    return sendCommand(EmmV5CommandCodec::enableMotor(motorId, sync));
  }

  bool disableMotor(uint8_t motorId, bool sync = false, bool highPriority = false) {
    return sendCommand(EmmV5CommandCodec::disableMotor(motorId, sync), highPriority);
  }

  bool unlockMotor(uint8_t motorId, bool highPriority = false) {
    return sendCommand(EmmV5CommandCodec::unlockMotor(motorId), highPriority);
  }

  bool moveAbsolute(uint8_t motorId,
                    MotorDirection direction,
                    uint16_t rpm,
                    uint8_t acceleration,
                    uint32_t targetSteps,
                    bool sync,
                    bool highPriority = false) {
    return sendCommand(EmmV5CommandCodec::moveAbsolute({motorId, direction, rpm, acceleration, targetSteps, sync}),
                       highPriority);
  }

  bool moveAbsoluteBatch(const MoveAbsoluteCommand* commands,
                         size_t count,
                         bool triggerBroadcast,
                         bool highPriority = false) {
    if ((commands == nullptr && count > 0) || count > kMaxMoveBatchCommands) {
      return false;
    }
    CanFrame frames[EmmV5CommandCodec::kMaxBatchFrames]{};
    size_t frameCount = 0;
    for (size_t i = 0; i < count; ++i) {
      const EmmV5Command command =
          EmmV5CommandCodec::moveAbsolute({commands[i].motorId,
                                           commands[i].direction,
                                           commands[i].rpm,
                                           commands[i].acceleration,
                                           commands[i].targetSteps,
                                           commands[i].sync});
      if (!EmmV5CommandCodec::append(command, frames, EmmV5CommandCodec::kMaxBatchFrames, frameCount)) {
        return false;
      }
    }
    if (triggerBroadcast) {
      if (!EmmV5CommandCodec::append(EmmV5CommandCodec::triggerSynchronousMotionBroadcast(),
                                     frames,
                                     EmmV5CommandCodec::kMaxBatchFrames,
                                     frameCount)) {
        return false;
      }
    }
    return tx_.enqueueBatch(frames, frameCount, highPriority);
  }

  size_t availableForWrite() {
    return tx_.availableForWrite();
  }

  static constexpr size_t moveAbsoluteFrameCount() {
    return 2;
  }

  bool clearPosition(uint8_t motorId, bool highPriority = false) {
    return sendCommand(EmmV5CommandCodec::clearPosition(motorId), highPriority);
  }

  static constexpr size_t triggerBroadcastFrameCount() {
    return 1;
  }

  bool triggerSynchronousMotion(uint8_t motorId) {
    return sendCommand(EmmV5CommandCodec::triggerSynchronousMotion(motorId));
  }

  bool triggerSynchronousMotionBroadcast() {
    return sendCommand(EmmV5CommandCodec::triggerSynchronousMotionBroadcast());
  }

  bool stopNow(uint8_t motorId, bool sync = false) {
    return sendCommand(EmmV5CommandCodec::stopNow(motorId, sync), true);
  }

  bool requestVelocity(uint8_t motorId) {
    return sendCommand(EmmV5CommandCodec::requestVelocity(motorId));
  }

  bool requestRealtimeAngle(uint8_t motorId) {
    return sendCommand(EmmV5CommandCodec::requestRealtimeAngle(motorId));
  }

  bool requestInputPulseCount(uint8_t motorId) {
    return sendCommand(EmmV5CommandCodec::requestInputPulseCount(motorId));
  }

  bool requestStatusFlags(uint8_t motorId) {
    return sendCommand(EmmV5CommandCodec::requestStatusFlags(motorId));
  }

  bool requestHomeStatusFlags(uint8_t motorId) {
    return sendCommand(EmmV5CommandCodec::requestHomeStatusFlags(motorId));
  }

 private:
  static constexpr size_t kMaxMoveBatchCommands = EmmV5CommandCodec::kMaxMoveBatchCommands;

  bool sendCommand(const EmmV5Command& command, bool highPriority = false) {
    CanFrame frames[EmmV5CommandCodec::kMaxCommandFrames]{};
    size_t frameCount = 0;
    if (!EmmV5CommandCodec::encode(command, frames, EmmV5CommandCodec::kMaxCommandFrames, frameCount)) {
      return false;
    }
    return tx_.enqueueBatch(frames, frameCount, highPriority);
  }

  CanTxQueue& tx_;
};

}  // namespace auto_typer
