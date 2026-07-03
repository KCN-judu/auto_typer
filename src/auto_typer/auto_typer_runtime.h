#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "auto_typer_config.h"
#include "can/CanRxTask.h"
#include "can/CanTxQueue.h"
#include "can/ProtocolTrace.h"
#include "drivers/EmmV5Driver.h"
#include "hal_display.h"
#include "keymap_feiyu200.h"
#include "keymap_store.h"
#include "motion/MotionExecutor.h"
#include "motion/MotionPlanner.h"
#include "typing_logic.h"

namespace auto_typer {

class AutoTyperApplication {
 public:
  struct RequiredActuatorCheck {
    bool ready;
    const char* code;
    const char* message;
  };

  AutoTyperApplication(const TypingConfig& config,
                       DisplayHal& display,
                       CanBus& canBus,
                       CanTxQueue& canTx,
                       CanRxTask& canRx,
                       EmmV5Driver& motion,
                       MotorFeedbackStore& feedback,
                       EmmV5EventStore& events,
                       ProtocolTrace& protocolTrace,
                       Print& log)
      : config_(config),
        display_(display),
        canBus_(canBus),
        canTx_(canTx),
        canRx_(canRx),
        motion_(motion),
        feedback_(feedback),
        events_(events),
        protocolTrace_(protocolTrace),
        executor_(config, motion, feedback),
        log_(log),
        keymapCount_(0),
        jobState_(JobState::None),
        jobId_(0),
        activeTextLength_(0),
        activePlan_{},
        activeMotionSteps_{},
        lastPlanStatus_(PlanStatus::Ok),
        failedKey_('\0'),
        faulted_(false),
        faultCode_(""),
        faultMessage_(""),
        remoteCurrentPoint_(config.homePoint),
        currentRemoteCommandId_{},
        startedRemoteGroupId_{},
        lastCompletedRemoteCommandId_{},
        remoteCommandActive_(false),
        remoteGroupStartedPending_(false),
        remoteDoneNotified_(false),
        remoteFaultNotified_(false),
        remoteCommandStartedAtMs_(0),
        lastRemoteCommandDurationMs_(0) {
    for (uint8_t i = 0; i < kTrackedMotorCount; ++i) {
      lastMotorProbeMs_[i] = 0;
    }
  }

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

    delay(200);
    if (!canBus_.begin() || !canTx_.begin()) {
      setFault("can_init_failed", "TWAI init failed");
      showError();
      return;
    }
    prepareMotorsBestEffort();
    log_.println("Auto typer ready");
    display_.showStatus(DisplayStatus::Idle);
  }

  void showMessage(const char* text) {
    display_.showMessage(text);
  }

  void tick() {
    canRx_.tick(16);
    canTx_.tick(4);

    if (canBus_.hasFatalFault()) {
      if (!faulted_) {
        const CanBusDiagnostics diagnostics = canBus_.diagnostics();
        setFault(diagnostics.lastFaultCode, diagnostics.lastFaultMessage);
        executor_.stopAll();
        jobState_ = JobState::Failed;
        showError();
      }
      return;
    }

    if (jobState_ == JobState::Queued) {
      const MachinePointMm startPoint = remoteCommandActive_ ? remoteCurrentPoint_ : MachinePointMm{NAN, NAN};
      if (!executor_.start(activeMotionSteps_.steps, activeMotionSteps_.count, startPoint)) {
        setFault("executor_busy", "Motion executor rejected job");
        jobState_ = JobState::Failed;
        if (remoteCommandActive_) {
          remoteFaultNotified_ = true;
          remoteCommandActive_ = false;
          startedRemoteGroupId_[0] = '\0';
        }
        showError();
        return;
      }
      jobState_ = JobState::Running;
      if (remoteCommandActive_) {
        remoteGroupStartedPending_ = true;
        copyString(startedRemoteGroupId_, sizeof(startedRemoteGroupId_), currentRemoteCommandId_);
        remoteCommandStartedAtMs_ = millis();
      }
      display_.showStatus(DisplayStatus::Printing);
    }

    executor_.tick();
    canTx_.tick(4);

    if (jobState_ == JobState::Running && executor_.completed()) {
      jobState_ = JobState::Completed;
      display_.showStatus(DisplayStatus::Complete);
      log_.println("Typing sequence complete");
    } else if ((jobState_ == JobState::Running || jobState_ == JobState::Cancelling) && executor_.cancelled()) {
      jobState_ = JobState::Cancelled;
      display_.showStatus(DisplayStatus::Idle);
    } else if (executor_.faulted()) {
      setFault(executor_.faultCode(), executor_.faultMessage());
      jobState_ = JobState::Failed;
      if (remoteCommandActive_) {
        remoteFaultNotified_ = true;
      }
      remoteCommandActive_ = false;
      remoteGroupStartedPending_ = false;
      startedRemoteGroupId_[0] = '\0';
      showError();
    }

    if (remoteCommandActive_) {
      if (executor_.stepStartedEvent() && remoteCommandStartedAtMs_ == 0) {
        remoteCommandStartedAtMs_ = millis();
      }
      if (executor_.completed()) {
        jobState_ = JobState::Completed;
        lastRemoteCommandDurationMs_ = executor_.lastCompletedDurationMs();
        remoteCurrentPoint_ = executor_.currentPoint();
        copyString(lastCompletedRemoteCommandId_, sizeof(lastCompletedRemoteCommandId_), currentRemoteCommandId_);
        currentRemoteCommandId_[0] = '\0';
        remoteCommandActive_ = false;
        remoteDoneNotified_ = true;
        startedRemoteGroupId_[0] = '\0';
        display_.showStatus(DisplayStatus::Idle);
      } else if (executor_.cancelled()) {
        jobState_ = JobState::Cancelled;
        currentRemoteCommandId_[0] = '\0';
        remoteCommandActive_ = false;
        remoteGroupStartedPending_ = false;
        startedRemoteGroupId_[0] = '\0';
        display_.showStatus(DisplayStatus::Idle);
      }
    }
  }

