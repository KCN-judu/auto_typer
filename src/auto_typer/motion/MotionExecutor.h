#pragma once

#include "../can/CanRxTask.h"
#include "../drivers/EmmV5Driver.h"
#include "../hal_servo_press.h"
#include "MachineKinematics.h"
#include "MotionPlanner.h"
#include "YPairController.h"

#include <math.h>

namespace auto_typer {

class MotionExecutor {
 public:
  MotionExecutor(const TypingConfig& config,
                 EmmV5Driver& driver,
                 ServoPressHal& servo,
                 MotorFeedbackStore& feedback)
      : config_(config),
        driver_(driver),
        servo_(servo),
        feedback_(feedback),
        yPair_(config, driver),
        state_(State::Idle),
        blocks_(nullptr),
        blockCount_(0),
        currentBlock_(0),
        blockStartedAtMs_(0),
        settleStartedAtMs_(0),
        faultCode_(""),
        faultMessage_(""),
        currentPoint_(config.homePoint),
        activeTextIndex_(0),
        activeFeedback_{},
        completionSampleCount_(0),
        lastFeedbackPollMs_(0) {}

  bool start(const MotionBlock* blocks, size_t count) {
    if (state_ == State::Running || state_ == State::Cancelling) {
      return false;
    }
    blocks_ = blocks;
    blockCount_ = count;
    currentBlock_ = 0;
    activeTextIndex_ = 0;
    currentPoint_ = config_.homePoint;
    faultCode_ = "";
    faultMessage_ = "";
    state_ = count == 0 ? State::Completed : State::Running;
    return true;
  }

  void tick() {
    if (state_ != State::Running) {
      return;
    }
    if (currentBlock_ >= blockCount_) {
      state_ = State::Completed;
      return;
    }
    const MotionBlock& block = blocks_[currentBlock_];
    if (blockStartedAtMs_ == 0) {
      if (!beginBlock(block)) {
        if (state_ != State::Faulted) {
          fail("motion_command_rejected", "Motion command rejected");
        }
        return;
      }
      blockStartedAtMs_ = millis();
      settleStartedAtMs_ = 0;
      completionSampleCount_ = 0;
      lastFeedbackPollMs_ = 0;
    }
    if (isBlockComplete(block)) {
      finishBlock(block);
      ++currentBlock_;
      blockStartedAtMs_ = 0;
      settleStartedAtMs_ = 0;
    }
  }

  void cancel() {
    if (state_ != State::Running) {
      return;
    }
    state_ = State::Cancelling;
    stopAll();
    servo_.neutral(1);
    state_ = State::Cancelled;
  }

