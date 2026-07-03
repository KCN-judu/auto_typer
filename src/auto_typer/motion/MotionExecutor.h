#pragma once

#include "../can/CanRxTask.h"
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
                 MotorFeedbackStore& feedback)
      : config_(config),
        driver_(driver),
        feedback_(feedback),
        yPair_(config, driver),
        state_(State::Idle),
        steps_(nullptr),
        stepCount_(0),
        currentStep_(0),
        stepStartedAtMs_(0),
        settleStartedAtMs_(0),
        faultCode_(""),
        faultMessage_(""),
        currentPoint_(config.homePoint),
        activeTextIndex_(0),
        activeFeedback_{},
        completionSampleCount_(0),
        lastFeedbackPollMs_(0),
        waitingForBaseline_(false),
        baselineRequestedAtMs_(0),
        currentStepStartedEvent_(false),
        lastCompletedStep_(0),
        lastCompletedDurationMs_(0) {}

  bool start(const MotionStep* steps, size_t count, MachinePointMm startPoint = {NAN, NAN}) {
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
    settleStartedAtMs_ = 0;
    completionSampleCount_ = 0;
    lastFeedbackPollMs_ = 0;
    waitingForBaseline_ = false;
    baselineRequestedAtMs_ = 0;
    currentStepStartedEvent_ = false;
    lastCompletedStep_ = 0;
    lastCompletedDurationMs_ = 0;
    state_ = count == 0 ? State::Completed : State::Running;
    return true;
  }

  void tick() {
    if (state_ != State::Running) {
      return;
    }
    if (currentStep_ >= stepCount_) {
      state_ = State::Completed;
      return;
    }
    const MotionStep& step = steps_[currentStep_];
    if (stepStartedAtMs_ == 0) {
      if (!prepareStepBaseline(step)) {
        return;
      }
      if (!beginStep(step)) {
        if (state_ != State::Faulted) {
          fail("motion_command_rejected", "Motion command rejected");
        }
        return;
      }
      stepStartedAtMs_ = millis();
      settleStartedAtMs_ = 0;
      completionSampleCount_ = 0;
      lastFeedbackPollMs_ = 0;
      waitingForBaseline_ = false;
      baselineRequestedAtMs_ = 0;
      currentStepStartedEvent_ = true;
    }
    if (isStepComplete(step)) {
      const size_t completedStep = currentStep_;
      const uint32_t completedDurationMs = millis() - stepStartedAtMs_;
      finishStep(step);
      ++currentStep_;
      lastCompletedStep_ = completedStep + 1;
      lastCompletedDurationMs_ = completedDurationMs;
      stepStartedAtMs_ = 0;
      settleStartedAtMs_ = 0;
      currentStepStartedEvent_ = false;
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
  struct ActiveFeedbackTargets {
    int32_t initialX;
    int32_t initialYLeft;
    int32_t initialYRight;
    int32_t initialLineFeed;
    int32_t initialPress;
    int32_t targetX;
    int32_t targetYLeft;
    int32_t targetYRight;
    int32_t targetLineFeed;
    int32_t targetPress;
  };

  enum class State : uint8_t {
    Idle,
    Running,
    Cancelling,
    Completed,
    Cancelled,
    Faulted,
  };

  bool beginStep(const MotionStep& step) {
    switch (step.kind) {
      case MotionStepKind::ServoPress:
        return startPressMotor(step, PressAction::Press);
      case MotionStepKind::ServoRelease:
        return startPressMotor(step, PressAction::Release);
      case MotionStepKind::Wait:
        return true;
      case MotionStepKind::CharacterRelease:
        return startLineFeed(step);
      case MotionStepKind::LineFeed:
        return startLineFeed(step);
      case MotionStepKind::MoveXY:
        return startMoveXY(step);
    }
    return false;
  }

  bool prepareStepBaseline(const MotionStep& step) {
    if (!requiresFeedbackBaseline(step)) {
      waitingForBaseline_ = false;
      baselineRequestedAtMs_ = 0;
      return true;
    }

    requestFeedback(step);
    if (baselineReady(step)) {
      waitingForBaseline_ = false;
      baselineRequestedAtMs_ = 0;
      return true;
    }

    const uint32_t nowMs = millis();
    if (!waitingForBaseline_) {
      waitingForBaseline_ = true;
      baselineRequestedAtMs_ = nowMs;
      return false;
    }
    if (nowMs - baselineRequestedAtMs_ > kBaselineAcquireTimeoutMs) {
      stopAll();
      fail("motor_feedback_baseline_timeout", "Motor feedback baseline timeout");
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
    if (!validateMoveXYBaseline(step)) {
      return false;
    }
    captureFeedbackTargets(step);
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
      return driver_.moveRelativeBatch(commands, sizeof(commands) / sizeof(commands[0]), true);
    }
    if (hasX && !driver_.moveRelative(config_.topology.xMotorId,
                                      directionForSignedSteps(step.deltaSteps.x),
                                      xRpm,
                                      step.profile.acceleration,
                                      xSteps,
                                      false)) {
      return false;
    }
    if (hasY && !yPair_.moveRelative(step.deltaSteps.yLeft, yRpm, step.profile.acceleration)) {
      return false;
    }
    return true;
  }

  bool startLineFeed(const MotionStep& step) {
    const uint32_t steps = absoluteSteps(step.deltaSteps.lineFeed);
    if (steps == 0) {
      return true;
    }
    if (!validateLineFeedBaseline()) {
      return false;
    }
    captureFeedbackTargets(step);
    return driver_.moveRelative(config_.topology.lineFeedMotorId,
                                directionForSignedSteps(step.deltaSteps.lineFeed),
                                step.profile.maxRpm,
                                step.profile.acceleration,
                                steps,
                                false);
  }

  bool startPressMotor(const MotionStep& step, PressAction action) {
    const int32_t signedSteps = pressMotorSignedSteps(action);
    const uint32_t steps = absoluteSteps(signedSteps);
    if (steps == 0) {
      return true;
    }
    if (!validatePressMotorBaseline()) {
      return false;
    }
    captureFeedbackTargets(step);
    return driver_.moveRelative(config_.topology.pressMotorId,
                                directionForSignedSteps(signedSteps),
                                step.profile.maxRpm,
                                step.profile.acceleration,
                                steps,
                                false);
  }

  bool isStepComplete(const MotionStep& step) {
    const uint32_t elapsed = millis() - stepStartedAtMs_;
    if (elapsed > step.profile.timeoutMs) {
      stopAll();
      fail("motion_timeout", "Motion timed out before reaching target");
      return false;
    }
    if (step.kind == MotionStepKind::Wait) {
      return elapsed >= step.waitMs;
    }

    requestFeedback(step);
    if (feedbackTimedOut(step)) {
      stopAll();
      fail("motion_feedback_timeout", "Required motor feedback became stale");
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
    if (step.kind == MotionStepKind::ServoPress || step.kind == MotionStepKind::ServoRelease) {
      driver_.requestInputPulseCount(config_.topology.pressMotorId);
      driver_.requestVelocity(config_.topology.pressMotorId);
    }
  }

  bool feedbackSatisfied(const MotionStep& step) {
    const uint32_t nowMs = millis();
    if (step.kind == MotionStepKind::MoveXY) {
      if (step.deltaSteps.x != 0 && commandResponseFault(config_.topology.xMotorId)) {
        return false;
      }
      if (step.deltaSteps.x != 0 &&
          !motorAtTarget(config_.topology.xMotorId, activeFeedback_.targetX, nowMs)) {
        return false;
      }
      if (step.deltaSteps.yLeft != 0) {
        const MotorState left = feedback_.get(config_.topology.yLeftMotorId);
        const MotorState right = feedback_.get(config_.topology.yRightMotorId);
        if (commandResponseFault(left) || commandResponseFault(right)) {
          return false;
        }
        if (feedbackFresh(left, nowMs) && feedbackFresh(right, nowMs) && yPairSkewExceeded(left, right)) {
          stopAll();
          fail("y_pair_skew", "Y pair skew exceeded tolerance");
          return false;
        }
        if (!motorAtTarget(left, activeFeedback_.targetYLeft, nowMs) ||
            !motorAtTarget(right, activeFeedback_.targetYRight, nowMs)) {
          return false;
        }
      }
      return true;
    }
    if (step.kind == MotionStepKind::LineFeed || step.kind == MotionStepKind::CharacterRelease) {
      if (commandResponseFault(config_.topology.lineFeedMotorId)) {
        return false;
      }
      return motorAtTarget(config_.topology.lineFeedMotorId, activeFeedback_.targetLineFeed, nowMs);
    }
    if (step.kind == MotionStepKind::ServoPress || step.kind == MotionStepKind::ServoRelease) {
      if (commandResponseFault(config_.topology.pressMotorId)) {
        return false;
      }
      return motorAtTarget(config_.topology.pressMotorId, activeFeedback_.targetPress, nowMs);
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
    if (step.kind == MotionStepKind::ServoPress) {
      ++activeTextIndex_;
    }
  }

  void fail(const char* code, const char* message) {
    faultCode_ = code;
    faultMessage_ = message;
    state_ = State::Faulted;
  }

  void captureFeedbackTargets(const MotionStep& step) {
    const MotorState x = feedback_.get(config_.topology.xMotorId);
    const MotorState yLeft = feedback_.get(config_.topology.yLeftMotorId);
    const MotorState yRight = feedback_.get(config_.topology.yRightMotorId);
    const MotorState lineFeed = feedback_.get(config_.topology.lineFeedMotorId);
    const MotorState press = feedback_.get(config_.topology.pressMotorId);
    activeFeedback_.initialX = x.inputPulseSteps;
    activeFeedback_.initialYLeft = yLeft.inputPulseSteps;
    activeFeedback_.initialYRight = yRight.inputPulseSteps;
    activeFeedback_.initialLineFeed = lineFeed.inputPulseSteps;
    activeFeedback_.initialPress = press.inputPulseSteps;
    activeFeedback_.targetX = x.inputPulseSteps + step.deltaSteps.x;
    activeFeedback_.targetYLeft = yLeft.inputPulseSteps + step.deltaSteps.yLeft;
    activeFeedback_.targetYRight = yRight.inputPulseSteps + step.deltaSteps.yRight;
    activeFeedback_.targetLineFeed = lineFeed.inputPulseSteps + step.deltaSteps.lineFeed;
    activeFeedback_.targetPress = press.inputPulseSteps + pressMotorSignedSteps(step.kind);
  }

  bool motorAtTarget(uint8_t motorId, int32_t target, uint32_t nowMs) const {
    return motorAtTarget(feedback_.get(motorId), target, nowMs);
  }

  bool motorAtTarget(const MotorState& state, int32_t target, uint32_t nowMs) const {
    if (state.driverFault || !state.hasInputPulse || !state.hasVelocity || state.lastInputPulseMs == 0 ||
        state.lastVelocityMs == 0 || nowMs - state.lastInputPulseMs > kFeedbackFreshMs ||
        nowMs - state.lastVelocityMs > kFeedbackFreshMs) {
      return false;
    }
    const uint32_t error = absoluteSigned(state.inputPulseSteps - target);
    return error <= config_.motionRuntime.positionToleranceSteps &&
           fabs(state.velocityRpm) <= config_.motionRuntime.idleVelocityThresholdRpm;
  }

  bool feedbackFresh(const MotorState& state, uint32_t nowMs) const {
    return state.hasInputPulse && state.lastInputPulseMs != 0 && nowMs - state.lastInputPulseMs <= kFeedbackFreshMs;
  }

  bool feedbackTimedOut(const MotionStep& step) const {
    const uint32_t nowMs = millis();
    if (nowMs - stepStartedAtMs_ <= kFeedbackFreshMs) {
      return false;
    }
    if (step.kind == MotionStepKind::MoveXY) {
      if (step.deltaSteps.x != 0 && !motorFeedbackFresh(config_.topology.xMotorId, nowMs)) {
        return true;
      }
      if (step.deltaSteps.yLeft != 0 || step.deltaSteps.yRight != 0) {
        return !motorFeedbackFresh(config_.topology.yLeftMotorId, nowMs) ||
               !motorFeedbackFresh(config_.topology.yRightMotorId, nowMs);
      }
      return false;
    }
    if (step.kind == MotionStepKind::LineFeed || step.kind == MotionStepKind::CharacterRelease) {
      return !motorFeedbackFresh(config_.topology.lineFeedMotorId, nowMs);
    }
    if (step.kind == MotionStepKind::ServoPress || step.kind == MotionStepKind::ServoRelease) {
      return !motorFeedbackFresh(config_.topology.pressMotorId, nowMs);
    }
    return false;
  }

  bool motorFeedbackFresh(uint8_t motorId, uint32_t nowMs) const {
    return feedback_.hasFreshInputPulse(motorId, nowMs, kFeedbackFreshMs) &&
           feedback_.hasFreshVelocity(motorId, nowMs, kFeedbackFreshMs);
  }

  bool yPairSkewExceeded(const MotorState& left, const MotorState& right) const {
    const int32_t skew = (left.inputPulseSteps - activeFeedback_.initialYLeft) +
                         (right.inputPulseSteps - activeFeedback_.initialYRight);
    return absoluteSigned(skew) > config_.motionRuntime.ySkewToleranceSteps;
  }

  bool commandResponseFault(uint8_t motorId) {
    return commandResponseFault(feedback_.get(motorId));
  }

  bool commandResponseFault(const MotorState& state) {
    if (stepStartedAtMs_ == 0) {
      return false;
    }
    if (state.driverFault && state.lastStatusMs >= stepStartedAtMs_) {
      stopAll();
      fail(state.lastErrorCode != nullptr && state.lastErrorCode[0] != '\0' ? state.lastErrorCode : "driver_fault",
           state.lastErrorMessage != nullptr && state.lastErrorMessage[0] != '\0' ? state.lastErrorMessage
                                                                                   : "Motor driver reported a fault");
      return true;
    }
    if (state.lastConditionNotMetCommand != 0 && state.lastConditionNotMetMs >= stepStartedAtMs_) {
      stopAll();
      fail("motion_condition_not_met", "Motor rejected motion conditions");
      return true;
    }
    if (state.lastMalformedCommand != 0 && state.lastMalformedMs >= stepStartedAtMs_) {
      stopAll();
      fail("motion_command_malformed", "Motor reported malformed command");
      return true;
    }
    return false;
  }

  static uint32_t absoluteSigned(int32_t value) {
    return value < 0 ? static_cast<uint32_t>(-value) : static_cast<uint32_t>(value);
  }

  static constexpr uint32_t kBaselineFreshMs = 1500;
  static constexpr uint32_t kBaselineAcquireTimeoutMs = 1500;
  static constexpr uint32_t kFeedbackFreshMs = 1000;

  bool requiresFeedbackBaseline(const MotionStep& step) const {
    if (step.kind == MotionStepKind::MoveXY) {
      return step.deltaSteps.x != 0 || step.deltaSteps.yLeft != 0 || step.deltaSteps.yRight != 0;
    }
    if (step.kind == MotionStepKind::LineFeed || step.kind == MotionStepKind::CharacterRelease) {
      return step.deltaSteps.lineFeed != 0;
    }
    if (step.kind == MotionStepKind::ServoPress || step.kind == MotionStepKind::ServoRelease) {
      return pressMotorSignedSteps(step.kind) != 0;
    }
    return false;
  }

  bool baselineReady(const MotionStep& step) const {
    if (step.kind == MotionStepKind::MoveXY) {
      return moveXYBaselineReady(step);
    }
    if (step.kind == MotionStepKind::LineFeed || step.kind == MotionStepKind::CharacterRelease) {
      return lineFeedBaselineReady();
    }
    if (step.kind == MotionStepKind::ServoPress || step.kind == MotionStepKind::ServoRelease) {
      return pressMotorBaselineReady();
    }
    return true;
  }

  bool motorBaselineReady(uint8_t motorId, uint32_t nowMs) const {
    return feedback_.hasFreshInputPulse(motorId, nowMs, kBaselineFreshMs) &&
           feedback_.hasFreshVelocity(motorId, nowMs, kBaselineFreshMs);
  }

  bool moveXYBaselineReady(const MotionStep& step) const {
    const uint32_t nowMs = millis();
    if (step.deltaSteps.x != 0 && !motorBaselineReady(config_.topology.xMotorId, nowMs)) {
      return false;
    }
    if (step.deltaSteps.yLeft != 0 || step.deltaSteps.yRight != 0) {
      return motorBaselineReady(config_.topology.yLeftMotorId, nowMs) &&
             motorBaselineReady(config_.topology.yRightMotorId, nowMs);
    }
    return true;
  }

  bool validateMoveXYBaseline(const MotionStep& step) {
    if (moveXYBaselineReady(step)) {
      return true;
    }
    const uint32_t nowMs = millis();
    if (step.deltaSteps.x != 0 && !motorBaselineReady(config_.topology.xMotorId, nowMs)) {
      fail("motor_feedback_baseline_missing", "X motor feedback baseline missing");
      return false;
    }
    if (step.deltaSteps.yLeft != 0 || step.deltaSteps.yRight != 0) {
      if (!motorBaselineReady(config_.topology.yLeftMotorId, nowMs)) {
        fail("motor_feedback_baseline_missing", "Y left motor feedback baseline missing");
        return false;
      }
      if (!motorBaselineReady(config_.topology.yRightMotorId, nowMs)) {
        fail("motor_feedback_baseline_missing", "Y right motor feedback baseline missing");
        return false;
      }
    }
    return true;
  }

  bool lineFeedBaselineReady() const {
    const uint32_t nowMs = millis();
    return motorBaselineReady(config_.topology.lineFeedMotorId, nowMs);
  }

  bool validateLineFeedBaseline() {
    if (!lineFeedBaselineReady()) {
      fail("line_feed_baseline_missing", "LineFeed motor feedback baseline missing");
      return false;
    }
    return true;
  }

  bool pressMotorBaselineReady() const {
    const uint32_t nowMs = millis();
    return motorBaselineReady(config_.topology.pressMotorId, nowMs);
  }

  bool validatePressMotorBaseline() {
    if (!pressMotorBaselineReady()) {
      fail("press_motor_baseline_missing", "Press motor feedback baseline missing");
      return false;
    }
    return true;
  }

  int32_t pressMotorSignedSteps(MotionStepKind kind) const {
    if (kind == MotionStepKind::ServoPress) {
      return signedStepsForDirection(config_.pressMotor.pressSteps, config_.pressMotor.pressDirection);
    }
    if (kind == MotionStepKind::ServoRelease) {
      return signedStepsForDirection(config_.pressMotor.releaseSteps, config_.pressMotor.releaseDirection);
    }
    return 0;
  }

  int32_t pressMotorSignedSteps(PressAction action) const {
    if (action == PressAction::Press) {
      return signedStepsForDirection(config_.pressMotor.pressSteps, config_.pressMotor.pressDirection);
    }
    if (action == PressAction::Release) {
      return signedStepsForDirection(config_.pressMotor.releaseSteps, config_.pressMotor.releaseDirection);
    }
    return 0;
  }

  const TypingConfig& config_;
  EmmV5Driver& driver_;
  MotorFeedbackStore& feedback_;
  YPairController yPair_;
  State state_;
  const MotionStep* steps_;
  size_t stepCount_;
  size_t currentStep_;
  uint32_t stepStartedAtMs_;
  uint32_t settleStartedAtMs_;
  const char* faultCode_;
  const char* faultMessage_;
  MachinePointMm currentPoint_;
  size_t activeTextIndex_;
  ActiveFeedbackTargets activeFeedback_;
  uint8_t completionSampleCount_;
  uint32_t lastFeedbackPollMs_;
  bool waitingForBaseline_;
  uint32_t baselineRequestedAtMs_;
  bool currentStepStartedEvent_;
  size_t lastCompletedStep_;
  uint32_t lastCompletedDurationMs_;
};

}  // namespace auto_typer
