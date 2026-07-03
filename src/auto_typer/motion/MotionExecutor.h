#pragma once

#include "../can/CanRxTask.h"
#include "../can/ProtocolTrace.h"
#include "../drivers/EmmV5Driver.h"
#include "MachineKinematics.h"
#include "MotionPlanner.h"
#include "YPairController.h"

#include <math.h>

namespace auto_typer {

class MotionExecutor {
 public:
  MotionExecutor(const TypingConfig& config,
                 EmmV5Driver& driver,
                 MotorFeedbackStore& feedback,
                 ProtocolTrace& trace)
      : config_(config),
        driver_(driver),
        feedback_(feedback),
        trace_(trace),
        yPair_(config, driver),
        state_(State::Idle),
        phase_(Phase::Idle),
        steps_(nullptr),
        stepCount_(0),
        currentStep_(0),
        stepStartedAtMs_(0),
        commandIssueMs_(0),
        settleStartedAtMs_(0),
        faultCode_(""),
        faultMessage_(""),
        currentPoint_(config.homePoint),
        activeTextIndex_(0),
        context_{},
        activeSupervision_{},
        completionSampleCount_(0),
        lastFeedbackPollMs_(0),
        currentStepStartedEvent_(false),
        lastCompletedStep_(0),
        lastCompletedDurationMs_(0) {}

  bool start(const MotionStep* steps,
             size_t count,
             MachinePointMm startPoint = {NAN, NAN},
             const char* groupId = "",
             uint32_t seq = 0) {
    if (state_ == State::Running || state_ == State::Cancelling) {
      return false;
    }
    steps_ = steps;
    stepCount_ = count;
    currentStep_ = 0;
    activeTextIndex_ = 0;
    if (isfinite(startPoint.xMm) && isfinite(startPoint.yMm)) {
      currentPoint_ = startPoint;
    } else {
      currentPoint_ = config_.homePoint;
    }
    faultCode_ = "";
    faultMessage_ = "";
    stepStartedAtMs_ = 0;
    commandIssueMs_ = 0;
    settleStartedAtMs_ = 0;
    completionSampleCount_ = 0;
    lastFeedbackPollMs_ = 0;
    activeSupervision_ = {};
    copyString(context_.groupId, sizeof(context_.groupId), groupId);
    context_.seq = seq;
    currentStepStartedEvent_ = false;
    lastCompletedStep_ = 0;
    lastCompletedDurationMs_ = 0;
    state_ = count == 0 ? State::Completed : State::Running;
    phase_ = count == 0 ? Phase::Idle : Phase::PreflightBaseline;
    trace_.clearMotionContext();
    return true;
  }

  void tick() {
    if (state_ != State::Running) {
      return;
    }
    if (currentStep_ >= stepCount_) {
      state_ = State::Completed;
      phase_ = Phase::Idle;
      trace_.clearMotionContext();
      return;
    }
    const MotionStep& step = steps_[currentStep_];
    if (stepStartedAtMs_ == 0) {
      stepStartedAtMs_ = millis();
      commandIssueMs_ = 0;
      lastFeedbackPollMs_ = 0;
      settleStartedAtMs_ = 0;
      completionSampleCount_ = 0;
      currentStepStartedEvent_ = true;
      phase_ = Phase::PreflightBaseline;
      trace_.setMotionContext(context_.groupId, context_.seq, currentStep_, motionStepKindName(step.kind));
    }

    if (phase_ == Phase::PreflightBaseline && !preflightBaseline(step)) {
      return;
    }
    if (phase_ == Phase::IssueCommand && !issueStepCommand(step)) {
      return;
    }
    if (phase_ == Phase::CommandAck && !waitForCommandAck(step)) {
      return;
    }
    if (phase_ == Phase::TargetWait && isStepComplete(step)) {
      const size_t completedStep = currentStep_;
      const uint32_t completedDurationMs = millis() - stepStartedAtMs_;
      finishStep(step);
      ++currentStep_;
      lastCompletedStep_ = completedStep + 1;
      lastCompletedDurationMs_ = completedDurationMs;
      stepStartedAtMs_ = 0;
      commandIssueMs_ = 0;
      settleStartedAtMs_ = 0;
      currentStepStartedEvent_ = false;
      phase_ = Phase::PreflightBaseline;
      trace_.clearMotionContext();
    }
  }

  void cancel() {
    if (state_ != State::Running) {
      return;
    }
    state_ = State::Cancelling;
    stopAll();
    state_ = State::Cancelled;
  }

  void emergencyStop() {
    stopAll();
    fail("emergency_stop", "Emergency stop requested");
  }

  void resetFault() {
    if (state_ == State::Faulted) {
      faultCode_ = "";
      faultMessage_ = "";
      state_ = State::Idle;
      phase_ = Phase::Idle;
      trace_.clearMotionContext();
    }
  }

  void stopAll() {
    driver_.stopNow(config_.topology.xMotorId);
    driver_.stopNow(config_.topology.yLeftMotorId);
    driver_.stopNow(config_.topology.yRightMotorId);
    driver_.stopNow(config_.topology.lineFeedMotorId);
    driver_.stopNow(config_.topology.pressMotorId);
  }

  bool idle() const {
    return state_ == State::Idle || state_ == State::Completed || state_ == State::Cancelled;
  }

  bool running() const {
    return state_ == State::Running || state_ == State::Cancelling;
  }

  bool completed() const {
    return state_ == State::Completed;
  }

  bool cancelled() const {
    return state_ == State::Cancelled;
  }

  bool faulted() const {
    return state_ == State::Faulted;
  }

  size_t currentStep() const {
    return currentStep_;
  }

  size_t totalSteps() const {
    return stepCount_;
  }

  size_t activeTextIndex() const {
    return activeTextIndex_;
  }

  MachinePointMm currentPoint() const {
    return currentPoint_;
  }

  bool stepStartedEvent() const {
    return currentStepStartedEvent_;
  }

  size_t lastCompletedStep() const {
    return lastCompletedStep_;
  }

  uint32_t lastCompletedDurationMs() const {
    return lastCompletedDurationMs_;
  }

  const char* faultCode() const {
    return faultCode_;
  }

  const char* faultMessage() const {
    return faultMessage_;
  }

  MotorState motorState(uint8_t motorId) const {
    return feedback_.get(motorId);
  }

 private:
  struct MotorSupervisionState {
    bool active;
    uint8_t motorId;
    int32_t deltaSteps;
    bool baselineKnown;
    int32_t initialPulse;
    int32_t targetPulse;
    bool ackSeenForCurrentBlock;
    bool reachedSeenForCurrentBlock;
    bool velocitySeen;
    bool velocityEverNonZero;
    bool pulseReferenceKnown;
    uint32_t startedAtMs;
    uint32_t commandIssueMs;
    uint32_t firstFeedbackMs;
  };

