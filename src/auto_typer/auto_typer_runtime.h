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
#include "typing_logic.h"

namespace auto_typer {

class AutoTyperApplication {
 public:
  AutoTyperApplication(const TypingConfig& config,
                       DisplayHal& display,
                       CanBus& canBus,
                       CanTxQueue& canTx,
                       CanRxTask& canRx,
                       EmmV5Driver& motion,
                       ServoPressHal& servo,
                       MotorFeedbackStore& feedback,
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
        deviceReadyWarning_(false),
        faulted_(false),
        faultCode_(""),
        faultMessage_("") {}

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
    canBus_.setSoftFaultEscalationEnabled(false);
    prepareMotors();
    canBus_.setSoftFaultEscalationEnabled(true);
    if (faulted_) {
      return;
    }
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
      showError();
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
    if (!motionReady()) {
      result.planStatus = PlanStatus::DeviceNotReady;
      result.rejectionCode = "motion_not_ready";
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
    deviceReadyWarning_ = false;
    canBus_.setSoftFaultEscalationEnabled(false);
    requestMotorFeedback();
    const uint32_t startedAt = millis();
    while (millis() - startedAt < 300) {
      canTx_.tick(4);
      canRx_.tick(16);
      delay(10);
    }
    canBus_.setSoftFaultEscalationEnabled(true);
    refreshDeviceReadyWarning();
    if (!recovered || canBus_.hasFatalFault()) {
      const CanBusDiagnostics diagnostics = canBus_.diagnostics();
      setFault(diagnostics.lastFaultCode, diagnostics.lastFaultMessage);
      showError();
      return false;
    }
    display_.showStatus(DisplayStatus::Idle);
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
    return motion_.moveRelative(motorId, direction, rpm, acceleration, steps, sync) &&
           (!sync || motion_.triggerSynchronousMotion(motorId));
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
    return feedback_.get(motorId);
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
            executor_.currentPoint(),
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
    return canBus_.motionReady() && !deviceReadyWarning_;
  }

  bool healthWarning() const {
    return !faulted_ && (!motionReady() || canBus_.diagnostics().lastAlerts != 0 || deviceReadyWarning_);
  }

  CanBusDiagnostics canDiagnostics() const {
    return canBus_.diagnostics();
  }

  size_t protocolTraceSnapshot(ProtocolTraceItem* out, size_t maxItems) const {
    return protocolTrace_.snapshot(out, maxItems);
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

  void prepareMotors() {
    const uint8_t motors[] = {
      config_.topology.xMotorId,
      config_.topology.yLeftMotorId,
      config_.topology.yRightMotorId,
      config_.topology.lineFeedMotorId,
    };
    for (uint8_t motor : motors) {
      if (!motion_.setClosedLoopControlMode(motor) || !motion_.enableMotor(motor)) {
        deviceReadyWarning_ = true;
      }
      motion_.requestStatusFlags(motor);
      motion_.requestInputPulseCount(motor);
    }
    const uint32_t startedAt = millis();
    while (millis() - startedAt < 800) {
      canTx_.tick(4);
      canRx_.tick(16);
      delay(10);
    }
    for (uint8_t motor : motors) {
      const MotorState state = feedback_.get(motor);
      if (state.driverFault) {
        setFault("motor_fault", "Motor reported a fault");
        showError();
        return;
      }
      if (state.lastAnyFrameMs == 0) {
        deviceReadyWarning_ = true;
      }
    }
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

  void requestMotorFeedback() {
    const uint8_t motors[] = {
      config_.topology.xMotorId,
      config_.topology.yLeftMotorId,
      config_.topology.yRightMotorId,
      config_.topology.lineFeedMotorId,
    };
    for (uint8_t motor : motors) {
      motion_.requestStatusFlags(motor);
      motion_.requestInputPulseCount(motor);
      motion_.requestVelocity(motor);
    }
  }

  void refreshDeviceReadyWarning() {
    const uint8_t motors[] = {
      config_.topology.xMotorId,
      config_.topology.yLeftMotorId,
      config_.topology.yRightMotorId,
      config_.topology.lineFeedMotorId,
    };
    deviceReadyWarning_ = false;
    for (uint8_t motor : motors) {
      const MotorState state = feedback_.get(motor);
      if (state.lastAnyFrameMs == 0) {
        deviceReadyWarning_ = true;
      }
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
    if (deviceReadyWarning_) {
      return "Motor feedback is not ready";
    }
    if (diagnostics.rxQueueFullCount > 0) {
      return "CAN RX queue has overflow warnings";
    }
    if (diagnostics.txFailedCount > 0 || diagnostics.busErrorCount > 0) {
      return "CAN warning counters are non-zero";
    }
    return "Motion subsystem is not ready";
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
  bool deviceReadyWarning_;
  bool faulted_;
  const char* faultCode_;
  const char* faultMessage_;
};

}  // namespace auto_typer
