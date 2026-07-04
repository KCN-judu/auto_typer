#include "AutoTyperFirmware.h"

#include <Arduino.h>

#include "auto_typer_config.h"
#include "auto_typer_runtime.h"
#include "can/CanBus.h"
#include "can/CanRxTask.h"
#include "can/CanTxQueue.h"
#include "can/EmmV5EventStore.h"
#include "can/ProtocolTrace.h"
#include "drivers/EmmV5Driver.h"
#include "hal_display.h"
#include "network/StaticWifiConnector.h"
#include "transport/GroupCommandServer.h"
#include "transport/NullPrint.h"

#ifndef AUTO_TYPER_SERIAL_DEBUG_LOGS
#define AUTO_TYPER_SERIAL_DEBUG_LOGS 0
#endif

namespace {

using namespace auto_typer;

const TypingConfig kConfig = defaultTypingConfig();
FirmwareConfig gFirmwareConfig = {};
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
StaticWifiConnector gWifi(gLog);
bool gGroupServerStarted = false;

}  // namespace

namespace auto_typer {

void autoTyperSetup(const FirmwareConfig& config) {
  gFirmwareConfig = config;
  Serial.begin(kConfig.serialBaudrate);
  delay(300);
  gWifi.begin(gFirmwareConfig.wifi);
  gApp.setup();
}

void autoTyperLoop() {
  gWifi.tick();
  if (!gGroupServerStarted && gWifi.consumeTcpReady()) {
    gGroupServer.begin();
    gGroupServerStarted = true;
  }
  if (gGroupServerStarted) {
    gGroupServer.tick();
  }
  gApp.tick();
  delay(1);
}

}  // namespace auto_typer