  struct ActiveSupervision {
    MotorSupervisionState x;
    MotorSupervisionState yLeft;
    MotorSupervisionState yRight;
    MotorSupervisionState lineFeed;
    MotorSupervisionState press;
  };

  enum class State : uint8_t {
    Idle,
    Running,
    Cancelling,
    Completed,
    Cancelled,
    Faulted,
  };

  enum class Phase : uint8_t {
    Idle,
    PreflightBaseline,
    IssueCommand,
    CommandAck,
    TargetWait,
  };

  struct ExecutionContext {
    char groupId[49];
    uint32_t seq;
  };

  bool beginStep(const MotionStep& step) {
    switch (step.kind) {
      case MotionStepKind::PressDown:
      case MotionStepKind::PressUp:
        return startPressMotor(step);
      case MotionStepKind::Wait:
        return true;
      case MotionStepKind::CharacterRelease:
        return startLineFeed(step);
      case MotionStepKind::LineFeed:
        return startLineFeed(step);
      case MotionStepKind::MoveXY:
        return startMoveXY(step);
      case MotionStepKind::ReturnZero:
        return startReturnZero(step);
    }
    return false;
  }

  bool preflightBaseline(const MotionStep& step) {
    if (step.kind == MotionStepKind::Wait || !stepHasActiveMotion(step)) {
      phase_ = Phase::TargetWait;
      return true;
    }
    const uint32_t nowMs = millis();
    if (baselineReady(step, nowMs)) {
      phase_ = Phase::IssueCommand;
      return true;
    }
    requestFeedback(step);
    if (nowMs - stepStartedAtMs_ > kBaselinePreflightTimeoutMs) {
      stopAll();
      fail("motion_feedback_timeout", "Motion feedback baseline timed out");
      return false;
    }
    return false;
  }

  bool issueStepCommand(const MotionStep& step) {
    commandIssueMs_ = millis();
    captureSupervisionState(step, commandIssueMs_);
    if (!beginStep(step)) {
      if (state_ != State::Faulted) {
        fail("motion_command_rejected", "Motion command rejected");
      }
      return false;
    }
    phase_ = activeCommandCount() == 0 ? Phase::TargetWait : Phase::CommandAck;
    return true;
  }

  bool waitForCommandAck(const MotionStep& step) {
    (void)step;
    const uint32_t nowMs = millis();
    updateActiveSupervision(nowMs);
    if (activeCommandResponseFault()) {
      return false;
    }
    if (allActiveCommandsAcked()) {
      lastFeedbackPollMs_ = 0;
      phase_ = Phase::TargetWait;
      return true;
    }
    if (nowMs - commandIssueMs_ > config_.motionRuntime.motionCommandAckTimeoutMs) {
      logMotionCommandNoAckDiagnostics(step, nowMs - commandIssueMs_);
      stopAll();
      fail("motion_command_no_ack", "Motor did not ACK motion command");
      return false;
    }
    return false;
  }

  bool startMoveXY(const MotionStep& step) {
    const uint32_t xSteps = absoluteSteps(step.deltaSteps.x);
    const uint32_t ySteps = absoluteSteps(step.deltaSteps.yLeft);
    const bool hasX = xSteps > 0;
    const bool hasY = ySteps > 0;
    if (!hasX && !hasY) {
      return true;
    }
    const uint32_t primarySteps = xSteps > ySteps ? xSteps : ySteps;
    const uint16_t xRpm = scaledCoordinatedRpm(xSteps,
                                               primarySteps,
                                               step.profile.maxRpm,
                                               config_.motionRuntime.minimumCoordinatedRpm);
    const uint16_t yRpm = scaledCoordinatedRpm(ySteps,
                                               primarySteps,
                                               step.profile.maxRpm,
                                               config_.motionRuntime.minimumCoordinatedRpm);
    if (hasX && hasY) {
      const MotorDirection yLeftDirection = directionForSignedSteps(step.deltaSteps.yLeft);
      const MotorDirection yRightDirection = yLeftDirection == MotorDirection::Cw ? MotorDirection::Ccw
                                                                                  : MotorDirection::Cw;
      const EmmV5Driver::MoveRelativeCommand commands[] = {
        {config_.topology.xMotorId,
         directionForSignedSteps(step.deltaSteps.x),
         xRpm,
         step.profile.acceleration,
         xSteps,
         true},
        {config_.topology.yLeftMotorId, yLeftDirection, yRpm, step.profile.acceleration, ySteps, true},
        {config_.topology.yRightMotorId, yRightDirection, yRpm, step.profile.acceleration, ySteps, true},
      };
      const size_t queueBefore = driver_.availableForWrite();
      const bool ok = driver_.moveRelativeBatch(commands, sizeof(commands) / sizeof(commands[0]), true, true);
      logMoveCommandBatchResult(step, commands, sizeof(commands) / sizeof(commands[0]), queueBefore, true, ok);
      return ok;
    }
    if (hasX) {
      const MotorDirection direction = directionForSignedSteps(step.deltaSteps.x);
      const size_t queueBefore = driver_.availableForWrite();
      const bool ok = driver_.moveRelative(config_.topology.xMotorId,
                                           direction,
                                           xRpm,
                                           step.profile.acceleration,
                                           xSteps,
                                           false,
                                           true);
      logMoveCommandIssueResult(step,
                                config_.topology.xMotorId,
                                step.deltaSteps.x,
                                direction,
                                xRpm,
                                step.profile.acceleration,
                                false,
                                queueBefore,
                                ok,
                                EmmV5Driver::moveRelativeFrameCount());
      if (!ok) {
        return false;
      }
    }
    if (hasY) {
      const MotorDirection yLeftDirection = directionForSignedSteps(step.deltaSteps.yLeft);
      const MotorDirection yRightDirection = yLeftDirection == MotorDirection::Cw ? MotorDirection::Ccw
                                                                                  : MotorDirection::Cw;
      const EmmV5Driver::MoveRelativeCommand commands[] = {
        {config_.topology.yLeftMotorId, yLeftDirection, yRpm, step.profile.acceleration, ySteps, true},
        {config_.topology.yRightMotorId, yRightDirection, yRpm, step.profile.acceleration, ySteps, true},
      };
      const size_t queueBefore = driver_.availableForWrite();
      const bool ok = driver_.moveRelativeBatch(commands, sizeof(commands) / sizeof(commands[0]), true, true);
      logMoveCommandBatchResult(step, commands, sizeof(commands) / sizeof(commands[0]), queueBefore, true, ok);
      if (!ok) {
        return false;
      }
    }
    return true;
  }

