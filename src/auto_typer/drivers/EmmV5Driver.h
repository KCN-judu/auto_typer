#pragma once

#include <Arduino.h>

#include "../can/CanTxQueue.h"
#include "../can/ProtocolTrace.h"

namespace auto_typer {

class EmmV5Driver {
 public:
  explicit EmmV5Driver(CanTxQueue& tx, ProtocolTrace* trace = nullptr) : tx_(tx), trace_(trace) {}

  bool setClosedLoopControlMode(uint8_t motorId) {
    const uint8_t command[] = {motorId, 0x46, 0x69, 0x00, 0x02, 0x6B};
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

  bool triggerSynchronousMotion(uint8_t motorId) {
    const uint8_t command[] = {motorId, 0xFF, 0x66, 0x6B};
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
  bool sendCommand(const uint8_t* command, size_t len, bool highPriority = false) {
    if (len < 2) {
      return false;
    }
    const size_t payloadLen = len - 2;
    size_t offset = 0;
    uint8_t packetNumber = 0;
    while (offset < payloadLen) {
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
      if (trace_ != nullptr) {
        trace_->addTx(frame);
      }
      if (!tx_.enqueue(frame, highPriority)) {
        return false;
      }
      ++packetNumber;
    }
    return true;
  }

  CanTxQueue& tx_;
  ProtocolTrace* trace_;
};

}  // namespace auto_typer
