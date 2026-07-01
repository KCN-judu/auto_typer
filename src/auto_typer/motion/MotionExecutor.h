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
        lastFeedbackRequestMs_(0) {}

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
        fail("motion_command_rejected", "Motion command rejected");
        return;
      }
      blockStartedAtMs_ = millis();
      settleStartedAtMs_ = 0;
      completionSampleCount_ = 0;
      lastFeedbackRequestMs_ = 0;
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
    const bool sync = hasX && hasY;
    if (hasX && !driver_.moveRelative(config_.topology.xMotorId,
                                      directionForSignedSteps(block.deltaSteps.x),
                                      xRpm,
                                      block.profile.acceleration,
                                      xSteps,
                                      sync)) {
      return false;
    }
    if (hasY && !yPair_.moveRelative(block.deltaSteps.yLeft, yRpm, block.profile.acceleration)) {
      return false;
    }
    if (sync && !driver_.triggerSynchronousMotion(config_.topology.xMotorId)) {
      return false;
    }
    if (hasY && !yPair_.trigger()) {
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
    if (lastFeedbackRequestMs_ != 0 && nowMs - lastFeedbackRequestMs_ < config_.motionRuntime.motionPollIntervalMs) {
      return;
    }
    lastFeedbackRequestMs_ = nowMs;
    if (block.kind == MotionBlockKind::MoveXY) {
      if (block.deltaSteps.x != 0) {
        driver_.requestPosition(config_.topology.xMotorId);
        driver_.requestVelocity(config_.topology.xMotorId);
      }
      if (block.deltaSteps.yLeft != 0) {
        driver_.requestPosition(config_.topology.yLeftMotorId);
        driver_.requestVelocity(config_.topology.yLeftMotorId);
        driver_.requestPosition(config_.topology.yRightMotorId);
        driver_.requestVelocity(config_.topology.yRightMotorId);
      }
    }
    if (block.kind == MotionBlockKind::LineFeed || block.kind == MotionBlockKind::CharacterRelease) {
      driver_.requestPosition(config_.topology.lineFeedMotorId);
      driver_.requestVelocity(config_.topology.lineFeedMotorId);
    }
  }

  bool feedbackSatisfied(const MotionBlock& block) {
    const uint32_t nowMs = millis();
    if (block.kind == MotionBlockKind::MoveXY) {
      if (block.deltaSteps.x != 0 &&
          !motorAtTarget(config_.topology.xMotorId, activeFeedback_.targetX, nowMs)) {
        return false;
      }
      if (block.deltaSteps.yLeft != 0) {
        const MotorState left = feedback_.get(config_.topology.yLeftMotorId);
        const MotorState right = feedback_.get(config_.topology.yRightMotorId);
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
    activeFeedback_.initialX = x.observedPositionSteps;
    activeFeedback_.initialYLeft = yLeft.observedPositionSteps;
    activeFeedback_.initialYRight = yRight.observedPositionSteps;
    activeFeedback_.initialLineFeed = lineFeed.observedPositionSteps;
    activeFeedback_.targetX = x.observedPositionSteps + block.deltaSteps.x;
    activeFeedback_.targetYLeft = yLeft.observedPositionSteps + block.deltaSteps.yLeft;
    activeFeedback_.targetYRight = yRight.observedPositionSteps + block.deltaSteps.yRight;
    activeFeedback_.targetLineFeed = lineFeed.observedPositionSteps + block.deltaSteps.lineFeed;
  }

  bool motorAtTarget(uint8_t motorId, int32_t target, uint32_t nowMs) const {
    return motorAtTarget(feedback_.get(motorId), target, nowMs);
  }

  bool motorAtTarget(const MotorState& state, int32_t target, uint32_t nowMs) const {
    if (state.fault || state.lastFeedbackMs == 0 || nowMs - state.lastFeedbackMs > 300) {
      return false;
    }
    const uint32_t error = absoluteSigned(state.observedPositionSteps - target);
    return error <= config_.motionRuntime.positionToleranceSteps &&
           fabs(state.velocityRpm) <= config_.motionRuntime.idleVelocityThresholdRpm;
  }

  bool feedbackFresh(const MotorState& state, uint32_t nowMs) const {
    return state.lastFeedbackMs != 0 && nowMs - state.lastFeedbackMs <= 300;
  }

  bool yPairSkewExceeded(const MotorState& left, const MotorState& right) const {
    const int32_t skew = (left.observedPositionSteps - activeFeedback_.initialYLeft) +
                         (right.observedPositionSteps - activeFeedback_.initialYRight);
    return absoluteSigned(skew) > config_.motionRuntime.ySkewToleranceSteps;
  }

  static uint32_t absoluteSigned(int32_t value) {
    return value < 0 ? static_cast<uint32_t>(-value) : static_cast<uint32_t>(value);
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
  uint32_t lastFeedbackRequestMs_;
};

}  // namespace auto_typer