  bool startLineFeed(const MotionStep& step) {
    const uint32_t steps = absoluteSteps(step.deltaSteps.lineFeed);
    if (steps == 0) {
      return true;
    }
    const MotorDirection direction = directionForSignedSteps(step.deltaSteps.lineFeed);
    const size_t queueBefore = driver_.availableForWrite();
    const bool ok = driver_.moveRelative(config_.topology.lineFeedMotorId,
                                         direction,
                                         step.profile.maxRpm,
                                         step.profile.acceleration,
                                         steps,
                                         false,
                                         true);
    logMoveCommandIssueResult(step,
                              config_.topology.lineFeedMotorId,
                              step.deltaSteps.lineFeed,
                              direction,
                              step.profile.maxRpm,
                              step.profile.acceleration,
                              false,
                              queueBefore,
                              ok,
                              EmmV5Driver::moveRelativeFrameCount());
    return ok;
  }

  bool startPressMotor(const MotionStep& step) {
    const int32_t signedSteps = step.deltaSteps.press;
    const uint32_t steps = absoluteSteps(signedSteps);
    if (steps == 0) {
      return true;
    }
    const MotorDirection direction = directionForSignedSteps(signedSteps);
    const size_t queueBefore = driver_.availableForWrite();
    const bool ok = driver_.moveRelative(config_.topology.pressMotorId,
                                         direction,
                                         step.profile.maxRpm,
                                         step.profile.acceleration,
                                         steps,
                                         false,
                                         true);
    logMoveCommandIssueResult(step,
                              config_.topology.pressMotorId,
                              signedSteps,
                              direction,
                              step.profile.maxRpm,
                              step.profile.acceleration,
                              false,
                              queueBefore,
                              ok,
                              EmmV5Driver::moveRelativeFrameCount());
    return ok;
  }

  bool startReturnZero(const MotionStep& step) {
    EmmV5Driver::MoveRelativeCommand commands[3]{};
    size_t count = 0;
    appendReturnZeroCommand(commands, count, activeSupervision_.x, step, true);
    appendReturnZeroCommand(commands, count, activeSupervision_.yLeft, step, true);
    appendReturnZeroCommand(commands, count, activeSupervision_.yRight, step, true);
    if (count > 0) {
      const size_t queueBefore = driver_.availableForWrite();
      const bool ok = driver_.moveRelativeBatch(commands, count, true, true);
      logMoveCommandBatchResult(step, commands, count, queueBefore, true, ok);
      if (!ok) {
        return false;
      }
    }
    if (activeSupervision_.press.active) {
      const MotorDirection direction = directionForSignedSteps(activeSupervision_.press.deltaSteps);
      const size_t queueBefore = driver_.availableForWrite();
      const bool ok = driver_.moveRelative(config_.topology.pressMotorId,
                                           direction,
                                           step.profile.maxRpm,
                                           step.profile.acceleration,
                                           absoluteSteps(activeSupervision_.press.deltaSteps),
                                           false,
                                           true);
      logMoveCommandIssueResult(step,
                                config_.topology.pressMotorId,
                                activeSupervision_.press.deltaSteps,
                                direction,
                                step.profile.maxRpm,
                                step.profile.acceleration,
                                false,
                                queueBefore,
                                ok,
                                EmmV5Driver::moveRelativeFrameCount());
      return ok;
    }
    return true;
  }

  void appendReturnZeroCommand(EmmV5Driver::MoveRelativeCommand* commands,
                               size_t& count,
                               const MotorSupervisionState& supervision,
                               const MotionStep& step,
                               bool sync) const {
    if (!supervision.active || count >= 3) {
      return;
    }
    commands[count] = {
      supervision.motorId,
      directionForSignedSteps(supervision.deltaSteps),
      step.profile.maxRpm,
      step.profile.acceleration,
      absoluteSteps(supervision.deltaSteps),
      sync,
    };
    ++count;
  }

  bool isStepComplete(const MotionStep& step) {
    const uint32_t elapsed = millis() - stepStartedAtMs_;
    if (elapsed > step.profile.timeoutMs) {
      logMotionTimeoutDiagnostics(step, elapsed);
      stopAll();
      fail(motionTargetTimeoutCode(), motionTargetTimeoutMessage());
      return false;
    }
    if (step.kind == MotionStepKind::Wait) {
      return elapsed >= step.waitMs;
    }

    requestFeedback(step);
    updateActiveSupervision(millis());
    if (activeCommandResponseFault()) {
      return false;
    }
    if (activeAckedButNotMoving(millis())) {
      logMotionNoMovementDiagnostics(step, millis() - commandIssueMs_);
      stopAll();
      fail("motion_no_movement", "Motor ACKed motion command but did not move");
      return false;
    }
    if (feedbackSatisfied(step)) {
      ++completionSampleCount_;
      if (completionSampleCount_ >= config_.motionRuntime.completionSamples) {
        if (settleStartedAtMs_ == 0) {
          settleStartedAtMs_ = millis();
        }
        return millis() - settleStartedAtMs_ >= step.profile.settleMs;
      }
      return false;
    }
    completionSampleCount_ = 0;
    settleStartedAtMs_ = 0;
    return false;
  }

  void requestFeedback(const MotionStep& step) {
    const uint32_t nowMs = millis();
    if (lastFeedbackPollMs_ != 0 && nowMs - lastFeedbackPollMs_ < config_.motionRuntime.motionPollIntervalMs) {
      return;
    }
    lastFeedbackPollMs_ = nowMs;
    if (step.kind == MotionStepKind::MoveXY) {
      if (step.deltaSteps.x != 0) {
        driver_.requestInputPulseCount(config_.topology.xMotorId);
        driver_.requestVelocity(config_.topology.xMotorId);
      }
      if (step.deltaSteps.yLeft != 0) {
        driver_.requestInputPulseCount(config_.topology.yLeftMotorId);
        driver_.requestVelocity(config_.topology.yLeftMotorId);
        driver_.requestInputPulseCount(config_.topology.yRightMotorId);
        driver_.requestVelocity(config_.topology.yRightMotorId);
      }
    }
    if (step.kind == MotionStepKind::LineFeed || step.kind == MotionStepKind::CharacterRelease) {
      driver_.requestInputPulseCount(config_.topology.lineFeedMotorId);
      driver_.requestVelocity(config_.topology.lineFeedMotorId);
    }
    if (step.kind == MotionStepKind::PressDown || step.kind == MotionStepKind::PressUp) {
      driver_.requestInputPulseCount(config_.topology.pressMotorId);
      driver_.requestVelocity(config_.topology.pressMotorId);
    }
    if (step.kind == MotionStepKind::ReturnZero) {
      requestReturnZeroFeedback();
    }
  }

