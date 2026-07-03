#pragma once

#include "app_types.h"

namespace test_app {

inline DisplayFrame frameBootOk() {
  return {"BOOT OK", "I2C OK", "OLED OK", "READY"};
}

inline const char* textForStatus(TestStatus status, const char* okText, const char* failText, const char* pendingText) {
  switch (status) {
    case TestStatus::Passed:
      return okText;
    case TestStatus::Failed:
      return failText;
    case TestStatus::Skipped:
      return "SKIP";
    case TestStatus::Pending:
    default:
      return pendingText;
  }
}

inline DisplayFrame frameForSnapshot(const AppSnapshot& snapshot) {
  return {"BOOT OK",
          textForStatus(snapshot.i2cStatus, "I2C OK", "I2C FAIL", "I2C WAIT"),
          textForStatus(snapshot.servoStatus, "SERVO OK", "SERVO FAIL", "SERVO WAIT"),
          textForStatus(snapshot.canStatus, "CAN OK", "CAN FAIL", "CAN WAIT")};
}

inline TestLoopSchedule defaultTestLoopSchedule() {
  return {3000, 8000, 5000};
}

inline const ServoStep* servoSequence(size_t& count) {
  static const ServoStep kSequence[] = {
    {ServoMotion::Neutral, 1500, "Servo neutral / stop"},
    {ServoMotion::Forward, 2000, "Servo pulse 2000us"},
    {ServoMotion::Neutral, 1500, "Servo neutral / stop"},
    {ServoMotion::Reverse, 2000, "Servo pulse 1000us"},
    {ServoMotion::Neutral, 0, "Servo neutral / stop"},
  };
  count = sizeof(kSequence) / sizeof(kSequence[0]);
  return kSequence;
}

inline CanCommandPlan defaultCanCommandPlan() {
  return {MotorDirection::Cw, 300, 10};
}

inline bool shouldInitDisplay(const DevicePresence& presence) {
  return presence.oledAddress != 0;
}

inline I2cRefreshResult summarizeI2cRefresh(const DevicePresence& presence, bool displayReady) {
  return {presence.deviceCount > 0 ? TestStatus::Passed : TestStatus::Failed,
          displayReady,
          presence.pca9685Detected,
          presence};
}

inline CanResponseKind classifyResponse(const CanRxFrame& frame) {
  switch (frame.lastCommand) {
    case 0x35:
    case 0x36:
    case 0x3A:
    case 0x3B:
      return CanResponseKind::Data;
    default:
      return frame.isExtended ? CanResponseKind::Normal : CanResponseKind::Unknown;
  }
}

inline CanDataKind classifyDataResponse(const CanRxFrame& frame) {
  switch (frame.lastCommand) {
    case 0x35:
      return CanDataKind::Velocity;
    case 0x36:
      return CanDataKind::Position;
    case 0x3A:
    case 0x3B:
      return CanDataKind::StatusFlags;
    default:
      return CanDataKind::Unknown;
  }
}

inline CanRxState defaultCanRxState() {
  CanRxState state{};
  state.hasNormalResponse = false;
  state.hasVelocity = false;
  state.hasPosition = false;
  state.hasStatusFlags = false;
  state.sawUnexpectedFrame = false;
  state.sawAnyFrame = false;
  state.busError = false;
  state.errorPassive = false;
  state.busOff = false;
  state.rxQueueFull = false;
  state.txFailed = false;
  state.velocityRpm = 0.0f;
  state.positionRaw = 0u;
  state.positionDegrees = 0.0f;
  state.statusFlags = 0u;
  state.lastIdentifier = 0u;
  state.lastDlc = 0u;
  state.lastCommand = 0u;
  state.lastIsExtended = false;
  state.lastIsRemoteRequest = false;
  for (uint8_t& byte : state.lastData) {
    byte = 0u;
  }
  state.normalResponseCount = 0u;
  state.dataResponseCount = 0u;
  state.unexpectedFrameCount = 0u;
  state.anyFrameCount = 0u;
  return state;
}

inline CanRxState handleNormalResponse(const CanRxState& current, const CanRxFrame& frame) {
  CanRxState next = current;
  next.hasNormalResponse = true;
  next.lastIdentifier = frame.lastIdentifier;
  next.lastDlc = frame.lastDlc;
  next.lastCommand = frame.lastCommand;
  next.normalResponseCount += 1;
  return next;
}

inline uint32_t combinePayloadWord(const CanRxFrame& frame) {
  uint32_t value = 0;
  for (uint8_t i = 1; i < frame.lastDlc && i < 5; ++i) {
    value = (value << 8) | frame.data[i];
  }
  return value;
}

inline CanRxState handleDataResponse(const CanRxState& current, const CanRxFrame& frame) {
  CanRxState next = current;
  next.lastIdentifier = frame.lastIdentifier;
  next.lastDlc = frame.lastDlc;
  next.lastCommand = frame.lastCommand;
  next.dataResponseCount += 1;

  switch (classifyDataResponse(frame)) {
    case CanDataKind::Velocity: {
      if (frame.lastDlc >= 4) {
        const uint16_t rawRpm = (static_cast<uint16_t>(frame.data[2]) << 8) | frame.data[3];
        next.velocityRpm = static_cast<float>(rawRpm);
        if (frame.data[1] != 0) {
          next.velocityRpm = -next.velocityRpm;
        }
        next.hasVelocity = true;
      }
      break;
    }
    case CanDataKind::Position: {
      if (frame.lastDlc >= 6) {
        next.positionRaw = (static_cast<uint32_t>(frame.data[2]) << 24) |
                           (static_cast<uint32_t>(frame.data[3]) << 16) |
                           (static_cast<uint32_t>(frame.data[4]) << 8) |
                           static_cast<uint32_t>(frame.data[5]);
        next.positionDegrees = static_cast<float>(next.positionRaw) * 360.0f / 65536.0f;
        if (frame.data[1] != 0) {
          next.positionDegrees = -next.positionDegrees;
        }
        next.hasPosition = true;
      }
      break;
    }
    case CanDataKind::StatusFlags: {
      next.statusFlags = combinePayloadWord(frame);
      next.hasStatusFlags = true;
      break;
    }
    case CanDataKind::Unknown:
    default:
      next.sawUnexpectedFrame = true;
      next.unexpectedFrameCount += 1;
      break;
  }

  return next;
}

inline bool canFeedbackOk(const CanRxState& state) {
  return state.hasVelocity || state.hasPosition || state.hasStatusFlags;
}

inline ServoTestResult summarizeServoTest(bool deviceReady, uint8_t completedSteps) {
  return {deviceReady ? TestStatus::Passed : TestStatus::Failed, deviceReady, completedSteps};
}

inline CanTestResult summarizeCanTest(bool controllerReady,
                                      bool modeCommandSent,
                                      bool enableCommandSent,
                                      bool velocityCommandSent,
                                      bool requestSent,
                                      bool stopSent,
                                      const CanRxState& feedback) {
  const bool transportOk =
      controllerReady && modeCommandSent && enableCommandSent && velocityCommandSent && requestSent && stopSent;
  return {transportOk && canFeedbackOk(feedback) ? TestStatus::Passed : TestStatus::Failed,
          controllerReady,
          modeCommandSent,
          enableCommandSent,
          velocityCommandSent,
          requestSent,
          stopSent,
          feedback};
}

inline AppSnapshot defaultAppSnapshot() {
  AppSnapshot snapshot{};
  snapshot.activeStage = TestStage::Boot;
  snapshot.i2cStatus = TestStatus::Pending;
  snapshot.servoStatus = TestStatus::Pending;
  snapshot.canStatus = TestStatus::Pending;
  snapshot.displayReady = false;
  snapshot.servoReady = false;
  snapshot.canReady = false;
  snapshot.oledAddress = 0;
  snapshot.devices = {0, 0, false};
  snapshot.canFeedback = defaultCanRxState();
  return snapshot;
}

inline void printHexByte(Print& log, uint8_t value) {
  if (value < 0x10) {
    log.print('0');
  }
  log.print(value, HEX);
}

inline void printHex32(Print& log, uint32_t value) {
  for (int shift = 28; shift >= 0; shift -= 4) {
    const uint8_t nibble = (value >> shift) & 0x0F;
    log.print(nibble < 10 ? char('0' + nibble) : char('A' + nibble - 10));
  }
}

inline void printCanFrameSummary(Print& log, const CanRxState& feedback) {
  log.print("Last RX frame: id=0x");
  printHex32(log, feedback.lastIdentifier);
  log.print(" dlc=");
  log.print(feedback.lastDlc);
  log.print(" cmd=0x");
  printHexByte(log, feedback.lastCommand);
  log.print(" ext=");
  log.print(feedback.lastIsExtended ? "1" : "0");
  log.print(" rtr=");
  log.println(feedback.lastIsRemoteRequest ? "1" : "0");

  log.print("Last RX data:");
  for (uint8_t i = 0; i < feedback.lastDlc && i < 8; ++i) {
    log.print(' ');
    printHexByte(log, feedback.lastData[i]);
  }
  log.println();
}

inline void printCanFeedback(Print& log, const CanRxState& feedback) {
  if (feedback.hasVelocity) {
    log.print("Motor velocity response: ");
    log.print(feedback.velocityRpm, 1);
    log.println(" RPM");
  }

  if (feedback.hasPosition) {
    log.print("Motor position response: ");
    log.print(feedback.positionDegrees, 2);
    log.println(" deg");
  }

  if (feedback.hasStatusFlags) {
    log.print("Motor status flags: 0x");
    log.println(feedback.statusFlags, HEX);
  }

  if (feedback.txFailed) {
    log.println("CAN TX failed: no ACK or frame not sent successfully");
  }
  if (feedback.busError) {
    log.println("CAN controller reported bus error");
  }
  if (feedback.errorPassive) {
    log.println("CAN controller entered error-passive state");
  }
  if (feedback.busOff) {
    log.println("CAN controller entered bus-off state");
  }
  if (feedback.rxQueueFull) {
    log.println("CAN RX queue full");
  }

  log.print("CAN frame counters: any=");
  log.print(feedback.anyFrameCount);
  log.print(", normal=");
  log.print(feedback.normalResponseCount);
  log.print(", data=");
  log.print(feedback.dataResponseCount);
  log.print(", unexpected=");
  log.println(feedback.unexpectedFrameCount);

  if (feedback.sawAnyFrame) {
    printCanFrameSummary(log, feedback);
  }

  if (!feedback.hasVelocity && !feedback.hasPosition && !feedback.hasStatusFlags && feedback.sawUnexpectedFrame) {
    log.println("RX frame was received but not classified as a valid motor feedback frame");
  } else if (!feedback.hasVelocity && !feedback.hasPosition && !feedback.hasStatusFlags && !feedback.sawAnyFrame) {
    log.println("No CAN frame received");
  } else if (!feedback.hasVelocity && !feedback.hasPosition && !feedback.hasStatusFlags) {
    log.println("No valid motor feedback frame");
  }
}

}  // namespace test_app
