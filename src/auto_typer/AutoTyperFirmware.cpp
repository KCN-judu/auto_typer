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
#include "network/ProvisioningWifiConnector.h"
#include "transport/MotionProtocolServer.h"
#include "transport/NullPrint.h"

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
EmmV5EventStore gEvents;
ProtocolTrace gTrace;
CanTxQueue gCanTx(gCanBus, &gTrace);
CanRxTask gCanRx(gCanBus, gFeedback, gEvents, gTrace);
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
MotionProtocolServer gMotionServer(kConfig, gApp, gLog);
ProvisioningWifiConnector gWifi(gLog);
bool gMotionServerStarted = false;

}  // namespace

namespace auto_typer {

void autoTyperSetup() {
  Serial.begin(kConfig.serialBaudrate);
  delay(300);
  gWifi.begin();
  gApp.setup();
}

void autoTyperLoop() {
  gWifi.tick();
  if (!gMotionServerStarted && gWifi.consumeTcpReady()) {
    gMotionServer.begin();
    gMotionServerStarted = true;
  }
  if (gMotionServerStarted) {
    gMotionServer.tick();
  }
  gApp.tick();
  delay(1);
}

}  // namespace auto_typer
