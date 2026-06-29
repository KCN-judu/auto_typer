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
    if (!ensureReady()) {
      return false;
    }
    const ServoMotion motion =
        action == PressAction::Release ? config_.semantics.releaseMotion : config_.semantics.pressMotion;
    const uint16_t pulseUs = pulseFor(motion);
    for (uint8_t i = 0; i < config_.channelCount; ++i) {
      driver_.writeMicroseconds(config_.channels[i], pulseUs);
    }
    delay(action == PressAction::Release ? config_.releaseMs : config_.pressMs);
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
    const uint16_t pulseUs = pulseFor(motion);
    for (uint8_t i = 0; i < config_.channelCount; ++i) {
      driver_.writeMicroseconds(config_.channels[i], pulseUs);
    }
    delay(dwellMs);
    disableOutputs();
    return true;
  }

  bool ensureReady() {
    return ready_ || begin();
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
