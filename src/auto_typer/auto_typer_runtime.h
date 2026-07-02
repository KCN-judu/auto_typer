#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "auto_typer_config.h"
#include "can/CanRxTask.h"
#include "can/CanTxQueue.h"
#include "can/ProtocolTrace.h"
#include "drivers/EmmV5Driver.h"
#include "hal_display.h"
#include "hal_servo_press.h"
#include "keymap_feiyu200.h"
#include "keymap_store.h"
#include "motion/MotionExecutor.h"
#include "motion/MotionPlanner.h"
#include "protocol/DesktopCommand.h"
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
                       ServoPressHal& servo,
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
        servo_(servo),
        feedback_(feedback),
        events_(events),
        protocolTrace_(protocolTrace),
        executor_(config, motion, servo, feedback),
        log_(log),
        keymapCount_(0),
        jobState_(JobState::None),
        jobId_(0),
        activeTextLength_(0),
        activePlan_{},
        activeBlocks_{},
        lastPlanStatus_(PlanStatus::Ok),
        failedKey_('\0'),
        faulted_(false),
        faultCode_(""),
        faultMessage_(""),
        remoteBlock_{},
        remoteCurrentPoint_(config.homePoint),
        currentRemoteCommandId_{},
        lastCompletedRemoteCommandId_{},
        lastCompletedRemoteOp_(""),
        remoteCommandActive_(false),
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
    if (!servo_.begin()) {
      log_.println("Servo driver init failed");
    } else {
      log_.println("Servo driver initialized with outputs disabled");
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
      if (!executor_.start(activeBlocks_.blocks, activeBlocks_.count)) {
        setFault("executor_busy", "Motion executor rejected job");
        jobState_ = JobState::Failed;
        showError();
        return;
      }
      jobState_ = JobState::Running;
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
      showError();
    }

    if (remoteCommandActive_) {
      if (executor_.blockStartedEvent() && remoteCommandStartedAtMs_ == 0) {
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
        display_.showStatus(DisplayStatus::Idle);
      } else if (executor_.cancelled()) {
        jobState_ = JobState::Cancelled;
        currentRemoteCommandId_[0] = '\0';
        remoteCommandActive_ = false;
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
    activePlan_ = planText(text, keymap_, keymapCount_, config_);
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

    activeBlocks_ = planMotionBlocks(activePlan_, config_);
    if (activeBlocks_.status != PlanStatus::Ok) {
      lastPlanStatus_ = activeBlocks_.status;
      jobState_ = JobState::None;
      result.planStatus = activeBlocks_.status;
      result.rejectionCode = statusText(activeBlocks_.status);
      result.rejectionMessage = statusText(activeBlocks_.status);
      result.stepCount = activeBlocks_.count;
      return result;
    }

    const RequiredActuatorCheck required = checkRequiredActuators(activeBlocks_);
    if (!required.ready) {
      jobState_ = JobState::None;
      result.planStatus = PlanStatus::DeviceNotReady;
      result.rejectionCode = required.code;
      result.rejectionMessage = required.message;
      result.stepCount = activeBlocks_.count;
      return result;
    }

    jobState_ = JobState::Queued;
    result.accepted = true;
    result.jobId = jobId_;
    result.planStatus = PlanStatus::Ok;
    result.stepCount = activeBlocks_.count;
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
    return true;
  }

  SubmitRemoteCommandResult submitRemoteCommand(const DesktopCommand& command) {
    SubmitRemoteCommandResult result{false, "", "", false};
    if (strcmp(command.id, currentRemoteCommandId_) == 0 && remoteCommandActive_) {
      result.accepted = true;
      result.code = "duplicate_running";
      result.message = "Command is already running";
      return result;
    }
    if (strcmp(command.id, lastCompletedRemoteCommandId_) == 0 && lastCompletedRemoteCommandId_[0] != '\0') {
      result.accepted = true;
      result.code = "duplicate_completed";
      result.message = "Command already completed";
      result.duplicateCompleted = true;
      remoteDoneNotified_ = true;
      return result;
    }

    if (command.op == DesktopCommandOp::Cancel) {
      cancelCurrentJob();
      finishImmediateRemoteCommand(command);
      result.accepted = true;
      return result;
    }
    if (command.op == DesktopCommandOp::ResetFault) {
      const bool ok = resetFault();
      result.accepted = ok;
      result.code = ok ? "" : "reset_rejected";
      result.message = ok ? "" : "Fault reset rejected";
      if (ok) {
        finishImmediateRemoteCommand(command);
      }
      return result;
    }
    if (command.op == DesktopCommandOp::EmergencyStop) {
      emergencyStop();
      finishImmediateRemoteCommand(command);
      remoteFaultNotified_ = true;
      result.accepted = true;
      return result;
    }

    if (faulted_ || executor_.faulted()) {
      result.code = "faulted";
      result.message = faultMessage_;
      return result;
    }
    if (jobState_ == JobState::Queued || jobState_ == JobState::Planning || jobState_ == JobState::Running ||
        jobState_ == JobState::Cancelling || remoteCommandActive_) {
      result.code = "busy";
      result.message = "Device is already executing a command";
      return result;
    }
    if (!motionTransportReady()) {
      result.code = "device_not_ready";
      result.message = motionNotReadyMessage();
      return result;
    }

    MotionBlock motionBlock{};
    if (!convertRemoteCommand(command, motionBlock, result)) {
      return result;
    }

    MotionBlockPlan single{};
    single.status = PlanStatus::Ok;
    single.count = 1;
    single.blocks[0] = motionBlock;
    const RequiredActuatorCheck required = checkRequiredActuators(single);
    if (!required.ready) {
      result.code = "device_not_ready";
      result.message = required.message;
      return result;
    }

    remoteBlock_ = motionBlock;
    copyString(currentRemoteCommandId_, sizeof(currentRemoteCommandId_), command.id);
    lastCompletedRemoteOp_ = commandOpText(command.op);
    remoteDoneNotified_ = false;
    remoteFaultNotified_ = false;
    remoteCommandStartedAtMs_ = 0;
    lastRemoteCommandDurationMs_ = 0;
    if (!executor_.start(&remoteBlock_, 1, remoteCurrentPoint_)) {
      currentRemoteCommandId_[0] = '\0';
      result.code = "busy";
      result.message = "Motion executor rejected command";
      return result;
    }
    remoteCommandActive_ = true;
    jobState_ = JobState::Running;
    ++jobId_;
    activeTextLength_ = 0;
    activePlan_ = TypingPlan{};
    activeBlocks_ = single;
    display_.showStatus(DisplayStatus::Printing);
    result.accepted = true;
    return result;
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
    return dwellMs > 0 ? servo_.apply(action, dwellMs) : servo_.apply(action);
  }

  bool debugServoNeutral(uint16_t dwellMs = 0) {
    if (!canDebug()) {
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

  MotorState motorState(uint8_t motorId) const {
    return enrichedMotorState(motorId);
  }

  JobSnapshot snapshot() const {
    return {jobState_,
            jobId_,
            activeTextLength_,
            executor_.activeTextIndex(),
            executor_.currentBlock(),
            activePlan_.count,
            executor_.currentBlock(),
            activeBlocks_.count,
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
    return servo_.ready();
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

  bool consumeRemoteDone(char* outCommandId, size_t outCommandIdSize, const char*& op, uint32_t& durationMs, MachinePointMm& point) {
    if (!remoteDoneNotified_) {
      return false;
    }
    remoteDoneNotified_ = false;
    copyString(outCommandId, outCommandIdSize, lastCompletedRemoteCommandId_);
    op = lastCompletedRemoteOp_;
    durationMs = lastRemoteCommandDurationMs_;
    point = remoteCurrentPoint_;
    return true;
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

  bool convertRemoteCommand(const DesktopCommand& command, MotionBlock& block, SubmitRemoteCommandResult& result) const {
    block = MotionBlock{};
    block.profile = defaultMotionProfile(config_);
    block.profile.maxRpm = command.profile.hasRpm ? command.profile.rpm : kRemoteDefaultRpm;
    block.profile.acceleration =
        command.profile.hasAccelRaw ? command.profile.accelRaw : kRemoteDefaultAccelerationRaw;
    block.profile.timeoutMs =
        command.profile.hasTimeoutMs ? command.profile.timeoutMs : config_.motionRuntime.motionTimeoutMs;
    block.targetMm = remoteCurrentPoint_;
    if (block.profile.maxRpm == 0 || block.profile.acceleration == 0 || block.profile.timeoutMs == 0) {
      result.code = "invalid_command";
      result.message = "Invalid remote command profile";
      return false;
    }

    switch (command.op) {
      case DesktopCommandOp::MoveTo: {
        if (!isfinite(command.xMm) || !isfinite(command.yMm)) {
          result.code = "invalid_command";
          result.message = "Move target must be finite";
          return false;
        }
        const MachinePointMm current = remoteCurrentPoint_;
        const MachinePointMm target{command.xMm, command.yMm};
        block.kind = MotionBlockKind::MoveXY;
        block.targetMm = target;
        block.deltaSteps = xyDeltaSteps(current, target, config_.calibration);
        block.profile.settleMs = config_.xProfile.settleMs > config_.yProfile.settleMs ? config_.xProfile.settleMs
                                                                                        : config_.yProfile.settleMs;
        return true;
      }
      case DesktopCommandOp::Press:
        block.kind = MotionBlockKind::ServoPress;
        block.waitMs = command.durationMs > 0 ? static_cast<uint16_t>(command.durationMs) : config_.servo.pressMs;
        return true;
      case DesktopCommandOp::Release:
        block.kind = MotionBlockKind::ServoRelease;
        block.waitMs = command.durationMs > 0 ? static_cast<uint16_t>(command.durationMs) : config_.servo.releaseMs;
        return true;
      case DesktopCommandOp::CharacterRelease:
        block.kind = MotionBlockKind::CharacterRelease;
        block.deltaSteps.lineFeed =
            signedStepsForDirection(config_.lineFeed.characterReleaseSteps, config_.lineFeed.releaseDirection);
        block.profile.maxRpm = config_.lineFeed.rpm;
        block.profile.acceleration = config_.lineFeed.acceleration;
        block.profile.settleMs = config_.lineFeed.characterReleaseSettleMs;
        return true;
      case DesktopCommandOp::LineFeed:
        block.kind = MotionBlockKind::LineFeed;
        block.deltaSteps.lineFeed =
            signedStepsForDirection(config_.lineFeed.returnTotalSteps, config_.lineFeed.returnDirection);
        block.profile.maxRpm = config_.lineFeed.rpm;
        block.profile.acceleration = config_.lineFeed.acceleration;
        block.profile.settleMs = config_.lineFeed.settleMs;
        block.targetMm = {config_.homePoint.xMm, remoteCurrentPoint_.yMm};
        return true;
      case DesktopCommandOp::Wait:
        if (command.durationMs > 65535) {
          result.code = "invalid_command";
          result.message = "Wait duration is too large";
          return false;
        }
        block.kind = MotionBlockKind::Wait;
        block.waitMs = static_cast<uint16_t>(command.durationMs);
        return true;
      case DesktopCommandOp::Cancel:
      case DesktopCommandOp::ResetFault:
      case DesktopCommandOp::EmergencyStop:
        break;
    }
    result.code = "invalid_command";
    result.message = "Unsupported motion command op";
    return false;
  }

  void finishImmediateRemoteCommand(const DesktopCommand& command) {
    copyString(lastCompletedRemoteCommandId_, sizeof(lastCompletedRemoteCommandId_), command.id);
    lastCompletedRemoteOp_ = commandOpText(command.op);
    lastRemoteCommandDurationMs_ = 0;
    currentRemoteCommandId_[0] = '\0';
    remoteDoneNotified_ = true;
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

  RequiredActuatorCheck checkRequiredActuators(const MotionBlockPlan& blocks) const {
    bool needsX = false;
    bool needsY = false;
    bool needsLineFeed = false;
    bool needsServo = false;
    for (size_t i = 0; i < blocks.count; ++i) {
      const MotionBlock& block = blocks.blocks[i];
      if (block.deltaSteps.x != 0) {
        needsX = true;
      }
      if (block.deltaSteps.yLeft != 0 || block.deltaSteps.yRight != 0) {
        needsY = true;
      }
      if (block.deltaSteps.lineFeed != 0) {
        needsLineFeed = true;
      }
      if (block.kind == MotionBlockKind::ServoPress || block.kind == MotionBlockKind::ServoRelease) {
        needsServo = true;
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
    if (needsServo && !servo_.ready()) {
      return {false, "servo_not_ready", "Servo controller is not ready"};
    }
    return {true, "", ""};
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
    return -1;
  }

  void showError() {
    display_.showStatus(DisplayStatus::Error);
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
  ServoPressHal& servo_;
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
  MotionBlockPlan activeBlocks_;
  PlanStatus lastPlanStatus_;
  char failedKey_;
  bool faulted_;
  const char* faultCode_;
  const char* faultMessage_;
  MotionBlock remoteBlock_;
  MachinePointMm remoteCurrentPoint_;
  char currentRemoteCommandId_[49];
  char lastCompletedRemoteCommandId_[49];
  const char* lastCompletedRemoteOp_;
  bool remoteCommandActive_;
  bool remoteDoneNotified_;
  bool remoteFaultNotified_;
  uint32_t remoteCommandStartedAtMs_;
  uint32_t lastRemoteCommandDurationMs_;
  static constexpr uint8_t kTrackedMotorCount = 4;
  static constexpr uint32_t kFeedbackFreshMs = 1500;
  static constexpr uint32_t kProbeOfflineMs = 250;
  static constexpr uint32_t kRecentAlertWindowMs = 5000;
  static constexpr uint16_t kRemoteDefaultRpm = 1600;
  static constexpr uint8_t kRemoteDefaultAccelerationRaw = 128;
  uint32_t lastMotorProbeMs_[kTrackedMotorCount];
};

}  // namespace auto_typer
