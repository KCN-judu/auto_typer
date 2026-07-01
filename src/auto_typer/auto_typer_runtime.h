#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "auto_typer_config.h"
#include "hal_display.h"
#include "hal_emm_can_motion.h"
#include "hal_servo_press.h"
#include "keymap_feiyu200.h"
#include "keymap_store.h"
#include "typing_logic.h"

namespace auto_typer {

class AutoTyperApplication {
 public:
  AutoTyperApplication(const TypingConfig& config,
                       DisplayHal& display,
                       EmmCanMotionHal& motion,
                       ServoPressHal& servo,
                       Print& log)
      : config_(config),
        display_(display),
        motion_(motion),
        servo_(servo),
        log_(log),
        keymapCount_(0),
        currentPoint_(config.homePoint),
        jobState_(JobState::None),
        jobId_(0),
        activeTextLength_(0),
        currentIndex_(0),
        currentStep_(0),
        xMotorPositionSteps_(0),
        yMotorPositionSteps_(0),
        lineFeedMotorPositionSteps_(0),
        jobStartXMotorPositionSteps_(0),
        jobStartYMotorPositionSteps_(0),
        jobStartLineFeedMotorPositionSteps_(0),
        activePlan_{},
        lastPlanStatus_(PlanStatus::Ok),
        failedKey_('\0'),
        faulted_(false) {}

  void setup() {
    buildKeymap();
    const bool loadedKeymap = keymapStore_.load(keymap_, sizeof(keymap_) / sizeof(keymap_[0]), keymapCount_);
    if (!loadedKeymap || keymapStore_.layoutVersion() != kFeiyu200KeymapLayoutVersion) {
      buildKeymap();
      keymapStore_.save(keymap_, keymapCount_, kFeiyu200KeymapLayoutVersion);
    }
    printBanner();

    Wire.begin(6, 7);
    if (!display_.begin()) {
      log_.println("OLED init failed");
    }

    if (!servo_.begin()) {
      log_.println("Servo driver init failed");
    } else {
      log_.println("Servo driver initialized with outputs disabled");
    }

    delay(2000);
    if (!motion_.begin()) {
      log_.println("TWAI init failed");
      showError();
      return;
    }

    if (!prepareMotors()) {
      log_.println("Motor preparation failed");
      showError();
      return;
    }
    if (!executeInitialLineFeed()) {
      log_.println("Initial carriage return failed");
      showError();
      return;
    }
    log_.println("Auto typer ready");
    display_.showStatus(DisplayStatus::Idle);
  }

  void tick() {
    if (jobState_ != JobState::Queued || !motion_.ready()) {
      return;
    }

    log_.print("Executing steps: ");
    log_.println(activePlan_.count);
    display_.showStatus(DisplayStatus::Printing);
    jobState_ = JobState::Running;
    if (!executePlan(activePlan_)) {
      log_.println("Execution stopped on command failure");
      jobState_ = JobState::Failed;
      faulted_ = true;
      showError();
      return;
    }
    if (!returnToInitialPosition()) {
      log_.println("Return to initial position failed");
      jobState_ = JobState::Failed;
      faulted_ = true;
      showError();
      return;
    }
    jobState_ = JobState::Completed;
    log_.println("Typing sequence complete");
    display_.showStatus(DisplayStatus::Complete);
  }

  bool submitTextJob(const char* text) {
    if (jobState_ == JobState::Queued || jobState_ == JobState::Running) {
      return false;
    }

    activePlan_ = planText(text, keymap_, keymapCount_, config_);
    lastPlanStatus_ = activePlan_.status;
    failedKey_ = activePlan_.failedKey;
    activeTextLength_ = strlen(text);
    currentIndex_ = 0;
    currentStep_ = 0;
    captureJobStartMotorPositions();
    ++jobId_;

    log_.print("Plan status: ");
    log_.println(statusText(activePlan_.status));
    if (activePlan_.status != PlanStatus::Ok) {
      jobState_ = JobState::Failed;
      showError();
      return false;
    }

    jobState_ = JobState::Queued;
    display_.showStatus(DisplayStatus::Printing);
    return true;
  }

  bool cancelCurrentJob() {
    if (jobState_ != JobState::Queued && jobState_ != JobState::Running) {
      return false;
    }
    stopAllMotors();
    servo_.release();
    jobState_ = JobState::Cancelled;
    display_.showStatus(DisplayStatus::Idle);
    return true;
  }

