#pragma once

#include <Arduino.h>

#include "auto_typer_types.h"
#include "config/MachineConfig.h"

namespace auto_typer {

inline TypingConfig defaultTypingConfig() {
  TypingConfig config{};
  config.serialBaudrate = 115200;
  config.deviceId = "esp32-s3-auto-typer";
  config.firmwareVersion = "0.1.0";

  config.canBus.txPin = GPIO_NUM_4;
  config.canBus.rxPin = GPIO_NUM_5;
  config.canBus.bitrate = 500000;

  config.oled = {0x3C, 0x3D, 128, 32, 400000, 100000};

  config.topology = {1, 2, 3, 4, 5};
  config.calibration = {2.0f, 20, 3200};

  config.motionRuntime = defaultMotionRuntimeConfig();
  config.pressMotor = {3000, 255, -2700, 0, 80, 8000};
  config.xProfile = {config.motionRuntime.defaultMoveRpm, config.motionRuntime.defaultAccelerationRaw, 120};
  config.yProfile = {config.motionRuntime.defaultMoveRpm, config.motionRuntime.defaultAccelerationRaw, 120};
  config.xReturn = {true, 200, 3, 200, 180};
  config.yReturn = {true, 200, 3, 100, 180};
  config.lineFeed = {500, 10, 16400, 10000, 180, MotorDirection::Ccw, 400, 80};

  config.homePoint = {0.0f, 0.0f};
  config.sampleText = "asdf jkl";

  return config;
}

}  // namespace auto_typer
