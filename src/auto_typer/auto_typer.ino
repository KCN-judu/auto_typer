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
#include "transport/NullPrint.h"
#include "transport/SerialWifiSetup.h"

#ifndef AUTO_TYPER_SERIAL_DEBUG_LOGS
#define AUTO_TYPER_SERIAL_DEBUG_LOGS 0
#endif

namespace {

using namespace auto_typer;

const TypingConfig kConfig = defaultTypingConfig();
#if AUTO_TYPER_SERIAL_DEBUG_LOGS
Print& gLog = Serial;
#else
NullPrint gNullLog;
Print& gLog = gNullLog;
#endif
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
                          gLog);
GroupCommandServer gGroupServer(kConfig, gApp, gMotorTelemetry, gLog);
SerialWifiSetup gSerialWifi(kConfig, gApp, Serial, gLog);

}  // namespace

void setup() {
  Serial.begin(kConfig.serialBaudrate);
  delay(300);
  gApp.setup();
  gSerialWifi.begin();
  gGroupServer.begin();
  WiFi.setSleep(false);
}

void loop() {
  gSerialWifi.tick();
  gGroupServer.tick();
  gApp.tick();
  delay(1);
}