  bool emergencyStop() {
    stopAllMotors();
    servo_.release();
    jobState_ = JobState::Failed;
    faulted_ = true;
    showError();
    return true;
  }

  bool debugMotorEnable(uint8_t motorId, bool enabled, bool sync) {
    if (jobState_ == JobState::Running || jobState_ == JobState::Queued) {
      return false;
    }
    if (isYMotorGroup(motorId)) {
      if (enabled) {
        return motion_.enableMotor(config_.topology.yLeftMotorId, false) &&
               motion_.enableMotor(config_.topology.yRightMotorId, false);
      }
      return motion_.disableMotor(config_.topology.yLeftMotorId, false) &&
             motion_.disableMotor(config_.topology.yRightMotorId, false);
    }
    return enabled ? motion_.enableMotor(motorId, sync) : motion_.disableMotor(motorId, sync);
  }

  bool debugMotorMoveRelative(uint8_t motorId,
                              MotorDirection direction,
                              uint16_t rpm,
                              uint8_t acceleration,
                              uint32_t steps,
                              bool sync) {
    if (jobState_ == JobState::Running || jobState_ == JobState::Queued) {
      return false;
    }
    if (isYMotorGroup(motorId)) {
      return moveYMotorGroup(direction, rpm, acceleration, steps, 0);
    }
    if (motorId == config_.topology.xMotorId) {
      return moveXMotor(direction, rpm, acceleration, steps, sync, 0);
    }
    if (motorId == config_.topology.lineFeedMotorId) {
      return moveLineFeedMotor(direction, rpm, acceleration, steps, sync, 0);
    }
    return moveUntrackedMotor(motorId, direction, rpm, acceleration, steps, sync, 0);
  }

  bool debugMotorStop(uint8_t motorId, bool sync) {
    if (isYMotorGroup(motorId)) {
      return motion_.stopNow(config_.topology.yLeftMotorId, false) &&
             motion_.stopNow(config_.topology.yRightMotorId, false);
    }
    return motion_.stopNow(motorId, sync);
  }

  bool debugServo(PressAction action, uint16_t dwellMs = 0) {
    if (jobState_ == JobState::Running || jobState_ == JobState::Queued) {
      return false;
    }
    const bool useCustomDwell = dwellMs > 0;
    switch (action) {
      case PressAction::Press:
        return useCustomDwell ? servo_.apply(PressAction::Press, dwellMs) : servo_.press();
      case PressAction::Release:
      default:
        return useCustomDwell ? servo_.apply(PressAction::Release, dwellMs) : servo_.release();
    }
  }

  bool debugServoNeutral(uint16_t dwellMs = 0) {
    if (jobState_ == JobState::Running || jobState_ == JobState::Queued) {
      return false;
    }
    return dwellMs > 0 ? servo_.neutral(dwellMs) : servo_.neutral();
  }

  bool upsertKeyBinding(char key, MachinePointMm point) {
    const char normalized = normalizeKey(key);
    for (size_t i = 0; i < keymapCount_; ++i) {
      if (keymap_[i].key == normalized) {
        keymap_[i].point = point;
        return keymapStore_.save(keymap_, keymapCount_, kFeiyu200KeymapLayoutVersion);
      }
    }
    return appendKey(keymap_, sizeof(keymap_) / sizeof(keymap_[0]), keymapCount_, normalized, point) &&
           keymapStore_.save(keymap_, keymapCount_, kFeiyu200KeymapLayoutVersion);
  }

  bool replaceKeymap(const KeyBinding* bindings, size_t count) {
    if (count == 0 || count > sizeof(keymap_) / sizeof(keymap_[0])) {
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      keymap_[i] = bindings[i];
    }
    keymapCount_ = count;
    return keymapStore_.save(keymap_, keymapCount_, kFeiyu200KeymapLayoutVersion);
  }

  const KeyBinding* keymap() const {
    return keymap_;
  }

  size_t keymapCount() const {
    return keymapCount_;
  }

  uint32_t keymapVersion() const {
    return keymapStore_.version();
  }

  JobSnapshot snapshot() const {
    return {jobState_,
            jobId_,
            activeTextLength_,
            currentIndex_,
            currentStep_,
            activePlan_.count,
            currentPoint_,
            lastPlanStatus_,
            failedKey_};
  }

