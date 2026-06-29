#pragma once

#include <Arduino.h>

namespace auto_typer {

enum class MotorDirection : uint8_t {
  Cw = 0,
  Ccw = 1,
};

enum class ServoMotion : uint8_t {
  Neutral,
  Forward,
  Reverse,
};

enum class PressAction : uint8_t {
  Release,
  Press,
};

enum class TypingStepKind : uint8_t {
  Release,
  MoveTo,
  Press,
  CharacterRelease,
  LineFeed,
  Wait,
};

enum class PlanStatus : uint8_t {
  Ok,
  KeyNotFound,
  PlanFull,
};

enum class JobState : uint8_t {
  None,
  Queued,
  Running,
  Completed,
  Cancelled,
  Failed,
};

enum class DeviceMode : uint8_t {
  Idle,
  Running,
  Faulted,
  Debug,
};

struct MachinePointMm {
  float xMm;
  float yMm;
};

struct KeyBinding {
  char key;
  MachinePointMm point;
};

struct AxisTopology {
  uint8_t xMotorId;
  uint8_t yLeftMotorId;
  uint8_t yRightMotorId;
  uint8_t lineFeedMotorId;
};

struct MotionCalibration {
  float beltPitchMm;
  uint16_t pulleyTeeth;
  uint32_t stepsPerRev;
};

struct AxisMotionProfile {
  uint16_t rpm;
  uint8_t acceleration;
  uint16_t settleMs;
};

struct AxisConservativeReturnProfile {
  bool enabled;
  uint16_t rpm;
  uint8_t acceleration;
  uint32_t errorSteps;
  uint16_t settleMs;
};

struct LineFeedProfile {
  uint16_t rpm;
  uint8_t acceleration;
  uint32_t returnTotalSteps;
  MotorDirection returnDirection;
  uint32_t returnReleaseSteps;
  uint32_t characterReleaseSteps;
  MotorDirection releaseDirection;
  uint16_t settleMs;
  uint16_t characterReleaseSettleMs;
};

struct ServoPressSemantics {
  ServoMotion releaseMotion;
  ServoMotion pressMotion;
};

struct ServoActuatorConfig {
  uint8_t address;
  uint8_t channels[2];
  uint8_t channelCount;
  float pwmFrequencyHz;
  uint32_t oscillatorHz;
  uint16_t neutralPulseUs;
  uint16_t forwardPulseUs;
  uint16_t reversePulseUs;
  uint16_t releaseMs;
  uint16_t pressMs;
  uint16_t settleMs;
  ServoPressSemantics semantics;
};

struct CanBusConfig {
  gpio_num_t txPin;
  gpio_num_t rxPin;
  uint32_t bitrate;
};

struct OledConfig {
  uint8_t primaryAddress;
  uint8_t fallbackAddress;
  uint16_t width;
  uint16_t height;
  uint32_t preclkHz;
  uint32_t postclkHz;
};

struct TypingConfig {
  uint32_t serialBaudrate;
  const char* wifiSsid;
  const char* wifiPassword;
  const char* deviceId;
  const char* firmwareVersion;
  CanBusConfig canBus;
  OledConfig oled;
  ServoActuatorConfig servo;
  AxisTopology topology;
  MotionCalibration calibration;
  AxisMotionProfile xProfile;
  AxisMotionProfile yProfile;
  AxisConservativeReturnProfile xReturn;
  AxisConservativeReturnProfile yReturn;
  LineFeedProfile lineFeed;
  MachinePointMm homePoint;
  const char* sampleText;
};

struct AxisDeltaSteps {
  MotorDirection direction;
  uint32_t steps;
};

struct MovePlan {
  AxisDeltaSteps x;
  AxisDeltaSteps y;
  MachinePointMm target;
};

struct TypingStep {
  TypingStepKind kind;
  MachinePointMm point;
  uint16_t waitMs;
};

struct TypingPlan {
  PlanStatus status;
  char failedKey;
  size_t count;
  TypingStep steps[256];
};

struct JobSnapshot {
  JobState state;
  uint32_t jobId;
  size_t textLength;
  size_t currentIndex;
  size_t currentStep;
  size_t totalSteps;
  MachinePointMm currentPoint;
  PlanStatus planStatus;
  char failedKey;
};

}  // namespace auto_typer
