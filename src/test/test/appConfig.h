#pragma once

#include <Arduino.h>

namespace test_app {

struct I2cConfig {
  uint8_t sdaPin;
  uint8_t sclPin;
  uint32_t frequencyHz;
};

struct OledConfig {
  uint8_t primaryAddress;
  uint8_t fallbackAddress;
  uint16_t width;
  uint16_t height;
  uint32_t preclkHz;
  uint32_t postclkHz;
};

struct ServoConfig {
  uint8_t address;
  uint8_t channels[2];
  uint8_t channelCount;
  float pwmFrequencyHz;
  uint32_t oscillatorHz;
  uint16_t neutralPulseUs;
  uint16_t forwardPulseUs;
  uint16_t reversePulseUs;
};

struct CanConfig {
  gpio_num_t txPin;
  gpio_num_t rxPin;
  uint32_t bitrate;
  uint8_t motorAddress;
};

struct AppConfig {
  uint32_t serialBaudrate;
  I2cConfig i2c;
  OledConfig oled;
  ServoConfig servo;
  CanConfig canBus;
  bool enableServoMotionTest;
};

inline AppConfig defaultAppConfig() {
  AppConfig config{};
  config.serialBaudrate = 115200;

  config.i2c = {6, 7, 400000};
  config.oled = {0x3C, 0x3D, 128, 32, 400000, 100000};

  config.servo.address = 0x40;
  config.servo.channels[0] = 0;
  config.servo.channels[1] = 1;
  config.servo.channelCount = 2;
  config.servo.pwmFrequencyHz = 50.0f;
  config.servo.oscillatorHz = 27000000;
  config.servo.neutralPulseUs = 1500;
  config.servo.forwardPulseUs = 2000;
  config.servo.reversePulseUs = 1000;

  config.canBus.txPin = GPIO_NUM_4;
  config.canBus.rxPin = GPIO_NUM_5;
  config.canBus.bitrate = 500000;
  config.canBus.motorAddress = 1;
  config.enableServoMotionTest = false;

  return config;
}

}  // namespace test_app