  SubmitJobResult submitTextJob(const char* text) {
    SubmitJobResult result{false, "", "", PlanStatus::Ok, 0, 0, '\0'};
    if (faulted_) {
      result.planStatus = PlanStatus::DeviceFault;
      result.rejectionCode = faultCode_;
      result.rejectionMessage = faultMessage_;
      result.stepCount = 0;
      return result;
    }
    if (jobState_ == JobState::Queued || jobState_ == JobState::Planning || jobState_ == JobState::Running ||
        jobState_ == JobState::Cancelling) {
      result.planStatus = PlanStatus::DeviceBusy;
      result.rejectionCode = "device_busy";
      result.rejectionMessage = "Device is already processing a job";
      return result;
    }
    if (!motionTransportReady()) {
      result.planStatus = PlanStatus::DeviceNotReady;
      result.rejectionCode = "motion_transport_not_ready";
      result.rejectionMessage = motionNotReadyMessage();
      return result;
    }

    jobState_ = JobState::Planning;
    planTextInto(activePlan_, text, keymap_, keymapCount_, config_);
    lastPlanStatus_ = activePlan_.status;
    failedKey_ = activePlan_.failedKey;
    activeTextLength_ = strlen(text);
    ++jobId_;

    log_.print("Plan status: ");
    log_.println(statusText(activePlan_.status));
    if (activePlan_.status != PlanStatus::Ok) {
      jobState_ = JobState::None;
      result.planStatus = activePlan_.status;
      result.rejectionCode = statusText(activePlan_.status);
      result.rejectionMessage = statusText(activePlan_.status);
      result.failedKey = failedKey_;
      result.stepCount = activePlan_.count;
      return result;
    }

    planMotionStepsInto(activeMotionSteps_, activePlan_, config_);
    if (activeMotionSteps_.status != PlanStatus::Ok) {
      lastPlanStatus_ = activeMotionSteps_.status;
      jobState_ = JobState::None;
      result.planStatus = activeMotionSteps_.status;
      result.rejectionCode = statusText(activeMotionSteps_.status);
      result.rejectionMessage = statusText(activeMotionSteps_.status);
      result.stepCount = activeMotionSteps_.count;
      return result;
    }

    const RequiredActuatorCheck required = checkRequiredActuators(activeMotionSteps_);
    if (!required.ready) {
      jobState_ = JobState::None;
      result.planStatus = PlanStatus::DeviceNotReady;
      result.rejectionCode = required.code;
      result.rejectionMessage = required.message;
      result.stepCount = activeMotionSteps_.count;
      return result;
    }

    jobState_ = JobState::Queued;
    result.accepted = true;
    result.jobId = jobId_;
    result.planStatus = PlanStatus::Ok;
    result.stepCount = activeMotionSteps_.count;
    return result;
  }