  DeviceMode mode() const {
    if (faulted_) {
      return DeviceMode::Faulted;
    }
    return jobState_ == JobState::Running || jobState_ == JobState::Queued ? DeviceMode::Running : DeviceMode::Idle;
  }

  bool servoReady() const {
    return servo_.ready();
  }

  bool motionReady() const {
    return motion_.ready();
  }

 private:
  void buildKeymap() {
    keymapCount_ = buildFeiyu200Keymap(keymap_, sizeof(keymap_) / sizeof(keymap_[0]));
  }

  void printBanner() {
    log_.println();
    log_.println("==== AUTO TYPER ====");
    log_.println("Machine: Feiyu 200");
    log_.print("CAN bitrate: ");
    log_.println(config_.canBus.bitrate);
    log_.print("Steps per mm: ");
    log_.println(stepsPerMm(config_.calibration));
    log_.print("Keymap entries: ");
    log_.println(keymapCount_);
    log_.print("Sample text: ");
    log_.println(config_.sampleText);
  }

  bool prepareMotors() {
    const uint8_t motors[] = {
      config_.topology.xMotorId,
      config_.topology.yLeftMotorId,
      config_.topology.yRightMotorId,
      config_.topology.lineFeedMotorId,
    };

    for (uint8_t motor : motors) {
      if (!motion_.setClosedLoopControlMode(motor)) {
        return false;
      }
      delay(80);
      if (!motion_.enableMotor(motor)) {
        return false;
      }
      delay(80);
    }
    return true;
  }

  bool executePlan(const TypingPlan& plan) {
    for (size_t i = 0; i < plan.count; ++i) {
      currentStep_ = i + 1;
      const TypingStep& step = plan.steps[i];
      switch (step.kind) {
        case TypingStepKind::Release:
          if (!servo_.release()) {
            return false;
          }
          break;
        case TypingStepKind::Press:
          if (!servo_.press()) {
            return false;
          }
          ++currentIndex_;
          break;
        case TypingStepKind::CharacterRelease:
          if (!executeCharacterRelease()) {
            return false;
          }
          break;
        case TypingStepKind::MoveTo:
          if (!executeMove(step.point)) {
            return false;
          }
          currentPoint_ = step.point;
          break;
        case TypingStepKind::LineFeed:
          if (!executeLineFeed()) {
            return false;
          }
          break;
        case TypingStepKind::Wait:
          delay(step.waitMs);
          break;
      }
    }
    return true;
  }

  enum class MotorIdlePollResult : uint8_t {
    Waiting,
    Completed,
    Failed,
  };

  struct MotorIdleWaitState {
    uint8_t motorId;
    uint32_t steps;
    uint16_t rpm;
    uint32_t startedAtMs;
    uint32_t timeoutMs;
    uint32_t earliestCompletionMs;
    uint8_t idleSamples;
    bool completed;
    bool usingEstimatedWait;
    bool loggedEstimatedFallback;
  };

  bool executeMove(const MachinePointMm& target) {
    const MovePlan move = planMoveTo(currentPoint_, target, config_.calibration);
    return executeCombinedXYMove(move);
  }

  bool moveYMotorGroup(MotorDirection direction,
                       uint16_t rpm,
                       uint8_t acceleration,
                       uint32_t steps,
                       uint16_t settleMs) {
    motion_.drainRx();
    if (!startYMotorGroup(direction, rpm, acceleration, steps)) {
      return false;
    }
    if (!triggerYMotorGroup()) {
      stopYMotorGroup();
      return false;
    }
    if (!waitForMotorIdle(config_.topology.yLeftMotorId, steps, rpm, settleMs) ||
        !waitForMotorIdle(config_.topology.yRightMotorId, steps, rpm, settleMs)) {
      stopYMotorGroup();
      return false;
    }
    trackSignedMotorMove(yMotorPositionSteps_, direction, steps);
    return true;
  }