  void emergencyStop() {
    stopAll();
    servo_.neutral(1);
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

  size_t currentBlock() const {
    return currentBlock_;
  }

  size_t totalBlocks() const {
    return blockCount_;
  }

  size_t activeTextIndex() const {
    return activeTextIndex_;
  }

  MachinePointMm currentPoint() const {
    return currentPoint_;
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
    int32_t targetX;
    int32_t targetYLeft;
    int32_t targetYRight;
    int32_t targetLineFeed;
  };

  enum class State : uint8_t {
    Idle,
    Running,
    Cancelling,
    Completed,
    Cancelled,
    Faulted,
  };

  bool beginBlock(const MotionBlock& block) {
    switch (block.kind) {
      case MotionBlockKind::ServoPress:
        return servo_.start(PressAction::Press, block.waitMs);
      case MotionBlockKind::ServoRelease:
        return servo_.start(PressAction::Release, block.waitMs);
      case MotionBlockKind::Wait:
        return true;
      case MotionBlockKind::CharacterRelease:
        return startLineFeed(block, config_.lineFeed.releaseDirection);
      case MotionBlockKind::LineFeed:
        return startLineFeed(block, config_.lineFeed.returnDirection);
      case MotionBlockKind::MoveXY:
        return startMoveXY(block);
    }
    return false;
  }

  bool startMoveXY(const MotionBlock& block) {
    const uint32_t xSteps = absoluteSteps(block.deltaSteps.x);
    const uint32_t ySteps = absoluteSteps(block.deltaSteps.yLeft);
    const bool hasX = xSteps > 0;
    const bool hasY = ySteps > 0;
    if (!hasX && !hasY) {
      return true;
    }
    if (!validateMoveXYBaseline(block)) {
      return false;
    }
    captureFeedbackTargets(block);
    const uint32_t primarySteps = xSteps > ySteps ? xSteps : ySteps;
    const uint16_t xRpm = scaledCoordinatedRpm(xSteps,
                                               primarySteps,
                                               block.profile.maxRpm,
                                               config_.motionRuntime.minimumCoordinatedRpm);
    const uint16_t yRpm = scaledCoordinatedRpm(ySteps,
                                               primarySteps,
                                               block.profile.maxRpm,
                                               config_.motionRuntime.minimumCoordinatedRpm);
    if (hasX && hasY) {
      const MotorDirection yLeftDirection = directionForSignedSteps(block.deltaSteps.yLeft);
      const MotorDirection yRightDirection = yLeftDirection == MotorDirection::Cw ? MotorDirection::Ccw
                                                                                  : MotorDirection::Cw;
      const EmmV5Driver::MoveRelativeCommand commands[] = {
        {config_.topology.xMotorId,
         directionForSignedSteps(block.deltaSteps.x),
         xRpm,
         block.profile.acceleration,
         xSteps,
         true},
        {config_.topology.yLeftMotorId, yLeftDirection, yRpm, block.profile.acceleration, ySteps, true},
        {config_.topology.yRightMotorId, yRightDirection, yRpm, block.profile.acceleration, ySteps, true},
      };
      return driver_.moveRelativeBatch(commands, sizeof(commands) / sizeof(commands[0]), true);
    }
    if (hasX && !driver_.moveRelative(config_.topology.xMotorId,
                                      directionForSignedSteps(block.deltaSteps.x),
                                      xRpm,
                                      block.profile.acceleration,
                                      xSteps,
                                      false)) {
      return false;
    }
    if (hasY && !yPair_.moveRelative(block.deltaSteps.yLeft, yRpm, block.profile.acceleration)) {
      return false;
    }
    return true;
  }

  bool startLineFeed(const MotionBlock& block, MotorDirection direction) {
    const uint32_t steps = absoluteSteps(block.deltaSteps.lineFeed);
    if (steps == 0) {
      return true;
    }
    captureFeedbackTargets(block);
    return driver_.moveRelative(config_.topology.lineFeedMotorId,
                                direction,
                                block.profile.maxRpm,
                                block.profile.acceleration,
                                steps,
                                false);
  }

  bool isBlockComplete(const MotionBlock& block) {
    const uint32_t elapsed = millis() - blockStartedAtMs_;
    if (elapsed > block.profile.timeoutMs) {
      stopAll();
      fail("motion_feedback_timeout", "Motion feedback timed out");
      return false;
    }
    if (block.kind == MotionBlockKind::Wait) {
      return elapsed >= block.waitMs;
    }
    if (block.kind == MotionBlockKind::ServoPress || block.kind == MotionBlockKind::ServoRelease) {
      servo_.tick();
      return !servo_.busy();
    }

    requestFeedback(block);
    if (feedbackSatisfied(block)) {
      ++completionSampleCount_;
      if (completionSampleCount_ >= config_.motionRuntime.completionSamples) {
        if (settleStartedAtMs_ == 0) {
          settleStartedAtMs_ = millis();
        }
        return millis() - settleStartedAtMs_ >= block.profile.settleMs;
      }
      return false;
    }
    completionSampleCount_ = 0;
    settleStartedAtMs_ = 0;
    return false;
  }

  void requestFeedback(const MotionBlock& block) {
    const uint32_t nowMs = millis();
    if (lastFeedbackPollMs_ != 0 && nowMs - lastFeedbackPollMs_ < config_.motionRuntime.motionPollIntervalMs) {
      return;
    }
    lastFeedbackPollMs_ = nowMs;
    if (block.kind == MotionBlockKind::MoveXY) {
      if (block.deltaSteps.x != 0) {
        driver_.requestInputPulseCount(config_.topology.xMotorId);
        driver_.requestVelocity(config_.topology.xMotorId);
      }
      if (block.deltaSteps.yLeft != 0) {
        driver_.requestInputPulseCount(config_.topology.yLeftMotorId);
        driver_.requestVelocity(config_.topology.yLeftMotorId);
        driver_.requestInputPulseCount(config_.topology.yRightMotorId);
        driver_.requestVelocity(config_.topology.yRightMotorId);
      }
    }
    if (block.kind == MotionBlockKind::LineFeed || block.kind == MotionBlockKind::CharacterRelease) {
      driver_.requestInputPulseCount(config_.topology.lineFeedMotorId);
      driver_.requestVelocity(config_.topology.lineFeedMotorId);
    }
  }

  bool feedbackSatisfied(const MotionBlock& block) {
    const uint32_t nowMs = millis();
    if (block.kind == MotionBlockKind::MoveXY) {
      if (block.deltaSteps.x != 0 && commandResponseFault(config_.topology.xMotorId)) {
        return false;
      }
      if (block.deltaSteps.x != 0 &&
          !motorAtTarget(config_.topology.xMotorId, activeFeedback_.targetX, nowMs)) {
        return false;
      }
      if (block.deltaSteps.yLeft != 0) {
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
    if (block.kind == MotionBlockKind::LineFeed || block.kind == MotionBlockKind::CharacterRelease) {
      if (commandResponseFault(config_.topology.lineFeedMotorId)) {
        return false;
      }
      return motorAtTarget(config_.topology.lineFeedMotorId, activeFeedback_.targetLineFeed, nowMs);
    }
    return true;
  }

  void finishBlock(const MotionBlock& block) {
    if (block.kind == MotionBlockKind::MoveXY) {
      currentPoint_ = block.targetMm;
    }
    if (block.kind == MotionBlockKind::LineFeed) {
      currentPoint_.xMm = config_.homePoint.xMm;
    }
    if (block.kind == MotionBlockKind::ServoPress) {
      ++activeTextIndex_;
    }
  }

  void fail(const char* code, const char* message) {
    faultCode_ = code;
    faultMessage_ = message;
    state_ = State::Faulted;
  }

  void captureFeedbackTargets(const MotionBlock& block) {
    const MotorState x = feedback_.get(config_.topology.xMotorId);
    const MotorState yLeft = feedback_.get(config_.topology.yLeftMotorId);
    const MotorState yRight = feedback_.get(config_.topology.yRightMotorId);
    const MotorState lineFeed = feedback_.get(config_.topology.lineFeedMotorId);
    activeFeedback_.initialX = x.inputPulseSteps;
    activeFeedback_.initialYLeft = yLeft.inputPulseSteps;
    activeFeedback_.initialYRight = yRight.inputPulseSteps;
    activeFeedback_.initialLineFeed = lineFeed.inputPulseSteps;
    activeFeedback_.targetX = x.inputPulseSteps + block.deltaSteps.x;
    activeFeedback_.targetYLeft = yLeft.inputPulseSteps + block.deltaSteps.yLeft;
    activeFeedback_.targetYRight = yRight.inputPulseSteps + block.deltaSteps.yRight;
    activeFeedback_.targetLineFeed = lineFeed.inputPulseSteps + block.deltaSteps.lineFeed;
  }

  bool motorAtTarget(uint8_t motorId, int32_t target, uint32_t nowMs) const {
    return motorAtTarget(feedback_.get(motorId), target, nowMs);
  }

  bool motorAtTarget(const MotorState& state, int32_t target, uint32_t nowMs) const {
    if (state.driverFault || !state.hasInputPulse || !state.hasVelocity || state.lastInputPulseMs == 0 ||
        state.lastVelocityMs == 0 || nowMs - state.lastInputPulseMs > 300 || nowMs - state.lastVelocityMs > 300) {
      return false;
    }
    const uint32_t error = absoluteSigned(state.inputPulseSteps - target);
    return error <= config_.motionRuntime.positionToleranceSteps &&
           fabs(state.velocityRpm) <= config_.motionRuntime.idleVelocityThresholdRpm;
  }

  bool feedbackFresh(const MotorState& state, uint32_t nowMs) const {
    return state.hasInputPulse && state.lastInputPulseMs != 0 && nowMs - state.lastInputPulseMs <= 300;
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
    if (blockStartedAtMs_ == 0) {
      return false;
    }
    if (state.lastConditionNotMetCommand != 0 && state.lastConditionNotMetMs >= blockStartedAtMs_) {
      stopAll();
      fail("motion_condition_not_met", "Motor rejected motion conditions");
      return true;
    }
    if (state.lastMalformedCommand != 0 && state.lastMalformedMs >= blockStartedAtMs_) {
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

  bool hasFreshInputPulse(uint8_t motorId, uint32_t nowMs) const {
    const MotorState state = feedback_.get(motorId);
    return state.hasInputPulse && state.lastInputPulseMs != 0 && nowMs - state.lastInputPulseMs <= kBaselineFreshMs;
  }

  bool validateMoveXYBaseline(const MotionBlock& block) {
    const uint32_t nowMs = millis();
    if (block.deltaSteps.x != 0 && !hasFreshInputPulse(config_.topology.xMotorId, nowMs)) {
      fail("motor_feedback_baseline_missing", "X motor input pulse baseline missing");
      return false;
    }
    if (block.deltaSteps.yLeft != 0 || block.deltaSteps.yRight != 0) {
      if (!hasFreshInputPulse(config_.topology.yLeftMotorId, nowMs)) {
        fail("motor_feedback_baseline_missing", "Y left motor input pulse baseline missing");
        return false;
      }
      if (!hasFreshInputPulse(config_.topology.yRightMotorId, nowMs)) {
        fail("motor_feedback_baseline_missing", "Y right motor input pulse baseline missing");
        return false;
      }
    }
    return true;
  }

  const TypingConfig& config_;
  EmmV5Driver& driver_;
  ServoPressHal& servo_;
  MotorFeedbackStore& feedback_;
  YPairController yPair_;
  State state_;
  const MotionBlock* blocks_;
  size_t blockCount_;
  size_t currentBlock_;
  uint32_t blockStartedAtMs_;
  uint32_t settleStartedAtMs_;
  const char* faultCode_;
  const char* faultMessage_;
  MachinePointMm currentPoint_;
  size_t activeTextIndex_;
  ActiveFeedbackTargets activeFeedback_;
  uint8_t completionSampleCount_;
  uint32_t lastFeedbackPollMs_;
};

}  // namespace auto_typer
