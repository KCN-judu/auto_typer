#pragma once

#include <Wire.h>

#include "appConfig.h"
#include "appTypes.h"

namespace test_app {

class I2cBusHal {
 public:
  explicit I2cBusHal(const I2cConfig& config) : config_(config) {}

  void begin() const {
    Wire.begin(config_.sdaPin, config_.sclPin, config_.frequencyHz);
  }

  DevicePresence scan(const OledConfig& oled, const ServoConfig& servo, Print& log) const {
    log.println("I2C scan start");

    DevicePresence presence{};
    presence.oledAddress = 0;
    presence.deviceCount = 0;
    presence.pca9685Detected = false;

    for (uint8_t address = 1; address < 0x7F; ++address) {
      if (!probe(address)) {
        continue;
      }

      log.print("  found 0x");
      printHexByte(log, address);
      log.println();
      ++presence.deviceCount;

      if (address == oled.primaryAddress || address == oled.fallbackAddress) {
        presence.oledAddress = address;
      }
      if (address == servo.address) {
        presence.pca9685Detected = true;
      }
    }

    log.print("I2C scan done, devices=");
    log.println(presence.deviceCount);
    return presence;
  }

  bool probe(uint8_t address) const {
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
  }

  static void printHexByte(Print& log, uint8_t value) {
    if (value < 0x10) {
      log.print('0');
    }
    log.print(value, HEX);
  }

 private:
  I2cConfig config_;
};

}  // namespace test_app
