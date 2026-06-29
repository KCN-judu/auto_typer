#include <Arduino.h>

#include "app_config.h"
#include "app_runtime.h"
#include "hal_can_motor.h"
#include "hal_display.h"
#include "hal_i2c.h"
#include "hal_servo.h"

namespace {

using namespace test_app;

const AppConfig kConfig = defaultAppConfig();
I2cBusHal gI2cBus(kConfig.i2c);
DisplayHal gDisplay(kConfig.oled);
ServoHal gServo(kConfig.servo);
CanMotorHal gMotor(kConfig.canBus);
TestApplication gApp(kConfig, gI2cBus, gDisplay, gServo, gMotor, Serial);

}  // namespace

void setup() {
  Serial.begin(kConfig.serialBaudrate);
  delay(300);
  gApp.printBanner();
  gApp.setup();
}

void loop() {
  gApp.tick(millis());
  delay(100);
}