  bool cancelCurrentJob() {
    if (jobState_ != JobState::Queued && jobState_ != JobState::Planning && jobState_ != JobState::Running) {
      return false;
    }
    if (jobState_ == JobState::Running) {
      jobState_ = JobState::Cancelling;
      executor_.cancel();
    } else {
      jobState_ = JobState::Cancelled;
    }
    display_.showStatus(DisplayStatus::Idle);
    remoteCommandActive_ = false;
    remoteGroupStartedPending_ = false;
    startedRemoteGroupId_[0] = '\0';
    return true;
  }

  SubmitRemoteGroupResult submitRemoteGroup(const RemoteMotionStep* steps, size_t count, const char* groupId) {
    SubmitRemoteGroupResult result{false, "", ""};
    if (steps == nullptr || count == 0) {
      result.rejectionCode = "invalid_group";
      result.rejectionMessage = "Remote group must contain at least one step";
      return result;
    }
    if (count > kRemoteGroupMaxSteps) {
      result.rejectionCode = "group_too_large";
      result.rejectionMessage = "Remote group contains too many steps";
      return result;
    }
    if (faulted_ || executor_.faulted()) {
      result.rejectionCode = "faulted";
      result.rejectionMessage = faultMessage_;
      return result;
    }
    if (jobState_ == JobState::Queued || jobState_ == JobState::Planning || jobState_ == JobState::Running ||
        jobState_ == JobState::Cancelling || remoteCommandActive_) {
      result.rejectionCode = "device_busy";
      result.rejectionMessage = "Device is already executing a job";
      return result;
    }
    if (!motionTransportReady()) {
      result.rejectionCode = "motion_transport_not_ready";
      result.rejectionMessage = motionNotReadyMessage();
      return result;
    }

    activeMotionSteps_.status = PlanStatus::Ok;
    activeMotionSteps_.count = 0;
    MachinePointMm plannedPoint = remoteCurrentPoint_;
    for (size_t i = 0; i < count; ++i) {
      MotionStep motionStep{};
      if (!convertRemoteStep(steps[i], motionStep, result, plannedPoint)) {
        activeMotionSteps_.count = 0;
        return result;
      }
      activeMotionSteps_.steps[i] = motionStep;
      activeMotionSteps_.count = i + 1;
      if (motionStep.kind == MotionStepKind::MoveXY || motionStep.kind == MotionStepKind::LineFeed) {
        plannedPoint = motionStep.targetMm;
      }
    }

    RequiredActuatorCheck required = checkRequiredActuators(activeMotionSteps_);
    if (!required.ready && actuatorProbeMayRecover(required)) {
      probeMotorsBestEffort(400);
      required = checkRequiredActuators(activeMotionSteps_);
    }
    if (!required.ready) {
      activeMotionSteps_.count = 0;
      result.rejectionCode = required.code;
      result.rejectionMessage = required.message;
      return result;
    }

    copyString(currentRemoteCommandId_, sizeof(currentRemoteCommandId_), groupId);
    startedRemoteGroupId_[0] = '\0';
    remoteGroupStartedPending_ = false;
    remoteDoneNotified_ = false;
    remoteFaultNotified_ = false;
    remoteCommandStartedAtMs_ = 0;
    lastRemoteCommandDurationMs_ = 0;
    remoteCommandActive_ = true;
    jobState_ = JobState::Queued;
    ++jobId_;
    activeTextLength_ = 0;
    clearActivePlan();
    result.accepted = true;
    return result;
  }

  bool consumeRemoteGroupStarted(char* outGroupId, size_t outGroupIdSize) {
    if (!remoteGroupStartedPending_) {
      return false;
    }
    remoteGroupStartedPending_ = false;
    copyString(outGroupId, outGroupIdSize, startedRemoteGroupId_);
    return true;
  }

  bool consumeRemoteGroupDone(char* outGroupId, size_t outGroupIdSize, uint32_t& durationMs) {
    if (!remoteDoneNotified_) {
      return false;
    }
    remoteDoneNotified_ = false;
    copyString(outGroupId, outGroupIdSize, lastCompletedRemoteCommandId_);
    durationMs = lastRemoteCommandDurationMs_;
    return true;
  }

  bool emergencyStop() {
    return emergencyStopWithReason("emergency_stop", "Emergency stop requested");
  }

