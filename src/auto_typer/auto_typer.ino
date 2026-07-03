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
#include "transport/GroupCommandServer.h"

namespace {

using namespace auto_typer;

const TypingConfig kConfig = defaultTypingConfig();
DisplayHal gDisplay(kConfig.oled);
CanBus gCanBus(kConfig.canBus);
MotorFeedbackStore gFeedback;
MotorTelemetryBuffer gMotorTelemetry;
EmmV5EventStore gEvents;
ProtocolTrace gTrace;
CanTxQueue gCanTx(gCanBus, &gTrace);
CanRxTask gCanRx(gCanBus, gFeedback, gEvents, gTrace, &gMotorTelemetry);
EmmV5Driver gMotion(gCanTx);
AutoTyperApplication gApp(kConfig,
                          gDisplay,
                          gCanBus,
                          gCanTx,
                          gCanRx,
                          gMotion,
                          gFeedback,
                          gEvents,
                          gTrace,
                          Serial);
GroupCommandServer gGroupServer(kConfig, gApp, gMotorTelemetry, Serial);

}  // namespace

void setup() {
  Serial.begin(kConfig.serialBaudrate);
  delay(300);
  gApp.setup();
  gGroupServer.begin();
  WiFi.setSleep(false);
}

void loop() {
  gGroupServer.tick();
  gApp.tick();
  delay(1);
}
