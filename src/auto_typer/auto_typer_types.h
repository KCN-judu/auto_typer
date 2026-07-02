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
  DeviceFault,
  DeviceBusy,
  DeviceNotReady,
};

enum class JobState : uint8_t {
  None,
  Queued,
  Planning,
  Running,
  Cancelling,
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

enum class MotorRole : uint8_t {
  X,
  YLeft,
  YRight,
  LineFeed,
  Press,
};

enum class MotorReadiness : uint8_t {
  Unknown,
  ConfigPending,
  ConfigSent,
  Acked,
  Ready,
  Offline,
  ConditionNotMet,
  Faulted,
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
  uint8_t pressMotorId;
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

struct MotionRuntimeConfig {
  uint8_t yPairVirtualMotorId;
  uint16_t defaultMoveRpm;
  uint8_t defaultAccelerationPercent;
  uint8_t defaultAccelerationRaw;
  uint32_t positionToleranceSteps;
  uint32_t ySkewToleranceSteps;
  float idleVelocityThresholdRpm;
  uint16_t motionPollIntervalMs;
  uint32_t motionTimeoutMs;
  uint8_t completionSamples;
  uint16_t minimumCoordinatedRpm;
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
  MotionRuntimeConfig motionRuntime;
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

enum class MotionBlockKind : uint8_t {
  MoveXY,
  LineFeed,
  CharacterRelease,
  ServoPress,
  ServoRelease,
  Wait,
};

struct MotorTargetSteps {
  int32_t x;
  int32_t yLeft;
  int32_t yRight;
  int32_t lineFeed;
};

struct MotionProfile {
  uint16_t maxRpm;
  uint8_t acceleration;
  uint16_t settleMs;
  uint32_t timeoutMs;
};

struct MotionBlock {
  MotionBlockKind kind;
  MachinePointMm targetMm;
  MotorTargetSteps deltaSteps;
  MotionProfile profile;
  uint16_t waitMs;
};

enum class RemoteMotionBlockKind : uint8_t {
  MoveXY,
  ServoPress,
  ServoRelease,
  CharacterRelease,
  LineFeed,
  Wait,
};

struct RemoteMotionProfile {
  bool hasRpm;
  uint16_t rpm;
  bool hasAccelRaw;
  uint8_t accelRaw;
  bool hasTimeoutMs;
  uint32_t timeoutMs;
};

struct RemoteMotionBlock {
  RemoteMotionBlockKind kind;
  float dxMm;
  float dyMm;
  uint16_t durationMs;
  RemoteMotionProfile profile;
};

struct SubmitRemoteBlockResult {
  bool accepted;
  const char* rejectionCode;
  const char* rejectionMessage;
};

struct MotorState {
  uint8_t id;
  MotorRole role;
  MotorReadiness readiness;
  bool hasVelocity;
  bool hasRealtimeAngle;
  bool hasInputPulse;
  bool hasStatus;
  bool hasRecentStatus;
  bool hasRecentInputPulse;
  bool hasRecentVelocity;
  float velocityRpm;
  int32_t realtimeAngleRaw65536;
  int32_t inputPulseSteps;
  uint32_t statusFlags;
  bool driverFault;
  bool conditionNotMet;
  bool commandMalformed;
  uint8_t lastAckCommand;
  uint8_t lastConditionNotMetCommand;
  uint8_t lastMalformedCommand;
  uint32_t lastAckMs;
  uint32_t lastConditionNotMetMs;
  uint32_t lastMalformedMs;
  bool motionReached;
  uint32_t lastMotionReachedMs;
  uint32_t lastVelocityMs;
  uint32_t lastRealtimeAngleMs;
  uint32_t lastInputPulseMs;
  uint32_t lastStatusMs;
  uint32_t lastAnyFrameMs;
  uint32_t lastProbeMs;
  const char* lastErrorCode;
  const char* lastErrorMessage;
};

struct SubmitJobResult {
  bool accepted;
  const char* rejectionCode;
  const char* rejectionMessage;
  PlanStatus planStatus;
  uint32_t jobId;
  size_t stepCount;
  char failedKey;
};

struct JobSnapshot {
  JobState state;
  uint32_t jobId;
  size_t textLength;
  size_t currentIndex;
  size_t currentStep;
  size_t totalSteps;
  size_t currentBlock;
  size_t totalBlocks;
  MachinePointMm currentPoint;
  PlanStatus planStatus;
  char failedKey;
  const char* faultCode;
  const char* faultMessage;
};

}  // namespace auto_typer
