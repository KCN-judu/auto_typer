#pragma once

#include <Arduino.h>

#include "app_config.h"
#include "app_logic.h"
#include "hal_can_motor.h"
#include "hal_display.h"
#include "hal_i2c.h"
#include "hal_servo.h"

namespace test_app {

class TestApplication {
 public:
  TestApplication(const AppConfig& config,
                  I2cBusHal& i2cBus,
                  DisplayHal& display,
                  ServoHal& servo,
                  CanMotorHal& motor,
                  Print& log)
      : config_(config),
        i2cBus_(i2cBus),
        display_(display),
        servo_(servo),
        motor_(motor),
        log_(log),
        schedule_(defaultTestLoopSchedule()),
        canPlan_(defaultCanCommandPlan()),
        snapshot_(defaultAppSnapshot()),
        initializedOledAddress_(0),
        servoInitialized_(false),
        lastI2cAtMs_(0),
        lastServoAtMs_(0),
        lastCanAtMs_(0) {}

  void printBanner() {
    log_.println();
    log_.println("==== BOOT ====");
    log_.println("test sketch");
    log_.print("Expected CAN bitrate: ");
    log_.println(config_.canBus.bitrate);
    log_.println("Servo note: tie PCA9685 OE to GND if the module OE pin is not wired by the controller board.");
    log_.println("Servo note: connect MG996R external power to PCA9685 V+ and GND.");
  }

  void setup() {
    i2cBus_.begin();
    snapshot_.activeStage = TestStage::Boot;

    log_.println("Waiting 2000 ms for motor CAN module power-up");
    delay(2000);

    refreshI2c();
    runServoCheck(false);
    runCanCheck();

    snapshot_.activeStage = TestStage::Idle;
    updateDisplay();

    log_.println();
    log_.println("Connectivity test sequence complete.");
  }

  void tick(uint32_t nowMs) {
    if (isDue(nowMs, lastI2cAtMs_, schedule_.i2cIntervalMs)) {
      refreshI2c();
      lastI2cAtMs_ = nowMs;
      return;
    }

    if (isDue(nowMs, lastServoAtMs_, schedule_.servoIntervalMs)) {
      runServoCheck(true);
      lastServoAtMs_ = nowMs;
      return;
    }

    if (isDue(nowMs, lastCanAtMs_, schedule_.canIntervalMs)) {
      runCanCheck();
      lastCanAtMs_ = nowMs;
    }
  }

 private:
  static bool isDue(uint32_t nowMs, uint32_t lastRunAtMs, uint32_t intervalMs) {
    return lastRunAtMs == 0 || nowMs - lastRunAtMs >= intervalMs;
  }

  void updateDisplay() {
    if (snapshot_.displayReady) {
      display_.show(frameForSnapshot(snapshot_));
    }
  }

  void refreshI2c() {
    snapshot_.activeStage = TestStage::I2c;
    const DevicePresence devices = i2cBus_.scan(config_.oled, config_.servo, log_);
    const bool hasDisplay = shouldInitDisplay(devices);
    bool displayReady = hasDisplay && display_.ready();
    const bool shouldInitializeDisplay =
        hasDisplay && (!displayReady || initializedOledAddress_ != devices.oledAddress);

    if (shouldInitializeDisplay) {
      displayReady = display_.init(devices.oledAddress);
      if (displayReady) {
        initializedOledAddress_ = devices.oledAddress;
        display_.show(frameBootOk());
      }
    }

    if (!displayReady) {
      display_.markUnavailable();
      initializedOledAddress_ = 0;
      log_.println("OLED not detected at 0x3C or 0x3D");
    } else if (snapshot_.oledAddress != devices.oledAddress) {
      log_.print("OLED address: 0x");
      printHexByte(log_, devices.oledAddress);
      log_.println();
    }

    const I2cRefreshResult result = summarizeI2cRefresh(devices, displayReady);
    snapshot_.devices = result.presence;
    snapshot_.i2cStatus = result.status;
    snapshot_.displayReady = result.displayReady;
    if (!result.servoDetected) {
      servoInitialized_ = false;
      snapshot_.servoReady = false;
    } else if (!servoInitialized_) {
      servoInitialized_ = servo_.init();
      snapshot_.servoReady = servoInitialized_;
    } else {
      snapshot_.servoReady = servo_.ready();
    }
    snapshot_.oledAddress = result.presence.oledAddress;

    updateDisplay();
  }

  void runServoCheck(bool fromLoop) {
    snapshot_.activeStage = TestStage::Servo;
    updateDisplay();
    log_.println();
    log_.println(fromLoop ? "==== SERVO LOOP ====" : "==== SERVO ====");

    if (!snapshot_.servoReady) {
      log_.println("PCA9685 not detected at 0x40");
      snapshot_.servoStatus = TestStatus::Failed;
      updateDisplay();
      return;
    }

    log_.println("PCA9685 detected");
    if (!config_.enableServoMotionTest) {
      log_.println("Servo motion test disabled; PWM outputs remain off.");
      snapshot_.servoStatus = TestStatus::Passed;
      updateDisplay();
      return;
    }

    log_.println("Driving PCA9685 CH0 and CH1 together to cover 0-based/1-based port naming.");

    size_t stepCount = 0;
    const ServoStep* steps = servoSequence(stepCount);
    for (size_t i = 0; i < stepCount; ++i) {
      log_.println(steps[i].logLine);
    }

    const ServoTestResult result = servo_.runConnectivitySweep();
    snapshot_.servoReady = result.deviceReady;
    snapshot_.servoStatus = result.status;
    updateDisplay();
  }

  void runCanCheck() {
    snapshot_.activeStage = TestStage::Can;
    updateDisplay();
    log_.println();
    log_.println("==== CAN ====");

    const CanTestResult result = motor_.runConnectivityCheck(canPlan_);
    snapshot_.canReady = result.controllerReady;
    snapshot_.canStatus = result.status;
    snapshot_.canFeedback = result.feedback;

    if (!result.controllerReady) {
      log_.println("TWAI init failed");
      updateDisplay();
      return;
    }

    log_.println("TWAI init ok");
    log_.println("Setting control mode: closed-loop");
    log_.println("Sending enable command");
    log_.print("Sending velocity command: CW ");
    log_.print(canPlan_.rpm);
    log_.print(" RPM, acc=");
    log_.println(canPlan_.acceleration);
    log_.println("Reading live velocity");
    printCanFeedback(log_, result.feedback);
    updateDisplay();
  }

  const AppConfig& config_;
  I2cBusHal& i2cBus_;
  DisplayHal& display_;
  ServoHal& servo_;
  CanMotorHal& motor_;
  Print& log_;
  const TestLoopSchedule schedule_;
  const CanCommandPlan canPlan_;
  AppSnapshot snapshot_;
  uint8_t initializedOledAddress_;
  bool servoInitialized_;
  uint32_t lastI2cAtMs_;
  uint32_t lastServoAtMs_;
  uint32_t lastCanAtMs_;
};

}  // namespace test_app