  bool executeCombinedXYMove(const MovePlan& move) {
    const bool hasX = move.x.steps > 0;
    const bool hasY = move.y.steps > 0;
    if (!hasX && !hasY) {
      return true;
    }

    motion_.drainRx();
    const bool synchronizeX = hasX && hasY;
    if (hasX && !startXMotor(move.x.direction,
                             config_.xProfile.rpm,
                             config_.xProfile.acceleration,
                             move.x.steps,
                             synchronizeX)) {
      return false;
    }
    if (hasY && !startYMotorGroup(move.y.direction,
                                  config_.yProfile.rpm,
                                  config_.yProfile.acceleration,
                                  move.y.steps)) {
      if (hasX) {
        motion_.stopNow(config_.topology.xMotorId);
      }
      return false;
    }

    bool triggered = true;
    if (hasX && synchronizeX) {
      triggered = motion_.triggerSynchronousMotion(config_.topology.xMotorId);
    }
    if (hasY) {
      triggered = triggerYMotorGroup() && triggered;
    }
    if (!triggered) {
      if (hasX) {
        motion_.stopNow(config_.topology.xMotorId);
      }
      if (hasY) {
        stopYMotorGroup();
      }
      return false;
    }

    return waitForCombinedXYMove(move, hasX, hasY);
  }

  bool startXMotor(MotorDirection direction, uint16_t rpm, uint8_t acceleration, uint32_t steps, bool sync) {
    return motion_.moveRelative(config_.topology.xMotorId, direction, rpm, acceleration, steps, sync);
  }

  bool startYMotorGroup(MotorDirection direction, uint16_t rpm, uint8_t acceleration, uint32_t steps) {
    const MotorDirection pairedDirection = invertDirection(direction);
    if (!motion_.moveRelative(config_.topology.yLeftMotorId,
                              direction,
                              rpm,
                              acceleration,
                              steps,
                              true)) {
      return false;
    }
    if (!motion_.moveRelative(config_.topology.yRightMotorId,
                              pairedDirection,
                              rpm,
                              acceleration,
                              steps,
                              true)) {
      return false;
    }
    return true;
  }

  bool triggerYMotorGroup() {
    return motion_.triggerSynchronousMotion(config_.topology.yLeftMotorId) &&
           motion_.triggerSynchronousMotion(config_.topology.yRightMotorId);
  }

  void stopYMotorGroup() {
    motion_.stopNow(config_.topology.yLeftMotorId);
    motion_.stopNow(config_.topology.yRightMotorId);
  }

  static bool isYMotorGroup(uint8_t motorId) {
    return motorId == 23;
  }

  static MotorDirection invertDirection(MotorDirection direction) {
    return direction == MotorDirection::Cw ? MotorDirection::Ccw : MotorDirection::Cw;
  }

  bool moveXMotor(MotorDirection direction,
                  uint16_t rpm,
                  uint8_t acceleration,
                  uint32_t steps,
                  bool sync,
                  uint16_t settleMs) {
    if (!moveTrackedMotor(config_.topology.xMotorId,
                          direction,
                          rpm,
                          acceleration,
                          steps,
                          sync,
                          settleMs,
                          xMotorPositionSteps_)) {
      return false;
    }
    return true;
  }

  bool executeLineFeed() {
    const uint32_t returnSteps = lineFeedReturnSteps();
    if (returnSteps > 0 &&
        !moveLineFeedMotor(config_.lineFeed.returnDirection,
                           config_.lineFeed.rpm,
                           config_.lineFeed.acceleration,
                           returnSteps,
                           false,
                           config_.lineFeed.settleMs)) {
      return false;
    }
    if (config_.lineFeed.returnReleaseSteps > 0 &&
        !moveLineFeedMotor(config_.lineFeed.releaseDirection,
                           config_.lineFeed.rpm,
                           config_.lineFeed.acceleration,
                           config_.lineFeed.returnReleaseSteps,
                           false,
                           config_.lineFeed.settleMs)) {
      return false;
    }
    currentPoint_.xMm = config_.homePoint.xMm;
    return true;
  }

  bool executeInitialLineFeed() {
    constexpr uint16_t kInitialLineFeedReturnExtraSettleMs = 1500;
    if (config_.lineFeed.returnTotalSteps > 0 &&
        !moveLineFeedMotor(config_.lineFeed.returnDirection,
                           config_.lineFeed.rpm,
                           config_.lineFeed.acceleration,
                           config_.lineFeed.returnTotalSteps,
                           false,
                           config_.lineFeed.settleMs + kInitialLineFeedReturnExtraSettleMs)) {
      return false;
    }
    currentPoint_.xMm = config_.homePoint.xMm;
    if (config_.lineFeed.returnReleaseSteps > 0 &&
        !moveLineFeedMotor(config_.lineFeed.releaseDirection,
                           config_.lineFeed.rpm,
                           config_.lineFeed.acceleration,
                           config_.lineFeed.returnReleaseSteps,
                           false,
                           config_.lineFeed.settleMs)) {
      return false;
    }
    return true;
  }