  bool feedbackSatisfied(const MotionStep& step) {
    const uint32_t nowMs = millis();
    const uint32_t elapsed = nowMs - stepStartedAtMs_;
    updateActiveSupervision(nowMs);
    if (step.kind == MotionStepKind::MoveXY) {
      if (step.deltaSteps.x != 0 && commandResponseFault(activeSupervision_.x)) {
        return false;
      }
      if (step.deltaSteps.x != 0 && !motorSupervisionSatisfied(activeSupervision_.x, nowMs, elapsed, step)) {
        return false;
      }
      if (step.deltaSteps.yLeft != 0) {
        if (commandResponseFault(activeSupervision_.yLeft) || commandResponseFault(activeSupervision_.yRight)) {
          return false;
        }
        if (yPairSkewCheckReady(nowMs) && yPairSkewExceeded(nowMs)) {
          stopAll();
          fail("y_pair_skew", "Y pair skew exceeded tolerance");
          return false;
        }
        if (!motorSupervisionSatisfied(activeSupervision_.yLeft, nowMs, elapsed, step) ||
            !motorSupervisionSatisfied(activeSupervision_.yRight, nowMs, elapsed, step)) {
          return false;
        }
      }
      return true;
    }
    if (step.kind == MotionStepKind::LineFeed || step.kind == MotionStepKind::CharacterRelease) {
      if (commandResponseFault(activeSupervision_.lineFeed)) {
        return false;
      }
      return motorSupervisionSatisfied(activeSupervision_.lineFeed, nowMs, elapsed, step);
    }
    if (step.kind == MotionStepKind::PressDown || step.kind == MotionStepKind::PressUp) {
      if (commandResponseFault(activeSupervision_.press)) {
        return false;
      }
      return motorSupervisionSatisfied(activeSupervision_.press, nowMs, elapsed, step);
    }
    if (step.kind == MotionStepKind::ReturnZero) {
      if (commandResponseFault(activeSupervision_.x) || commandResponseFault(activeSupervision_.yLeft) ||
          commandResponseFault(activeSupervision_.yRight) || commandResponseFault(activeSupervision_.press)) {
        return false;
      }
      return motorSupervisionSatisfied(activeSupervision_.x, nowMs, elapsed, step) &&
             motorSupervisionSatisfied(activeSupervision_.yLeft, nowMs, elapsed, step) &&
             motorSupervisionSatisfied(activeSupervision_.yRight, nowMs, elapsed, step) &&
             motorSupervisionSatisfied(activeSupervision_.press, nowMs, elapsed, step);
    }
    return true;
  }

  void finishStep(const MotionStep& step) {
    if (step.kind == MotionStepKind::MoveXY) {
      currentPoint_ = step.targetMm;
    }
    if (step.kind == MotionStepKind::LineFeed) {
      currentPoint_.xMm = config_.homePoint.xMm;
    }
    if (step.kind == MotionStepKind::ReturnZero) {
      currentPoint_ = config_.homePoint;
    }
    if (step.kind == MotionStepKind::PressDown) {
      ++activeTextIndex_;
    }
  }

  void fail(const char* code, const char* message) {
    faultCode_ = code;
    faultMessage_ = message;
    state_ = State::Faulted;
    phase_ = Phase::Idle;
    trace_.clearMotionContext();
  }

  void logMotionTimeoutDiagnostics(const MotionStep& step, uint32_t elapsedMs) {
    const uint32_t nowMs = millis();
    updateActiveSupervision(nowMs);
    Serial.print("[motion_timeout] blockIndex=");
    Serial.print(currentStep_);
    Serial.print(" blockKind=");
    Serial.print(motionStepKindName(step.kind));
    Serial.print(" elapsedMs=");
    Serial.print(elapsedMs);
    Serial.print(" timeoutMs=");
    Serial.print(step.profile.timeoutMs);
    Serial.print(" dxSteps=");
    Serial.print(step.deltaSteps.x);
    Serial.print(" dySteps=");
    Serial.print(step.deltaSteps.yRight);
    Serial.print(" pressSteps=");
    Serial.print(step.deltaSteps.press);
    Serial.print(" lineFeedSteps=");
    Serial.print(step.deltaSteps.lineFeed);
    Serial.print(" completionSampleCount=");
    Serial.print(completionSampleCount_);
    Serial.print(" settleStartedAtMs=");
    Serial.print(settleStartedAtMs_);
    Serial.println();

    logMotorTimeoutDiagnostics("x", activeSupervision_.x, nowMs);
    logMotorTimeoutDiagnostics("y_left", activeSupervision_.yLeft, nowMs);
    logMotorTimeoutDiagnostics("y_right", activeSupervision_.yRight, nowMs);
    logMotorTimeoutDiagnostics("line_feed", activeSupervision_.lineFeed, nowMs);
    logMotorTimeoutDiagnostics("press", activeSupervision_.press, nowMs);
  }

  void logMotionCommandNoAckDiagnostics(const MotionStep& step, uint32_t elapsedMs) {
    const uint32_t nowMs = millis();
    updateActiveSupervision(nowMs);
    Serial.print("[motion_command_no_ack] groupId=");
    Serial.print(context_.groupId);
    Serial.print(" seq=");
    Serial.print(context_.seq);
    Serial.print(" blockIndex=");
    Serial.print(currentStep_);
    Serial.print(" blockKind=");
    Serial.print(motionStepKindName(step.kind));
    Serial.print(" elapsedMs=");
    Serial.print(elapsedMs);
    Serial.print(" ackTimeoutMs=");
    Serial.print(config_.motionRuntime.motionCommandAckTimeoutMs);
    Serial.print(" commandIssueMs=");
    Serial.print(commandIssueMs_);
    Serial.println();
    logMotorTimeoutDiagnostics("x", activeSupervision_.x, nowMs);
    logMotorTimeoutDiagnostics("y_left", activeSupervision_.yLeft, nowMs);
    logMotorTimeoutDiagnostics("y_right", activeSupervision_.yRight, nowMs);
    logMotorTimeoutDiagnostics("line_feed", activeSupervision_.lineFeed, nowMs);
    logMotorTimeoutDiagnostics("press", activeSupervision_.press, nowMs);
  }

  void logMotionNoMovementDiagnostics(const MotionStep& step, uint32_t elapsedMs) {
    const uint32_t nowMs = millis();
    updateActiveSupervision(nowMs);
    Serial.print("[motion_no_movement] groupId=");
    Serial.print(context_.groupId);
    Serial.print(" seq=");
    Serial.print(context_.seq);
    Serial.print(" blockIndex=");
    Serial.print(currentStep_);
    Serial.print(" blockKind=");
    Serial.print(motionStepKindName(step.kind));
    Serial.print(" elapsedMs=");
    Serial.print(elapsedMs);
    Serial.print(" noMovementTimeoutMs=");
    Serial.print(config_.motionRuntime.motionNoMovementTimeoutMs);
    Serial.println();
    logMotorTimeoutDiagnostics("x", activeSupervision_.x, nowMs);
    logMotorTimeoutDiagnostics("y_left", activeSupervision_.yLeft, nowMs);
    logMotorTimeoutDiagnostics("y_right", activeSupervision_.yRight, nowMs);
    logMotorTimeoutDiagnostics("line_feed", activeSupervision_.lineFeed, nowMs);
    logMotorTimeoutDiagnostics("press", activeSupervision_.press, nowMs);
  }

