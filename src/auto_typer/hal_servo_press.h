#pragma once

#include <Adafruit_PWMServoDriver.h>

#include "auto_typer_types.h"

namespace auto_typer {

class ServoPressHal {
 public:
  explicit ServoPressHal(const ServoActuatorConfig& config)
      : config_(config), driver_(config.address), ready_(false) {}

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
    if (!ensureReady()) {
      return false;
    }
    const ServoMotion motion =
        action == PressAction::Release ? config_.semantics.releaseMotion : config_.semantics.pressMotion;
    writeMotion(motion);
    delay(dwellMs);
    writeMotion(ServoMotion::Neutral);
    delay(config_.settleMs);
    disableOutputs();
    return true;
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
};

}  // namespace auto_typer