  bool executeCharacterRelease() {
    if (config_.lineFeed.characterReleaseSteps == 0) {
      return true;
    }
    return moveLineFeedMotor(config_.lineFeed.releaseDirection,
                             config_.lineFeed.rpm,
                             config_.lineFeed.acceleration,
                             config_.lineFeed.characterReleaseSteps,
                             false,
                             config_.lineFeed.characterReleaseSettleMs);
  }

  bool moveLineFeedMotor(MotorDirection direction,
                         uint16_t rpm,
                         uint8_t acceleration,
                         uint32_t steps,
                         bool sync,
                         uint16_t settleMs) {
    const bool sent = motion_.moveRelative(config_.topology.lineFeedMotorId,
                                           direction,
                                           rpm,
                                           acceleration,
                                           steps,
                                           sync);
    if (!sent) {
      return false;
    }
    if (sync && !motion_.triggerSynchronousMotion(config_.topology.lineFeedMotorId)) {
      return false;
    }
    if (!waitForMotorIdle(config_.topology.lineFeedMotorId, steps, rpm, settleMs)) {
      return false;
    }
    trackSignedMotorMove(lineFeedMotorPositionSteps_, direction, steps);
    return true;
  }

  uint32_t lineFeedReturnSteps() const {
    if (lineFeedMotorPositionSteps_ <= 0) {
      return config_.lineFeed.returnTotalSteps;
    }
    const uint32_t currentSteps = static_cast<uint32_t>(lineFeedMotorPositionSteps_);
    return currentSteps >= config_.lineFeed.returnTotalSteps ? 0 : config_.lineFeed.returnTotalSteps - currentSteps;
  }

  void trackSignedMotorMove(int32_t& positionSteps, MotorDirection direction, uint32_t steps) {
    if (steps > static_cast<uint32_t>(INT32_MAX)) {
      positionSteps = direction == MotorDirection::Cw ? INT32_MAX : INT32_MIN;
      return;
    }
    const int32_t signedSteps = static_cast<int32_t>(steps);
    if (direction == MotorDirection::Cw) {
      positionSteps += signedSteps;
    } else {
      positionSteps -= signedSteps;
    }
  }

  bool returnToInitialPosition() {
    if (!conservativeReturnXMotor()) {
      return false;
    }
    if (!conservativeReturnYMotorGroup()) {
      return false;
    }
    currentPoint_ = config_.homePoint;
    return true;
  }

  void captureJobStartMotorPositions() {
    jobStartXMotorPositionSteps_ = xMotorPositionSteps_;
    jobStartYMotorPositionSteps_ = yMotorPositionSteps_;
    jobStartLineFeedMotorPositionSteps_ = lineFeedMotorPositionSteps_;
  }

  bool conservativeReturnXMotor() {
    if (!config_.xReturn.enabled) {
      return true;
    }
    const int32_t delta = xMotorPositionSteps_ - jobStartXMotorPositionSteps_;
    const uint32_t steps = conservativeReturnSteps(delta, config_.xReturn.errorSteps);
    if (steps == 0) {
      return true;
    }
    const MotorDirection direction = delta > 0 ? MotorDirection::Ccw : MotorDirection::Cw;
    return moveXMotor(direction,
                      config_.xReturn.rpm,
                      config_.xReturn.acceleration,
                      steps,
                      false,
                      config_.xReturn.settleMs);
  }

  bool conservativeReturnYMotorGroup() {
    if (!config_.yReturn.enabled) {
      return true;
    }
    const int32_t delta = yMotorPositionSteps_ - jobStartYMotorPositionSteps_;
    const uint32_t steps = conservativeReturnSteps(delta, config_.yReturn.errorSteps);
    if (steps == 0) {
      return true;
    }
    const MotorDirection direction = delta > 0 ? MotorDirection::Ccw : MotorDirection::Cw;
    if (!moveYMotorGroup(direction, config_.yReturn.rpm, config_.yReturn.acceleration, steps, config_.yReturn.settleMs)) {
      return false;
    }
    return true;
  }