  void logMoveCommandBatchResult(const MotionStep& step,
                                 const EmmV5Driver::MoveRelativeCommand* commands,
                                 size_t count,
                                 size_t queueBefore,
                                 bool triggerBroadcast,
                                 bool enqueueResult) const {
    const size_t frames = count * EmmV5Driver::moveRelativeFrameCount() +
                          (triggerBroadcast ? EmmV5Driver::triggerBroadcastFrameCount() : 0);
    for (size_t i = 0; i < count; ++i) {
      const int32_t signedSteps = commands[i].direction == MotorDirection::Cw
                                      ? static_cast<int32_t>(commands[i].steps)
                                      : -static_cast<int32_t>(commands[i].steps);
      logMoveCommandIssueResult(step,
                                commands[i].motorId,
                                signedSteps,
                                commands[i].direction,
                                commands[i].rpm,
                                commands[i].acceleration,
                                commands[i].sync,
                                queueBefore,
                                enqueueResult,
                                frames);
    }
  }

  void logMoveCommandIssueResult(const MotionStep& step,
                                 uint8_t motorId,
                                 int32_t signedSteps,
                                 MotorDirection direction,
                                 uint16_t rpm,
                                 uint8_t acceleration,
                                 bool sync,
                                 size_t queueBefore,
                                 bool enqueueResult,
                                 size_t frameCount) const {
    Serial.print("[motion_command_issue] groupId=");
    Serial.print(context_.groupId);
    Serial.print(" seq=");
    Serial.print(context_.seq);
    Serial.print(" blockIndex=");
    Serial.print(currentStep_);
    Serial.print(" blockKind=");
    Serial.print(motionStepKindName(step.kind));
    Serial.print(" motorId=");
    Serial.print(motorId);
    Serial.print(" command=moveRelative commandByte=0xFD signedSteps=");
    Serial.print(signedSteps);
    Serial.print(" direction=");
    Serial.print(direction == MotorDirection::Cw ? "cw" : "ccw");
    Serial.print(" rpm=");
    Serial.print(rpm);
    Serial.print(" acceleration=");
    Serial.print(acceleration);
    Serial.print(" sync=");
    Serial.print(sync ? 1 : 0);
    Serial.print(" canTxAvailableForWriteBefore=");
    Serial.print(queueBefore);
    Serial.print(" enqueueResult=");
    Serial.print(enqueueResult ? 1 : 0);
    Serial.print(" framesEnqueued=");
    Serial.print(enqueueResult ? frameCount : 0);
    Serial.print(" commandIssueMs=");
    Serial.print(commandIssueMs_);
    Serial.println();
  }

  void logMotorTimeoutDiagnostics(const char* label,
                                  const MotorSupervisionState& supervision,
                                  uint32_t nowMs) const {
    if (!supervision.active) {
      return;
    }
    const MotorState state = feedback_.get(supervision.motorId);
    const bool pulseFresh = freshInputPulse(state, nowMs);
    const bool velocityFresh = freshVelocity(state, nowMs);
    const bool hasPositionError = supervision.pulseReferenceKnown && state.hasInputPulse;
    Serial.print("[motion_timeout] motor=");
    Serial.print(label);
    Serial.print(" id=");
    Serial.print(supervision.motorId);
    Serial.print(" deltaSteps=");
    Serial.print(supervision.deltaSteps);
    Serial.print(" baselineKnown=");
    Serial.print(supervision.baselineKnown ? 1 : 0);
    Serial.print(" initialPulse=");
    Serial.print(supervision.pulseReferenceKnown ? supervision.initialPulse : 0);
    Serial.print(" currentPulse=");
    Serial.print(state.hasInputPulse ? state.inputPulseSteps : 0);
    Serial.print(" targetPulse=");
    Serial.print(supervision.pulseReferenceKnown ? supervision.targetPulse : 0);
    Serial.print(" positionError=");
    Serial.print(hasPositionError ? state.inputPulseSteps - supervision.targetPulse : 0);
    Serial.print(" velocityRpm=");
    Serial.print(state.hasVelocity ? state.velocityRpm : 0.0f);
    Serial.print(" pulseFresh=");
    Serial.print(pulseFresh ? 1 : 0);
    Serial.print(" velocityFresh=");
    Serial.print(velocityFresh ? 1 : 0);
    Serial.print(" firstFeedbackMs=");
    Serial.print(supervision.firstFeedbackMs);
    Serial.print(" ackSeen=");
    Serial.print(supervision.ackSeenForCurrentBlock ? 1 : 0);
    Serial.print(" reachedSeen=");
    Serial.print(supervision.reachedSeenForCurrentBlock ? 1 : 0);
    Serial.print(" velocitySeen=");
    Serial.print(supervision.velocitySeen ? 1 : 0);
    Serial.print(" velocityEverNonZero=");
    Serial.print(supervision.velocityEverNonZero ? 1 : 0);
    Serial.print(" lastInputPulseMs=");
    Serial.print(state.lastInputPulseMs);
    Serial.print(" lastVelocityMs=");
    Serial.print(state.lastVelocityMs);
    Serial.print(" lastAckMs=");
    Serial.print(state.lastAckMs);
    Serial.print(" lastMotionReachedMs=");
    Serial.print(state.lastMotionReachedMs);
    Serial.print(" driverFault=");
    Serial.print(state.driverFault ? 1 : 0);
    Serial.println();
  }

  void captureSupervisionState(const MotionStep& step, uint32_t startedAtMs) {
    activeSupervision_ = {};
    if (step.kind == MotionStepKind::ReturnZero) {
      activeSupervision_.x = makeReturnZeroSupervision(config_.topology.xMotorId, startedAtMs);
      activeSupervision_.yLeft = makeReturnZeroSupervision(config_.topology.yLeftMotorId, startedAtMs);
      activeSupervision_.yRight = makeReturnZeroSupervision(config_.topology.yRightMotorId, startedAtMs);
      activeSupervision_.press = makeReturnZeroSupervision(config_.topology.pressMotorId, startedAtMs);
      return;
    }
    activeSupervision_.x = makeSupervision(config_.topology.xMotorId, step.deltaSteps.x, startedAtMs);
    activeSupervision_.yLeft = makeSupervision(config_.topology.yLeftMotorId, step.deltaSteps.yLeft, startedAtMs);
    activeSupervision_.yRight = makeSupervision(config_.topology.yRightMotorId, step.deltaSteps.yRight, startedAtMs);
    activeSupervision_.lineFeed =
        makeSupervision(config_.topology.lineFeedMotorId, step.deltaSteps.lineFeed, startedAtMs);
    activeSupervision_.press = makeSupervision(config_.topology.pressMotorId, step.deltaSteps.press, startedAtMs);
  }

