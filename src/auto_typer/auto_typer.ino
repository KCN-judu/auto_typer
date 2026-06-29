#include <Arduino.h>

#include "auto_typer_config.h"
#include "auto_typer_runtime.h"
#include "hal_display.h"
#include "hal_emm_can_motion.h"
#include "hal_servo_press.h"
#include "http_control_server.h"

namespace {

using namespace auto_typer;

const TypingConfig kConfig = defaultTypingConfig();
DisplayHal gDisplay(kConfig.oled);
EmmCanMotionHal gMotion(kConfig.canBus);
ServoPressHal gServo(kConfig.servo);
AutoTyperApplication gApp(kConfig, gDisplay, gMotion, gServo, Serial);
HttpControlServer gHttp(kConfig, gApp, Serial);

}  // namespace

void setup() {
  Serial.begin(kConfig.serialBaudrate);
  delay(300);
  gApp.setup();
  gHttp.begin();
}

void loop() {
  gHttp.tick();
  gApp.tick();
  delay(10);
}