  bool returnLineFeedMotorToJobStart() {
    const int32_t delta = lineFeedMotorPositionSteps_ - jobStartLineFeedMotorPositionSteps_;
    if (delta == 0) {
      return true;
    }
    const MotorDirection direction = delta > 0 ? MotorDirection::Ccw : MotorDirection::Cw;
    const uint32_t steps = absoluteTrackedSteps(delta);
    return moveLineFeedMotor(direction,
                             config_.lineFeed.rpm,
                             config_.lineFeed.acceleration,
                             steps,
                             false,
                             config_.lineFeed.settleMs);
  }

  static uint32_t absoluteTrackedSteps(int32_t positionSteps) {
    const int64_t signedPosition = positionSteps;
    return static_cast<uint32_t>(signedPosition > 0 ? signedPosition : -signedPosition);
  }

  static uint32_t conservativeReturnSteps(int32_t delta, uint32_t errorSteps) {
    const uint32_t steps = absoluteTrackedSteps(delta);
    return steps > errorSteps ? steps - errorSteps : 0;
  }

  bool moveTrackedMotor(uint8_t motorId,
                        MotorDirection direction,
                        uint16_t rpm,
                        uint8_t acceleration,
                        uint32_t steps,
                        bool sync,
                        uint16_t settleMs,
                        int32_t& positionSteps) {
    if (!moveUntrackedMotor(motorId, direction, rpm, acceleration, steps, sync, settleMs)) {
      return false;
    }
    trackSignedMotorMove(positionSteps, direction, steps);
    return true;
  }

  bool moveUntrackedMotor(uint8_t motorId,
                          MotorDirection direction,
                          uint16_t rpm,
                          uint8_t acceleration,
                          uint32_t steps,
                          bool sync,
                          uint16_t settleMs) {
    motion_.drainRx();
    if (!motion_.moveRelative(motorId, direction, rpm, acceleration, steps, sync)) {
      return false;
    }
    if (sync && !motion_.triggerSynchronousMotion(motorId)) {
      return false;
    }
    return waitForMotorIdle(motorId, steps, rpm, settleMs);
  }

  bool waitForMotorIdle(uint8_t motorId, uint32_t steps, uint16_t rpm, uint16_t settleMs) {
    MotorIdleWaitState state = makeMotorIdleWaitState(motorId, steps, rpm);
    while (true) {
      const MotorIdlePollResult result = pollMotorIdle(state);
      if (result == MotorIdlePollResult::Completed) {
        if (settleMs > 0) {
          delay(settleMs);
        }
        return true;
      }
      if (result == MotorIdlePollResult::Waiting) {
        continue;
      }
      log_.print("Motor wait failed id=");
      log_.print(motorId);
      log_.print(" status=");
      log_.println(waitStatusText(EmmCanMotionHal::WaitStatus::Timeout));
      return false;
    }
  }

