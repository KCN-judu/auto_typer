#pragma once

#include <Arduino.h>

#include "../can/CanTxQueue.h"
#include "../can/ProtocolTrace.h"

namespace auto_typer {

class EmmV5Driver {
 public:
  struct MoveRelativeCommand {
    uint8_t motorId;
    MotorDirection direction;
    uint16_t rpm;
    uint8_t acceleration;
    uint32_t steps;
    bool sync;
  };

  explicit EmmV5Driver(CanTxQueue& tx, ProtocolTrace* trace = nullptr) : tx_(tx), trace_(trace) {}

  bool setClosedLoopControlMode(uint8_t motorId, bool store = false) {
    const uint8_t command[] = {motorId, 0x46, 0x69, static_cast<uint8_t>(store ? 0x01 : 0x00), 0x02, 0x6B};
    return sendCommand(command, sizeof(command));
  }

  bool enableMotor(uint8_t motorId, bool sync = false) {
    const uint8_t command[] = {motorId, 0xF3, 0xAB, 0x01, static_cast<uint8_t>(sync), 0x6B};
    return sendCommand(command, sizeof(command));
  }

  bool disableMotor(uint8_t motorId, bool sync = false) {
    const uint8_t command[] = {motorId, 0xF3, 0xAB, 0x00, static_cast<uint8_t>(sync), 0x6B};
    return sendCommand(command, sizeof(command));
  }

  bool moveRelative(uint8_t motorId,
                    MotorDirection direction,
                    uint16_t rpm,
                    uint8_t acceleration,
                    uint32_t steps,
                    bool sync) {
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

  bool moveRelativeBatch(const MoveRelativeCommand* commands, size_t count, bool triggerBroadcast) {
    if ((commands == nullptr && count > 0) || count > kMaxMoveBatchCommands) {
      return false;
    }
    CanFrame frames[kMaxBatchFrames]{};
    size_t frameCount = 0;
    for (size_t i = 0; i < count; ++i) {
      if (!appendMoveRelativeFrames(commands[i], frames, kMaxBatchFrames, frameCount)) {
        return false;
      }
    }
    if (triggerBroadcast) {
      const uint8_t triggerCommand[] = {0x00, 0xFF, 0x66, 0x6B};
      if (!appendCommandFrames(triggerCommand, sizeof(triggerCommand), frames, kMaxBatchFrames, frameCount)) {
        return false;
      }
    }
    if (!tx_.enqueueBatch(frames, frameCount)) {
      return false;
    }
    traceQueuedFrames(frames, frameCount);
    return true;
  }

  bool triggerSynchronousMotion(uint8_t motorId) {
    const uint8_t command[] = {motorId, 0xFF, 0x66, 0x6B};
    return sendCommand(command, sizeof(command));
  }

  bool triggerSynchronousMotionBroadcast() {
    const uint8_t command[] = {0x00, 0xFF, 0x66, 0x6B};
    return sendCommand(command, sizeof(command));
  }

  bool stopNow(uint8_t motorId, bool sync = false) {
    const uint8_t command[] = {motorId, 0xFE, 0x98, static_cast<uint8_t>(sync), 0x6B};
    return sendCommand(command, sizeof(command), true);
  }

  bool requestVelocity(uint8_t motorId) {
    const uint8_t command[] = {motorId, 0x35, 0x6B};
    return sendCommand(command, sizeof(command));
  }

  bool requestPosition(uint8_t motorId) {
    const uint8_t command[] = {motorId, 0x36, 0x6B};
    return sendCommand(command, sizeof(command));
  }

  bool requestInputPulseCount(uint8_t motorId) {
    const uint8_t command[] = {motorId, 0x32, 0x6B};
    return sendCommand(command, sizeof(command));
  }

  bool requestStatusFlags(uint8_t motorId) {
    const uint8_t command[] = {motorId, 0x3A, 0x6B};
    return sendCommand(command, sizeof(command));
  }

 private:
  static constexpr size_t kMaxCommandFrames = 4;
  static constexpr size_t kMaxMoveBatchCommands = 3;
  static constexpr size_t kMaxBatchFrames = kMaxMoveBatchCommands * 2 + 1;

  bool sendCommand(const uint8_t* command, size_t len, bool highPriority = false) {
    CanFrame frames[kMaxCommandFrames]{};
    size_t frameCount = 0;
    if (!appendCommandFrames(command, len, frames, kMaxCommandFrames, frameCount)) {
      return false;
    }
    if (!tx_.enqueueBatch(frames, frameCount, highPriority)) {
      return false;
    }
    traceQueuedFrames(frames, frameCount);
    return true;
  }

  bool appendMoveRelativeFrames(const MoveRelativeCommand& move,
                                CanFrame* frames,
                                size_t maxFrames,
                                size_t& frameCount) const {
    const uint8_t command[] = {
      move.motorId,
      0xFD,
      static_cast<uint8_t>(move.direction),
      static_cast<uint8_t>((move.rpm >> 8) & 0xFF),
      static_cast<uint8_t>(move.rpm & 0xFF),
      move.acceleration,
      static_cast<uint8_t>((move.steps >> 24) & 0xFF),
      static_cast<uint8_t>((move.steps >> 16) & 0xFF),
      static_cast<uint8_t>((move.steps >> 8) & 0xFF),
      static_cast<uint8_t>(move.steps & 0xFF),
      0x00,
      static_cast<uint8_t>(move.sync),
      0x6B,
    };
    return appendCommandFrames(command, sizeof(command), frames, maxFrames, frameCount);
  }

  bool appendCommandFrames(const uint8_t* command,
                           size_t len,
                           CanFrame* frames,
                           size_t maxFrames,
                           size_t& frameCount) const {
    if (len < 2) {
      return false;
    }
    const size_t payloadLen = len - 2;
    size_t offset = 0;
    uint8_t packetNumber = 0;
    while (offset < payloadLen) {
      if (frameCount >= maxFrames) {
        return false;
      }
      CanFrame frame{};
      const size_t remaining = payloadLen - offset;
      const size_t chunkLen = remaining < 7 ? remaining : 7;
      frame.extd = 1;
      frame.identifier = (static_cast<uint32_t>(command[0]) << 8) | packetNumber;
      frame.data_length_code = static_cast<uint8_t>(chunkLen + 1);
      frame.data[0] = command[1];
      for (size_t i = 0; i < chunkLen; ++i) {
        frame.data[i + 1] = command[offset + 2];
        ++offset;
      }
      frames[frameCount] = frame;
      ++frameCount;
      ++packetNumber;
    }
    return true;
  }

  void traceQueuedFrames(const CanFrame* frames, size_t count) {
    if (trace_ == nullptr) {
      return;
    }
    for (size_t i = 0; i < count; ++i) {
      trace_->addTxQueued(frames[i]);
    }
  }

  CanTxQueue& tx_;
  ProtocolTrace* trace_;
};

}  // namespace auto_typer
