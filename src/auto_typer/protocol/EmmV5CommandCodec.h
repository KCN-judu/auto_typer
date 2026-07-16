#pragma once

#include <Arduino.h>
#include <initializer_list>

#include "../auto_typer_types.h"
#include "../can/CanFrame.h"

namespace auto_typer {

struct EmmV5Command {
  uint8_t bytes[16];
  size_t length;
};

struct EmmV5MoveAbsoluteCommand {
  uint8_t motorId;
  MotorDirection direction;
  uint16_t rpm;
  uint8_t acceleration;
  uint32_t targetSteps;
  bool sync;
};

class EmmV5CommandCodec {
 public:
  static constexpr size_t kMaxCommandFrames = 4;
  static constexpr size_t kMaxMoveBatchCommands = 3;
  static constexpr size_t kMaxBatchFrames = kMaxMoveBatchCommands * 2 + 1;

  static EmmV5Command setClosedLoopControlMode(uint8_t motorId, bool store = false) {
    return command({motorId, 0x46, 0x69, static_cast<uint8_t>(store ? 0x01 : 0x00), 0x02, 0x6B});
  }

  static EmmV5Command enableMotor(uint8_t motorId, bool sync = false) {
    return command({motorId, 0xF3, 0xAB, 0x01, static_cast<uint8_t>(sync), 0x6B});
  }

  static EmmV5Command disableMotor(uint8_t motorId, bool sync = false) {
    return command({motorId, 0xF3, 0xAB, 0x00, static_cast<uint8_t>(sync), 0x6B});
  }

  static EmmV5Command unlockMotor(uint8_t motorId) {
    return command({motorId, 0x0E, 0x52, 0x6B});
  }

  static EmmV5Command moveAbsolute(const EmmV5MoveAbsoluteCommand& move) {
    return command({
      move.motorId,
      0xFD,
      static_cast<uint8_t>(move.direction),
      static_cast<uint8_t>((move.rpm >> 8) & 0xFF),
      static_cast<uint8_t>(move.rpm & 0xFF),
      move.acceleration,
      static_cast<uint8_t>((move.targetSteps >> 24) & 0xFF),
      static_cast<uint8_t>((move.targetSteps >> 16) & 0xFF),
      static_cast<uint8_t>((move.targetSteps >> 8) & 0xFF),
      static_cast<uint8_t>(move.targetSteps & 0xFF),
      0x01,
      static_cast<uint8_t>(move.sync),
      0x6B,
    });
  }

  static EmmV5Command clearPosition(uint8_t motorId) {
    return command({motorId, 0x0A, 0x6D, 0x6B});
  }

  static EmmV5Command triggerSynchronousMotion(uint8_t motorId) {
    return command({motorId, 0xFF, 0x66, 0x6B});
  }

  static EmmV5Command triggerSynchronousMotionBroadcast() {
    return command({0x00, 0xFF, 0x66, 0x6B});
  }

  static EmmV5Command stopNow(uint8_t motorId, bool sync = false) {
    return command({motorId, 0xFE, 0x98, static_cast<uint8_t>(sync), 0x6B});
  }

  static EmmV5Command requestVelocity(uint8_t motorId) {
    return command({motorId, 0x35, 0x6B});
  }

  static EmmV5Command requestRealtimeAngle(uint8_t motorId) {
    return command({motorId, 0x36, 0x6B});
  }

  static EmmV5Command requestInputPulseCount(uint8_t motorId) {
    return command({motorId, 0x32, 0x6B});
  }

  static EmmV5Command requestStatusFlags(uint8_t motorId) {
    return command({motorId, 0x3A, 0x6B});
  }

  static EmmV5Command requestHomeStatusFlags(uint8_t motorId) {
    return command({motorId, 0x3B, 0x6B});
  }

  static bool encode(const EmmV5Command& command, CanFrame* frames, size_t maxFrames, size_t& frameCount) {
    frameCount = 0;
    return append(command, frames, maxFrames, frameCount);
  }

  static bool append(const EmmV5Command& command, CanFrame* frames, size_t maxFrames, size_t& frameCount) {
    if (command.length < 2 || command.length > sizeof(command.bytes)) {
      return false;
    }
    const size_t payloadLen = command.length - 2;
    size_t offset = 0;
    uint8_t packetNumber = 0;
    while (offset < payloadLen) {
      if (frameCount >= maxFrames) {
        return false;
      }
      CanFrame frame{};
      const size_t remaining = payloadLen - offset;
      const size_t chunkLen = remaining < 7 ? remaining : 7;
      frame.extd = true;
      frame.rtr = false;
      frame.identifier = (static_cast<uint32_t>(command.bytes[0]) << 8) | packetNumber;
      frame.data_length_code = static_cast<uint8_t>(chunkLen + 1);
      frame.data[0] = command.bytes[1];
      for (size_t i = 0; i < chunkLen; ++i) {
        frame.data[i + 1] = command.bytes[offset + 2];
        ++offset;
      }
      frames[frameCount] = frame;
      ++frameCount;
      ++packetNumber;
    }
    return true;
  }

 private:
  static EmmV5Command command(std::initializer_list<uint8_t> bytes) {
    EmmV5Command out{};
    out.length = bytes.size();
    size_t i = 0;
    for (uint8_t byte : bytes) {
      if (i < sizeof(out.bytes)) {
        out.bytes[i] = byte;
      }
      ++i;
    }
    return out;
  }
};

}  // namespace auto_typer