  bool waitForCombinedXYMove(const MovePlan& move, bool hasX, bool hasY) {
    constexpr uint32_t kNoSettleStarted = UINT32_MAX;

    MotorIdleWaitState xWait = makeMotorIdleWaitState(config_.topology.xMotorId, move.x.steps, config_.xProfile.rpm);
    MotorIdleWaitState yLeftWait =
        makeMotorIdleWaitState(config_.topology.yLeftMotorId, move.y.steps, config_.yProfile.rpm);
    MotorIdleWaitState yRightWait =
        makeMotorIdleWaitState(config_.topology.yRightMotorId, move.y.steps, config_.yProfile.rpm);

    bool xMotorDone = !hasX;
    bool yLeftMotorDone = !hasY;
    bool yRightMotorDone = !hasY;
    bool xTaskDone = !hasX;
    bool yTaskDone = !hasY;
    uint32_t xSettleStartedAt = kNoSettleStarted;
    uint32_t ySettleStartedAt = kNoSettleStarted;

    while (!xTaskDone || !yTaskDone) {
      bool madeProgress = false;

      if (hasX && !xMotorDone) {
        const MotorIdlePollResult result = pollMotorIdle(xWait);
        if (result == MotorIdlePollResult::Failed) {
          stopIncompleteXYTasks(hasX, xMotorDone, hasY, yLeftMotorDone && yRightMotorDone);
          log_.print("Motor wait failed id=");
          log_.print(config_.topology.xMotorId);
          log_.print(" status=");
          log_.println(waitStatusText(EmmCanMotionHal::WaitStatus::Timeout));
          return false;
        }
        if (result == MotorIdlePollResult::Completed) {
          xMotorDone = true;
          xSettleStartedAt = millis();
          madeProgress = true;
        }
      }

      if (hasY && !yLeftMotorDone) {
        const MotorIdlePollResult result = pollMotorIdle(yLeftWait);
        if (result == MotorIdlePollResult::Failed) {
          stopIncompleteXYTasks(hasX, xMotorDone, hasY, yLeftMotorDone && yRightMotorDone);
          log_.print("Motor wait failed id=");
          log_.print(config_.topology.yLeftMotorId);
          log_.print(" status=");
          log_.println(waitStatusText(EmmCanMotionHal::WaitStatus::Timeout));
          return false;
        }
        if (result == MotorIdlePollResult::Completed) {
          yLeftMotorDone = true;
          madeProgress = true;
        }
      }

      if (hasY && !yRightMotorDone) {
        const MotorIdlePollResult result = pollMotorIdle(yRightWait);
        if (result == MotorIdlePollResult::Failed) {
          stopIncompleteXYTasks(hasX, xMotorDone, hasY, yLeftMotorDone && yRightMotorDone);
          log_.print("Motor wait failed id=");
          log_.print(config_.topology.yRightMotorId);
          log_.print(" status=");
          log_.println(waitStatusText(EmmCanMotionHal::WaitStatus::Timeout));
          return false;
        }
        if (result == MotorIdlePollResult::Completed) {
          yRightMotorDone = true;
          madeProgress = true;
        }
      }

      if (hasY && yLeftMotorDone && yRightMotorDone && ySettleStartedAt == kNoSettleStarted) {
        ySettleStartedAt = millis();
        madeProgress = true;
      }

      if (hasX && !xTaskDone && xSettleStartedAt != kNoSettleStarted &&
          millis() - xSettleStartedAt >= config_.xProfile.settleMs) {
        xTaskDone = true;
        trackSignedMotorMove(xMotorPositionSteps_, move.x.direction, move.x.steps);
        madeProgress = true;
      }

      if (hasY && !yTaskDone && ySettleStartedAt != kNoSettleStarted &&
          millis() - ySettleStartedAt >= config_.yProfile.settleMs) {
        yTaskDone = true;
        trackSignedMotorMove(yMotorPositionSteps_, move.y.direction, move.y.steps);
        madeProgress = true;
      }

      if (!madeProgress && (xMotorDone || !hasX) && (yLeftMotorDone || !hasY) && (yRightMotorDone || !hasY)) {
        delay(1);
      }
    }

    return true;
  }

  MotorIdleWaitState makeMotorIdleWaitState(uint8_t motorId, uint32_t steps, uint16_t rpm) const {
    return {motorId, steps, rpm, millis(), motorWaitTimeoutMs(steps, rpm), moveDurationMs(steps, rpm), 0, false, false, false};
  }

  MotorIdlePollResult pollMotorIdle(MotorIdleWaitState& state) {
    constexpr uint32_t kPollIntervalMs = 25;
    constexpr uint8_t kRequiredIdleSamples = 3;
    constexpr float kIdleVelocityRpm = 1.0f;

    if (state.completed) {
      return MotorIdlePollResult::Completed;
    }

    const uint32_t elapsedMs = millis() - state.startedAtMs;
    if (state.usingEstimatedWait) {
      if (elapsedMs >= state.earliestCompletionMs) {
        state.completed = true;
        return MotorIdlePollResult::Completed;
      }
      return MotorIdlePollResult::Waiting;
    }

    if (elapsedMs >= state.timeoutMs) {
      return MotorIdlePollResult::Failed;
    }

    if (!motion_.requestVelocity(state.motorId)) {
      switchMotorWaitToEstimated(state);
      return MotorIdlePollResult::Waiting;
    }
    delay(kPollIntervalMs);

    const EmmCanMotionHal::MotorFeedback feedback = motion_.pollFeedback(state.motorId);
    if (feedback.transportError) {
      switchMotorWaitToEstimated(state);
      return MotorIdlePollResult::Waiting;
    }
    if (!feedback.hasVelocity) {
      state.idleSamples = 0;
      return MotorIdlePollResult::Waiting;
    }

    const uint32_t elapsedAfterPollMs = millis() - state.startedAtMs;
    const float velocity = feedback.velocityRpm < 0.0f ? -feedback.velocityRpm : feedback.velocityRpm;
    if (velocity <= kIdleVelocityRpm && elapsedAfterPollMs >= state.earliestCompletionMs) {
      ++state.idleSamples;
      if (state.idleSamples >= kRequiredIdleSamples) {
        state.completed = true;
        return MotorIdlePollResult::Completed;
      }
    } else {
      state.idleSamples = 0;
    }
    return MotorIdlePollResult::Waiting;
  }