  bool resetFault() {
    executor_.resetFault();
    const bool recovered = canBus_.recoverOrClearFault();
    faulted_ = false;
    faultCode_ = "";
    faultMessage_ = "";
    jobState_ = JobState::None;
    remoteCommandActive_ = false;
    currentRemoteCommandId_[0] = '\0';
    probeMotorsBestEffort(300);
    if (!recovered || canBus_.hasFatalFault()) {
      const CanBusDiagnostics diagnostics = canBus_.diagnostics();
      setFault(diagnostics.lastFaultCode, diagnostics.lastFaultMessage);
      showError();
      return false;
    }
    display_.showStatus(DisplayStatus::Idle);
    return true;
  }

  bool probeMotors() {
    if (jobState_ == JobState::Queued || jobState_ == JobState::Planning || jobState_ == JobState::Running ||
        jobState_ == JobState::Cancelling) {
      return false;
    }
    probeMotorsBestEffort(400);
    return true;
  }

  bool debugMotorEnable(uint8_t motorId, bool enabled, bool sync) {
    if (!canDebug()) {
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
    if (motorId == config_.topology.yLeftMotorId || motorId == config_.topology.yRightMotorId) {
      return false;
    }
    return enabled ? motion_.enableMotor(motorId, sync) : motion_.disableMotor(motorId, sync);
  }

  bool debugMotorMoveRelative(uint8_t motorId,
                              MotorDirection direction,
                              uint16_t rpm,
                              uint8_t acceleration,
                              uint32_t steps,
                              bool sync) {
    if (!canDebug()) {
      return false;
    }
    if (isYMotorGroup(motorId)) {
      const int32_t signedSteps = direction == MotorDirection::Cw ? static_cast<int32_t>(steps)
                                                                  : -static_cast<int32_t>(steps);
      YPairController yPair(config_, motion_);
      return yPair.moveRelative(signedSteps, rpm, acceleration);
    }
    if (motorId == config_.topology.yLeftMotorId || motorId == config_.topology.yRightMotorId) {
      return false;
    }
    if (sync) {
      return false;
    }
    return motion_.moveRelative(motorId, direction, rpm, acceleration, steps, false);
  }

  bool debugMotorStop(uint8_t motorId, bool sync) {
    if (isYMotorGroup(motorId)) {
      return motion_.stopNow(config_.topology.yLeftMotorId) && motion_.stopNow(config_.topology.yRightMotorId);
    }
    return motion_.stopNow(motorId, sync);
  }

  bool debugServo(PressAction action, uint16_t dwellMs = 0) {
    if (!canDebug()) {
      return false;
    }
    if (action == PressAction::Neutral) {
      return true;
    }
    const int32_t signedSteps = pressMotorSignedSteps(action);
    const uint32_t steps = absoluteSteps(signedSteps);
    if (steps == 0) {
      return true;
    }
    const uint16_t rpm = dwellMs > 0 ? rpmForDuration(steps, dwellMs) : config_.pressMotor.rpm;
    return motion_.moveRelative(config_.topology.pressMotorId,
                                directionForSignedSteps(signedSteps),
                                rpm,
                                config_.pressMotor.acceleration,
                                steps,
                                false);
  }

  bool debugServoNeutral(uint16_t dwellMs = 0) {
    (void)dwellMs;
    return debugServo(PressAction::Neutral, 0);
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

  MotorState motorState(uint8_t motorId) const {
    return enrichedMotorState(motorId);
  }

  JobSnapshot snapshot() const {
    return {jobState_,
            jobId_,
            activeTextLength_,
            executor_.activeTextIndex(),
            executor_.currentStep(),
            activePlan_.count,
            currentPoint(),
            lastPlanStatus_,
            failedKey_,
            faultCode_,
            faultMessage_};
  }

  DeviceMode mode() const {
    if (faulted_) {
      return DeviceMode::Faulted;
    }
    return jobState_ == JobState::Queued || jobState_ == JobState::Planning || jobState_ == JobState::Running ||
                   jobState_ == JobState::Cancelling
               ? DeviceMode::Running
               : DeviceMode::Idle;
  }

  bool servoReady() const {
    return requiredMotorReady(config_.topology.pressMotorId);
  }

  bool motionReady() const {
    return motionTransportReady() && requiredMotorReady(config_.topology.xMotorId) &&
           requiredMotorReady(config_.topology.yLeftMotorId) &&
           requiredMotorReady(config_.topology.yRightMotorId) &&
           requiredMotorReady(config_.topology.lineFeedMotorId);
  }

  bool healthWarning() const {
    const CanBusDiagnostics diagnostics = canBus_.diagnostics();
    return !faulted_ && diagnostics.lastAlertAtMs != 0 && millis() - diagnostics.lastAlertAtMs <= kRecentAlertWindowMs;
  }

  bool healthNotReady() const {
    return !faulted_ && !motionReady();
  }

  CanBusDiagnostics canDiagnostics() const {
    return canBus_.diagnostics();
  }

  size_t protocolTraceSnapshot(ProtocolTraceItem* out, size_t maxItems) const {
    return protocolTrace_.snapshot(out, maxItems);
  }

  ProtocolDiagnostics protocolDiagnostics() const {
    return events_.diagnostics();
  }

  const char* currentRemoteCommandId() const {
    return currentRemoteCommandId_;
  }

  const char* lastCompletedRemoteCommandId() const {
    return lastCompletedRemoteCommandId_;
  }

  MachinePointMm currentPoint() const {
    return remoteCommandActive_ ? executor_.currentPoint() : remoteCurrentPoint_;
  }

  bool consumeRemoteFault(char* outCommandId, size_t outCommandIdSize, const char*& code, const char*& message) {
    if (!remoteFaultNotified_) {
      return false;
    }
    remoteFaultNotified_ = false;
    copyString(outCommandId,
               outCommandIdSize,
               currentRemoteCommandId_[0] != '\0' ? currentRemoteCommandId_ : lastCompletedRemoteCommandId_);
    code = faultCode_;
    message = faultMessage_;
    return true;
  }

  bool executorRunning() const {
    return executor_.running();
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
    log_.print("Default motion RPM: ");
    log_.println(config_.motionRuntime.defaultMoveRpm);
  }

  void prepareMotorsBestEffort() {
    probeMotorsBestEffort(150);
  }

  bool emergencyStopWithReason(const char* code, const char* message) {
    canTx_.clear();
    executor_.emergencyStop();
    setFault(code, message);
    jobState_ = JobState::Failed;
    showError();
    canTx_.tick(8);
    return true;
  }

  bool canDebug() const {
    return !faulted_ && jobState_ != JobState::Queued && jobState_ != JobState::Planning &&
           jobState_ != JobState::Running && jobState_ != JobState::Cancelling;
  }

  bool isYMotorGroup(uint8_t motorId) const {
    return motorId == config_.motionRuntime.yPairVirtualMotorId;
  }

  void setFault(const char* code, const char* message) {
    faulted_ = true;
    faultCode_ = code;
    faultMessage_ = message;
  }

  bool convertRemoteStep(const RemoteMotionStep& remoteStep,
                          MotionStep& step,
                          SubmitRemoteGroupResult& result,
                          MachinePointMm currentPoint) const {
    step = MotionStep{};
    step.profile = defaultMotionProfile(config_);
    step.profile.maxRpm = remoteStep.profile.hasRpm ? remoteStep.profile.rpm : kRemoteDefaultRpm;
    step.profile.acceleration =
        remoteStep.profile.hasAccelRaw ? remoteStep.profile.accelRaw : kRemoteDefaultAccelerationRaw;
    step.profile.timeoutMs =
        remoteStep.profile.hasTimeoutMs ? remoteStep.profile.timeoutMs : config_.motionRuntime.motionTimeoutMs;
    step.targetMm = currentPoint;
    if (step.profile.maxRpm == 0 || step.profile.acceleration == 0 || step.profile.timeoutMs == 0) {
      result.rejectionCode = "invalid_step";
      result.rejectionMessage = "Invalid remote step profile";
      return false;
    }

    switch (remoteStep.kind) {
      case RemoteMotionStepKind::MoveXY: {
        if (!isfinite(remoteStep.dxMm) || !isfinite(remoteStep.dyMm)) {
          result.rejectionCode = "invalid_step";
          result.rejectionMessage = "move_xy delta must be finite";
          return false;
        }
        const MachinePointMm target{currentPoint.xMm + remoteStep.dxMm, currentPoint.yMm + remoteStep.dyMm};
        step.kind = MotionStepKind::MoveXY;
        step.targetMm = target;
        step.deltaSteps = xyDeltaSteps(currentPoint, target, config_.calibration);
        step.profile.settleMs = config_.xProfile.settleMs > config_.yProfile.settleMs ? config_.xProfile.settleMs
                                                                                        : config_.yProfile.settleMs;
        return true;
      }
      case RemoteMotionStepKind::ServoPress:
        step.kind = MotionStepKind::ServoPress;
        step.waitMs = config_.pressMotor.pressMs;
        step.profile.maxRpm = config_.pressMotor.rpm;
        step.profile.acceleration = config_.pressMotor.acceleration;
        step.profile.settleMs = config_.pressMotor.settleMs;
        step.profile.timeoutMs = config_.pressMotor.timeoutMs;
        return true;
      case RemoteMotionStepKind::ServoRelease:
        step.kind = MotionStepKind::ServoRelease;
        step.waitMs = config_.pressMotor.releaseMs;
        step.profile.maxRpm = config_.pressMotor.rpm;
        step.profile.acceleration = config_.pressMotor.acceleration;
        step.profile.settleMs = config_.pressMotor.settleMs;
        step.profile.timeoutMs = config_.pressMotor.timeoutMs;
        return true;
      case RemoteMotionStepKind::CharacterRelease:
        step.kind = MotionStepKind::CharacterRelease;
        step.deltaSteps.lineFeed =
            signedStepsForDirection(config_.lineFeed.characterReleaseSteps, config_.lineFeed.releaseDirection);
        step.profile.maxRpm = config_.lineFeed.rpm;
        step.profile.acceleration = config_.lineFeed.acceleration;
        step.profile.settleMs = config_.lineFeed.characterReleaseSettleMs;
        return true;
      case RemoteMotionStepKind::LineFeed:
        step.kind = MotionStepKind::LineFeed;
        step.deltaSteps.lineFeed =
            signedStepsForDirection(config_.lineFeed.returnTotalSteps, config_.lineFeed.returnDirection);
        step.profile.maxRpm = config_.lineFeed.rpm;
        step.profile.acceleration = config_.lineFeed.acceleration;
        step.profile.settleMs = config_.lineFeed.settleMs;
        step.targetMm = {config_.homePoint.xMm, currentPoint.yMm};
        return true;
      case RemoteMotionStepKind::Wait:
        step.kind = MotionStepKind::Wait;
        step.waitMs = remoteStep.durationMs;
        return true;
    }
    result.rejectionCode = "invalid_step";
    result.rejectionMessage = "Unsupported remote step kind";
    return false;
  }

  static const char* remoteStepKindText(RemoteMotionStepKind kind) {
    switch (kind) {
      case RemoteMotionStepKind::MoveXY:
        return "move_xy";
      case RemoteMotionStepKind::ServoPress:
        return "servo_press";
      case RemoteMotionStepKind::ServoRelease:
        return "servo_release";
      case RemoteMotionStepKind::CharacterRelease:
        return "character_release";
      case RemoteMotionStepKind::LineFeed:
        return "line_feed";
      case RemoteMotionStepKind::Wait:
      default:
        return "wait";
    }
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

  void requestMotorFeedback(bool includeConfig) {
    const uint8_t motors[] = {
      config_.topology.xMotorId,
      config_.topology.yLeftMotorId,
      config_.topology.yRightMotorId,
      config_.topology.lineFeedMotorId,
      config_.topology.pressMotorId,
    };
    const uint32_t nowMs = millis();
    for (uint8_t motor : motors) {
      setLastProbeMs(motor, nowMs);
      if (includeConfig) {
        motion_.setClosedLoopControlMode(motor, false);
        motion_.enableMotor(motor);
      }
      motion_.requestStatusFlags(motor);
      motion_.requestInputPulseCount(motor);
      motion_.requestVelocity(motor);
    }
  }

  void probeMotorsBestEffort(uint16_t pumpMs) {
    requestMotorFeedback(true);
    const uint32_t startedAt = millis();
    while (millis() - startedAt < pumpMs) {
      canTx_.tick(4);
      canRx_.tick(16);
      delay(10);
    }
  }

  const char* motionNotReadyMessage() const {
    const CanBusDiagnostics diagnostics = canBus_.diagnostics();
    if (!canBus_.driverReady()) {
      return "CAN driver is not ready";
    }
    if (diagnostics.fatalFault) {
      return diagnostics.lastFaultMessage;
    }
    return "Required motor feedback is not ready";
  }

  RequiredActuatorCheck checkRequiredActuators(const MotionStepPlan& steps) const {
    bool needsX = false;
    bool needsY = false;
    bool needsLineFeed = false;
    bool needsPressMotor = false;
    for (size_t i = 0; i < steps.count; ++i) {
      const MotionStep& step = steps.steps[i];
      if (step.deltaSteps.x != 0) {
        needsX = true;
      }
      if (step.deltaSteps.yLeft != 0 || step.deltaSteps.yRight != 0) {
        needsY = true;
      }
      if (step.deltaSteps.lineFeed != 0) {
        needsLineFeed = true;
      }
      if (step.kind == MotionStepKind::ServoPress || step.kind == MotionStepKind::ServoRelease) {
        needsPressMotor = true;
      }
    }
    if (needsX && !requiredMotorReady(config_.topology.xMotorId)) {
      return {false, "x_motor_not_ready", "X motor is not ready"};
    }
    if (needsY &&
        (!requiredMotorReady(config_.topology.yLeftMotorId) || !requiredMotorReady(config_.topology.yRightMotorId))) {
      return {false, "y_pair_not_ready", "Y pair is not ready"};
    }
    if (needsLineFeed && !requiredMotorReady(config_.topology.lineFeedMotorId)) {
      return {false, "line_feed_not_ready", "Line feed motor is not ready"};
    }
    if (needsPressMotor && !requiredMotorReady(config_.topology.pressMotorId)) {
      return {false, "press_motor_not_ready", "Press motor is not ready"};
    }
    return {true, "", ""};
  }

  RequiredActuatorCheck checkRequiredActuator(const MotionStep& step) const {
    if (step.deltaSteps.x != 0 && !requiredMotorReady(config_.topology.xMotorId)) {
      return {false, "x_motor_not_ready", "X motor is not ready"};
    }
    if ((step.deltaSteps.yLeft != 0 || step.deltaSteps.yRight != 0) &&
        (!requiredMotorReady(config_.topology.yLeftMotorId) || !requiredMotorReady(config_.topology.yRightMotorId))) {
      return {false, "y_pair_not_ready", "Y pair is not ready"};
    }
    if (step.deltaSteps.lineFeed != 0 && !requiredMotorReady(config_.topology.lineFeedMotorId)) {
      return {false, "line_feed_not_ready", "Line feed motor is not ready"};
    }
    if ((step.kind == MotionStepKind::ServoPress || step.kind == MotionStepKind::ServoRelease) &&
        !requiredMotorReady(config_.topology.pressMotorId)) {
      return {false, "press_motor_not_ready", "Press motor is not ready"};
    }
    return {true, "", ""};
  }

  static bool actuatorProbeMayRecover(const RequiredActuatorCheck& required) {
    return required.code != nullptr;
  }

  bool motionTransportReady() const {
    return canBus_.motionReady();
  }

  bool requiredMotorReady(uint8_t motorId) const {
    return enrichedMotorState(motorId).readiness == MotorReadiness::Ready;
  }

  MotorState enrichedMotorState(uint8_t motorId) const {
    MotorState state = feedback_.get(motorId);
    const uint32_t nowMs = millis();
    const uint32_t lastProbeMs = lastProbeMsFor(motorId);
    state.role = motorRole(motorId);
    state.hasRecentStatus = state.hasStatus && state.lastStatusMs != 0 && nowMs - state.lastStatusMs <= kFeedbackFreshMs;
    state.hasRecentInputPulse =
        state.hasInputPulse && state.lastInputPulseMs != 0 && nowMs - state.lastInputPulseMs <= kFeedbackFreshMs;
    state.hasRecentVelocity =
        state.hasVelocity && state.lastVelocityMs != 0 && nowMs - state.lastVelocityMs <= kFeedbackFreshMs;
    state.lastProbeMs = lastProbeMs;
    if (state.lastErrorCode == nullptr) {
      state.lastErrorCode = "";
    }
    if (state.lastErrorMessage == nullptr) {
      state.lastErrorMessage = "";
    }
    state.readiness = computeReadiness(state, lastProbeMs);
    return state;
  }

  MotorReadiness computeReadiness(const MotorState& state, uint32_t lastProbeMs) const {
    if (state.driverFault) {
      return MotorReadiness::Faulted;
    }
    if (state.lastMalformedMs != 0 && state.lastMalformedMs >= lastProbeMs) {
      return MotorReadiness::Faulted;
    }
    if (state.lastConditionNotMetMs != 0 && state.lastConditionNotMetMs >= lastProbeMs) {
      return MotorReadiness::ConditionNotMet;
    }
    if (state.hasRecentStatus && state.hasRecentInputPulse && state.hasRecentVelocity) {
      return MotorReadiness::Ready;
    }
    if (state.lastAckMs != 0 && state.lastAckMs >= lastProbeMs) {
      return MotorReadiness::Acked;
    }
    if (lastProbeMs != 0 && state.lastAnyFrameMs < lastProbeMs && millis() - lastProbeMs > kProbeOfflineMs) {
      return MotorReadiness::Offline;
    }
    if (lastProbeMs != 0) {
      return MotorReadiness::ConfigSent;
    }
    return MotorReadiness::Unknown;
  }

  MotorRole motorRole(uint8_t motorId) const {
    if (motorId == config_.topology.yLeftMotorId) {
      return MotorRole::YLeft;
    }
    if (motorId == config_.topology.yRightMotorId) {
      return MotorRole::YRight;
    }
    if (motorId == config_.topology.lineFeedMotorId) {
      return MotorRole::LineFeed;
    }
    if (motorId == config_.topology.pressMotorId) {
      return MotorRole::Press;
    }
    return MotorRole::X;
  }

  uint32_t lastProbeMsFor(uint8_t motorId) const {
    const int8_t index = trackedMotorIndex(motorId);
    return index < 0 ? 0 : lastMotorProbeMs_[index];
  }

  void setLastProbeMs(uint8_t motorId, uint32_t timeMs) {
    const int8_t index = trackedMotorIndex(motorId);
    if (index >= 0) {
      lastMotorProbeMs_[index] = timeMs;
    }
  }

  int8_t trackedMotorIndex(uint8_t motorId) const {
    if (motorId == config_.topology.xMotorId) {
      return 0;
    }
    if (motorId == config_.topology.yLeftMotorId) {
      return 1;
    }
    if (motorId == config_.topology.yRightMotorId) {
      return 2;
    }
    if (motorId == config_.topology.lineFeedMotorId) {
      return 3;
    }
    if (motorId == config_.topology.pressMotorId) {
      return 4;
    }
    return -1;
  }

  void showError() {
    display_.showStatus(DisplayStatus::Error);
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

  uint16_t rpmForDuration(uint32_t steps, uint16_t durationMs) const {
    if (steps == 0 || durationMs == 0 || config_.calibration.stepsPerRev == 0) {
      return config_.pressMotor.rpm;
    }
    const uint32_t numerator = steps * 60000UL;
    uint32_t rpm = numerator / (static_cast<uint32_t>(durationMs) * config_.calibration.stepsPerRev);
    if (rpm == 0) {
      rpm = 1;
    }
    return rpm > 65535UL ? 65535 : static_cast<uint16_t>(rpm);
  }

  void clearActivePlan() {
    activePlan_.status = PlanStatus::Ok;
    activePlan_.failedKey = '\0';
    activePlan_.count = 0;
  }

  char normalizeKey(char key) const {
    return (key >= 'A' && key <= 'Z') ? static_cast<char>(key - 'A' + 'a') : key;
  }

  const TypingConfig& config_;
  DisplayHal& display_;
  CanBus& canBus_;
  CanTxQueue& canTx_;
  CanRxTask& canRx_;
  EmmV5Driver& motion_;
  MotorFeedbackStore& feedback_;
  EmmV5EventStore& events_;
  ProtocolTrace& protocolTrace_;
  MotionExecutor executor_;
  Print& log_;
  KeymapStore keymapStore_;
  KeyBinding keymap_[64];
  size_t keymapCount_;
  JobState jobState_;
  uint32_t jobId_;
  size_t activeTextLength_;
  TypingPlan activePlan_;
  MotionStepPlan activeMotionSteps_;
  PlanStatus lastPlanStatus_;
  char failedKey_;
  bool faulted_;
  const char* faultCode_;
  const char* faultMessage_;
  MachinePointMm remoteCurrentPoint_;
  char currentRemoteCommandId_[49];
  char startedRemoteGroupId_[49];
  char lastCompletedRemoteCommandId_[49];
  bool remoteCommandActive_;
  bool remoteGroupStartedPending_;
  bool remoteDoneNotified_;
  bool remoteFaultNotified_;
  uint32_t remoteCommandStartedAtMs_;
  uint32_t lastRemoteCommandDurationMs_;
  static constexpr uint8_t kTrackedMotorCount = 5;
  static constexpr uint32_t kFeedbackFreshMs = 1500;
  static constexpr uint32_t kProbeOfflineMs = 250;
  static constexpr uint32_t kRecentAlertWindowMs = 5000;
  static constexpr uint16_t kRemoteDefaultRpm = 1600;
  static constexpr uint8_t kRemoteDefaultAccelerationRaw = 128;
  uint32_t lastMotorProbeMs_[kTrackedMotorCount];
};

}  // namespace auto_typer
