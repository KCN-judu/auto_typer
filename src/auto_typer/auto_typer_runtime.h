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
#include "motion/MachineKinematics.h"
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
        executor_(config, motion, feedback, protocolTrace),
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
        remoteGroupState_(RemoteGroupState::Idle),
        remoteGroupStartedPending_(false),
        remoteDoneNotified_(false),
        remoteFaultNotified_(false),
        remoteFinalPending_(false),
        remoteFinalStatus_(""),
        remoteFinalCode_(""),
        remoteFinalMessage_(""),
        remoteFinalGroupId_{},
        remoteFinalSeq_(0),
        remoteBlockStartedPending_(false),
        remoteBlockDonePending_(false),
        currentRemoteSeq_(0),
        lastRemoteBlockStartedIndex_(static_cast<size_t>(-1)),
        lastRemoteBlockDoneIndex_(static_cast<size_t>(-1)),
        remoteCommandStartedAtMs_(0),
        lastRemoteCommandDurationMs_(0) {
    for (uint8_t i = 0; i < kTrackedMotorCount; ++i) {
      lastMotorProbeMs_[i] = 0;
    }
  }

  void setup() {
    buildKeymap();
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

    const CanBusDiagnostics loopCanDiagnostics = canBus_.diagnostics();
    if (loopCanDiagnostics.fatalFault) {
      if (!faulted_) {
        setFault(loopCanDiagnostics.lastFaultCode, loopCanDiagnostics.lastFaultMessage);
        executor_.stopAll();
        jobState_ = JobState::Failed;
        if (remoteCommandActive_) {
          finalizeRemoteGroup("failed", faultCode_, faultMessage_, remoteElapsedMs());
        }
        showError();
      }
      return;
    }

    if (jobState_ == JobState::Queued) {
      const MachinePointMm startPoint = remoteCommandActive_ ? remoteCurrentPoint_ : MachinePointMm{NAN, NAN};
      const char* groupId = remoteCommandActive_ ? currentRemoteCommandId_ : "";
      const uint32_t seq = remoteCommandActive_ ? currentRemoteSeq_ : 0;
      if (!executor_.start(activeMotionSteps_.steps, activeMotionSteps_.count, startPoint, groupId, seq)) {
        setFault("executor_busy", "Motion executor rejected job");
        jobState_ = JobState::Failed;
        if (remoteCommandActive_) {
          finalizeRemoteGroup("failed", "executor_busy", "Motion executor rejected job", 0);
        }
        showError();
        return;
      }
      jobState_ = JobState::Running;
      if (remoteCommandActive_) {
        remoteGroupStartedPending_ = true;
        remoteGroupState_ = RemoteGroupState::Running;
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
      if (remoteCommandActive_) {
        setFault(executor_.faultCode(), executor_.faultMessage());
        jobState_ = JobState::Failed;
        finalizeRemoteGroup("failed", faultCode_, faultMessage_, remoteElapsedMs());
        showError();
      } else {
        setFault(executor_.faultCode(), executor_.faultMessage());
        jobState_ = JobState::Failed;
        showError();
      }
    }

    if (remoteCommandActive_) {
      const uint32_t groupDurationMs =
          remoteCommandStartedAtMs_ == 0 ? 0 : static_cast<uint32_t>(millis() - remoteCommandStartedAtMs_);
      if (executor_.stepStartedEvent() && executor_.currentStep() != lastRemoteBlockStartedIndex_) {
        lastRemoteBlockStartedIndex_ = executor_.currentStep();
        remoteBlockStartedPending_ = true;
      }
      if (executor_.lastCompletedStep() > 0 && executor_.lastCompletedStep() - 1 != lastRemoteBlockDoneIndex_) {
        lastRemoteBlockDoneIndex_ = executor_.lastCompletedStep() - 1;
        remoteBlockDonePending_ = true;
      }
      if (executor_.completed()) {
        jobState_ = JobState::Completed;
        lastRemoteCommandDurationMs_ = groupDurationMs;
        remoteCurrentPoint_ = executor_.currentPoint();
        copyString(lastCompletedRemoteCommandId_, sizeof(lastCompletedRemoteCommandId_), currentRemoteCommandId_);
        remoteDoneNotified_ = true;
        finalizeRemoteGroup("done", "", "", groupDurationMs);
        display_.showStatus(DisplayStatus::Idle);
      } else if (executor_.cancelled()) {
        jobState_ = JobState::Cancelled;
        finalizeRemoteGroup("cancelled", "cancelled", "Remote group cancelled", groupDurationMs);
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
    const bool cancellingRemote = remoteCommandActive_;
    if (jobState_ == JobState::Running) {
      jobState_ = JobState::Cancelling;
      executor_.cancel();
    } else {
      jobState_ = JobState::Cancelled;
    }
    display_.showStatus(DisplayStatus::Idle);
    if (cancellingRemote && jobState_ == JobState::Cancelled) {
      finalizeRemoteGroup("cancelled", "cancelled", "Remote group cancelled", remoteElapsedMs());
    } else if (cancellingRemote) {
      remoteGroupState_ = RemoteGroupState::Cancelled;
    }
    return true;
  }

  SubmitRemoteGroupResult submitRemoteGroup(const RemoteMotionStep* steps, size_t count, const char* groupId, uint32_t seq) {
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
      result.rejectionCode = "device_fault";
      result.rejectionMessage = faultMessage_;
      return result;
    }
    if (jobState_ == JobState::Queued || jobState_ == JobState::Planning || jobState_ == JobState::Running ||
        jobState_ == JobState::Cancelling || remoteCommandActive_ || remoteFinalPending_) {
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
      if (motionStep.kind == MotionStepKind::MoveXY || motionStep.kind == MotionStepKind::LineFeed ||
          motionStep.kind == MotionStepKind::ReturnZero) {
        plannedPoint = motionStep.targetMm;
      }
    }

    copyString(currentRemoteCommandId_, sizeof(currentRemoteCommandId_), groupId);
    startedRemoteGroupId_[0] = '\0';
    remoteGroupStartedPending_ = false;
    remoteDoneNotified_ = false;
    remoteFaultNotified_ = false;
    remoteFinalPending_ = false;
    remoteFinalStatus_ = "";
    remoteFinalCode_ = "";
    remoteFinalMessage_ = "";
    remoteFinalGroupId_[0] = '\0';
    remoteFinalSeq_ = seq;
    remoteBlockStartedPending_ = false;
    remoteBlockDonePending_ = false;
    currentRemoteSeq_ = seq;
    lastRemoteBlockStartedIndex_ = static_cast<size_t>(-1);
    lastRemoteBlockDoneIndex_ = static_cast<size_t>(-1);
    remoteCommandStartedAtMs_ = 0;
    lastRemoteCommandDurationMs_ = 0;
    remoteCommandActive_ = true;
    remoteGroupState_ = RemoteGroupState::Queued;
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

  bool consumeRemoteBlockStarted(char* outGroupId,
                                 size_t outGroupIdSize,
                                 uint32_t& seq,
                                 size_t& blockIndex,
                                 const char*& blockType) {
    if (!remoteBlockStartedPending_) {
      return false;
    }
    remoteBlockStartedPending_ = false;
    copyString(outGroupId, outGroupIdSize, currentRemoteCommandId_);
    seq = currentRemoteSeq_;
    blockIndex = lastRemoteBlockStartedIndex_;
    blockType = blockIndex < activeMotionSteps_.count ? motionStepKindText(activeMotionSteps_.steps[blockIndex].kind) : "wait";
    return true;
  }

  bool consumeRemoteBlockDone(char* outGroupId,
                              size_t outGroupIdSize,
                              uint32_t& seq,
                              size_t& blockIndex,
                              const char*& blockType) {
    if (!remoteBlockDonePending_) {
      return false;
    }
    remoteBlockDonePending_ = false;
    copyString(outGroupId, outGroupIdSize, currentRemoteCommandId_);
    seq = currentRemoteSeq_;
    blockIndex = lastRemoteBlockDoneIndex_;
    blockType = blockIndex < activeMotionSteps_.count ? motionStepKindText(activeMotionSteps_.steps[blockIndex].kind) : "wait";
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

  bool consumeRemoteGroupFinal(char* outGroupId,
                               size_t outGroupIdSize,
                               uint32_t& seq,
                               const char*& status,
                               const char*& code,
                               const char*& message,
                               uint32_t& durationMs) {
    if (!remoteFinalPending_) {
      return false;
    }
    remoteFinalPending_ = false;
    copyString(outGroupId, outGroupIdSize, remoteFinalGroupId_);
    seq = remoteFinalSeq_;
    status = remoteFinalStatus_;
    code = remoteFinalCode_;
    message = remoteFinalMessage_;
    durationMs = lastRemoteCommandDurationMs_;
    remoteCommandActive_ = false;
    currentRemoteCommandId_[0] = '\0';
    startedRemoteGroupId_[0] = '\0';
    remoteFinalGroupId_[0] = '\0';
    if (strcmp(remoteFinalStatus_, "done") == 0) {
      remoteGroupState_ = RemoteGroupState::Completed;
    } else if (strcmp(remoteFinalStatus_, "cancelled") == 0) {
      remoteGroupState_ = RemoteGroupState::Cancelled;
    } else {
      remoteGroupState_ = RemoteGroupState::Failed;
    }
    return true;
  }

  bool finishRemoteTask() {
    if (jobState_ == JobState::Queued || jobState_ == JobState::Planning || jobState_ == JobState::Running ||
        jobState_ == JobState::Cancelling || remoteCommandActive_) {
      return false;
    }
    jobState_ = JobState::Completed;
    currentRemoteCommandId_[0] = '\0';
    startedRemoteGroupId_[0] = '\0';
    display_.showStatus(DisplayStatus::Complete);
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
    remoteGroupState_ = RemoteGroupState::Idle;
    remoteFinalPending_ = false;
    currentRemoteCommandId_[0] = '\0';
    probeMotorsBestEffort(300, true);
    const CanBusDiagnostics diagnostics = canBus_.diagnostics();
    if (!recovered || diagnostics.fatalFault) {
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
    probeMotorsBestEffort(400, false);
    return true;
  }

  PressDiagResult debugPressDiagM5() {
    PressDiagResult result{};
    result.code = "";
    result.message = "";
    if (!canDebug()) {
      result.code = "device_busy";
      result.message = "Device is not idle for press diagnostic";
      return result;
    }
    const uint8_t motorId = config_.topology.pressMotorId;
    protocolTrace_.setMotionContext("press_diag_m5", 0, 0, "press_down");
    if (!samplePressFeedback(500)) {
      result.code = "motion_feedback_timeout";
      result.message = "M5 feedback baseline timed out";
      protocolTrace_.clearMotionContext();
      return result;
    }
    MotorState state = feedback_.get(motorId);
    result.initialPulse = state.inputPulseSteps;
    result.downTargetPulse = state.inputPulseSteps + config_.pressMotor.pressDeltaSteps;
    if (!runPressDiagMove("press_down", 0, config_.pressMotor.pressDeltaSteps, result.downTargetPulse, result, true)) {
      protocolTrace_.clearMotionContext();
      printPressDiagTrace(result);
      return result;
    }

    protocolTrace_.setMotionContext("press_diag_m5", 0, 1, "press_up");
    state = feedback_.get(motorId);
    result.downPulse = state.inputPulseSteps;
    result.upTargetPulse = state.inputPulseSteps + config_.pressMotor.releaseDeltaSteps;
    if (!runPressDiagMove("press_up", 1, config_.pressMotor.releaseDeltaSteps, result.upTargetPulse, result, false)) {
      protocolTrace_.clearMotionContext();
      printPressDiagTrace(result);
      return result;
    }
    state = feedback_.get(motorId);
    result.finalPulse = state.inputPulseSteps;
    result.ok = true;
    result.code = "";
    result.message = "press_diag_m5 completed";
    protocolTrace_.clearMotionContext();
    printPressDiagTrace(result);
    return result;
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

  bool upsertKeyBinding(char key, MachinePointMm point) {
    const char normalized = normalizeKey(key);
    for (size_t i = 0; i < keymapCount_; ++i) {
      if (keymap_[i].key == normalized) {
        keymap_[i].point = point;
        return true;
      }
    }
    return appendKey(keymap_, sizeof(keymap_) / sizeof(keymap_[0]), keymapCount_, normalized, point);
  }

  bool replaceKeymap(const KeyBinding* bindings, size_t count) {
    if (count == 0 || count > sizeof(keymap_) / sizeof(keymap_[0])) {
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      keymap_[i] = bindings[i];
    }
    keymapCount_ = count;
    return true;
  }

  const KeyBinding* keymap() const {
    return keymap_;
  }

  size_t keymapCount() const {
    return keymapCount_;
  }

  uint32_t keymapVersion() const {
    return kFeiyu200KeymapLayoutVersion;
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

  bool pressReady() const {
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

  uint32_t currentRemoteSeq() const {
    return currentRemoteSeq_;
  }

  RemoteGroupState remoteGroupState() const {
    return remoteGroupState_;
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
  bool samplePressFeedback(uint16_t timeoutMs) {
    const uint8_t motorId = config_.topology.pressMotorId;
    const uint32_t startedAt = millis();
    uint32_t lastPollMs = 0;
    while (millis() - startedAt < timeoutMs) {
      const uint32_t nowMs = millis();
      if (lastPollMs == 0 || nowMs - lastPollMs >= config_.motionRuntime.motionPollIntervalMs) {
        lastPollMs = nowMs;
        motion_.requestInputPulseCount(motorId);
        motion_.requestVelocity(motorId);
      }
      canTx_.tick(4);
      canRx_.tick(16);
      const MotorState state = feedback_.get(motorId);
      if (state.hasInputPulse && state.hasVelocity && state.lastInputPulseMs >= startedAt &&
          state.lastVelocityMs >= startedAt) {
        return true;
      }
      delay(5);
    }
    return false;
  }

  bool runPressDiagMove(const char* label,
                        size_t blockIndex,
                        int32_t signedSteps,
                        int32_t targetPulse,
                        PressDiagResult& result,
                        bool down) {
    protocolTrace_.setMotionContext("press_diag_m5", 0, blockIndex, label);
    const uint8_t motorId = config_.topology.pressMotorId;
    const uint32_t steps = absoluteSteps(signedSteps);
    const MotorDirection direction = directionForSignedSteps(signedSteps);
    const size_t queueBefore = motion_.availableForWrite();
    const uint32_t issueMs = millis();
    const bool enqueued = motion_.moveRelative(motorId,
                                               direction,
                                               config_.pressMotor.rpm,
                                               config_.pressMotor.acceleration,
                                               steps,
                                               false,
                                               true);
    logPressDiagIssue(label, signedSteps, direction, queueBefore, enqueued, issueMs);
    if (!enqueued) {
      result.code = "motion_command_rejected";
      result.message = "M5 diagnostic motion command was rejected";
      return false;
    }
    if (!waitForPressAck(issueMs, config_.motionRuntime.motionCommandAckTimeoutMs)) {
      executor_.stopAll();
      result.code = "motion_command_no_ack";
      result.message = down ? "M5 press_down did not ACK" : "M5 press_up did not ACK";
      if (down) {
        result.downAckSeen = false;
      } else {
        result.upAckSeen = false;
      }
      return false;
    }
    if (down) {
      result.downAckSeen = true;
    } else {
      result.upAckSeen = true;
    }
    if (!waitForPressTarget(issueMs, targetPulse, config_.pressMotor.timeoutMs, down)) {
      executor_.stopAll();
      result.code = "motion_target_timeout";
      result.message = down ? "M5 press_down target timed out" : "M5 press_up target timed out";
      const MotorState state = feedback_.get(motorId);
      if (down) {
        result.downPulse = state.inputPulseSteps;
      } else {
        result.finalPulse = state.inputPulseSteps;
      }
      return false;
    }
    const MotorState state = feedback_.get(motorId);
    if (down) {
      result.downPulse = state.inputPulseSteps;
      result.downReachedSeen = state.lastMotionReachedMs >= issueMs;
    } else {
      result.finalPulse = state.inputPulseSteps;
      result.upReachedSeen = state.lastMotionReachedMs >= issueMs;
    }
    return true;
  }

  bool waitForPressAck(uint32_t issueMs, uint16_t timeoutMs) {
    const uint8_t motorId = config_.topology.pressMotorId;
    while (millis() - issueMs <= timeoutMs) {
      canTx_.tick(4);
      canRx_.tick(16);
      const MotorState state = feedback_.get(motorId);
      if (state.lastAckCommand == 0xFD && state.lastAckMs >= issueMs) {
        return true;
      }
      if ((state.lastConditionNotMetCommand == 0xFD && state.lastConditionNotMetMs >= issueMs) ||
          (state.lastMalformedCommand == 0xFD && state.lastMalformedMs >= issueMs)) {
        return false;
      }
      delay(5);
    }
    return false;
  }

  bool waitForPressTarget(uint32_t issueMs, int32_t targetPulse, uint32_t timeoutMs, bool down) {
    const uint8_t motorId = config_.topology.pressMotorId;
    uint32_t lastPollMs = 0;
    while (millis() - issueMs <= timeoutMs) {
      const uint32_t nowMs = millis();
      if (lastPollMs == 0 || nowMs - lastPollMs >= config_.motionRuntime.motionPollIntervalMs) {
        lastPollMs = nowMs;
        motion_.requestInputPulseCount(motorId);
        motion_.requestVelocity(motorId);
      }
      canTx_.tick(4);
      canRx_.tick(16);
      const MotorState state = feedback_.get(motorId);
      if (state.lastMotionReachedMs >= issueMs) {
        return true;
      }
      if (state.hasInputPulse && state.hasVelocity &&
          nowMs - state.lastInputPulseMs <= kFeedbackFreshMs &&
          nowMs - state.lastVelocityMs <= kFeedbackFreshMs &&
          absoluteSteps(state.inputPulseSteps - targetPulse) <= config_.motionRuntime.positionToleranceSteps &&
          fabs(state.velocityRpm) <= config_.motionRuntime.idleVelocityThresholdRpm) {
        return true;
      }
      delay(5);
    }
    (void)down;
    return false;
  }

  void logPressDiagIssue(const char* label,
                         int32_t signedSteps,
                         MotorDirection direction,
                         size_t queueBefore,
                         bool enqueued,
                         uint32_t issueMs) {
    log_.print("[press_diag_m5] blockKind=");
    log_.print(label);
    log_.print(" motorId=");
    log_.print(config_.topology.pressMotorId);
    log_.print(" command=moveRelative signedSteps=");
    log_.print(signedSteps);
    log_.print(" direction=");
    log_.print(direction == MotorDirection::Cw ? "cw" : "ccw");
    log_.print(" rpm=");
    log_.print(config_.pressMotor.rpm);
    log_.print(" acceleration=");
    log_.print(config_.pressMotor.acceleration);
    log_.print(" sync=0 canTxAvailableForWriteBefore=");
    log_.print(queueBefore);
    log_.print(" enqueueResult=");
    log_.print(enqueued ? 1 : 0);
    log_.print(" framesEnqueued=");
    log_.print(enqueued ? EmmV5Driver::moveRelativeFrameCount() : 0);
    log_.print(" commandIssueMs=");
    log_.println(issueMs);
  }

  void printPressDiagTrace(PressDiagResult& result) {
    ProtocolTraceItem items[ProtocolTrace::capacity()];
    const size_t count = protocolTrace_.snapshot(items, ProtocolTrace::capacity());
    result.traceCount = count;
    log_.print("[press_diag_m5] ok=");
    log_.print(result.ok ? 1 : 0);
    log_.print(" code=");
    log_.print(result.code != nullptr ? result.code : "");
    log_.print(" initialPulse=");
    log_.print(result.initialPulse);
    log_.print(" downPulse=");
    log_.print(result.downPulse);
    log_.print(" finalPulse=");
    log_.println(result.finalPulse);
    for (size_t i = 0; i < count; ++i) {
      log_.print("[press_diag_m5_trace] timeMs=");
      log_.print(items[i].timeMs);
      log_.print(" dir=");
      log_.print(items[i].dir);
      log_.print(" motorId=");
      log_.print(items[i].motorId);
      log_.print(" command=0x");
      printHexByte(items[i].command);
      log_.print(" status=0x");
      printHexByte(items[i].status);
      log_.print(" packetIndex=");
      log_.print(items[i].packetIndex);
      log_.print(" canId=");
      log_.print(items[i].canId);
      log_.print(" parsed=");
      log_.print(items[i].parsed);
      log_.print(" data=");
      log_.println(items[i].dataHex);
    }
  }

  void printHexByte(uint8_t value) {
    static const char kHex[] = "0123456789ABCDEF";
    log_.print(kHex[(value >> 4) & 0x0F]);
    log_.print(kHex[value & 0x0F]);
  }

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
    probeMotorsBestEffort(150, true);
  }

  bool emergencyStopWithReason(const char* code, const char* message) {
    canTx_.clear();
    executor_.emergencyStop();
    canTx_.tick(8);
    disableAllMotorsBestEffort();
    setFault(code, message);
    jobState_ = JobState::Failed;
    if (remoteCommandActive_) {
      finalizeRemoteGroup("failed", code, message, remoteElapsedMs());
    }
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

  uint32_t remoteElapsedMs() const {
    return remoteCommandStartedAtMs_ == 0 ? 0 : static_cast<uint32_t>(millis() - remoteCommandStartedAtMs_);
  }

  void finalizeRemoteGroup(const char* status, const char* code, const char* message, uint32_t durationMs) {
    if (!remoteCommandActive_ || remoteFinalPending_) {
      return;
    }
    copyString(remoteFinalGroupId_, sizeof(remoteFinalGroupId_), currentRemoteCommandId_);
    remoteFinalSeq_ = currentRemoteSeq_;
    remoteFinalStatus_ = status != nullptr && status[0] != '\0' ? status : "failed";
    remoteFinalCode_ = code != nullptr ? code : "";
    remoteFinalMessage_ = message != nullptr ? message : "";
    lastRemoteCommandDurationMs_ = durationMs;
    if (strcmp(remoteFinalStatus_, "done") == 0) {
      copyString(lastCompletedRemoteCommandId_, sizeof(lastCompletedRemoteCommandId_), currentRemoteCommandId_);
    }
    remoteFinalPending_ = true;
    remoteGroupStartedPending_ = false;
    remoteBlockStartedPending_ = false;
    remoteBlockDonePending_ = false;
  }

  MachinePointMm plannedRemoteCompletionPoint() const {
    if (activeMotionSteps_.count == 0) {
      return executor_.currentPoint();
    }
    const MotionStep& step = activeMotionSteps_.steps[activeMotionSteps_.count - 1];
    if (step.kind == MotionStepKind::MoveXY || step.kind == MotionStepKind::LineFeed ||
        step.kind == MotionStepKind::ReturnZero) {
      return step.targetMm;
    }
    return executor_.currentPoint();
  }

  void disableAllMotorsBestEffort() {
    motion_.disableMotor(config_.topology.xMotorId, false, true);
    motion_.disableMotor(config_.topology.yLeftMotorId, false, true);
    motion_.disableMotor(config_.topology.yRightMotorId, false, true);
    motion_.disableMotor(config_.topology.lineFeedMotorId, false, true);
    motion_.disableMotor(config_.topology.pressMotorId, false, true);
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
    if (step.profile.maxRpm == 0 || step.profile.acceleration == 0 || step.profile.timeoutMs == 0 ||
        step.profile.timeoutMs > kMaxBlockTimeoutMs) {
      result.rejectionCode = "invalid_block";
      result.rejectionMessage = "Invalid remote step profile";
      return false;
    }

    switch (remoteStep.kind) {
      case RemoteMotionStepKind::MoveXY: {
        step.kind = MotionStepKind::MoveXY;
        step.deltaSteps.x = remoteStep.dxSteps;
        step.deltaSteps.yLeft = -remoteStep.dySteps;
        step.deltaSteps.yRight = remoteStep.dySteps;
        step.deltaSteps.lineFeed = 0;
        step.deltaSteps.press = 0;
        const float stepsPerMmValue = kinematicsStepsPerMm(config_.calibration);
        const float dxMm = stepsPerMmValue == 0.0f ? 0.0f : static_cast<float>(remoteStep.dxSteps) / stepsPerMmValue;
        const float dyMm = stepsPerMmValue == 0.0f ? 0.0f : static_cast<float>(remoteStep.dySteps) / stepsPerMmValue;
        step.targetMm = {currentPoint.xMm + dxMm, currentPoint.yMm + dyMm};
        step.profile.settleMs = config_.xProfile.settleMs > config_.yProfile.settleMs ? config_.xProfile.settleMs
                                                                                        : config_.yProfile.settleMs;
        return true;
      }
      case RemoteMotionStepKind::PressDown:
        step.kind = MotionStepKind::PressDown;
        step.deltaSteps.press = config_.pressMotor.pressDeltaSteps;
        step.profile.maxRpm = config_.pressMotor.rpm;
        step.profile.acceleration = config_.pressMotor.acceleration;
        step.profile.settleMs = config_.pressMotor.settleMs;
        step.profile.timeoutMs = config_.pressMotor.timeoutMs;
        return true;
      case RemoteMotionStepKind::PressUp:
        step.kind = MotionStepKind::PressUp;
        step.deltaSteps.press = config_.pressMotor.releaseDeltaSteps;
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
            signedStepsForDirection(config_.lineFeed.returnTotalSteps * remoteStep.lines, config_.lineFeed.returnDirection);
        step.profile.maxRpm = config_.lineFeed.rpm;
        step.profile.acceleration = config_.lineFeed.acceleration;
        step.profile.settleMs = config_.lineFeed.settleMs;
        step.targetMm = {config_.homePoint.xMm, currentPoint.yMm};
        return true;
      case RemoteMotionStepKind::ReturnZero:
        step.kind = MotionStepKind::ReturnZero;
        step.profile.settleMs = max3(config_.xProfile.settleMs, config_.yProfile.settleMs, config_.pressMotor.settleMs);
        step.targetMm = config_.homePoint;
        return true;
      case RemoteMotionStepKind::Wait:
        step.kind = MotionStepKind::Wait;
        step.waitMs = remoteStep.durationMs;
        return true;
    }
    result.rejectionCode = "invalid_block";
    result.rejectionMessage = "Unsupported remote block type";
    return false;
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

  static uint16_t max3(uint16_t a, uint16_t b, uint16_t c) {
    const uint16_t ab = a > b ? a : b;
    return ab > c ? ab : c;
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

  void probeMotorsBestEffort(uint16_t pumpMs, bool includeConfig) {
    requestMotorFeedback(includeConfig);
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
      if (step.kind == MotionStepKind::PressDown || step.kind == MotionStepKind::PressUp || step.deltaSteps.press != 0) {
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
    if ((step.kind == MotionStepKind::PressDown || step.kind == MotionStepKind::PressUp || step.deltaSteps.press != 0) &&
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
    if (state.hasRecentInputPulse && state.hasRecentVelocity) {
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

  static const char* motionStepKindText(MotionStepKind kind) {
    switch (kind) {
      case MotionStepKind::MoveXY:
        return "move_xy";
      case MotionStepKind::LineFeed:
        return "line_feed";
      case MotionStepKind::CharacterRelease:
        return "character_release";
      case MotionStepKind::PressDown:
        return "press_down";
      case MotionStepKind::PressUp:
        return "press_up";
      case MotionStepKind::ReturnZero:
        return "return_zero";
      case MotionStepKind::Wait:
      default:
        return "wait";
    }
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
  RemoteGroupState remoteGroupState_;
  bool remoteGroupStartedPending_;
  bool remoteDoneNotified_;
  bool remoteFaultNotified_;
  bool remoteFinalPending_;
  const char* remoteFinalStatus_;
  const char* remoteFinalCode_;
  const char* remoteFinalMessage_;
  char remoteFinalGroupId_[49];
  uint32_t remoteFinalSeq_;
  bool remoteBlockStartedPending_;
  bool remoteBlockDonePending_;
  uint32_t currentRemoteSeq_;
  size_t lastRemoteBlockStartedIndex_;
  size_t lastRemoteBlockDoneIndex_;
  uint32_t remoteCommandStartedAtMs_;
  uint32_t lastRemoteCommandDurationMs_;
  static constexpr uint8_t kTrackedMotorCount = 5;
  static constexpr uint32_t kFeedbackFreshMs = 1500;
  static constexpr uint32_t kProbeOfflineMs = 250;
  static constexpr uint32_t kRecentAlertWindowMs = 5000;
  static constexpr uint16_t kRemoteDefaultRpm = 2000;
  static constexpr uint8_t kRemoteDefaultAccelerationRaw = 128;
  uint32_t lastMotorProbeMs_[kTrackedMotorCount];
};

}  // namespace auto_typer