  void switchMotorWaitToEstimated(MotorIdleWaitState& state) {
    state.usingEstimatedWait = true;
    if (!state.loggedEstimatedFallback) {
      log_.print("Motor feedback unavailable id=");
      log_.print(state.motorId);
      log_.println("; using estimated wait");
      state.loggedEstimatedFallback = true;
    }
  }

  void stopIncompleteXYTasks(bool hasX, bool xMotorDone, bool hasY, bool yMotorDone) {
    if (hasX && !xMotorDone) {
      motion_.stopNow(config_.topology.xMotorId);
    }
    if (hasY && !yMotorDone) {
      stopYMotorGroup();
    }
  }

  void waitForEstimatedMove(uint32_t steps, uint16_t rpm, uint16_t settleMs) const {
    delay(moveDurationMs(steps, rpm) + settleMs);
  }

  uint32_t motorWaitTimeoutMs(uint32_t steps, uint16_t rpm) const {
    constexpr uint32_t kMinimumTimeoutMs = 1500;
    constexpr uint32_t kTimeoutSlackMs = 2500;
    const uint32_t estimatedMs = moveDurationMs(steps, rpm);
    const uint64_t timeoutMs = static_cast<uint64_t>(estimatedMs) * 4ULL + kTimeoutSlackMs;
    return timeoutMs < kMinimumTimeoutMs ? kMinimumTimeoutMs : static_cast<uint32_t>(timeoutMs);
  }

  uint32_t moveDurationMs(uint32_t steps, uint16_t rpm) const {
    if (steps == 0 || rpm == 0 || config_.calibration.stepsPerRev == 0) {
      return 0;
    }
    const uint64_t numerator = static_cast<uint64_t>(steps) * 60000ULL;
    const uint64_t denominator = static_cast<uint64_t>(rpm) * config_.calibration.stepsPerRev;
    return static_cast<uint32_t>((numerator + denominator - 1ULL) / denominator);
  }

  static const char* waitStatusText(EmmCanMotionHal::WaitStatus status) {
    switch (status) {
      case EmmCanMotionHal::WaitStatus::Completed:
        return "completed";
      case EmmCanMotionHal::WaitStatus::TransportError:
        return "transport_error";
      case EmmCanMotionHal::WaitStatus::Timeout:
      default:
        return "timeout";
    }
  }

  void showError() {
    display_.showStatus(DisplayStatus::Error);
  }

  void stopAllMotors() {
    motion_.stopNow(config_.topology.xMotorId);
    motion_.stopNow(config_.topology.yLeftMotorId);
    motion_.stopNow(config_.topology.yRightMotorId);
    motion_.stopNow(config_.topology.lineFeedMotorId);
  }

  char normalizeKey(char key) const {
    return (key >= 'A' && key <= 'Z') ? static_cast<char>(key - 'A' + 'a') : key;
  }

  const TypingConfig& config_;
  DisplayHal& display_;
  EmmCanMotionHal& motion_;
  ServoPressHal& servo_;
  Print& log_;
  KeymapStore keymapStore_;
  KeyBinding keymap_[64];
  size_t keymapCount_;
  MachinePointMm currentPoint_;
  JobState jobState_;
  uint32_t jobId_;
  size_t activeTextLength_;
  size_t currentIndex_;
  size_t currentStep_;
  int32_t xMotorPositionSteps_;
  int32_t yMotorPositionSteps_;
  int32_t lineFeedMotorPositionSteps_;
  int32_t jobStartXMotorPositionSteps_;
  int32_t jobStartYMotorPositionSteps_;
  int32_t jobStartLineFeedMotorPositionSteps_;
  TypingPlan activePlan_;
  PlanStatus lastPlanStatus_;
  char failedKey_;
  bool faulted_;
};

}  // namespace auto_typer
