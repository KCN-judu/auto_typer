#pragma once

#include <Adafruit_PWMServoDriver.h>

#include "appConfig.h"
#include "appLogic.h"
#include "appTypes.h"

namespace test_app {

class ServoHal {
 public:
  explicit ServoHal(const ServoConfig& config) : config_(config), driver_(config.address), ready_(false) {}

  bool init() {
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

  void apply(ServoMotion motion) {
    if (!ready_) {
      return;
    }

    const uint16_t pulseUs = pulseFor(motion);
    for (uint8_t i = 0; i < config_.channelCount; ++i) {
      driver_.writeMicroseconds(config_.channels[i], pulseUs);
    }
  }

  ServoTestResult runConnectivitySweep() {
    if (!ready_) {
      return summarizeServoTest(false, 0);
    }

    size_t stepCount = 0;
    const ServoStep* steps = servoSequence(stepCount);
    for (size_t i = 0; i < stepCount; ++i) {
      apply(steps[i].motion);
      if (steps[i].dwellMs > 0) {
        delay(steps[i].dwellMs);
      }
    }
    disableOutputs();

    return summarizeServoTest(true, static_cast<uint8_t>(stepCount));
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

  ServoConfig config_;
  Adafruit_PWMServoDriver driver_;
  bool ready_;
};

}  // namespace test_app
