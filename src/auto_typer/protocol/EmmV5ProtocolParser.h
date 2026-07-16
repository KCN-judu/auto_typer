#pragma once

#include <Arduino.h>

#include "../can/CanFrame.h"

namespace auto_typer {

enum class EmmV5EventKind : uint8_t {
  None,
  CommandAcked,
  CommandConditionNotMet,
  CommandMalformed,
  MotionReached,
  VelocityFeedback,
  RealtimeAngleFeedback,
  InputPulseFeedback,
  StatusFlagsFeedback,
  HomeStatusFeedback,
  UnknownFrame,
  InvalidFrame,
};

struct EmmV5Event {
  EmmV5EventKind kind;
  uint8_t motorId;
  uint8_t packetIndex;
  uint8_t command;
  uint8_t status;
  uint8_t raw[8];
  uint8_t dlc;
  uint32_t canId;
  uint32_t timeMs;

  float velocityRpm;
  int32_t angleRaw65536;
  int32_t inputPulseSteps;
  uint32_t statusFlags;

  const char* errorCode;
};

class EmmV5ProtocolParser {
 public:
  EmmV5Event parse(const CanFrame& frame, uint32_t timeMs) const {
    EmmV5Event event{};
    event.kind = EmmV5EventKind::None;
    event.canId = frame.identifier;
    event.dlc = frame.data_length_code > 8 ? 8 : frame.data_length_code;
    event.timeMs = timeMs;
    event.errorCode = "";
    for (uint8_t i = 0; i < event.dlc; ++i) {
      event.raw[i] = frame.data[i];
    }

    if (!frame.extd) {
      event.kind = EmmV5EventKind::InvalidFrame;
      event.errorCode = "not_extended_frame";
      return event;
    }
    if (frame.rtr || event.dlc == 0) {
      event.kind = EmmV5EventKind::InvalidFrame;
      event.errorCode = "non_data_frame";
      return event;
    }

    event.motorId = static_cast<uint8_t>((frame.identifier >> 8) & 0xFF);
    event.packetIndex = static_cast<uint8_t>(frame.identifier & 0xFF);
    event.command = event.raw[0];

    if (event.raw[event.dlc - 1] != 0x6B) {
      event.kind = EmmV5EventKind::InvalidFrame;
      event.errorCode = "bad_checksum";
      return event;
    }

    if (event.command == 0x32 && event.dlc >= 7) {
      const uint32_t rawPulse = readU32(&event.raw[2]);
      event.kind = EmmV5EventKind::InputPulseFeedback;
      event.inputPulseSteps = event.raw[1] == 0 ? static_cast<int32_t>(rawPulse) : -static_cast<int32_t>(rawPulse);
      return event;
    }
    if (event.command == 0x35 && event.dlc >= 5) {
      const uint16_t rawRpm = (static_cast<uint16_t>(event.raw[2]) << 8) | event.raw[3];
      event.kind = EmmV5EventKind::VelocityFeedback;
      event.velocityRpm = event.raw[1] == 0 ? static_cast<float>(rawRpm) : -static_cast<float>(rawRpm);
      return event;
    }
    if (event.command == 0x36 && event.dlc >= 7) {
      const uint32_t rawAngle = readU32(&event.raw[2]);
      event.kind = EmmV5EventKind::RealtimeAngleFeedback;
      event.angleRaw65536 = event.raw[1] == 0 ? static_cast<int32_t>(rawAngle) : -static_cast<int32_t>(rawAngle);
      return event;
    }
    if (event.command == 0x3A) {
      event.kind = EmmV5EventKind::StatusFlagsFeedback;
      event.statusFlags = readStatusFlags(&event.raw[1], event.dlc > 2 ? event.dlc - 2 : 0);
      return event;
    }
    if (event.command == 0x3B) {
      event.kind = EmmV5EventKind::HomeStatusFeedback;
      event.statusFlags = readStatusFlags(&event.raw[1], event.dlc > 2 ? event.dlc - 2 : 0);
      return event;
    }
    if (event.dlc == 3 && event.raw[0] == 0x00 && event.raw[1] == 0xEE) {
      event.kind = EmmV5EventKind::CommandMalformed;
      event.command = 0x00;
      event.status = 0xEE;
      return event;
    }
    if (event.dlc == 3 && event.raw[0] == 0xFD && event.raw[1] == 0x9F) {
      event.kind = EmmV5EventKind::MotionReached;
      event.command = 0xFD;
      event.status = 0x9F;
      return event;
    }
    if (event.dlc == 3 && event.raw[1] == 0x02) {
      event.kind = EmmV5EventKind::CommandAcked;
      event.status = 0x02;
      return event;
    }
    if (event.dlc == 3 && event.raw[1] == 0xE2) {
      event.kind = EmmV5EventKind::CommandConditionNotMet;
      event.status = 0xE2;
      return event;
    }

    event.kind = EmmV5EventKind::UnknownFrame;
    return event;
  }

  static const char* kindText(EmmV5EventKind kind) {
    switch (kind) {
      case EmmV5EventKind::CommandAcked:
        return "CommandAcked";
      case EmmV5EventKind::CommandConditionNotMet:
        return "CommandConditionNotMet";
      case EmmV5EventKind::CommandMalformed:
        return "CommandMalformed";
      case EmmV5EventKind::MotionReached:
        return "MotionReached";
      case EmmV5EventKind::VelocityFeedback:
        return "VelocityFeedback";
      case EmmV5EventKind::RealtimeAngleFeedback:
        return "RealtimeAngleFeedback";
      case EmmV5EventKind::InputPulseFeedback:
        return "InputPulseFeedback";
      case EmmV5EventKind::StatusFlagsFeedback:
        return "StatusFlagsFeedback";
      case EmmV5EventKind::HomeStatusFeedback:
        return "HomeStatusFeedback";
      case EmmV5EventKind::UnknownFrame:
        return "UnknownFrame";
      case EmmV5EventKind::InvalidFrame:
        return "InvalidFrame";
      case EmmV5EventKind::None:
      default:
        return "None";
    }
  }

 private:
  static uint32_t readU32(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
  }

  static uint32_t readStatusFlags(const uint8_t* bytes, uint8_t count) {
    uint32_t flags = 0;
    const uint8_t limited = count > 4 ? 4 : count;
    for (uint8_t i = 0; i < limited; ++i) {
      flags = (flags << 8) | bytes[i];
    }
    return flags;
  }
};

}  // namespace auto_typer
