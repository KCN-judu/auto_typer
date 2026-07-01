#pragma once

#include <Adafruit_PWMServoDriver.h>

#include "auto_typer_types.h"

namespace auto_typer {

class ServoPressHal {
 public:
  explicit ServoPressHal(const ServoActuatorConfig& config)
      : config_(config),
        driver_(config.address),
        ready_(false),
        actionActive_(false),
        actionStartedAtMs_(0),
        actionDwellMs_(0),
        settling_(false) {}

  bool begin() {
    ready_ = driver_.begin();
    if (!ready_) {
      return false;
    }
    driver_.setOscillatorFrequency(config_.oscillatorHz);
    driver_.setPWMFreq(config_.pwmFrequencyHz);
    delay(10);
    disableOutputs();
    return true;
  }

  bool apply(PressAction action) {
    return apply(action, action == PressAction::Release ? config_.releaseMs : config_.pressMs);
  }

  bool apply(PressAction action, uint16_t dwellMs) {
    if (!start(action, dwellMs)) {
      return false;
    }
    while (busy()) {
      tick();
      delay(1);
    }
    return true;
  }

  bool start(PressAction action, uint16_t dwellMs) {
    if (!ensureReady()) {
      return false;
    }
    const ServoMotion motion =
        action == PressAction::Release ? config_.semantics.releaseMotion : config_.semantics.pressMotion;
    writeMotion(motion);
    actionActive_ = true;
    settling_ = false;
    actionStartedAtMs_ = millis();
    actionDwellMs_ = dwellMs;
    return true;
  }

  void tick() {
    if (!actionActive_) {
      return;
    }
    const uint32_t elapsed = millis() - actionStartedAtMs_;
    if (!settling_ && elapsed >= actionDwellMs_) {
      writeMotion(ServoMotion::Neutral);
      settling_ = true;
      actionStartedAtMs_ = millis();
      return;
    }
    if (settling_ && elapsed >= config_.settleMs) {
      disableOutputs();
      actionActive_ = false;
      settling_ = false;
    }
  }

  bool busy() const {
    return actionActive_;
  }

  bool release() {
    return apply(PressAction::Release);
  }

  bool press() {
    return apply(PressAction::Press);
  }

  bool neutral() {
    return applyMotion(ServoMotion::Neutral, config_.settleMs);
  }

  bool neutral(uint16_t dwellMs) {
    return applyMotion(ServoMotion::Neutral, dwellMs);
  }

  bool ready() const {
    return ready_;
  }

 private:
  void disableOutputs() {
    if (!ready_) {
      return;
    }

    for (uint8_t i = 0; i < config_.channelCount; ++i) {
      driver_.setPin(config_.channels[i], 0);
    }
  }

  bool applyMotion(ServoMotion motion, uint16_t dwellMs) {
    if (!ensureReady()) {
      return false;
    }
    writeMotion(motion);
    delay(dwellMs);
    disableOutputs();
    return true;
  }

  bool ensureReady() {
    return ready_ || begin();
  }

  void writeMotion(ServoMotion motion) {
    const uint16_t pulseUs = pulseFor(motion);
    for (uint8_t i = 0; i < config_.channelCount; ++i) {
      driver_.writeMicroseconds(config_.channels[i], pulseUs);
    }
  }

  uint16_t pulseFor(ServoMotion motion) const {
    switch (motion) {
      case ServoMotion::Forward:
        return config_.forwardPulseUs;
      case ServoMotion::Reverse:
        return config_.reversePulseUs;
      case ServoMotion::Neutral:
      default:
        return config_.neutralPulseUs;
    }
  }

  ServoActuatorConfig config_;
  Adafruit_PWMServoDriver driver_;
  bool ready_;
  bool actionActive_;
  uint32_t actionStartedAtMs_;
  uint16_t actionDwellMs_;
  bool settling_;
};

}  // namespace auto_typer