  bool stepHasActiveMotion(const MotionStep& step) const {
    if (step.kind == MotionStepKind::ReturnZero) {
      return true;
    }
    return step.deltaSteps.x != 0 || step.deltaSteps.yLeft != 0 || step.deltaSteps.yRight != 0 ||
           step.deltaSteps.lineFeed != 0 || step.deltaSteps.press != 0;
  }

  bool baselineReady(const MotionStep& step, uint32_t nowMs) const {
    if (step.kind == MotionStepKind::ReturnZero) {
      return motorBaselineReady(feedback_.get(config_.topology.xMotorId), nowMs) &&
             motorBaselineReady(feedback_.get(config_.topology.yLeftMotorId), nowMs) &&
             motorBaselineReady(feedback_.get(config_.topology.yRightMotorId), nowMs) &&
             motorBaselineReady(feedback_.get(config_.topology.pressMotorId), nowMs);
    }
    if (step.deltaSteps.x != 0 && !motorBaselineReady(feedback_.get(config_.topology.xMotorId), nowMs)) {
      return false;
    }
    if (step.deltaSteps.yLeft != 0 &&
        (!motorBaselineReady(feedback_.get(config_.topology.yLeftMotorId), nowMs) ||
         !motorBaselineReady(feedback_.get(config_.topology.yRightMotorId), nowMs))) {
      return false;
    }
    if (step.deltaSteps.lineFeed != 0 &&
        !motorBaselineReady(feedback_.get(config_.topology.lineFeedMotorId), nowMs)) {
      return false;
    }
    if (step.deltaSteps.press != 0 && !motorBaselineReady(feedback_.get(config_.topology.pressMotorId), nowMs)) {
      return false;
    }
    return true;
  }

  void requestReturnZeroFeedback() {
    driver_.requestInputPulseCount(config_.topology.xMotorId);
    driver_.requestVelocity(config_.topology.xMotorId);
    driver_.requestInputPulseCount(config_.topology.yLeftMotorId);
    driver_.requestVelocity(config_.topology.yLeftMotorId);
    driver_.requestInputPulseCount(config_.topology.yRightMotorId);
    driver_.requestVelocity(config_.topology.yRightMotorId);
    driver_.requestInputPulseCount(config_.topology.pressMotorId);
    driver_.requestVelocity(config_.topology.pressMotorId);
  }

  MotorSupervisionState makeSupervision(uint8_t motorId, int32_t deltaSteps, uint32_t startedAtMs) const {
    MotorSupervisionState supervision{};
    supervision.active = deltaSteps != 0;
    supervision.motorId = motorId;
    supervision.deltaSteps = deltaSteps;
    supervision.startedAtMs = startedAtMs;
    supervision.commandIssueMs = startedAtMs;
    if (!supervision.active) {
      return supervision;
    }
    const uint32_t nowMs = millis();
    const MotorState state = feedback_.get(motorId);
    supervision.baselineKnown = motorBaselineReady(state, nowMs);
    if (supervision.baselineKnown) {
      supervision.initialPulse = state.inputPulseSteps;
      supervision.targetPulse = state.inputPulseSteps + deltaSteps;
      supervision.pulseReferenceKnown = true;
    }
    return supervision;
  }

  MotorSupervisionState makeReturnZeroSupervision(uint8_t motorId, uint32_t startedAtMs) const {
    MotorSupervisionState supervision{};
    supervision.motorId = motorId;
    supervision.startedAtMs = startedAtMs;
    supervision.commandIssueMs = startedAtMs;
    const uint32_t nowMs = millis();
    const MotorState state = feedback_.get(motorId);
    supervision.baselineKnown = motorBaselineReady(state, nowMs);
    if (!supervision.baselineKnown || absoluteSigned(state.inputPulseSteps) <= config_.motionRuntime.positionToleranceSteps) {
      return supervision;
    }
    supervision.active = true;
    supervision.initialPulse = state.inputPulseSteps;
    supervision.targetPulse = 0;
    supervision.deltaSteps = -state.inputPulseSteps;
    supervision.pulseReferenceKnown = true;
    return supervision;
  }

  void updateActiveSupervision(uint32_t nowMs) {
    updateMotorSupervision(activeSupervision_.x, nowMs);
    updateMotorSupervision(activeSupervision_.yLeft, nowMs);
    updateMotorSupervision(activeSupervision_.yRight, nowMs);
    updateMotorSupervision(activeSupervision_.lineFeed, nowMs);
    updateMotorSupervision(activeSupervision_.press, nowMs);
  }

  void updateMotorSupervision(MotorSupervisionState& supervision, uint32_t nowMs) {
    if (!supervision.active) {
      return;
    }
    const MotorState state = feedback_.get(supervision.motorId);
    if (state.lastAckMs >= supervision.commandIssueMs && state.lastAckCommand == 0xFD) {
      supervision.ackSeenForCurrentBlock = true;
      noteFirstFeedback(supervision, state.lastAckMs);
    }
    if (state.lastMotionReachedMs >= supervision.startedAtMs) {
      supervision.reachedSeenForCurrentBlock = true;
      noteFirstFeedback(supervision, state.lastMotionReachedMs);
    }
    if (freshInputPulse(state, nowMs) && state.lastInputPulseMs >= supervision.startedAtMs) {
      if (!supervision.pulseReferenceKnown) {
        supervision.initialPulse = state.inputPulseSteps;
        supervision.pulseReferenceKnown = true;
      }
      noteFirstFeedback(supervision, state.lastInputPulseMs);
    }
    if (freshVelocity(state, nowMs) && state.lastVelocityMs >= supervision.startedAtMs) {
      supervision.velocitySeen = true;
      if (fabs(state.velocityRpm) > movingVelocityThresholdRpm()) {
        supervision.velocityEverNonZero = true;
      }
      noteFirstFeedback(supervision, state.lastVelocityMs);
    }
  }

  void noteFirstFeedback(MotorSupervisionState& supervision, uint32_t timeMs) {
    if (supervision.firstFeedbackMs == 0 || timeMs < supervision.firstFeedbackMs) {
      supervision.firstFeedbackMs = timeMs;
    }
  }

