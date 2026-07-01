#include <Arduino.h>

#include "auto_typer_config.h"
#include "auto_typer_runtime.h"
#include "can/CanBus.h"
#include "can/EmmV5EventStore.h"
#include "can/CanRxTask.h"
#include "can/CanTxQueue.h"
#include "can/ProtocolTrace.h"
#include "drivers/EmmV5Driver.h"
#include "hal_display.h"
#include "hal_servo_press.h"
#include "http_control_server.h"

namespace {

using namespace auto_typer;

const TypingConfig kConfig = defaultTypingConfig();
DisplayHal gDisplay(kConfig.oled);
CanBus gCanBus(kConfig.canBus);
MotorFeedbackStore gFeedback;
EmmV5EventStore gEvents;
ProtocolTrace gTrace;
CanTxQueue gCanTx(gCanBus, &gTrace);
CanRxTask gCanRx(gCanBus, gFeedback, gEvents, gTrace);
EmmV5Driver gMotion(gCanTx, &gTrace);
ServoPressHal gServo(kConfig.servo);
AutoTyperApplication gApp(kConfig,
                          gDisplay,
                          gCanBus,
                          gCanTx,
                          gCanRx,
                          gMotion,
                          gServo,
                          gFeedback,
                          gEvents,
                          gTrace,
                          Serial);
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
