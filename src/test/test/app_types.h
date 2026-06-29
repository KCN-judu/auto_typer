#pragma once

#include <Arduino.h>

namespace test_app {

enum class ServoMotion : uint8_t {
  Neutral,
  Forward,
  Reverse,
};

enum class MotorDirection : uint8_t {
  Cw = 0,
  Ccw = 1,
};

enum class TestStage : uint8_t {
  Boot,
  I2c,
  Servo,
  Can,
  Idle,
};

enum class TestStatus : uint8_t {
  Pending,
  Passed,
  Failed,
  Skipped,
};

struct DisplayFrame {
  const char* line0;
  const char* line1;
  const char* line2;
  const char* line3;
};

struct DevicePresence {
  uint8_t oledAddress;
  uint8_t deviceCount;
  bool pca9685Detected;
};

struct ServoStep {
  ServoMotion motion;
  uint16_t dwellMs;
  const char* logLine;
};

struct ServoTestResult {
  TestStatus status;
  bool deviceReady;
  uint8_t completedSteps;
};

struct CanCommandPlan {
  MotorDirection direction;
  uint16_t rpm;
  uint8_t acceleration;
};

enum class CanResponseKind : uint8_t {
  Normal,
  Data,
  Unknown,
};

enum class CanDataKind : uint8_t {
  Velocity,
  Position,
  StatusFlags,
  Unknown,
};

struct CanRxFrame {
  uint32_t lastIdentifier;
  uint8_t lastDlc;
  uint8_t lastCommand;
  bool isExtended;
  bool isRemoteRequest;
  uint8_t data[8];
};

struct CanRxState {
  bool hasNormalResponse;
  bool hasVelocity;
  bool hasPosition;
  bool hasStatusFlags;
  bool sawUnexpectedFrame;
  bool sawAnyFrame;
  bool busError;
  bool errorPassive;
  bool busOff;
  bool rxQueueFull;
  bool txFailed;
  float velocityRpm;
  uint32_t positionRaw;
  float positionDegrees;
  uint32_t statusFlags;
  uint32_t lastIdentifier;
  uint8_t lastDlc;
  uint8_t lastCommand;
  bool lastIsExtended;
  bool lastIsRemoteRequest;
  uint8_t lastData[8];
  uint32_t normalResponseCount;
  uint32_t dataResponseCount;
  uint32_t unexpectedFrameCount;
  uint32_t anyFrameCount;
};

struct CanTestResult {
  TestStatus status;
  bool controllerReady;
  bool modeCommandSent;
  bool enableCommandSent;
  bool velocityCommandSent;
  bool requestSent;
  bool stopSent;
  CanRxState feedback;
};

struct I2cRefreshResult {
  TestStatus status;
  bool displayReady;
  bool servoDetected;
  DevicePresence presence;
};

struct TestLoopSchedule {
  uint32_t i2cIntervalMs;
  uint32_t servoIntervalMs;
  uint32_t canIntervalMs;
};

struct AppSnapshot {
  TestStage activeStage;
  TestStatus i2cStatus;
  TestStatus servoStatus;
  TestStatus canStatus;
  bool displayReady;
  bool servoReady;
  bool canReady;
  uint8_t oledAddress;
  DevicePresence devices;
  CanRxState canFeedback;
};

}  // namespace test_app