  bool motorSupervisionSatisfied(const MotorSupervisionState& supervision,
                                  uint32_t nowMs,
                                  uint32_t elapsedMs,
                                  const MotionStep& step) const {
    if (!supervision.active) {
      return true;
    }
    const MotorState state = feedback_.get(supervision.motorId);
    if (supervision.baselineKnown) {
      return motorAtTarget(state, supervision.targetPulse, nowMs);
    }
    if (elapsedMs < minimumMotionMs(step)) {
      return false;
    }
    if (supervision.reachedSeenForCurrentBlock) {
      return true;
    }
    return supervision.ackSeenForCurrentBlock && supervision.velocityEverNonZero && freshVelocity(state, nowMs) &&
           fabs(state.velocityRpm) <= config_.motionRuntime.idleVelocityThresholdRpm;
  }

  bool motorAtTarget(const MotorState& state, int32_t target, uint32_t nowMs) const {
    if (state.driverFault || !freshInputPulse(state, nowMs) || !freshVelocity(state, nowMs)) {
      return false;
    }
    const uint32_t error = absoluteSigned(state.inputPulseSteps - target);
    return error <= config_.motionRuntime.positionToleranceSteps &&
           fabs(state.velocityRpm) <= config_.motionRuntime.idleVelocityThresholdRpm;
  }

  bool activeSupervisionHasUsefulFeedback() const {
    return motorHasUsefulFeedback(activeSupervision_.x) || motorHasUsefulFeedback(activeSupervision_.yLeft) ||
           motorHasUsefulFeedback(activeSupervision_.yRight) || motorHasUsefulFeedback(activeSupervision_.lineFeed) ||
           motorHasUsefulFeedback(activeSupervision_.press);
  }

  bool motorHasUsefulFeedback(const MotorSupervisionState& supervision) const {
    return supervision.active &&
           (supervision.firstFeedbackMs != 0 || supervision.ackSeenForCurrentBlock ||
            supervision.reachedSeenForCurrentBlock);
  }

  uint8_t activeCommandCount() const {
    uint8_t count = 0;
    if (activeSupervision_.x.active) {
      ++count;
    }
    if (activeSupervision_.yLeft.active) {
      ++count;
    }
    if (activeSupervision_.yRight.active) {
      ++count;
    }
    if (activeSupervision_.lineFeed.active) {
      ++count;
    }
    if (activeSupervision_.press.active) {
      ++count;
    }
    return count;
  }

  bool allActiveCommandsAcked() const {
    return motorAckSatisfied(activeSupervision_.x) && motorAckSatisfied(activeSupervision_.yLeft) &&
           motorAckSatisfied(activeSupervision_.yRight) && motorAckSatisfied(activeSupervision_.lineFeed) &&
           motorAckSatisfied(activeSupervision_.press);
  }

  static bool motorAckSatisfied(const MotorSupervisionState& supervision) {
    return !supervision.active || supervision.ackSeenForCurrentBlock;
  }

  bool activeCommandResponseFault() {
    return commandResponseFault(activeSupervision_.x) || commandResponseFault(activeSupervision_.yLeft) ||
           commandResponseFault(activeSupervision_.yRight) || commandResponseFault(activeSupervision_.lineFeed) ||
           commandResponseFault(activeSupervision_.press);
  }

  bool activeAckedButNotMoving(uint32_t nowMs) const {
    if (commandIssueMs_ == 0 || nowMs - commandIssueMs_ <= config_.motionRuntime.motionNoMovementTimeoutMs) {
      return false;
    }
    return motorAckedButNotMoving(activeSupervision_.x, nowMs) ||
           motorAckedButNotMoving(activeSupervision_.yLeft, nowMs) ||
           motorAckedButNotMoving(activeSupervision_.yRight, nowMs) ||
           motorAckedButNotMoving(activeSupervision_.lineFeed, nowMs) ||
           motorAckedButNotMoving(activeSupervision_.press, nowMs);
  }

  bool motorAckedButNotMoving(const MotorSupervisionState& supervision, uint32_t nowMs) const {
    if (!supervision.active || !supervision.ackSeenForCurrentBlock || supervision.velocityEverNonZero ||
        !supervision.pulseReferenceKnown) {
      return false;
    }
    const MotorState state = feedback_.get(supervision.motorId);
    return freshInputPulse(state, nowMs) && freshVelocity(state, nowMs) &&
           absoluteSigned(state.inputPulseSteps - supervision.initialPulse) <= config_.motionRuntime.positionToleranceSteps;
  }

  const char* motionTargetTimeoutCode() const {
    if (!activeSupervisionHasUsefulFeedback()) {
      return "motion_feedback_timeout";
    }
    return "motion_target_timeout";
  }

  const char* motionTargetTimeoutMessage() const {
    if (!activeSupervisionHasUsefulFeedback()) {
      return "Motion feedback timed out";
    }
    return "Motion target timed out";
  }

  bool yPairSkewCheckReady(uint32_t nowMs) const {
    if (!activeSupervision_.yLeft.active || !activeSupervision_.yRight.active) {
      return false;
    }
    if (activeSupervision_.yLeft.baselineKnown && activeSupervision_.yRight.baselineKnown) {
      return true;
    }
    const MotorState left = feedback_.get(config_.topology.yLeftMotorId);
    const MotorState right = feedback_.get(config_.topology.yRightMotorId);
    return activeSupervision_.yLeft.pulseReferenceKnown && activeSupervision_.yRight.pulseReferenceKnown &&
           freshInputPulse(left, nowMs) && freshInputPulse(right, nowMs) &&
           left.lastInputPulseMs >= activeSupervision_.yLeft.startedAtMs &&
           right.lastInputPulseMs >= activeSupervision_.yRight.startedAtMs;
  }

  bool yPairSkewExceeded(uint32_t nowMs) const {
    (void)nowMs;
    const MotorState left = feedback_.get(config_.topology.yLeftMotorId);
    const MotorState right = feedback_.get(config_.topology.yRightMotorId);
    const int32_t skew = (left.inputPulseSteps - activeSupervision_.yLeft.initialPulse) +
                         (right.inputPulseSteps - activeSupervision_.yRight.initialPulse);
    return absoluteSigned(skew) > config_.motionRuntime.ySkewToleranceSteps;
  }

