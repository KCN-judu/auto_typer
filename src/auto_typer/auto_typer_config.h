#pragma once

#include <Arduino.h>

#include "auto_typer_types.h"
#include "config/MachineConfig.h"

#if __has_include("config/Secrets.h")
#include "config/Secrets.h"
#else
#define AUTO_TYPER_WIFI_SSID ""
#define AUTO_TYPER_WIFI_PASSWORD ""
#endif

namespace auto_typer {

inline TypingConfig defaultTypingConfig() {
  TypingConfig config{};
  config.serialBaudrate = 115200;
  config.wifiSsid = AUTO_TYPER_WIFI_SSID;
  config.wifiPassword = AUTO_TYPER_WIFI_PASSWORD;
  config.deviceId = "esp32-s3-auto-typer";
  config.firmwareVersion = "0.1.0";

  config.canBus.txPin = GPIO_NUM_4;
  config.canBus.rxPin = GPIO_NUM_5;
  config.canBus.bitrate = 500000;

  config.oled = {0x3C, 0x3D, 128, 32, 400000, 100000};

  config.servo.address = 0x40;
  config.servo.channels[0] = 0;
  config.servo.channels[1] = 1;
  config.servo.channelCount = 2;
  config.servo.pwmFrequencyHz = 50.0f;
  config.servo.oscillatorHz = 27000000;
  config.servo.neutralPulseUs = 1500;
  config.servo.forwardPulseUs = 2400;
  config.servo.reversePulseUs = 600;
  config.servo.releaseMs = 300;
  config.servo.pressMs = 600;
  config.servo.settleMs = 80;
  config.servo.semantics = {ServoMotion::Reverse, ServoMotion::Forward};

  config.topology = {1, 2, 3, 4};
  config.calibration = {2.0f, 20, 3200};

  config.motionRuntime = defaultMotionRuntimeConfig();
  config.xProfile = {config.motionRuntime.defaultMoveRpm, config.motionRuntime.defaultAccelerationRaw, 120};
  config.yProfile = {config.motionRuntime.defaultMoveRpm, config.motionRuntime.defaultAccelerationRaw, 120};
  config.xReturn = {true, 200, 3, 200, 180};
  config.yReturn = {true, 200, 3, 100, 180};
  config.lineFeed = {500, 10, 16440, MotorDirection::Cw, 6400, 180, MotorDirection::Ccw, 400, 80};

  config.homePoint = {0.0f, 0.0f};
  config.sampleText = "asdf jkl";

  return config;
}

}  // namespace auto_typer
