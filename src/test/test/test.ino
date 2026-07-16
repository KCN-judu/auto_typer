#include <Arduino.h>

#include "appConfig.h"
#include "appRuntime.h"
#include "halCanMotor.h"
#include "halDisplay.h"
#include "halI2c.h"
#include "halServo.h"

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