  bool commandResponseFault(const MotorSupervisionState& supervision) {
    if (!supervision.active) {
      return false;
    }
    const MotorState state = feedback_.get(supervision.motorId);
    if (state.driverFault && state.lastStatusMs >= supervision.startedAtMs) {
      stopAll();
      fail(state.lastErrorCode != nullptr && state.lastErrorCode[0] != '\0' ? state.lastErrorCode : "driver_fault",
           state.lastErrorMessage != nullptr && state.lastErrorMessage[0] != '\0' ? state.lastErrorMessage
                                                                                   : "Motor driver reported a fault");
      return true;
    }
    if (state.lastConditionNotMetCommand != 0 && state.lastConditionNotMetMs >= supervision.startedAtMs) {
      stopAll();
      fail("motion_condition_not_met", "Motor rejected motion conditions");
      return true;
    }
    if (state.lastMalformedCommand != 0 && state.lastMalformedMs >= supervision.startedAtMs) {
      stopAll();
      fail("motion_command_malformed", "Motor reported malformed command");
      return true;
    }
    return false;
  }

  static uint32_t absoluteSigned(int32_t value) {
    return value < 0 ? static_cast<uint32_t>(-value) : static_cast<uint32_t>(value);
  }

  static uint32_t clampUint32(uint32_t value, uint32_t minValue, uint32_t maxValue) {
    if (value < minValue) {
      return minValue;
    }
    if (value > maxValue) {
      return maxValue;
    }
    return value;
  }

  static const char* motionStepKindName(MotionStepKind kind) {
    switch (kind) {
      case MotionStepKind::MoveXY:
        return "move_xy";
      case MotionStepKind::PressDown:
        return "press_down";
      case MotionStepKind::PressUp:
        return "press_up";
      case MotionStepKind::CharacterRelease:
        return "character_release";
      case MotionStepKind::LineFeed:
        return "line_feed";
      case MotionStepKind::ReturnZero:
        return "return_zero";
      case MotionStepKind::Wait:
        return "wait";
    }
    return "unknown";
  }

  bool freshInputPulse(const MotorState& state, uint32_t nowMs) const {
    return state.hasInputPulse && state.lastInputPulseMs != 0 && nowMs - state.lastInputPulseMs <= kFeedbackFreshMs;
  }

  bool freshVelocity(const MotorState& state, uint32_t nowMs) const {
    return state.hasVelocity && state.lastVelocityMs != 0 && nowMs - state.lastVelocityMs <= kFeedbackFreshMs;
  }

  bool motorBaselineReady(const MotorState& state, uint32_t nowMs) const {
    return state.hasInputPulse && state.lastInputPulseMs != 0 && nowMs - state.lastInputPulseMs <= kBaselineFreshMs &&
           state.hasVelocity && state.lastVelocityMs != 0 && nowMs - state.lastVelocityMs <= kBaselineFreshMs;
  }

  float movingVelocityThresholdRpm() const {
    const float doubled = config_.motionRuntime.idleVelocityThresholdRpm * 2.0f;
    return doubled < 5.0f ? 5.0f : doubled;
  }

  uint32_t expectedMoveMs(const MotionStep& step) const {
    if (step.kind == MotionStepKind::ReturnZero) {
      uint32_t maxSteps = absoluteSteps(activeSupervision_.x.deltaSteps);
      const uint32_t yLeftSteps = absoluteSteps(activeSupervision_.yLeft.deltaSteps);
      const uint32_t yRightSteps = absoluteSteps(activeSupervision_.yRight.deltaSteps);
      const uint32_t pressSteps = absoluteSteps(activeSupervision_.press.deltaSteps);
      if (yLeftSteps > maxSteps) {
        maxSteps = yLeftSteps;
      }
      if (yRightSteps > maxSteps) {
        maxSteps = yRightSteps;
      }
      if (pressSteps > maxSteps) {
        maxSteps = pressSteps;
      }
      if (maxSteps == 0 || step.profile.maxRpm == 0 || config_.calibration.stepsPerRev == 0) {
        return 0;
      }
      const uint64_t numerator = static_cast<uint64_t>(maxSteps) * 60000ULL;
      const uint64_t denominator = static_cast<uint64_t>(step.profile.maxRpm) * config_.calibration.stepsPerRev;
      return static_cast<uint32_t>((numerator + denominator - 1) / denominator);
    }
    uint32_t maxSteps = absoluteSteps(step.deltaSteps.x);
    const uint32_t ySteps = absoluteSteps(step.deltaSteps.yLeft);
    const uint32_t lineFeedSteps = absoluteSteps(step.deltaSteps.lineFeed);
    const uint32_t pressSteps = absoluteSteps(step.deltaSteps.press);
    if (ySteps > maxSteps) {
      maxSteps = ySteps;
    }
    if (lineFeedSteps > maxSteps) {
      maxSteps = lineFeedSteps;
    }
    if (pressSteps > maxSteps) {
      maxSteps = pressSteps;
    }
    if (maxSteps == 0 || step.profile.maxRpm == 0 || config_.calibration.stepsPerRev == 0) {
      return 0;
    }
    const uint64_t numerator = static_cast<uint64_t>(maxSteps) * 60000ULL;
    const uint64_t denominator = static_cast<uint64_t>(step.profile.maxRpm) * config_.calibration.stepsPerRev;
    return static_cast<uint32_t>((numerator + denominator - 1) / denominator);
  }

  uint32_t minimumMotionMs(const MotionStep& step) const {
    const uint32_t timeoutHalf = step.profile.timeoutMs / 2;
    const uint32_t upper = timeoutHalf < 800 ? timeoutHalf : 800;
    const uint32_t lower = timeoutHalf < 80 ? timeoutHalf : 80;
    return clampUint32(expectedMoveMs(step) / 4, lower, upper);
  }

  static void copyString(char* out, size_t outSize, const char* value) {
    if (outSize == 0) {
      return;
    }
    const char* source = value != nullptr ? value : "";
    size_t i = 0;
    for (; i + 1 < outSize && source[i] != '\0'; ++i) {
      out[i] = source[i];
    }
    out[i] = '\0';
  }

  static constexpr uint32_t kBaselineFreshMs = 1500;
  static constexpr uint32_t kFeedbackFreshMs = 1000;
  static constexpr uint32_t kBaselinePreflightTimeoutMs = 600;

  const TypingConfig& config_;
  EmmV5Driver& driver_;
  MotorFeedbackStore& feedback_;
  ProtocolTrace& trace_;
  YPairController yPair_;
  State state_;
  Phase phase_;
  const MotionStep* steps_;
  size_t stepCount_;
  size_t currentStep_;
  uint32_t stepStartedAtMs_;
  uint32_t commandIssueMs_;
  uint32_t settleStartedAtMs_;
  const char* faultCode_;
  const char* faultMessage_;
  MachinePointMm currentPoint_;
  size_t activeTextIndex_;
  ExecutionContext context_;
  ActiveSupervision activeSupervision_;
  uint8_t completionSampleCount_;
  uint32_t lastFeedbackPollMs_;
  bool currentStepStartedEvent_;
  size_t lastCompletedStep_;
  uint32_t lastCompletedDurationMs_;
};

}  // namespace auto_typer
